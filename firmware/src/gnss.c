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
#ifdef CONFIG_APP_DEMO_MODE
#include <ctype.h>
#endif
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

static void pvt_to_fix(const struct nrf_modem_gnss_pvt_data_frame *pvt,
                       struct gnss_fix *out)
{
    out->valid         = true;
    out->fix_uptime_ms = k_uptime_get();
    snprintf(out->lat_str, sizeof(out->lat_str), "%.6f", pvt->latitude);
    snprintf(out->lon_str, sizeof(out->lon_str), "%.6f", pvt->longitude);
    out->speed_kmh   = pvt->speed * 3.6f;
    out->altitude_m  = pvt->altitude;
    out->heading_deg = pvt->heading;
    out->hdop_x10    = (long)(pvt->hdop * 10.0f);
    long sats = 0;
    for (int i = 0; i < NRF_MODEM_GNSS_MAX_SATELLITES; i++) {
        if (pvt->sv[i].sv != 0 &&
            (pvt->sv[i].flags & NRF_MODEM_GNSS_SV_FLAG_USED_IN_FIX))
            sats++;
    }
    out->sats = sats;
    snprintf(out->time_iso, sizeof(out->time_iso),
             "%02u/%02u/%02u,%02u:%02u:%02u.%06u+00",
             pvt->datetime.day, pvt->datetime.month,
             (unsigned)(pvt->datetime.year % 100),
             pvt->datetime.hour, pvt->datetime.minute,
             pvt->datetime.seconds,
             (unsigned)pvt->datetime.ms * 1000U);
}

#ifdef CONFIG_APP_DEMO_MODE
/*
 * Demo mode: swap coordinates for DEMO_COORD_MASK on their way to the console.
 * Only the log is masked — the telemetry record and the alert text still carry
 * the real position to the server.
 *
 * Coordinates are recognised by shape rather than by position, so one pass
 * serves a CSV telemetry record, an alert string, or anything else that prints
 * one: an optional sign, digits, a dot, and five or more decimals, starting a
 * field.  lat/lon are the only "%.6f" the firmware emits, so demanding five
 * decimals leaves the "%.2f" voltages and "%.1f" temperatures alone, and
 * demanding a field start (never a ':') keeps the timestamp's fractional
 * seconds readable.  The masked copy is never longer than the input: a
 * six-decimal coordinate is at least 8 characters and the mask is 7.
 */
const char *demo_mask_coords(const char *in, size_t len,
                             char *out, size_t out_sz)
{
    size_t o = 0;

    if (out_sz == 0) return out;

    for (size_t i = 0; i < len && o < out_sz - 1; ) {
        bool field_start = (i == 0) || in[i - 1] == ',' ||
                           in[i - 1] == ' ' || in[i - 1] == '=';
        size_t j = i, int_digits = 0;

        if (field_start) {
            if (in[j] == '-' || in[j] == '+') j++;
            while (j < len && isdigit((unsigned char)in[j])) { j++; int_digits++; }

            if (int_digits > 0 && j < len && in[j] == '.') {
                size_t k = j + 1, decimals = 0;
                while (k < len && isdigit((unsigned char)in[k])) { k++; decimals++; }

                if (decimals >= 5) {
                    for (const char *m = DEMO_COORD_MASK;
                         *m && o < out_sz - 1; m++) {
                        out[o++] = *m;
                    }
                    i = k;
                    continue;
                }
            }
        }
        out[o++] = in[i++];
    }
    out[o] = '\0';
    return out;
}
#endif /* CONFIG_APP_DEMO_MODE */

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
            pvt_to_fix(&pvt, &g_gnss);
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

/* Optional callback run about once a second while gnss_collect() waits.
 * The GPS wait is most of a telemetry cycle and the thread is otherwise
 * asleep on a semaphore, so it is the one place a second subsystem can be
 * sampled at a useful rate without a thread of its own — and being called
 * from this thread, it needs no locking against the code that runs before
 * and after the wait.  Keep whatever runs here short. */
static void (*s_tick_cb)(void);

void gnss_set_tick(void (*cb)(void))
{
	s_tick_cb = cb;
}

int gnss_collect(int timeout_ms, struct gnss_fix *out)
{
    bool cold = !s_have_fix;

    if (cold && timeout_ms >= GPS_FIX_TIMEOUT_MS) {
        timeout_ms = GPS_COLD_FIX_TIMEOUT_MS;
        LOG_INF("cold start: timeout extended to %ds", timeout_ms / 1000);
    }

    if (cold && s_agnss_needed) {
        /* Fallback path: the receiver asked for something specific after
         * the boot-time fetch.  The request is only cleared once the fetch
         * actually succeeds — clearing it first meant one timeout cost the
         * assistance for the whole cold start, with no retry until the
         * receiver happened to ask again. */
        if (agnss_fetch(&s_agnss_req) == 0) {
            s_agnss_needed = false;
        } else {
            LOG_WRN("A-GNSS fetch failed — request kept for a retry");
        }
    }

    if (cold) {
        nrf_modem_gnss_prio_mode_enable();
    }

    int remaining = timeout_ms;
    int err = -EAGAIN;
    int since_prio = 0;
    const int tick = s_tick_cb ? 1000 : 10000;

    while (remaining > 0) {
        int chunk = (remaining > tick) ? tick : remaining;
        err = k_sem_take(&s_fix_sem, K_MSEC(chunk));
        if (err == 0) {
            break;
        }
        remaining -= chunk;
        since_prio += chunk;
        watchdog_kick();

        if (s_tick_cb) {
            s_tick_cb();
        }

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

    pvt_to_fix(&s_pvt, out);
    return 0;
}
