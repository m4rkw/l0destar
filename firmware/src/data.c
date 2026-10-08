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
/* Set once the "backup power" alert has gone out; cleared, with a "car power
 * restored" alert, by the first record built above the backup band again. */
static bool s_on_backup;
/* Uptime at which a record first carried the ignition off, for the warning's
 * settle time (with ignition_last_on_ms() — see ign_off_settled()).  -1
 * until a record has been built with it off. */
static int64_t s_ign_off_ms = -1;
static int8_t  s_ign_last = -1;
/* Cleared once a packet carrying fw= has actually left the device, so a
 * failed first send doesn't lose the version until the next reboot. */
static bool s_fw_pending = true;
/* Uptime at which the rail first read below ENGINE_RUNNING_VOLTAGE with the
 * vehicle stationary, or -1 when the hold is not running. */
static int64_t s_low_voltage_since = -1;

/* Device-side record id, stamped on every record as rid= and restarted at
 * each boot.  The server stores it alongside the record, which is
 * what lets a figure that can only be worked out after a record has gone —
 * the wake duration in wt= — name the record it belongs to instead of
 * relying on the order things arrive in.  That order is not dependable: a
 * record whose send failed sits in the backlog and is delivered behind the
 * live record that describes its wake, so "the row before this one" is
 * simply the wrong row.  A backlogged record keeps the id it was built with,
 * so it is still attributable whenever it turns up.
 *
 * Seeded at random rather than started from 1.  Ids are consecutive within a
 * boot, which is what makes a gap in them a lost record, but a counter that
 * restarted from 1 would hand a rebooted device the same ids it used before
 * — and a wake figure still waiting for a record that never arrived would
 * then attach itself to an unrelated record of the new boot.  A 16-bit seed
 * makes that a one-in-65536 coincidence instead of the ordinary case, and
 * costs a byte or two per record in width. */
static uint32_t s_rec_id;
static uint32_t s_last_rec_id;

uint32_t data_last_rec_id(void)
{
    return s_last_rec_id;
}

/* The last record built on a live fix with the key on: where the next one's
 * gap begins, both for the motion that fills it (mv=, motion.c) and for the
 * warning that explains a long one, whose figures are differences from the
 * totals noted here.  Only a live fix with the key on ends a gap or starts
 * the next; any other record breaks the run (see gap_end()). */
static struct {
    bool     valid;
    int64_t  epoch_ms;          /* the fix's moment, uptime */
    float    speed_kmh;         /* GNSS */
    uint32_t cid;
    uint32_t blocked_ms;        /* gnss_blocked_ms() */
    uint32_t starved;           /* gnss_starved_epochs() */
    uint32_t rrc_ms;            /* modem_rrc_connected_ms() */
    uint32_t reg_losses;        /* modem_reg_losses() */
} s_gap;
static int64_t s_gap_warned_ms;         /* when the warning last went out */
static int     s_gap_held;              /* warnings held back since */
/* Bytes of mv= fields in data_current: see data_motion_bytes(). */
static int     s_motion_bytes;

int data_motion_bytes(void)
{
    return s_motion_bytes;
}

void data_gap_reset(void)
{
    s_gap.valid = false;
}

static uint32_t next_rec_id(void)
{
    if (s_rec_id == 0) {
        uint16_t seed = 0;

        if (!crypto_random((uint8_t *)&seed, sizeof(seed))) {
            seed = (uint16_t)k_uptime_get();
        }
        /* Never 0: that is this function's "not seeded yet". */
        s_rec_id = seed ? seed : 1u;
    }
    return ++s_rec_id;
}

void data_reset(void)
{
    memset(data_current, 0, sizeof(data_current));
    data_index = 0;
    s_motion_bytes = 0;
}

/* Is anything driving the vehicle's electrics?  GNSS only: the fallback below
 * exists precisely because the ECU is not answering, so obd_speed_kmh() is no
 * use here.  A fix that is missing, thin on satellites or stale reads as "not
 * moving", which only ever makes the fallback more conservative. */
static bool gnss_says_moving(void)
{
    if (!g_gnss.valid || g_gnss.sats < SPEED_MIN_SATS) {
        return false;
    }
    if (k_uptime_get() - g_gnss.fix_uptime_ms >
        (int64_t)ENGINE_FIX_MAX_AGE_S * 1000) {
        return false;
    }
    return g_gnss.speed_kmh >= ENGINE_MOVING_KMH;
}

/* Is the engine running?
 *
 * RPM from the ECU is a direct measurement and settles it whenever a fresh
 * figure exists.  Fall back to the rail voltage only when the ECU is not
 * answering — no K wire on this build, no session, or a figure gone stale.
 *
 * That fallback cannot be a bare threshold.  This vehicle's ECU sheds the
 * alternator once the battery is topped up: logged drives sit at 12.2-12.5 V
 * for up to 140 s at a stretch, at any engine speed, and climb back to
 * 14.3 V on overrun.  Parked, the same battery rests at 12.0-13.1 V.  The
 * driving-with-charge-cut band therefore overlaps the engine-off band, and
 * is frequently below it, so no threshold separates them on voltage alone —
 * which is how a single sub-13 V reading used to drop the tracker to its
 * engine-off cadence mid-journey.
 *
 * The asymmetry that does hold: nothing but an alternator puts the rail
 * above ENGINE_RUNNING_VOLTAGE, so voltage may promote on its own.  Demotion
 * additionally needs the vehicle standing still for ENGINE_STOPPED_HOLD_S,
 * and any movement restarts that hold, so a charge-cut episode can never
 * accumulate towards a stop however long it lasts.  Within the hold the
 * previous verdict stands.
 *
 * Key-on-engine-off while rolling — a tow, a coast — holds the running
 * cadence instead.  That is the side to err on for a tracker: the vehicle is
 * moving and its position is worth having.  Stationary key-on-engine-off is
 * the one case voltage and GNSS genuinely cannot call, and the hold is what
 * bounds it.
 *
 * Latches: the hold timer advances here, so this is the single place the
 * voltage verdict is formed.  Callers take the result rather than testing
 * battery_v themselves. */
bool engine_is_running(void)
{
#if IS_ENABLED(CONFIG_APP_KLINE_OBD)
    int rpm = obd_rpm();

    if (rpm >= 0) {
        /* Reset the hold so that losing the ECU later falls back from a
         * clean slate rather than part-way through someone else's timer. */
        s_low_voltage_since = -1;
        return rpm > 0;
    }
#endif
    if (battery_v >= ENGINE_RUNNING_VOLTAGE) {
        s_low_voltage_since = -1;
        return true;
    }
    if (gnss_says_moving()) {
        s_low_voltage_since = -1;
        return true;
    }

    int64_t now = k_uptime_get();

    if (s_low_voltage_since < 0) {
        s_low_voltage_since = now;
    }
    if (now - s_low_voltage_since < (int64_t)ENGINE_STOPPED_HOLD_S * 1000) {
        return engine_running;
    }
    return false;
}

static float battery_sample_with_engine_check(void)
{
    battery_v = battery_read_voltage();
    engine_running = engine_is_running();
    return battery_v;
}

/* Off for BATTERY_WARN_SETTLE_S as far as anything has seen: since the first
 * record that carried it off, and since the line last read on anywhere.  The
 * records alone miss a key-on from sleep.  The wake reads the line on, the
 * crank drops it out a second later, and a record built then follows the
 * ignition-off record from before the sleep.  On 2026-10-03 that one was
 * eleven minutes old, and the crank's 9.82 V went out as "low battery". */
static bool ign_off_settled(void)
{
    int64_t since = MAX(s_ign_off_ms, ignition_last_on_ms());

    return s_ign_off_ms >= 0 &&
           k_uptime_get() - since >= (int64_t)BATTERY_WARN_SETTLE_S * 1000;
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

    /* This record's own id.  Every record carries one; the server keeps it
     * so a later record can refer back to this one. */
    s_last_rec_id = next_rec_id();
    n = snprintf(&data_current[data_index],
                 DATA_LIMIT - data_index - 1,
                 ",rid=%u", s_last_rec_id);
    if (n > 0) data_index += n;

    /* How long the previous engine-off wake took, in milliseconds, and the
     * record it belongs to.  A wake cannot report its own duration, so the
     * figure lags at least one record — see wake_pending.  Cleared only once
     * it is actually in the buffer. */
    if (wake_pending.rec_id != 0 && wake_pending.ms > 0) {
        /* A third part when the attach was measured: the send is uniformly
         * sub-second, so this is the part that explains a slow wake. */
        if (wake_pending.attach_ms >= 0) {
            n = snprintf(&data_current[data_index],
                         DATA_LIMIT - data_index - 1,
                         ",wt=%u:%lld:%d", wake_pending.rec_id,
                         wake_pending.ms, wake_pending.attach_ms);
        } else {
            n = snprintf(&data_current[data_index],
                         DATA_LIMIT - data_index - 1,
                         ",wt=%u:%lld", wake_pending.rec_id, wake_pending.ms);
        }
        if (n > 0 && n < DATA_LIMIT - data_index - 1) {
            data_index += n;
            /* The signal that wake read after its send, for the same
             * record: a wake from PSM built that record before the radio
             * was up, so it went out without one.  Dropped with the figure
             * if it does not fit, never sent on its own. */
            if (wake_pending.signal) {
                n = snprintf(&data_current[data_index],
                             DATA_LIMIT - data_index - 1,
                             ",ws=%u:%d:%d:%u", wake_pending.rec_id,
                             wake_pending.rsrp_dbm, wake_pending.snr_db,
                             wake_pending.band);
                if (n > 0 && n < DATA_LIMIT - data_index - 1) {
                    data_index += n;
                }
            }
            wake_pending.rec_id = 0;
            wake_pending.ms = 0;
            wake_pending.attach_ms = -1;
            wake_pending.signal = false;
        }
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

/* Fixes FIX_GAP_WARN_MS or more apart on the move: say where the time went.
 * Most such gaps are one of two things, and the figures tell them apart.
 * The radio: GNSS blocked and an RRC connection held for most of the gap,
 * often with a registration lost or a change of cell (a rejected
 * tracking-area update, 2026-10-03 22:37).  Or the loop: the fix was waited
 * for only briefly, and the rest of the gap went somewhere else.  Until now
 * neither left a trace, which is why a 31 s gap on 2026-10-03 at 20:28 has
 * no explanation. */
static void gap_warn(int64_t gap_ms, uint32_t blocked_ms, uint32_t starved,
                     uint32_t rrc_ms, bool lost)
{
    int64_t now = k_uptime_get();

    if (s_gap_warned_ms != 0 &&
        now - s_gap_warned_ms < FIX_GAP_WARN_SPACING_MS) {
        s_gap_held++;
        return;
    }
    s_gap_warned_ms = now;

    char cell[32];
    char held[32] = "";

    if (g_cell.cid != s_gap.cid) {
        snprintf(cell, sizeof(cell), "cell %u->%u", (unsigned)s_gap.cid,
                 (unsigned)g_cell.cid);
    } else {
        snprintf(cell, sizeof(cell), "cell %u", (unsigned)g_cell.cid);
    }
    if (s_gap_held > 0) {
        snprintf(held, sizeof(held), " (+%d held back)", s_gap_held);
        s_gap_held = 0;
    }

    LOG_WRN("%lld s between fixes on the move: waited %d s for this one, "
            "GNSS blocked %u s, RRC connected %u s, %u epochs starved, %s%s%s",
            (gap_ms + 500) / 1000, (int)((gnss_last_wait_ms() + 500) / 1000),
            (unsigned)((blocked_ms + 500) / 1000),
            (unsigned)((rrc_ms + 500) / 1000), (unsigned)starved, cell,
            lost ? ", registration lost" : "", held);
}

/* The record just built ends the gap since the last live fix with the key
 * on, if there was one, and starts the next.  A gap long enough gets the
 * warning above; one the dead reckoning can fill gets the motion through it
 * on the end of this record, within what the backlog's slots take
 * (APP_DATABUF_REC_MAX), since a record that goes there whole has to fit
 * one.  Anything other than a live fix with the key on breaks the run: a
 * record from the stored position, or the key off, says nothing the next
 * fix's gap could be measured from.
 *
 * `fix` is the record's own, as gnss_collect() returned it, not g_gnss: the
 * receiver's event handler rewrites that with every new fix, and by the end
 * of a record — after an OBD poll, say — it can be the next one. */
static void gap_end(int rec_start, int ignitionState, bool live,
                    const struct gnss_fix *fix)
{
    if (ignitionState != 0 || !live) {
        s_gap.valid = false;
        return;
    }

    int64_t to_ms = fix->epoch_ms;
    uint32_t blocked = gnss_blocked_ms();
    uint32_t starved = gnss_starved_epochs();
    uint32_t rrc = modem_rrc_connected_ms();
    uint32_t losses = modem_reg_losses();

    if (s_gap.valid && to_ms > s_gap.epoch_ms) {
        int64_t gap = to_ms - s_gap.epoch_ms;

        if (FIX_GAP_WARN_MS > 0 && gap >= FIX_GAP_WARN_MS &&
            (fix->speed_kmh >= FIX_GAP_MOVING_KMH ||
             s_gap.speed_kmh >= FIX_GAP_MOVING_KMH)) {
            gap_warn(gap, blocked - s_gap.blocked_ms, starved - s_gap.starved,
                     rrc - s_gap.rrc_ms, losses != s_gap.reg_losses);
        }

        if (IS_ENABLED(CONFIG_APP_DEAD_RECKONING) &&
            gap >= DR_MIN_GAP_MS && gap <= DR_MAX_GAP_MS) {
            /* The heading log up to the fix's moment and a little past. */
            dr_fifo_service();

            int room = CONFIG_APP_DATABUF_REC_MAX - 1 - (data_index - rec_start);

            if (room > DATA_LIMIT - 1 - data_index) {
                room = DATA_LIMIT - 1 - data_index;
            }

            int n = motion_field(&data_current[data_index], room,
                                 s_gap.epoch_ms, to_ms,
                                 fix->speed_kmh >= DR_MOVING_KMH ||
                                 s_gap.speed_kmh >= DR_MOVING_KMH);

            data_index += n;
            s_motion_bytes += n;
        }
    }

    s_gap.valid = true;
    s_gap.epoch_ms = to_ms;
    s_gap.speed_kmh = fix->speed_kmh;
    s_gap.cid = g_cell.cid;
    s_gap.blocked_ms = blocked;
    s_gap.starved = starved;
    s_gap.rrc_ms = rrc;
    s_gap.reg_losses = losses;
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
    /* This record's own fix, kept apart from g_gnss: see gap_end(). */
    struct gnss_fix fix = {0};

    if (use_cached_gps) {
        have_fix = g_gnss.valid;
    } else {
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

    const int rec_start = data_index;
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
    /* A speed is only true at the moment of its fix.  Records built from the
     * stored position — every timed check-in while parked — repeated the
     * last fix's figure for as long as the unit slept: on 2026-09-14 the
     * car's hourly records carried 0.68 mph from the fix after its 06:56
     * reboot, and the page showed 1 mph.  See SPEED_FIX_MAX_AGE_MS. */
    if (speed > 0.0f &&
        k_uptime_get() - g_gnss.fix_uptime_ms > SPEED_FIX_MAX_AGE_MS) {
        speed = 0.0f;
    }
    /* With the ignition off a residual GNSS speed is noise, not motion — see
     * IGN_OFF_STOPPED_KMH.  Every ignition-off record, not only the one where
     * the key turned, so a fresh fix taken while parked (after a reboot, say)
     * reads as stopped too; and only a GNSS speed below the threshold, so a
     * roll-away, a tow or a key cut while moving still reports what it
     * measured. */
    if (ignitionState != 0 && speed > 0.0f && speed < IGN_OFF_STOPPED_KMH) {
        if (s_ign_last == 0) {
            LOG_INF("ignition off at %.2f km/h — reporting stopped",
                    (double)speed);
        }
        speed = 0.0f;
    }
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

    /* Serving-cell signal quality, on its own cadence rather than the cell
     * group's.  One reading per wake says how strong the signal is where
     * the car is parked and nothing else; separating a weak location from a
     * weak antenna needs readings from many places, because coverage swings
     * tens of dB along a route while a detuned or badly-fed antenna is
     * close to the same offset everywhere.  See SIGNAL_SAMPLE_MS.
     *
     * Only while registered — there is nothing to measure otherwise — and
     * never in track mode, which builds two records a second and skips
     * every AT command for exactly that reason. */
    if (network_ready && SIGNAL_SAMPLE_MS > 0 &&
        k_uptime_get() - g_cell.signal_ms >= SIGNAL_SAMPLE_MS) {
        modem_read_signal();
    }

    /* Emitted only while the reading is this record's own.  A figure older
     * than SIGNAL_FRESH_MS belongs to another moment, and on a moving
     * vehicle possibly another cell, so it is dropped rather than reported
     * as if it had been taken now. */
    if (g_cell.signal_valid &&
        k_uptime_get() - g_cell.signal_ms <= SIGNAL_FRESH_MS) {
        n = snprintf(&data_current[data_index],
                     DATA_LIMIT - data_index - 1,
                     ",rsrp=%d;snr=%d;band=%d",
                     g_cell.rsrp_dbm, g_cell.snr_db, g_cell.band);
        if (n > 0) data_index += n;

        if (g_cell.conn_valid) {
            n = snprintf(&data_current[data_index],
                         DATA_LIMIT - data_index - 1,
                         ";pathloss=%d;rsrq=%d;ce=%d;txrep=%d",
                         g_cell.pathloss_db, g_cell.rsrq_x10,
                         (int)g_cell.ce_level, g_cell.tx_rep);
            if (n > 0) data_index += n;
        }
    }

    /* OBD-II data over the K wire: the live snapshot the fix-wait tick has
     * been refreshing, polled here only if it has gone stale, so the values
     * are within a second of the position they ride with and the record
     * normally costs no bus time.  No data is not an error: the vehicle may
     * have no K interface, the engine may be off, or the session may be
     * reopening — the record simply carries no o* fields and the server
     * leaves those columns null. */
#if IS_ENABLED(CONFIG_APP_KLINE_TELEMETRY)
    if (ignitionState == 0) {           /* active-low: 0 = ignition on */
        struct obd_snapshot obd;

        if (obd_snapshot_take(&obd) == 0) {
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
    gap_end(rec_start, ignitionState, !use_cached_gps && !stale, &fix);
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

    /* Backup module carrying the rail (CONFIG_APP_BACKUP_SUPPLY): the same
     * settle guard as the low-battery alert keeps a crank sag out of it, and
     * a reading back above the band means the car supply has returned.  A
     * cut supply on a parked car is a tamper signal, so it goes out at the
     * movement alarm's priority rather than the low battery's. */
    if (ignitionState != 0 && battery_on_backup(v)) {
        /* The settle wait exists for a crank sag, and the module's wake
         * pulse cannot be one: sleep is only entered with the ignition off. */
        bool settled = backup_woke || ign_off_settled();
        backup_woke = false;
        if (!s_on_backup && settled && ignition_read() != 0) {
            char msg[28];
            snprintf(msg, sizeof(msg), "backup power: %.2fV", (double)v);
            alert_enqueue(msg, 1);
            s_on_backup = true;
        }
    } else if (s_on_backup && v > BACKUP_SUPPLY_MAX) {
        char msg[32];
        snprintf(msg, sizeof(msg), "car power restored: %.2fV", (double)v);
        alert_enqueue(msg, 0);
        s_on_backup = false;
    }

    if (ignitionState != 0 && v >= IMPLAUSIBLE_VOLTAGE && !battery_on_backup(v)) {
        if (v < BATTERY_WARNING_LEVEL) {
            /* Not within the settle time of the ignition being on, and not
             * if the line already reads on again: both are what a crank
             * looks like from here — see BATTERY_WARN_SETTLE_S. */
            bool settled = ign_off_settled();
            if (s_battery_warning_status == 0 && settled &&
                ignition_read() != 0) {
                char msg[24];
                snprintf(msg, sizeof(msg), "low battery: %.2fV", (double)v);
                alert_enqueue(msg, 0);
                s_battery_warning_status = 1;
            } else if (!settled) {
                LOG_INF("battery %.2fV within %ds of the ignition being on — "
                        "not alerting", (double)v, BATTERY_WARN_SETTLE_S);
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

    /* Built from the last fix, not a new one: the run of live fixes, and
     * any gap it was measuring, ends here. */
    data_gap_reset();

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

    if (obd_poll_fast(&obd) == 0) {
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

/* -- server replies --------------------------------------------------------- */

/* A command out of a reply, kept for the state machine to run (cmd_run) at
 * its next safe point.  Every reply reaches here now, not only the ones a
 * send waited for, and several can arrive between two runs — so a command is
 * merged in rather than overwriting the one before: the server deletes a
 * command once it has put it in a reply, and this is the only copy.  The
 * fields that ride every reply (fota=, track=) are taken from the newest
 * reply, which goes first, since cmd_run acts on the first of each it finds;
 * the older copies are dropped. */
static void pending_cmd_add(const char *cmd)
{
    char merged[sizeof(pending_server_cmd)];
    size_t used = strlen(cmd);

    if (used >= sizeof(merged)) {
        used = sizeof(merged) - 1;
    }
    memcpy(merged, cmd, used);
    merged[used] = '\0';

    const char *p = pending_server_cmd;

    while (*p) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        bool routine = (len >= 5 && strncmp(p, "fota=", 5) == 0) ||
                       (len >= 6 && strncmp(p, "track=", 6) == 0);

        if (len > 0 && !routine) {
            if (used + 1 + len < sizeof(merged)) {
                merged[used++] = ',';
                memcpy(&merged[used], p, len);
                used += len;
                merged[used] = '\0';
            } else {
                LOG_WRN("server command dropped, no room: %.*s", (int)len, p);
            }
        }
        if (!comma) {
            break;
        }
        p = comma + 1;
    }

    memcpy(pending_server_cmd, merged, used + 1);
}

/* What a reply says, "1,<interval>,<movement_alarm>[,cmd]", applied. */
static void reply_apply(const char *resp)
{
    int interval = -1, ma = -1;
    char cmd[128] = "";
    int matched = sscanf(resp, "1,%d,%d,%127[^\n]", &interval, &ma, cmd);

    /* A decodable response is proof a datagram arrived, which is the only
     * evidence that fw= and rst= landed.  Clearing s_fw_pending on the send
     * instead threw the boot diagnostics away whenever the record was lost —
     * and a lost boot record is exactly the case where the reset cause is
     * worth having.  Left set, they ride the next record.  (The record that
     * carried them is held until answered for, and sent again if it is
     * not, so any reply will do.) */
    s_fw_pending = false;

    if (matched >= 2) {
        if (!send_int_to_server) {
            if (interval >= 0) g_settings.loop_interval = interval;
            if (ma >= 0)       g_settings.movement_alarm = (int8_t)ma;
        }
        if (matched == 3 && cmd[0]) {
            pending_cmd_add(cmd);
        }
    }
}

void data_reply(uint32_t id, const char *resp)
{
    databuf_ack(id);
    LOG_INF("resp #%u: %s", id, resp);
    reply_apply(resp);
}

/* -- send ----------------------------------------------------------------- */

/* The datagram being sent: the records, stamped with their ages, then the
 * log lines that ride along.  data_current itself is left as the records
 * alone, which is what the backlog takes if the send fails. */
static char s_tx[UDP_PACKET_SIZE];

/* When the previous record went out.  A reply from anything sent since then
 * is the evidence that the link is delivering, and the backlog waits for it:
 * pushed into a modem that has stopped delivering, it would only queue up
 * behind the stall and wait there with everything else. */
static int64_t s_prev_send_ms;

/* The records did not go.  Keep them rather than discard them: the caller
 * resets the send buffer either way, so without this the position is gone
 * for good and an outage costs a hole in the journey, not just late
 * telemetry.  Only a failure that means something is counted: with no
 * registration the send was never going to succeed and the modem is already
 * dealing with it — counting those is what used to escalate a tunnel into a
 * modem teardown. */
static int send_failed(bool counts)
{
    last_send_ok = false;
    if (counts && modem_is_registered()) {
        gsm_send_failures++;
    }
    databuf_push_lines(data_current, (size_t)data_index);
    return 0;
}

int send_data(void)
{
    /* Replies that came in since the last send are receipts; sends that
     * have waited too long for theirs go back in the backlog. */
    (void)transport_poll();
    databuf_expire();

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

    /* No registration: nothing handed to the modem now would reach the
     * server.  It would take the datagram all the same — in the middle of a
     * tracking-area update it still has its bearer — and then lose it with
     * the bearer, the way the datagrams it already held were lost at 10:57
     * and 11:50 on 2026-09-27.  So the records go straight to the backlog,
     * and the send counts as failed but not towards a recovery: the modem is
     * already dealing with it. */
    if (!modem_is_registered()) {
        /* And a reply from before the outage says nothing about the link
         * after it: the backlog waits for one to a send made since. */
        s_prev_send_ms = k_uptime_get();
        return send_failed(false);
    }

    /* The records, each marked with its age if it is not fresh — one held
     * through an outage says when it was built (see databuf_stamp) — then
     * the captured warnings/errors as "L," lines, within what the datagram
     * has room for and a per-record cap so a busy log never crowds out the
     * position.  The lines ride only the live send, never a record bound
     * for the backlog: the backlog's slots are sized for a record alone,
     * and the lines stay in their own buffer until a datagram carrying them
     * has gone. */
    int rec_len = databuf_stamp(data_current, (size_t)data_index,
                                s_tx, sizeof(s_tx) - 1);

    /* The motion through a gap rides outside the batch's byte budget (see
     * data_motion_bytes()), so a full batch can come out longer than the
     * transport takes.  Then the motion goes, never a record. */
    if (s_motion_bytes > 0 &&
        (rec_len < 0 || rec_len > UDP_PACKET_SIZE - 64)) {
        int was = data_index;

        data_index = motion_strip(data_current, data_index);
        data_current[data_index] = '\0';
        s_motion_bytes = 0;
        LOG_INF("motion left off a full batch (%d bytes)", was - data_index);
        rec_len = databuf_stamp(data_current, (size_t)data_index,
                                s_tx, sizeof(s_tx) - 1);
    }

    if (rec_len < 0) {
        if (data_index >= (int)sizeof(s_tx)) {
            /* More than a datagram holds: the backlog packs it smaller. */
            return send_failed(true);
        }
        /* No room for the ages: the records as they are. */
        memcpy(s_tx, data_current, (size_t)data_index);
        rec_len = data_index;
    }

    int tx_len = rec_len;
    int room = (UDP_PACKET_SIZE - 64) - tx_len;   /* transport envelope */

    if (room > (int)sizeof(s_tx) - 1 - tx_len) {
        room = (int)sizeof(s_tx) - 1 - tx_len;
    }
    if (room > CONFIG_APP_DEBUG_LOG_PER_RECORD) {
        room = CONFIG_APP_DEBUG_LOG_PER_RECORD;
    }
    if (room > 0) {
        size_t pending = dbglog_pending();
        int added = dbglog_take(&s_tx[tx_len], (size_t)room);

        if (added > 0) {
            tx_len += added;
            LOG_INF("log: sending %d of %u pending bytes", added,
                    (unsigned)pending);
        }
    }

    int err = transport_send((const uint8_t *)s_tx, (size_t)tx_len);
    if (err) {
        /* The log lines stay in their buffer for the next attempt. */
        dbglog_ack(false);
        return send_failed(true);
    }
    last_send_ok = true;
    gsm_send_failures = 0;
    modem_send_ok();
    powered_on = false;
    dbglog_ack(true);

    /* The modem has the datagram; the records are held until the server
     * answers for them, and go again if it does not.  Not in track mode:
     * its records carry an IMU burst too big for the backlog's slots, and a
     * second copy of a one-second sample, a minute late, is not worth the
     * airtime. */
    if (!transport_is_streaming()) {
        databuf_sent(transport_sent_id(), s_tx, (size_t)rec_len);
    }

    int64_t prev_send_ms = s_prev_send_ms;

    s_prev_send_ms = k_uptime_get();

    /* Server response, if requested */
    if (read_udp_response) {
        char resp[256];
        int n = transport_recv_response(resp, sizeof(resp),
                                        RESPONSE_TIMEOUT_MS);
        if (n > 0) {
            LOG_INF("resp: %s", resp);
            reply_apply(resp);
        }
    }

    /* Drain a little of whatever an outage, or an unanswered send, left
     * behind — once something sent since the last record has been answered,
     * so the link is known to be delivering.  Bounded per cycle so a
     * backlog never delays the live position. */
    if (databuf_count() > 0 && databuf_last_ack_ms() >= prev_send_ms) {
        databuf_flush(CONFIG_APP_DATABUF_FLUSH_PER_CYCLE);
    }

    send_int_to_server = false;
    alert_send();
    return 1;
}
