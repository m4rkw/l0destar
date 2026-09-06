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

/* The per-record cap only exists in Kconfig while APP_DEBUG_LOG is on; with
 * it off there is nothing to append and dbglog_take() is a stub anyway. */
#ifndef CONFIG_APP_DEBUG_LOG_PER_RECORD
#define CONFIG_APP_DEBUG_LOG_PER_RECORD 0
#endif

char  data_current[DATA_LIMIT];
int   data_index;
bool  send_int_to_server;
bool  force_record;
bool  last_record_stale;
bool  last_send_ok;

static int  s_battery_warning_status;
/* Uptime at which the ignition was last seen going off, for the warning's
 * settle time.  -1 until a record has been built with it off. */
static int64_t s_ign_off_ms = -1;
static int8_t  s_ign_last = -1;
/* Cleared once a packet carrying fw= has actually left the device, so a
 * failed first send doesn't lose the version until the next reboot. */
static bool s_fw_pending = true;
static int  s_below_voltage_count;

void data_reset(void)
{
    memset(data_current, 0, sizeof(data_current));
    data_index = 0;
}

/* Is the engine running?
 *
 * RPM from the ECU is a direct measurement and settles it whenever a fresh
 * figure exists.  Charging voltage is only a proxy, and a poor one: a tired
 * battery, a smart alternator shedding load, or a noisy INA228 read all put
 * the rail below ENGINE_RUNNING_VOLTAGE while the car is being driven, and
 * that used to drop the tracker to its engine-off cadence mid-journey.  Fall
 * back to voltage only when the ECU is not answering — no K wire on this
 * build, no session, or a reading that has gone stale. */
bool engine_is_running(void)
{
#if IS_ENABLED(CONFIG_APP_KLINE_OBD)
    int rpm = obd_rpm();

    if (rpm >= 0) {
        return rpm > 0;
    }
#endif
    return battery_v >= ENGINE_RUNNING_VOLTAGE;
}

static float battery_sample_with_engine_check(void)
{
    float v = battery_read_voltage();
    battery_v = v;

#if IS_ENABLED(CONFIG_APP_KLINE_OBD)
    /* Same precedence as engine_is_running(): a fresh RPM figure wins over
     * whatever the rail reads.  The voltage hysteresis below is reset so
     * that losing the ECU later falls back to it from a clean slate. */
    int rpm = obd_rpm();

    if (rpm >= 0) {
        s_below_voltage_count = 0;
        engine_running = rpm > 0;
        return v;
    }
#endif
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

/* The fields every record type ends with. */
static void append_sync_fields(void)
{
    int n;

    /* Settings sync */
    if (send_int_to_server) {
        n = snprintf(&data_current[data_index],
                     DATA_LIMIT - data_index - 1,
                     ",int=%d;ma=%d",
                     g_settings.loop_interval,
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

    /* Why this boot happened, once.  Rides with fw= on the first record
     * after every restart so a gap in telemetry can be told apart from a
     * reboot: the server files both in its debug log. */
    if (s_fw_pending) {
        const char *rst = dbglog_reset_cause();

        if (rst != NULL) {
            n = snprintf(&data_current[data_index],
                         DATA_LIMIT - data_index - 1,
                         ",rst=%s", rst);
            if (n > 0) data_index += n;
        }
    }
}

/* Wall-clock timestamp in the record's format, from the modem clock, or the
 * last fix's time, or the epoch placeholder when neither exists yet. */
static const char *clock_timestamp(char *buf, size_t len)
{
    struct timespec tp;

    if (clock_gettime(CLOCK_REALTIME, &tp) == 0 && tp.tv_sec > 1000000000) {
        struct tm tm;
        gmtime_r(&tp.tv_sec, &tm);
        snprintf(buf, len,
                 "%02d/%02d/%02d,%02d:%02d:%02d.%06ld+00",
                 tm.tm_mday, tm.tm_mon + 1, tm.tm_year % 100,
                 tm.tm_hour, tm.tm_min, tm.tm_sec,
                 tp.tv_nsec / 1000);
        return buf;
    }
    return g_gnss.time_iso[0] ? g_gnss.time_iso
                              : "01/01/00,00:00:00.000000+00";
}

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
        ts = clock_timestamp(now_iso, sizeof(now_iso));
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

    /* OBD-II data over the K wire.  Polled here so the values belong to the
     * same instant as the position they ride with.  A failed poll is not an
     * error: the vehicle may have no K interface, the engine may be off, or
     * the session may be reopening — the record simply carries no o* fields
     * and the server leaves those columns null. */
#if IS_ENABLED(CONFIG_APP_KLINE_TELEMETRY)
    if (ignitionState == 0) {           /* active-low: 0 = ignition on */
        struct obd_snapshot obd;

        if (obd_poll(&obd) == 0) {
            n = obd_append(&data_current[data_index],
                           DATA_LIMIT - data_index - 1, &obd);
            if (n > 0) data_index += n;
        }
    }
#endif

    /* Uptime */
    n = snprintf(&data_current[data_index],
                 DATA_LIMIT - data_index - 1,
                 ",up=%lld", k_uptime_get() / 1000);
    if (n > 0) data_index += n;

    append_sync_fields();
    data_current[data_index] = '\0';

    /* Battery warning - only meaningful with the engine off. While the engine
     * runs the vehicle's smart/regenerative charging system swings the rail
     * 11.8-14.9V by design (load-shed at idle/under acceleration, boosted on
     * overrun), so an instantaneous low reading mid-drive says nothing about
     * battery health. Gate on ignition-off (ignitionState != 0; the sense is
     * active-low) and skip implausible readings (sensor absent/broken). Normal
     * priority: a resting low battery is informational, not an emergency. */
    if (ignitionState != 0 && (s_ign_last == 0 || s_ign_last == -1)) {
        s_ign_off_ms = k_uptime_get();      /* just went off (or first seen off) */
    }
    s_ign_last = (ignitionState == 0) ? 0 : 1;

    if (ignitionState != 0 && v >= IMPLAUSIBLE_VOLTAGE) {
        if (v < BATTERY_WARNING_LEVEL) {
            /* Not within the settle time of the ignition going off, and not
             * if the line already reads on again: both are what a crank
             * looks like from here — see BATTERY_WARN_SETTLE_S. */
            bool settled = s_ign_off_ms >= 0 &&
                           k_uptime_get() - s_ign_off_ms >=
                               (int64_t)BATTERY_WARN_SETTLE_S * 1000;
            if (s_battery_warning_status == 0 && settled &&
                ignition_read() != 0) {
                char msg[24];
                snprintf(msg, sizeof(msg), "low battery: %.2fV", (double)v);
                alert_enqueue(msg, 0);
                s_battery_warning_status = 1;
            } else if (!settled) {
                LOG_INF("battery %.2fV within %ds of ignition off — not alerting",
                        (double)v, BATTERY_WARN_SETTLE_S);
            }
        } else {
            s_battery_warning_status = 0;
        }
    }

    return 1;
}

#if IS_ENABLED(CONFIG_APP_TRACK_MODE)
/* Track-mode record.  Same CSV head as collect_data() so the server needs
 * no second parser, but nothing here waits: the position is the last fix
 * before GNSS was stopped (or 0,0 on a unit that never had one — the page
 * hides the map in this mode), the speed is the ECU's, and the time is the
 * modem clock.  The extras carry tm=1 to mark the row, the fast OBD poll,
 * and acc=<burst>: the IMU samples batched since the previous record, one
 * ax/ay/az/gx/gy/gz group per sample separated by ':'.
 *
 * Skipped on purpose versus the normal record: the modem temperature and
 * VSYS reads (AT commands that cost tens of milliseconds and change on a
 * timescale of minutes) and the cell fields except on their normal dirty
 * flag.  Always returns 1: this mode's whole point is a record every cycle. */
int collect_track_data(void)
{
    float v = battery_sample_with_engine_check();
    char now_iso[40];
    int n;

    if (data_index > 0 && data_index < DATA_LIMIT - 1) {
        data_current[data_index++] = '\n';
    }

    const char *lat = g_gnss.lat_str[0] ? g_gnss.lat_str : "0.000000";
    const char *lon = g_gnss.lon_str[0] ? g_gnss.lon_str : "0.000000";
    float speed = 0.0f;
#if IS_ENABLED(CONFIG_APP_KLINE_OBD)
    int obd_kmh = obd_speed_kmh();
    if (obd_kmh >= 0) speed = (float)obd_kmh;
#endif

    n = snprintf(&data_current[data_index], DATA_LIMIT - data_index - 1,
        "%s,%s,%s,%.2f,%.2f,%.2f,%ld,%ld,%.2f,%d,%lld,%d",
        clock_timestamp(now_iso, sizeof(now_iso)), lat, lon,
        (double)speed, (double)g_gnss.altitude_m,
        (double)g_gnss.heading_deg,
        0L, 0L,
        (double)v,
        1,                                  /* ignition is on by definition */
        k_uptime_get() / 1000,
        powered_on ? 1 : 0);
    if (n > 0 && n < DATA_LIMIT - data_index - 1) data_index += n;

    /* Instantaneous IMU reading, as on every record, so the page's slow
     * panels keep working unchanged. */
    int ax, ay, az;
    if (accel_read(&ax, &ay, &az) == 0) {
        n = snprintf(&data_current[data_index], DATA_LIMIT - data_index - 1,
                     ",ax=%d;ay=%d;az=%d", ax, ay, az);
        if (n > 0) data_index += n;
    }
    int gx, gy, gz;
    if (accel_read_gyro(&gx, &gy, &gz) == 0) {
        n = snprintf(&data_current[data_index], DATA_LIMIT - data_index - 1,
                     ",gx=%d;gy=%d;gz=%d", gx, gy, gz);
        if (n > 0) data_index += n;
    }
    float imu_temp;
    if (accel_read_temp(&imu_temp) == 0) {
        n = snprintf(&data_current[data_index], DATA_LIMIT - data_index - 1,
                     ",it=%.1f", (double)imu_temp);
        if (n > 0) data_index += n;
    }

    if (g_cell.valid && g_cell.dirty) {
        n = snprintf(&data_current[data_index], DATA_LIMIT - data_index - 1,
                     ",mcc=%d;mnc=%d;lac=%u;cid=%u;cl=0;rat=%s",
                     g_cell.mcc, g_cell.mnc, g_cell.tac, g_cell.cid,
                     modem_rat());
        if (n > 0) data_index += n;
        g_cell.dirty = false;
    }

    n = snprintf(&data_current[data_index], DATA_LIMIT - data_index - 1,
                 ",tm=1");
    if (n > 0) data_index += n;

#if IS_ENABLED(CONFIG_APP_KLINE_TELEMETRY)
    struct obd_snapshot obd;

    if (obd_poll_track(&obd) == 0) {
        n = obd_append(&data_current[data_index],
                       DATA_LIMIT - data_index - 1, &obd);
        if (n > 0) data_index += n;
    }
#endif

    /* The IMU burst.  Bounded by what the datagram has room for as well as
     * by the sample cap: the envelope and a margin for the fields after
     * this one are reserved first, and the burst stops at the last sample
     * that fits rather than being cut mid-number. */
#if CONFIG_APP_TRACK_IMU_SAMPLES > 0
    struct accel_sample smp[CONFIG_APP_TRACK_IMU_SAMPLES];
    int cnt = accel_fifo_drain_samples(smp, CONFIG_APP_TRACK_IMU_SAMPLES);

    if (cnt > 0) {
        int limit = UDP_PACKET_SIZE - 64 - 96;      /* envelope + tail */
        if (limit > DATA_LIMIT - 1) limit = DATA_LIMIT - 1;

        int start = data_index;
        for (int i = 0; i < cnt; i++) {
            n = snprintf(&data_current[data_index], DATA_LIMIT - data_index - 1,
                         "%s%d/%d/%d/%d/%d/%d", i ? ":" : ",acc=",
                         smp[i].ax, smp[i].ay, smp[i].az,
                         smp[i].gx, smp[i].gy, smp[i].gz);
            if (n <= 0 || data_index + n >= limit) {
                data_current[data_index] = '\0';
                if (i == 0) data_index = start;
                break;
            }
            data_index += n;
        }
    }
#endif

    n = snprintf(&data_current[data_index], DATA_LIMIT - data_index - 1,
                 ",up=%lld", k_uptime_get() / 1000);
    if (n > 0) data_index += n;

    append_sync_fields();
    data_current[data_index] = '\0';
    last_record_stale = !g_gnss.valid;
    return 1;
}
#endif /* CONFIG_APP_TRACK_MODE */

/* Send one pre-formatted line as its own datagram — used for the "D,"
 * fault-code report.  Deliberately not routed through the telemetry buffer:
 * that buffer is discarded when a collection finds no fix, and a fault-code
 * report must not be lost that way.  The server splits datagrams on newlines
 * and dispatches each line by prefix, so a lone line is a valid packet. */
int data_send_line(const char *line)
{
    LOG_INF("send: %s", line);
    return transport_send((const uint8_t *)line, strlen(line));
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

    /* Captured warnings/errors ride along as "L," lines after the record,
     * within what the datagram has room for and a per-record cap so a busy
     * log never crowds out the position.  They are appended only to the
     * live send, never to a record bound for the backlog: the backlog's
     * slots are sized for a record alone, and the lines are kept here
     * until a datagram carrying them actually gets through. */
    int rec_end = data_index;
    int added = 0;
    int room = (UDP_PACKET_SIZE - 64) - data_index;   /* transport envelope */

    if (room > DATA_LIMIT - 1 - data_index) {
        room = DATA_LIMIT - 1 - data_index;
    }
    if (room > CONFIG_APP_DEBUG_LOG_PER_RECORD) {
        room = CONFIG_APP_DEBUG_LOG_PER_RECORD;
    }
    if (room > 0) {
        size_t pending = dbglog_pending();

        added = dbglog_take(&data_current[data_index], (size_t)room);
        if (added > 0) {
            data_index += added;
            data_current[data_index] = '\0';
            LOG_INF("log: sending %d of %u pending bytes", added,
                    (unsigned)pending);
        }
    }

    int err = transport_send((const uint8_t *)data_current,
                             (size_t)data_index);
    if (err) {
        last_send_ok = false;
        /* Put the record back the way it was: the log lines stay in their
         * buffer for the next attempt and must not be pushed as records. */
        data_index = rec_end;
        data_current[data_index] = '\0';
        dbglog_ack(false);
        /* Only count a failure that means something.  With no registration
         * the send was never going to succeed and the modem is already
         * dealing with it; counting those is what used to escalate a tunnel
         * into a modem teardown. */
        if (modem_is_registered()) {
            gsm_send_failures++;
        }
        /* Keep the record rather than discard it.  The caller resets the
         * send buffer either way, so without this the position is gone for
         * good and an outage costs a hole in the journey, not just late
         * telemetry. */
        databuf_push_lines(data_current, (size_t)data_index);
        return 0;
    }
    last_send_ok = true;
    gsm_send_failures = 0;
    modem_send_ok();
    powered_on = false;
    dbglog_ack(true);

    /* The link is working, so drain a little of whatever the last outage
     * left behind.  Bounded per cycle so a backlog never delays the live
     * position. */
    if (databuf_count() > 0) {
        databuf_flush(CONFIG_APP_DATABUF_FLUSH_PER_CYCLE);
    }

    /* Server response, if requested */
    if (read_udp_response) {
        char resp[256];
        int n = transport_recv_response(resp, sizeof(resp), 2000);
        if (n > 0) {
            LOG_INF("resp: %s", resp);
            /* Plaintext shape:  "1,int,ma[,cmd]"  */
            int interval = -1, ma = -1;
            char cmd[128] = "";
            int matched = sscanf(resp, "1,%d,%d,%127[^\n]",
                                 &interval, &ma, cmd);
            if (matched >= 2) {
                if (!send_int_to_server) {
                    if (interval >= 0) g_settings.loop_interval = interval;
                    if (ma >= 0)       g_settings.movement_alarm = (int8_t)ma;
                }
                if (matched == 3 && cmd[0]) {
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
