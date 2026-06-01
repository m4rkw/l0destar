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
static bool s_have_fix;

static void on_gnss_event(int event)
{
    if (event != NRF_MODEM_GNSS_EVT_PVT) {
        return;
    }
    struct nrf_modem_gnss_pvt_data_frame pvt;
    if (nrf_modem_gnss_read(&pvt, sizeof(pvt),
                            NRF_MODEM_GNSS_DATA_PVT) != 0) {
        return;
    }
    if (pvt.flags & NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID) {
        s_pvt = pvt;
        s_have_fix = true;
        k_sem_give(&s_fix_sem);
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
    bool need_prio = !s_have_fix;
    if (need_prio) {
        nrf_modem_gnss_prio_mode_enable();
    }

    /* Wait in ≤10 s chunks so the watchdog (32 s) stays fed. */
    int remaining = timeout_ms;
    int err = -EAGAIN;
    while (remaining > 0) {
        int chunk = (remaining > 10000) ? 10000 : remaining;
        err = k_sem_take(&s_fix_sem, K_MSEC(chunk));
        if (err == 0) {
            break;
        }
        remaining -= chunk;
        watchdog_kick();
    }

    if (need_prio) {
        nrf_modem_gnss_prio_mode_disable();
    }

    if (err) {
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
