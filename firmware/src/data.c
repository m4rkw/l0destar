/*
 * Telemetry record builder.  Ported from data.ino with cell tower fallback,
 * accelerometer fields, and settings sync.
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "app.h"

LOG_MODULE_REGISTER(data, CONFIG_APP_LOG_LEVEL);

char  data_current[DATA_LIMIT];
int   data_index;
bool  send_int_to_server;
bool  force_record;
bool  last_record_stale;
bool  last_send_ok;

static int  s_battery_warning_status;
/* Cleared once a packet carrying fw= has actually left the device, so a
 * failed first send doesn't lose the version until the next reboot. */
static bool s_fw_pending = true;
static int  s_below_voltage_count;

void data_reset(void)
{
    memset(data_current, 0, sizeof(data_current));
    data_index = 0;
}

static float battery_sample_with_engine_check(void)
{
    float v = battery_read_voltage();
    battery_v = v;
    if (v >= ENGINE_RUNNING_VOLTAGE) {
        s_below_voltage_count = 0;
        engine_running = true;
    } else {
        s_below_voltage_count++;
        if (s_below_voltage_count >= ENGINE_STOPPED_COUNT) {
            engine_running = false;
        }
    }
    return v;
}

/* -- data collection ------------------------------------------------------ */

int collect_data(int ignitionState)
{
    int have_fix = 0;
    bool stale = false;

    if (use_cached_gps) {
        have_fix = g_gnss.valid;
    } else {
        struct gnss_fix fix = {0};
        if (gnss_collect(GPS_FIX_TIMEOUT_MS, &fix) == 0 && fix.valid) {
            g_gnss = fix;
            have_fix = 1;
        }
    }

    if (!have_fix) {
        /* An ignition change has to reach the server whether or not GNSS can
         * see the sky, and "parked in a garage" is both when the final point
         * matters most and when there is no fix to be had.  Dropping the
         * record also swallowed the transition: the caller updated
         * previous_ignition regardless, so nothing ever retried it and the
         * ignition-off point was simply lost.
         *
         * So when the caller marks the record as one that must go out, build
         * it from the last known position instead.  At ignition-off that is
         * where the vehicle actually is, and cl=1 tells the server the
         * position is not a fresh fix. */
        if (!force_record) {
            LOG_WRN("no GPS fix");
            return 0;
        }
        /* Needs a last known position to fall back to.  With none — a unit
         * that has not had a fix since boot — the CSV would carry empty
         * lat/lon straight into the server's `log` row, which is worse than
         * the missing record.  Rare in practice: this path follows a drive. */
        if (g_gnss.lat_str[0] == '\0' || g_gnss.lon_str[0] == '\0') {
            LOG_WRN("no GPS fix and no last known position — record dropped");
            return 0;
        }
        stale = true;
        LOG_INF("no GPS fix — reporting from last known position");
    }
    last_record_stale = stale;

    float v = battery_sample_with_engine_check();

    if (data_index > 0 && data_index < DATA_LIMIT - 1) {
        data_current[data_index++] = '\n';
    }

    int remaining = DATA_LIMIT - data_index - 1;
    int n;
    char now_iso[40] = "";
    const char *ts;

    if (!use_cached_gps && g_gnss.time_iso[0]) {
        ts = g_gnss.time_iso;
    } else {
        struct timespec tp;
        if (clock_gettime(CLOCK_REALTIME, &tp) == 0 && tp.tv_sec > 1000000000) {
            struct tm tm;
            gmtime_r(&tp.tv_sec, &tm);
            snprintf(now_iso, sizeof(now_iso),
                     "%02d/%02d/%02d,%02d:%02d:%02d.%06ld+00",
                     tm.tm_mday, tm.tm_mon + 1, tm.tm_year % 100,
                     tm.tm_hour, tm.tm_min, tm.tm_sec,
                     tp.tv_nsec / 1000);
            ts = now_iso;
        } else {
            ts = g_gnss.time_iso[0] ? g_gnss.time_iso
                                     : "01/01/00,00:00:00.000000+00";
        }
    }

    /* Both describe a fix we don't have.  A stale speed would also read as
     * motion, which is wrong for a vehicle that has just been switched off. */
    float speed = stale ? 0.0f : g_gnss.speed_kmh;
    long  sats  = stale ? 0     : g_gnss.sats;
    n = snprintf(&data_current[data_index], remaining,
        "%s,%s,%s,%.2f,%.2f,%.2f,%ld,%ld,%.2f,%d,%lld,%d",
        ts, g_gnss.lat_str, g_gnss.lon_str,
        (double)speed, (double)g_gnss.altitude_m,
        (double)g_gnss.heading_deg,
        g_gnss.hdop_x10, sats,
        (double)v,
        (ignitionState == 0) ? 1 : 0,
        k_uptime_get() / 1000,
        powered_on ? 1 : 0);

    if (n > 0 && n < remaining) data_index += n;

    /* Accelerometer */
    int ax, ay, az;
    if (accel_read(&ax, &ay, &az) == 0) {
        n = snprintf(&data_current[data_index],
                     DATA_LIMIT - data_index - 1,
                     ",ax=%d;ay=%d;az=%d", ax, ay, az);
        if (n > 0) data_index += n;
    }

    /* Gyro (bias-corrected LSB at ±250 dps) + IMU die temperature (°C).
     * Re-learn the zero-rate bias whenever we're genuinely stopped (good
     * fix, ~0 speed) so the logged rates aren't skewed by the sensor's
     * temperature-dependent offset. */
    if (g_gnss.sats >= SPEED_MIN_SATS && g_gnss.speed_kmh < GYRO_REST_KMH) {
        accel_gyro_autozero();
    }
    int gx, gy, gz;
    if (accel_read_gyro(&gx, &gy, &gz) == 0) {
        n = snprintf(&data_current[data_index],
                     DATA_LIMIT - data_index - 1,
                     ",gx=%d;gy=%d;gz=%d", gx, gy, gz);
        if (n > 0) data_index += n;
    }
    float imu_temp;
    if (accel_read_temp(&imu_temp) == 0) {
        n = snprintf(&data_current[data_index],
                     DATA_LIMIT - data_index - 1,
                     ",it=%.1f", (double)imu_temp);
        if (n > 0) data_index += n;
    }

    /* nRF9151 SiP die temperature (°C) */
    float mcu_temp;
    if (modem_read_temp(&mcu_temp) == 0) {
        n = snprintf(&data_current[data_index],
                     DATA_LIMIT - data_index - 1,
                     ",mt=%.1f", (double)mcu_temp);
        if (n > 0) data_index += n;
    }

    /* nRF9151 supply voltage (V).  This is the SiP's VDD pin, which on the
     * Connect Kit is VSYS out of the BQ25180 — the carrier's 4.2V buck feeding
     * the battery connector, or ~4.5V when the charger is running SYS off USB
     * VBUS with a cable plugged in.  Distinct from the vehicle battery above:
     * that one comes off the INA228 and is the ~12V rail. */
    int vsys_mv;
    if (modem_read_vbat(&vsys_mv) == 0) {
        n = snprintf(&data_current[data_index],
                     DATA_LIMIT - data_index - 1,
                     ",vs=%.2f", vsys_mv / 1000.0);
        if (n > 0) data_index += n;
    }

    /* Cell tower fields — emit on first post-wake packet or as GPS fallback.
     * cl=1 is the server's "this position came from the network, not GNSS"
     * flag; until now it was hardcoded to 0, so the fallback the protocol
     * already allowed for could never actually happen. */
    if (g_cell.valid && (g_cell.dirty || stale)) {
        n = snprintf(&data_current[data_index],
                     DATA_LIMIT - data_index - 1,
                     ",mcc=%d;mnc=%d;lac=%u;cid=%u;cl=%d;rat=%s",
                     g_cell.mcc, g_cell.mnc,
                     g_cell.tac, g_cell.cid, stale ? 1 : 0,
                     modem_rat());
        if (n > 0) data_index += n;
        g_cell.dirty = false;
    }

    /* Uptime */
    n = snprintf(&data_current[data_index],
                 DATA_LIMIT - data_index - 1,
                 ",up=%lld", k_uptime_get() / 1000);
    if (n > 0) data_index += n;

    /* Settings sync */
    if (send_int_to_server) {
        n = snprintf(&data_current[data_index],
                     DATA_LIMIT - data_index - 1,
                     ",int=%d;ao=%d;ma=%d",
                     g_settings.loop_interval,
                     (int)g_settings.always_on,
                     (int)g_settings.movement_alarm);
        if (n > 0) data_index += n;
    }

    /* Firmware version.  send_int_to_server is only ever set by a settings
     * command from the server, so gating fw= on it alone meant a unit whose
     * settings never change never reported its build.  Emit it on the first
     * packet after every boot as well — including the one after a FOTA swap —
     * and the server carries it forward onto subsequent rows.  Still not on
     * every record: ~10 bytes that cannot change without a reboot. */
    if (send_int_to_server || s_fw_pending) {
        n = snprintf(&data_current[data_index],
                     DATA_LIMIT - data_index - 1,
                     ",fw=%s", fota_version());
        if (n > 0) data_index += n;
    }

    data_current[data_index] = '\0';

    /* Battery warning - only meaningful with the engine off. While the engine
     * runs the vehicle's smart/regenerative charging system swings the rail
     * 11.8-14.9V by design (load-shed at idle/under acceleration, boosted on
     * overrun), so an instantaneous low reading mid-drive says nothing about
     * battery health. Gate on ignition-off (ignitionState != 0; the sense is
     * active-low) and skip implausible readings (sensor absent/broken). Normal
     * priority: a resting low battery is informational, not an emergency. */
    if (ignitionState != 0 && v >= IMPLAUSIBLE_VOLTAGE) {
        if (v < BATTERY_WARNING_LEVEL) {
            if (s_battery_warning_status == 0) {
                char msg[24];
                snprintf(msg, sizeof(msg), "low battery: %.2fV", (double)v);
                alert_enqueue(msg, 0);
                s_battery_warning_status = 1;
            }
        } else {
            s_battery_warning_status = 0;
        }
    }

    return 1;
}

/* -- send ----------------------------------------------------------------- */

int send_data(void)
{
    int rec_count = 1;
    for (int i = 0; i < data_index; i++) {
        if (data_current[i] == '\n') rec_count++;
    }

    char *p = data_current;
    for (int r = 0; r < rec_count && p < data_current + data_index; r++) {
        char *nl = memchr(p, '\n', data_current + data_index - p);
        int len = nl ? (int)(nl - p) : (int)(data_current + data_index - p);
#ifdef CONFIG_APP_DEMO_MODE
        /* A record is ~250 bytes at its longest; anything past the buffer is
         * dropped from the console line rather than printed unmasked. */
        char masked[384];
        LOG_INF("[%d/%d] %s", r + 1, rec_count,
                demo_mask_coords(p, (size_t)len, masked, sizeof(masked)));
#else
        LOG_INF("[%d/%d] %.*s", r + 1, rec_count, len, p);
#endif
        p += len + 1;
    }

    int err = transport_send((const uint8_t *)data_current,
                             (size_t)data_index);
    if (err) {
        last_send_ok = false;
        gsm_send_failures++;
        return 0;
    }
    last_send_ok = true;
    gsm_send_failures = 0;
    powered_on = false;

    /* Server response, if requested */
    if (read_udp_response) {
        char resp[256];
        int n = transport_recv_response(resp, sizeof(resp), 2000);
        if (n > 0) {
            LOG_INF("resp: %s", resp);
            /* Plaintext shape:  "1,int,ao,ma[,cmd]"  */
            int interval = -1, ao = -1, ma = -1;
            char cmd[128] = "";
            int matched = sscanf(resp, "1,%d,%d,%d,%127[^\n]",
                                 &interval, &ao, &ma, cmd);
            if (matched >= 3) {
                if (!send_int_to_server) {
                    if (interval >= 0) g_settings.loop_interval = interval;
                    if (ao >= 0)       g_settings.always_on = (int8_t)(ao != 0);
                    if (ma >= 0)       g_settings.movement_alarm = (int8_t)ma;
                }
                if (matched == 4 && cmd[0]) {
                    strncpy(pending_server_cmd, cmd,
                            sizeof(pending_server_cmd) - 1);
                }
            }
        }
    }

    send_int_to_server = false;
    s_fw_pending = false;
    alert_send();
    return 1;
}
