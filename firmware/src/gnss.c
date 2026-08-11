/*
 * GNSS using the nRF9151's built-in receiver via nrf_modem_gnss.  Replaces
 * the BG96-driven NMEA parsing from gps.ino — fixes are delivered as parsed
 * PVT structs, no NMEA assembly required.
 *
 * Operating mode: continuous fix at 1 Hz once started.  collect() blocks
 * until a fix arrives or the timeout expires.
 */

#include <string.h>
#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <nrf_modem_gnss.h>

#include "app.h"

LOG_MODULE_REGISTER(gnss, CONFIG_APP_LOG_LEVEL);

struct gnss_fix g_gnss;

static K_SEM_DEFINE(s_fix_sem, 0, 1);
static struct nrf_modem_gnss_pvt_data_frame s_pvt;
static struct nrf_modem_gnss_agnss_data_frame s_agnss_req;
static bool s_have_fix;
static bool s_agnss_needed;
static int  s_tracked_sv;
static bool s_blocked;

static void on_gnss_event(int event)
{
    switch (event) {
    case NRF_MODEM_GNSS_EVT_PVT: {
        struct nrf_modem_gnss_pvt_data_frame pvt;
        if (nrf_modem_gnss_read(&pvt, sizeof(pvt),
                                NRF_MODEM_GNSS_DATA_PVT) != 0) {
            return;
        }

        int tracked = 0, in_fix = 0;
        int16_t best_cn0 = 0;
        for (int i = 0; i < NRF_MODEM_GNSS_MAX_SATELLITES; i++) {
            if (pvt.sv[i].sv == 0) continue;
            tracked++;
            if (pvt.sv[i].cn0 > best_cn0) best_cn0 = pvt.sv[i].cn0;
            if (pvt.sv[i].flags & NRF_MODEM_GNSS_SV_FLAG_USED_IN_FIX)
                in_fix++;
        }
        s_tracked_sv = tracked;

        if ((pvt.flags & NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID) && in_fix > 0) {
            s_pvt = pvt;
            s_have_fix = true;
            k_sem_give(&s_fix_sem);
        } else if (!s_have_fix && !s_blocked) {
            LOG_INF("searching: %d SVs tracked, best cn0=%d.%d",
                    tracked, best_cn0 / 10, best_cn0 % 10);
        }
        break;
    }
    case NRF_MODEM_GNSS_EVT_BLOCKED:
        LOG_WRN("GNSS blocked by LTE");
        s_blocked = true;
        break;
    case NRF_MODEM_GNSS_EVT_UNBLOCKED:
        LOG_INF("GNSS unblocked");
        s_blocked = false;
        break;
    case NRF_MODEM_GNSS_EVT_AGNSS_REQ:
        if (nrf_modem_gnss_read(&s_agnss_req, sizeof(s_agnss_req),
                                NRF_MODEM_GNSS_DATA_AGNSS_REQ) == 0) {
            s_agnss_needed = true;
            LOG_INF("A-GNSS data requested (flags=0x%02x)",
                    s_agnss_req.data_flags);
        }
        break;
    case NRF_MODEM_GNSS_EVT_SLEEP_AFTER_TIMEOUT:
        LOG_WRN("GNSS sleep after timeout");
        break;
    default:
        break;
    }
}

int gnss_init(void)
{
    int err = nrf_modem_gnss_event_handler_set(on_gnss_event);
    if (err) {
        LOG_ERR("event_handler_set: %d", err);
        return err;
    }
    nrf_modem_gnss_fix_interval_set(1);
    nrf_modem_gnss_fix_retry_set(0);
    LOG_INF("init ok");
    return 0;
}

int gnss_start(void)
{
    s_have_fix = false;
    s_blocked = false;
    k_sem_reset(&s_fix_sem);
    int err = nrf_modem_gnss_start();
    if (err) LOG_ERR("start: %d", err);
    return err;
}

int gnss_stop(void)
{
    return nrf_modem_gnss_stop();
}

int gnss_resume(void)
{
    k_sem_reset(&s_fix_sem);
    int err = nrf_modem_gnss_start();
    if (err) LOG_ERR("resume: %d", err);
    return err;
}

int gnss_collect(int timeout_ms, struct gnss_fix *out)
{
    bool cold = !s_have_fix;

    if (cold && timeout_ms >= GPS_FIX_TIMEOUT_MS) {
        timeout_ms = GPS_COLD_FIX_TIMEOUT_MS;
        LOG_INF("cold start: timeout extended to %ds", timeout_ms / 1000);
    }

    if (cold && s_agnss_needed) {
        s_agnss_needed = false;
        agnss_fetch(&s_agnss_req);
    }

    if (cold) {
        nrf_modem_gnss_prio_mode_enable();
    }

    int remaining = timeout_ms;
    int err = -EAGAIN;
    int since_prio = 0;
    while (remaining > 0) {
        int chunk = (remaining > 10000) ? 10000 : remaining;
        err = k_sem_take(&s_fix_sem, K_MSEC(chunk));
        if (err == 0) {
            break;
        }
        remaining -= chunk;
        since_prio += chunk;
        watchdog_kick();

        if (cold && since_prio >= 30000) {
            nrf_modem_gnss_prio_mode_enable();
            since_prio = 0;
            LOG_INF("cold start: %ds remaining, %d SVs%s",
                    remaining / 1000, s_tracked_sv,
                    s_blocked ? " (blocked)" : "");
        }
    }

    nrf_modem_gnss_prio_mode_disable();

    if (err) {
        LOG_WRN("%s fix timeout — restarting GNSS",
                cold ? "cold" : "warm");
        nrf_modem_gnss_stop();
        k_msleep(500);
        s_have_fix = false;
        k_sem_reset(&s_fix_sem);
        nrf_modem_gnss_start();
        out->valid = false;
        return err;
    }

    out->valid          = true;
    out->fix_uptime_ms  = k_uptime_get();
    snprintf(out->lat_str, sizeof(out->lat_str), "%.6f", s_pvt.latitude);
    snprintf(out->lon_str, sizeof(out->lon_str), "%.6f", s_pvt.longitude);
    out->speed_kmh   = s_pvt.speed * 3.6f;            /* m/s → km/h */
    out->altitude_m  = s_pvt.altitude;
    out->heading_deg = s_pvt.heading;
    out->hdop_x10    = (long)(s_pvt.hdop * 10.0f);
    /* Count satellites with sv > 0 and tracked flag set. */
    long sats = 0;
    for (int i = 0; i < NRF_MODEM_GNSS_MAX_SATELLITES; i++) {
        if (s_pvt.sv[i].sv != 0 &&
            (s_pvt.sv[i].flags & NRF_MODEM_GNSS_SV_FLAG_USED_IN_FIX)) {
            sats++;
        }
    }
    out->sats = sats;
    snprintf(out->time_iso, sizeof(out->time_iso),
             "%02u/%02u/%02u,%02u:%02u:%02u.%06u+00",
             s_pvt.datetime.day, s_pvt.datetime.month,
             (unsigned)(s_pvt.datetime.year % 100),
             s_pvt.datetime.hour, s_pvt.datetime.minute,
             s_pvt.datetime.seconds,
             (unsigned)s_pvt.datetime.ms * 1000U);
    return 0;
}
