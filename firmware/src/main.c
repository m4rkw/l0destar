/*
 * Main entry + state machine.  Full port from firmware.ino including sleep
 * states, movement detection, coast-to-stop, and network error recovery.
 *
 * States:
 *   IDLE            — poll ignition/voltage, decide when to send
 *   GPS_COLLECT     — get fix, buffer a record
 *   SEND            — transmit telemetry, process response, transition
 *   IGNITION_SLEEP  — ignition ON but engine OFF; periodic sends, watch for
 *                     engine start or ignition off
 *   SLEEP           — ignition OFF; low-power loop with timer/accel/ign wake
 */

#include <string.h>
#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/dt-bindings/gpio/nordic-nrf-gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/pm/device.h>

#include "app.h"
#include "hw_common.h"
#include "hw_domain.h"
#include "pins.h"

LOG_MODULE_REGISTER(main, CONFIG_APP_LOG_LEVEL);

/* The PSM timers only exist in Kconfig while APP_PSM_SLEEP is on.  The code
 * that reads them is behind IS_ENABLED(), which drops the branch but still
 * compiles it, so they need a value either way. */
#ifndef CONFIG_APP_PSM_ACTIVE_S
#define CONFIG_APP_PSM_ACTIVE_S 0
#endif
#ifndef CONFIG_APP_PSM_SLEEP_GRACE_S
#define CONFIG_APP_PSM_SLEEP_GRACE_S 0
#endif

/* -- shared state used across modules (declared in app.h) ------------------ */
char     pending_server_cmd[128];
bool     read_udp_response;
bool     power_reboot;
int      gsm_send_failures;
bool     network_ready;
bool     use_cached_gps;
bool     powered_on = true;
char     ignition;
int8_t   previous_ignition = -1;
/* The last exit from sleep was the ignition line.  IDLE clears it once the
 * key is confirmed on, or reports once and sleeps if the key is already off
 * again (a short key cycle) — see the ignition-off branch there. */
static bool s_key_wake;
/* The sleep loop found the backup module taking over — by its ignition pulse
 * or by the rail poll, see do_sleep.  data.c reads and clears it. */
bool     backup_woke;
/* The sleep loop has reported the module carrying the rail, so the rail poll
 * does not report it again every pass.  Cleared by a poll that finds the rail
 * back above the band. */
static bool s_backup_seen;
bool     engine_running;
float    battery_v;
struct wake_report wake_pending;

/* -- state machine --------------------------------------------------------- */
enum main_state {
    STATE_IDLE,
    STATE_GPS_COLLECT,
    STATE_SEND,
    STATE_IGNITION_SLEEP,
    STATE_SLEEP,
};

static enum main_state s_state = STATE_IDLE;
static int64_t s_last_send_ms;
static int     s_buffered_records;
/* When a server reply was last read.  While moving, sends normally do not
 * wait for one; this is what bounds how long a setting changed on the
 * server (track mode, interval) takes to reach a driving device. */
static int64_t s_last_resp_ms;

/* -- ignition wake interrupt ----------------------------------------------- */
static K_SEM_DEFINE(s_wake_sem, 0, 1);
static struct gpio_callback s_ign_cb;
static bool s_ign_cb_installed;
/* Latched by ign_isr.  The pin level is not enough on its own: the backup
 * module's wake pulse (CONFIG_APP_BACKUP_SUPPLY) holds the line for only
 * ~0.4 s, so by the time the loop's debounce re-reads it the level is gone. */
static atomic_t s_ign_int_flag;
/* k_uptime_get_32() when ign_isr fired: where the pulse began, for timing
 * how long the line stays on. */
static atomic_t s_ign_int_ms;

/* The sleep loop owes a report the moment the modem registers (resend_owed
 * in do_sleep).  Registration used to be noticed only by the loop's own
 * poll, every RESEND_POLL_S: on 2026-09-27 the network was back at 11:50:54
 * and the ignition-off report went at 11:51:32.  Now the LTE handler ends
 * the wait as registration lands. */
static atomic_t s_report_owed;

static void report_owed_set(bool *owed, bool on)
{
    *owed = on;
    atomic_set(&s_report_owed, on ? 1 : 0);
}

/* From the LTE event handler's thread: only signal. */
static void on_registered(void)
{
    if (atomic_get(&s_report_owed)) {
        k_sem_give(&s_wake_sem);
    }
}

static void ign_isr(const struct device *dev, struct gpio_callback *cb,
                    uint32_t pins)
{
    /* Level trigger: nrfx re-arms the sense after every callback for as
     * long as the pin holds the level, so left armed this would fire
     * back-to-back and starve the sleep loop.  Disarm here; the loop
     * re-arms before each wait. */
    gpio_pin_interrupt_configure(dev, PIN_IGN_SENSE, GPIO_INT_DISABLE);
    atomic_set(&s_ign_int_ms, (atomic_val_t)k_uptime_get_32());
    atomic_set(&s_ign_int_flag, 1);
    k_sem_give(&s_wake_sem);
}

static void ign_irq_enable(void)
{
    if (!s_ign_cb_installed) {
        gpio_init_callback(&s_ign_cb, ign_isr, BIT(PIN_IGN_SENSE));
        gpio_add_callback(hw_gpio0, &s_ign_cb);
        s_ign_cb_installed = true;
    }
    /* Level, not edge.  An edge trigger takes a GPIOTE IN channel, which
     * keeps the pin-detect logic clocked for the whole sleep (~20-45 uA on
     * the SiP per Nordic); a level trigger goes through the PORT/sense
     * path, which costs nothing.  One direction is enough: sleep is only
     * ever entered with the ignition off, so the only transition that can
     * matter is the sense pin going low (ignition present).  The awake
     * side polls ignition and is untouched by this. */
    gpio_pin_interrupt_configure(hw_gpio0, PIN_IGN_SENSE, GPIO_INT_LEVEL_LOW);
}

static void ign_irq_disable(void)
{
    gpio_pin_interrupt_configure(hw_gpio0, PIN_IGN_SENSE, GPIO_INT_DISABLE);
}

/* -- console suspend across the sleep wait ---------------------------------- */
/* An enabled UARTE holds the HF clock even with no traffic (~600-900 uA at
 * 3.3 V — the bulk of the parked board's input draw).  The console is
 * suspended only for the blocking wait in do_sleep(), so everything logged
 * while awake still reaches the port.  A message logged mid-wait is dropped
 * harmlessly: with the device suspended the driver's tx path is a no-op.
 *
 * A quiet sleep pass (a tilt poll that finds nothing) runs with the console
 * still suspended — see the quiet test in do_sleep().  Both calls are no-ops
 * in the state they would set, so console_resume() can be dropped in front
 * of any branch that logs without tracking whether it already ran. */
#if DT_HAS_CHOSEN(zephyr_console)
static const struct device *const s_console_dev =
    DEVICE_DT_GET_OR_NULL(DT_CHOSEN(zephyr_console));
#else
static const struct device *const s_console_dev = NULL;
#endif
static bool s_console_on = true;

static void console_suspend(void)
{
    if (!s_console_on || s_console_dev == NULL ||
        !device_is_ready(s_console_dev)) {
        return;
    }

    /* Bounded drain of the deferred log queue so the tail isn't lost. */
    for (int i = 0; i < 40 && log_data_pending(); i++) {
        k_msleep(5);
    }
    k_msleep(2);

    int err = pm_device_action_run(s_console_dev, PM_DEVICE_ACTION_SUSPEND);
    if (err && err != -EALREADY) {
        static bool s_warned;
        if (!s_warned) {
            s_warned = true;
            LOG_WRN("console suspend failed (%d)", err);
        }
        return;
    }
    s_console_on = false;
}

static void console_resume(void)
{
    if (s_console_on || s_console_dev == NULL ||
        !device_is_ready(s_console_dev)) {
        return;
    }
    (void)pm_device_action_run(s_console_dev, PM_DEVICE_ACTION_RESUME);
    s_console_on = true;
}

/* -- accelerometer wake interrupt ------------------------------------------ */
static struct gpio_callback s_accel_cb;
static bool s_accel_cb_installed;
static atomic_t s_accel_int_flag;
/* True while the sleep-side level trigger is armed on INT1.  The ISR must
 * disarm a level trigger (see ign_isr) but must leave the awake-side edge
 * trigger alone, or the second impact of a drive would never be seen. */
static atomic_t s_accel_level_armed;

static void accel_isr(const struct device *dev, struct gpio_callback *cb,
                      uint32_t pins)
{
    /* Same as ign_isr: the sleep-side trigger is a level, so disarm before
     * nrfx can re-arm it.  The awake-side crash trigger is an edge and
     * stays armed. */
    if (atomic_get(&s_accel_level_armed)) {
        gpio_pin_interrupt_configure(dev, PIN_ACC_INT1, GPIO_INT_DISABLE);
    }
    atomic_set(&s_accel_int_flag, 1);
    k_sem_give(&s_wake_sem);
}

static void accel_cb_install(void)
{
    if (!s_accel_cb_installed) {
        gpio_pin_configure(hw_gpio0, PIN_ACC_INT1, GPIO_INPUT);
        gpio_init_callback(&s_accel_cb, accel_isr, BIT(PIN_ACC_INT1));
        gpio_add_callback(hw_gpio0, &s_accel_cb);
        s_accel_cb_installed = true;
    }
}

static void accel_irq_enable(void)
{
    if (!accel_available()) return;
    accel_cb_install();
    accel_enable_wake_int();
    /* Drop anything latched before this arming — a stale flag from the awake
     * path, or the FS/ODR step that fires a spurious wake ~26 ms into
     * accel_enable_wake_int() — so the first sleep iteration doesn't confirm
     * a phantom.  Cleared before the GPIO interrupt is armed, never after,
     * or a real edge arriving here would be swallowed. */
    atomic_clear(&s_accel_int_flag);
    /* Level trigger for the same reason as the ignition wake: no GPIOTE IN
     * channel held for the whole sleep.  INT1 is an active-high pulse that
     * idles low, so level-high is the same event as the rising edge. */
    atomic_set(&s_accel_level_armed, 1);
    gpio_pin_interrupt_configure(hw_gpio0, PIN_ACC_INT1, GPIO_INT_LEVEL_HIGH);
}

static void accel_irq_disable(void)
{
    if (!accel_available()) return;
    atomic_clear(&s_accel_level_armed);
    gpio_pin_interrupt_configure(hw_gpio0, PIN_ACC_INT1, GPIO_INT_DISABLE);
    accel_disable_wake_int();
}

/* -- crash (impact) interrupt while awake ----------------------------------- */
static void crash_irq_enable(void)
{
    if (!accel_available()) return;
    accel_cb_install();
    atomic_clear(&s_accel_level_armed);
    atomic_clear(&s_accel_int_flag);
    accel_fifo_enable();
    accel_crash_int_enable(CRASH_THRESHOLD_MG);
    gpio_pin_interrupt_configure(hw_gpio0, PIN_ACC_INT1,
                                 GPIO_INT_EDGE_TO_ACTIVE);
}

static void crash_irq_disable(void)
{
    if (!accel_available()) return;
    gpio_pin_interrupt_configure(hw_gpio0, PIN_ACC_INT1, GPIO_INT_DISABLE);
    accel_crash_int_disable();
    accel_fifo_disable();
}

/* -- accelerometer alert priority backoff ----------------------------------
 *
 * Impact, tilt/tow and movement all raise the same high-priority alert, and
 * one physical event routinely trips several of them: opening the glovebox a
 * tracker lives in produces a movement alert, then a tilt alert, then another
 * movement alert.  Waking someone once for that is useful.  Waking them four
 * times teaches them to ignore the alerts entirely, which is worse than not
 * sending any.
 *
 * So the first alert goes out at full priority and opens a window.  Anything
 * inside the window is reported at APP_ACCEL_ALERT_BACKOFF_PRIORITY instead:
 * still sent, still logged, still in the history — just not urgent.
 *
 * The window is fixed rather than sliding.  Once it expires the next event is
 * urgent again, so sustained interference keeps producing high-priority
 * alerts at one per window, rather than being silenced indefinitely by its
 * own persistence.
 */
static int64_t s_accel_alert_window_end;

static int accel_alert_priority(void)
{
    if (ACCEL_ALERT_BACKOFF_S <= 0) {
        return ACCEL_ALERT_PRIORITY;          /* backoff disabled */
    }

    int64_t now = k_uptime_get();

    if (s_accel_alert_window_end && now < s_accel_alert_window_end) {
        LOG_INF("accel alert downgraded to priority %d (%lld s of backoff left)",
                ACCEL_ALERT_BACKOFF_PRIORITY,
                (s_accel_alert_window_end - now) / 1000);
        return ACCEL_ALERT_BACKOFF_PRIORITY;
    }

    s_accel_alert_window_end = now + (int64_t)ACCEL_ALERT_BACKOFF_S * 1000;
    return ACCEL_ALERT_PRIORITY;
}

/* Called from the awake loops.  The FIFO ring holds ~7 s of accel+gyro,
 * and nothing else drains it while the interrupt flag is up (see
 * dr_fifo_service()), so the impact profile is intact even with loop-cadence
 * latency. */
static void crash_check(void)
{
    if (!atomic_clear(&s_accel_int_flag)) return;

    uint8_t src = 0;
    accel_read_wake_src(&src);

    struct accel_impact imp;
    char msg[120];
    if (accel_fifo_drain_impact(&imp) == 0 && imp.samples > 0) {
        snprintf(msg, sizeof(msg),
                 "impact %d.%02dg x=%d y=%d z=%d gyr=%d.%ddps dur=%dms spd=%.1f",
                 imp.peak_mg / 1000, (imp.peak_mg % 1000) / 10,
                 imp.pax, imp.pay, imp.paz,
                 imp.peak_gyro_dps10 / 10, imp.peak_gyro_dps10 % 10,
                 imp.over_ms, (double)g_gnss.speed_kmh);
    } else {
        int ax, ay, az;
        accel_read(&ax, &ay, &az);
        snprintf(msg, sizeof(msg),
                 "impact: >%d.%dg (src=0x%02x) now=%d/%d/%dmg spd=%.1f",
                 CRASH_THRESHOLD_MG / 1000, (CRASH_THRESHOLD_MG % 1000) / 100,
                 src, ax, ay, az, (double)g_gnss.speed_kmh);
    }
    LOG_WRN("%s", msg);
    alert_enqueue(msg, accel_alert_priority());
    alert_send();
}

#if IS_ENABLED(CONFIG_APP_DEAD_RECKONING)
/* The FIFO into the dead reckoning.  Not with an impact waiting for
 * crash_check(): its profile is in the FIFO, and the FIFO gives each sample
 * once.  The impact drain feeds the dead reckoning as well, so waiting for
 * it loses nothing.  Not with the key off either: no drive to fill in. */
void dr_fifo_service(void)
{
    if (ignition == 0 && !atomic_get(&s_accel_int_flag)) {
        (void)accel_fifo_service();
    }
}
#endif

#if IS_ENABLED(CONFIG_APP_KLINE_TELEMETRY) || IS_ENABLED(CONFIG_APP_DEAD_RECKONING)
/* gnss_collect()'s tick: about once a second while it waits for a fix. */
static void fix_wait_tick(void)
{
#if IS_ENABLED(CONFIG_APP_KLINE_TELEMETRY)
    obd_sample_tick();
#endif
    dr_fifo_service();
}
#endif

/* -- coast-to-stop --------------------------------------------------------- */
static bool s_coasting;
static int  s_coast_iters;

/* -- movement alert state -------------------------------------------------- */
static const int s_move_cooldowns[] = {300, 900, 1800, 3600};
static int  s_move_alert_level;
static int  s_move_cooldown_secs;
static int  s_move_idle_secs;
static bool s_move_needs_gps;
static int  s_saved_loop_interval = -1;
/* Ignition state baked into the record currently buffered for sending.
 * previous_ignition means "what the server has been told", so it is latched
 * from this rather than from a fresh read — see STATE_SEND. */
static char s_record_ignition;
/* An ignition change was recorded while the modem had no registration.  The
 * record is in the send buffer; when the network returns it goes straight
 * out rather than being collected again with a later time and position. */
static bool s_transition_buffered;
static bool s_wait_logged;
/* When registration was first missed while awake, 0 while registered.  The
 * idle poll waits for the modem to sort itself out, but it cannot wait
 * forever: a modem that has been reinitialised after a fault comes back at
 * CFUN=0 and nothing else awake would ever bring it up again. */
static int64_t s_unregistered_ms;

/* The radio has been up without a registration for longer than a parked unit
 * is allowed to wait — see APP_NETWORK_SEARCH_TIMEOUT.  0 disables the cap,
 * and a modem that is registered or powered off is never "expired". */
static bool network_search_expired(void)
{
    return NETWORK_SEARCH_TIMEOUT > 0 &&
           modem_unregistered_s() >= NETWORK_SEARCH_TIMEOUT;
}

/* A parked search on LTE-M alone has run its course: try again with NB-IoT
 * allowed (APP_NBIOT_PARKED_FALLBACK), on a fresh search window.  True if
 * the radio is searching again, so the caller still owes its report; false
 * if the fallback is off, already in use, or could not be switched on. */
static bool nbiot_fallback_start(void)
{
    if (!IS_ENABLED(CONFIG_APP_NBIOT_PARKED_FALLBACK) ||
        modem_nbiot_fallback_on()) {
        return false;
    }
    LOG_WRN("no LTE-M registration %d s after bringing the radio up — "
            "searching with NB-IoT as well", modem_unregistered_s());
    return modem_nbiot_fallback(true) == 0;
}

/* Key-on: back to LTE-M only before the drive's connect, so a drive never
 * runs on NB-IoT.  Costs 3-5 s if the modem is up, nothing if it is off. */
static void nbiot_fallback_end(void)
{
    if (modem_nbiot_fallback_on()) {
        (void)modem_nbiot_fallback(false);
    }
}

/* Bring the radio up again every retry interval while there is no
 * registration.  The only thing that brings it back after a modem fault:
 * the reset thread reinitialises the library but leaves the modem offline
 * with none of the app's settings.  A modem that is already up and
 * searching is left alone (modem_radio_up checks), since reapplying +COPS=0
 * to it restarts the PLMN search it is in the middle of.  Not a blocking
 * connect — the caller's loop is already the wait, and it keeps servicing
 * the ignition line and the K-wire keep-alive while the network is away.
 * Called from STATE_IDLE's poll, and from STATE_SEND for a drive recording
 * through the outage, which never passes that poll. */
static void network_retry_tick(void)
{
    int64_t now = k_uptime_get();

    if (s_unregistered_ms == 0) {
        s_unregistered_ms = now;
    } else if (NETWORK_RETRY_INTERVAL > 0 &&
               now - s_unregistered_ms >=
                   (int64_t)NETWORK_RETRY_INTERVAL * 1000) {
        LOG_WRN("no registration for %ds — bringing the "
                "radio up again", NETWORK_RETRY_INTERVAL);
        modem_radio_up();
        s_unregistered_ms = now;
    }
}

/* Consecutive timed wakes that found no network.  Each one doubles the wait
 * until the next, up to NO_SIGNAL_MAX_INTERVAL — see the note there for why a
 * unit with no coverage should not keep waking on its usual cadence. */
static int s_nosignal_wakes;

/* Back to the configured cadence.  Called wherever the unit learns there is
 * signal again, and on movement: the likeliest reason coverage returns is
 * that the vehicle has been moved into it. */
static void network_backoff_reset(void)
{
    if (s_nosignal_wakes > 0) {
        LOG_INF("network back after %d wake%s without it — timed reports "
                "every %ds again", s_nosignal_wakes,
                s_nosignal_wakes == 1 ? "" : "s", g_settings.loop_interval);
        s_nosignal_wakes = 0;
    }
}

/* The engine-off wake interval with that backoff applied.  g_settings is left
 * alone on purpose: the backoff is local, and by the time a record reaches
 * the server there is signal and the count is already cleared, so the int=
 * the device reports back stays the interval it was given. */
static int telemetry_interval(void)
{
    int iv = g_settings.loop_interval;

    if (iv <= 0 || NO_SIGNAL_MAX_INTERVAL <= 0) {
        return iv;
    }
    for (int n = s_nosignal_wakes; n > 0 && iv < NO_SIGNAL_MAX_INTERVAL; n--) {
        iv *= 2;
    }
    return MIN(iv, NO_SIGNAL_MAX_INTERVAL);
}

/* Whether a pass of the sleep loop may power the modem off at its end.
 * Something must have brought it up (or it is registered), and no timed
 * report may be owed on a search that is still inside its window: that
 * report goes out the moment the modem registers, and the alert paths that
 * raise the radio for their own send must not take it down under it.
 *
 * A modem the modem itself says is asleep in PSM, with nothing on this pass
 * having raised it, has nothing to release.  Registered counts as "brought
 * up" above, and in PSM it is registered for good, so every tilt poll used
 * to come through here: woke the console, and had modem_sleep() log that it
 * was leaving the modem registered — ~9 ms of UART and HF clock every 30 s,
 * as much as the pass's own work.  A modem that is registered but awake
 * (just used, back on its own, or not entering PSM) still gets released. */
static bool sleep_modem_release(bool raised, bool owed)
{
    if (!raised && !network_ready) {
        return false;
    }
    if (!raised && modem_psm_asleep()) {
        return false;
    }
    if (owed && (modem_is_registered() || !network_search_expired())) {
        return false;
    }
    return true;
}

void movement_reset(void)
{
    s_move_alert_level = 0;
    s_move_cooldown_secs = 0;
    s_move_idle_secs = 0;
    s_move_needs_gps = false;
    if (s_saved_loop_interval >= 0) {
        g_settings.loop_interval = s_saved_loop_interval;
        s_saved_loop_interval = -1;
    }
}

/* -- helpers --------------------------------------------------------------- */
static void handle_ignition_state(void)
{
    ignition = (char)ignition_read();
}

static bool should_send_data(void)
{
    int64_t elapsed = k_uptime_get() - s_last_send_ms;

    if (previous_ignition == -1)                 return true;
    if (ignition == 0 && previous_ignition != 0) return true;
    if (ignition != 0 && previous_ignition == 0) return true;
    if (ignition == 0 && engine_running &&
        elapsed >= 1000)                         return true;
    if (ignition == 0 && !engine_running &&
        elapsed >= IGNITION_ON_SLEEP_INTERVAL * 1000)
                                                 return true;
    if (send_int_to_server)                      return true;
    if (!last_send_ok && ignition == 0)          return true;
    if (g_settings.loop_interval > 0 &&
        elapsed >= (int64_t)g_settings.loop_interval * 1000)
                                                 return true;
    return false;
}

/* ========================================================================= */
/*  STATE_SLEEP — low-power loop with timer / accel / ignition wake          */
/* ========================================================================= */
/* Vehicle speed for the tracker's own movement decisions.
 *
 * The ECU's figure comes from the wheel speed sensors and reads exactly zero
 * at a standstill.  GNSS speed is Doppler-derived and does not: across 106
 * stationary records it averaged 0.66 mph and peaked at 5.11 mph, which is
 * enough to look like creeping motion.  So prefer the ECU when it is
 * answering and fall back to GNSS when it is not.
 *
 * The ECU figure is not better in every respect — vehicle speed sensors
 * typically over-read by a couple of percent and are affected by tyre size —
 * but for "are we moving or not" the zero is what matters.
 *
 * Only a recent fix's speed counts.  g_gnss keeps the last fix for as long
 * as no other comes, and on 2026-10-10 at 17:28 the car drove into an
 * underground car park at 5.3 km/h by GNSS.  Switched off six minutes later,
 * the ECU unpowered and so silent, that speed started coast-to-stop: a
 * collection that never got a fix, so never a send to end the coast, and
 * the unit stayed awake with GNSS running, its ignition-off record held,
 * until the key came back on.  SPEED_FIX_MAX_AGE_MS is the same limit the
 * records' own speed field keeps to. */
static float vehicle_speed_kmh(void)
{
#if IS_ENABLED(CONFIG_APP_KLINE_OBD)
    int obd = obd_speed_kmh();

    if (obd >= 0) {
        return (float)obd;
    }
#endif
    if (!g_gnss.valid ||
        k_uptime_get() - g_gnss.fix_uptime_ms > SPEED_FIX_MAX_AGE_MS) {
        return 0.0f;
    }
    return g_gnss.speed_kmh;
}

/* obd_rpm() for the log lines, without needing the guard at every call site:
 * -1 on a build with no K wire, which prints as "-1 rpm" and reads correctly
 * as "no ECU figure". */
static int engine_rpm_or_na(void)
{
#if IS_ENABLED(CONFIG_APP_KLINE_OBD)
    return obd_rpm();
#else
    return -1;
#endif
}

#if IS_ENABLED(CONFIG_APP_KLINE_DTC_REPORT)
/* Ignition as the fault-code reader last saw it, so the ignition-on read
 * fires exactly once per key turn.  -1 until the first observation. */
static int8_t s_dtc_last_ign = -1;

/* Read the ECU's stored fault codes and send them to the server, which diffs
 * the set against what it holds and alerts on codes appearing and clearing.
 * Sent as its own "D," line rather than folded into a telemetry record,
 * because the server treats it as the device's complete current set and must
 * not infer one from a record that happens to lack the field.
 *
 * A failed read sends nothing at all: an empty report means "no codes
 * stored", so reporting one after a timeout would clear faults that are
 * still there. */
static void kline_report_dtcs(const char *when)
{
    char line[128];
    int len = obd_dtc_report(line, sizeof(line));

    if (len < 0) {
        LOG_WRN("DTC read at %s failed (%d) — reporting nothing", when, len);
        return;
    }
    LOG_INF("DTC read at %s: %s", when, line);
    data_send_line(line);
}
#endif

#if IS_ENABLED(CONFIG_APP_KLINE_OBD)
/* Everything the K-wire runtime needs done regularly, independent of which
 * state the tracker is in.  Called from every loop that can run for a while:
 * the main one and the ignition-sleep one.
 *
 * It must not live inside a single state.  With BATCH_SIZE 1 a drive cycles
 * STATE_GPS_COLLECT and STATE_SEND and never reaches STATE_IDLE, so anything
 * hung off idle would not run at all between key-on and key-off — which is
 * the entire window the fault-code watch exists to cover. */
static void obd_service(void)
{
    obd_keepalive();
#if IS_ENABLED(CONFIG_APP_KLINE_DTC_REPORT)
    if (obd_dtc_pending()) {
        kline_report_dtcs("code count changed");
    }
#endif
}
#endif

/* A last known position exists once there has been a fix since boot; without
 * one collect_data() cannot build a record. */
static bool have_position(void)
{
    return g_gnss.lat_str[0] != '\0' && g_gnss.lon_str[0] != '\0';
}

/* Timed wakes that go looking for a first fix: the 1st, 2nd, 4th, 8th and
 * 16th, then every 16th.  At the 3600 s boot interval that is 1, 2, 4, 8 and
 * 16 hours, then every 16 hours. */
static unsigned s_nofix_wakes;

static bool nofix_search_due(void)
{
    unsigned n = ++s_nofix_wakes;

    if (n <= 16) {
        return (n & (n - 1)) == 0;
    }
    return (n % 16) == 0;
}

/* After a wake on the ignition line, watch it to tell a key from a pulse.
 * True if it is still on window_ms after `since` (the interrupt, or the poll
 * that noticed the change).  False once it has read off for
 * IGN_OFF_CONFIRM_MS, with *on_ms how long after `since` it was last seen on,
 * or -1 if it was already off at the first look: a pulse latched while the
 * loop was busy, over before anything looked at it. */
static bool ign_line_held(uint32_t since, int window_ms, int *on_ms)
{
    int last_on = -1;

    for (;;) {
        int age = (int)(k_uptime_get_32() - since);

        if (ignition_read() == 0) {
            last_on = age;
            if (age >= window_ms) {
                *on_ms = age;
                return true;
            }
        } else if (last_on < 0 || age - last_on >= IGN_OFF_CONFIRM_MS) {
            *on_ms = last_on;
            return false;
        }
        k_msleep(5);
    }
}

/* For a sleep pass past its ignition check: has the key come on since?  The
 * line reading on, or the edge the interrupt latched, which also covers a
 * key whose line is out for the crank at the moment of looking.  A driver
 * who gets in, starts up and pulls away does all of that inside one pass's
 * tilt debounce or movement confirm, and the pass then finds the car being
 * driven.  The caller drops its alert and ends the pass; the next one, which
 * checks the ignition first, takes the key. */
static bool ign_on_since_check(void)
{
    return ignition_read() == 0 || atomic_get(&s_ign_int_flag) != 0;
}

/* After a key-on, the line reading off can be the starter turning rather
 * than the key going off — see IGN_CRANK_MAX_MS.  Either inside the wake's
 * own window, with the key turned straight through, or by the time IDLE
 * first looks.  True once it is back on, false if it stays off throughout. */
static bool ign_back_from_crank(void)
{
    int64_t start = k_uptime_get();

    while (k_uptime_get() - start < IGN_CRANK_MAX_MS) {
        if (ignition_read() == 0) {
            return true;
        }
        k_msleep(20);
    }
    return false;
}

/* After a pulse on the ignition line with the backup module fitted: was it
 * the module taking over the rail?  Watched rather than read once, because
 * the pulse comes before the rail has fallen to the module's level (see
 * BACKUP_SETTLE_MS).  A cut is the rail falling into the band and staying
 * there with the line off.  The watch ends early when the line comes back on
 * (a key, or a crank finishing) or the rail holds steady above the band (the
 * car is still connected).  *v comes in as the reading just taken and goes
 * out as the last one.  Never true without CONFIG_APP_BACKUP_SUPPLY, since
 * battery_on_backup() is not. */
static bool backup_cut_confirm(float *v)
{
    int64_t start = k_uptime_get();
    int64_t ref_ms = start;
    int64_t in_band_ms = -1;
    float   ref_v = *v;

    for (;;) {
        int64_t now = k_uptime_get();
        int     t = (int)(now - start);

        if (*v < 0) {
            LOG_WRN("wake: no rail reading - cannot tell whether that pulse "
                    "was the backup module");
            return false;
        }
        if (ignition_read() == 0) {
            LOG_INF("wake: ignition back on %d ms into the rail watch "
                    "(%.2fV) — not a cut", t, (double)*v);
            return false;
        }
        if (battery_on_backup(*v)) {
            if (in_band_ms < 0) {
                in_band_ms = now;
            }
            if (now - in_band_ms >= BACKUP_CONFIRM_MS) {
                LOG_WRN("wake: rail in the backup band at %.2fV, %d ms after "
                        "the pulse - backup module, reporting the cut",
                        (double)*v, (int)(in_band_ms - start));
                return true;
            }
        } else {
            in_band_ms = -1;
            if (now - ref_ms >= 1000) {
                if (*v > BACKUP_SUPPLY_MAX && ref_v - *v < BACKUP_FALL_MIN_V) {
                    LOG_INF("wake: rail holding at %.2fV (%.2fV a second "
                            "earlier) — car supply present, not a cut",
                            (double)*v, (double)ref_v);
                    return false;
                }
                ref_v = *v;
                ref_ms = now;
            }
        }
        if (t >= BACKUP_SETTLE_MS) {
            LOG_INF("wake: rail still %.2fV %d s after the pulse — not a cut",
                    (double)*v, BACKUP_SETTLE_MS / 1000);
            return false;
        }
        watchdog_kick();
        k_msleep(BACKUP_POLL_MS);
        *v = battery_read_voltage();
    }
}

/* The 6D orientation tamper check in the sleep loop, and the flag it
 * keeps across wakes.  TODO: re-enable once the unit is permanently
 * mounted.  One switch for both, so they cannot drift apart. */
#define SLEEP_D6D_TAMPER 0

static void do_sleep(void)
{
    LOG_INF("entering sleep");
    data_gap_reset();
    /* Track mode ends with the drive.  The server drops its switch on the
     * ignition-off record, so this only keeps the two in step: without it
     * the next key-on would start in the mode and stay there until the
     * first reply (up to APP_TRACK_RESP_INTERVAL_S) said otherwise. */
    if (g_settings.track_mode) {
        LOG_INF("track mode off: ignition off");
        g_settings.track_mode = 0;
    }
    /* No fault-code read here: sleep is only ever entered with the ignition
     * off, and the engine ECU is unpowered then, so the read would time out
     * and an empty report would wrongly clear live faults.  Codes raised
     * during a drive are caught while it is still running, by the stored-code
     * count in mode 01 PID 01 (see obd_dtc_pending). */
    crash_irq_disable();
    led_sleep_enter();
    LOG_INF("sleep: GNSS stop");
    gnss_stop();
    /* Replies to the last sends may still be on their way — the ignition-off
     * record's among them — and once the socket is closed they have nowhere
     * to land.  So collect them first, then send what the backlog holds
     * while the radio is still up rather than an hour from now.  Whatever
     * goes unanswered stays in the backlog for the next report. */
    LOG_INF("sleep: delivering what is still owed");
    databuf_deliver(RESPONSE_TIMEOUT_MS);
    LOG_INF("sleep: transport close");
    transport_close();
    transport_teardown();   /* the connection does not survive the modem going off */

    /* A timed report that could not go out because the modem had not
     * registered.  modem_connect() leaves the radio searching when it gives
     * up, so registration often arrives a minute or two later — and the next
     * pass used to power the modem straight off, leaving the record in the
     * backlog until the following timed wake an hour on.  Owed until the
     * modem registers, the search window closes (APP_NETWORK_SEARCH_TIMEOUT)
     * or the next timed report runs. */
    bool    resend_owed;
    int64_t resend_owed_ms = 0;

    report_owed_set(&resend_owed, false);

    /* A modem that is up, unregistered and still inside its search window
     * is left to it, with the report owed, rather than powered off: the
     * boot that could not register with the key off comes in this way, and
     * the poll below gives it the rest of the window and sends the first
     * record — and runs the power-on update check — if it registers in
     * time.  A search that has already run its course is ended here. */
    if (modem_unregistered_s() >= 0 && !network_search_expired()) {
        report_owed_set(&resend_owed, true);
        resend_owed_ms = k_uptime_get();
        LOG_INF("sleep: modem left searching (%d s so far, report owed)",
                modem_unregistered_s());
    } else if (modem_unregistered_s() >= 0 && nbiot_fallback_start()) {
        /* Parked after a search that had already run out on LTE-M: the
         * car went in somewhere it does not reach, an underground car park
         * typically, and the ignition-off record is in the backlog. */
        report_owed_set(&resend_owed, true);
        resend_owed_ms = k_uptime_get();
    } else {
        LOG_INF("sleep: modem down");
        modem_sleep();
    }
    LOG_INF("sleep: CAN power off");
    hw_can_power_off();
    LOG_INF("sleep: K-line power off");
#if IS_ENABLED(CONFIG_APP_KLINE_OBD)
    obd_close();        /* StopCommunication before the rails go */
#endif
    proge_mode_off();
    LOG_INF("sleep: aux power off");
    hw_aux_power_off();
    LOG_INF("sleep: INA228 shutdown");
    hw_power_shutdown();
    led_all_off();
    LOG_INF("sleep: all peripherals off");

    accel_read_baseline();
    accel_snapshot_tilt_ref();
    accel_irq_enable();

    bool tow_alerted = false;
#if SLEEP_D6D_TAMPER
    bool tamper_alerted = false;
#endif
    int  tow_last_tilt = -1;      /* tenths, for the "still" test */
    int  tow_stable_secs = 0;

    if (s_saved_loop_interval < 0) {
        s_saved_loop_interval = g_settings.loop_interval;
    }

    int telemetry_remaining = telemetry_interval();

    /* Uptime at which the timed report now in progress woke the unit, 0 when
     * none is.  The wake is not over when the send is: one that could not
     * register keeps the modem up and the loop polling for it every
     * RESEND_POLL_S, and that is where the power goes — so this spans those
     * passes too and closes only when nothing is owed any more. */
    int64_t wake_at = 0;
    /* The rid= of the record that wake produced, so the figure can name it
     * rather than leaving the server to guess from arrival order.  Taken
     * from the last record the report actually built: a resend builds a
     * fresh one, and it is that record the wake ends up delivering. */
    uint32_t wake_rec_id = 0;
    /* How much of the wake went on the attach.  -1 until one is measured;
     * a wake that found the modem already registered paid none. */
    int32_t  wake_attach_ms = -1;
    /* The serving cell as that wake left it, when its record went out
     * without a reading of its own — see wake_report.signal. */
    bool     wake_signal = false;
    int16_t  wake_rsrp_dbm = 0;
    int16_t  wake_snr_db = 0;
    uint8_t  wake_band = 0;

    /* A report owed on the module taking over the rail (the pulse or the
     * rail poll found it), or on the car supply coming back.  Kept across
     * passes until the report runs: a cut that also jolts the car can land
     * in a pass the movement check ends early. */
    bool backup_wake = false;
    bool restore_wake = false;

    ign_irq_enable();
    atomic_clear(&s_ign_int_flag);
    int ign_before = ignition_read();
    g_cell.dirty = true;

    for (;;) {
        watchdog_kick();

        /* pick shortest wake interval (accel uses interrupt, not polling) */
        int sleep_secs = 3600;
        if (telemetry_remaining > 0 && telemetry_remaining < sleep_secs)
            sleep_secs = telemetry_remaining;
        if (s_move_cooldown_secs > 0 && s_move_cooldown_secs < sleep_secs)
            sleep_secs = s_move_cooldown_secs;
        if (TOW_TILT_DEG > 0 && sleep_secs > TOW_POLL_S)
            sleep_secs = TOW_POLL_S;   /* slow-tilt poll cadence */
        if (IS_ENABLED(CONFIG_APP_BACKUP_SUPPLY) &&
            sleep_secs > BACKUP_RAIL_POLL_S)
            sleep_secs = BACKUP_RAIL_POLL_S;   /* backup rail poll */
        if (resend_owed && sleep_secs > RESEND_POLL_S)
            sleep_secs = RESEND_POLL_S;   /* notice the registration */
        if (sleep_secs < 1) sleep_secs = 1;

        k_sem_reset(&s_wake_sem);
        /* Re-arm the ignition wake after the reset, never before it: the
         * ISR disarms itself, and a level already present when the sense is
         * re-enabled fires immediately, which the reset would swallow. */
        ign_irq_enable();
        /* A pulse that landed while this pass was busy (a report, the tilt
         * debounce) is latched but its semaphore was just reset: give it
         * back so the wait returns at once instead of at the next timer. */
        if (atomic_get(&s_ign_int_flag)) {
            k_sem_give(&s_wake_sem);
        }
        /* Likewise a report owed on a registration that has already
         * arrived: the LTE handler signals it only as it lands, and a pass
         * that ends early — an accelerometer wake that came to nothing —
         * never reaches the check below. */
        if (resend_owed && modem_is_registered()) {
            k_sem_give(&s_wake_sem);
        }
        int64_t t0 = k_uptime_get();
        console_suspend();
        /* Waited out in slices so the watchdog keeps being fed: a sleep
         * runs up to an hour and the window is 32 s.  A slice that expires
         * does not take the semaphore, so a wake still ends the wait at the
         * first slice boundary after it arrives — and the wake sources are
         * interrupts, which give the semaphore immediately either way. */
        bool woke = false;
        for (int left = sleep_secs; left > 0; left -= WATCHDOG_SLEEP_SLICE_S) {
            int slice = MIN(left, WATCHDOG_SLEEP_SLICE_S);

            if (k_sem_take(&s_wake_sem, K_SECONDS(slice)) == 0) {
                woke = true;
                break;
            }
            watchdog_kick();
        }
        int64_t woke_ms = k_uptime_get();
        int elapsed = (int)((woke_ms - t0) / 1000);
        if (elapsed < 1) elapsed = 1;

        /* A quiet pass — the timer ran out, no wake interrupt is latched and
         * no report is due — is the tow poll every TOW_POLL_S: two accel
         * reads, an ignition read and, with the backup module, one rail
         * conversion, none of which log.  Leave the console
         * suspended for it rather than hold the HF clock for nothing.  Every
         * branch a quiet pass can still fall into (tilt over threshold, a
         * polled ignition change, INT1 still high, the tow re-arm, the modem
         * off, the wake-cost line, key-on) resumes it first.  A LOG added to the quiet path without a
         * console_resume() in front of it is silently dropped. */
        bool quiet = !woke &&
                     !atomic_get(&s_ign_int_flag) &&
                     !atomic_get(&s_accel_int_flag) &&
                     !resend_owed && !backup_wake && !restore_wake &&
                     !(g_settings.loop_interval > 0 &&
                       telemetry_remaining - elapsed <= 0);
        if (!quiet) {
            console_resume();
        }

        /* Set by every path below that brings the radio up.  network_ready is
         * not enough on its own: modem_connect() sets it only on success, but
         * it has already taken the modem out of offline mode by the time it
         * gives up waiting, so a failed connect leaves the radio powered and
         * searching with the flag still false. */
        bool modem_raised = false;

        /* update timers */
        if (telemetry_remaining > 0) telemetry_remaining -= elapsed;
        if (s_move_cooldown_secs > 0) s_move_cooldown_secs -= elapsed;
        s_move_idle_secs += elapsed;
        if (s_move_idle_secs >= MOVEMENT_INACTIVITY_RESET)
            s_move_alert_level = 0;

        /* --- ignition check --- */
        /* A wake on the ignition line is a key-on if the line stays on.
         * The inline backup module lifts it to ~12 V for a fraction of a
         * second as its boost takes over the rail, so a power cut wakes the
         * unit instead of waiting for the timer.  What tells that pulse from
         * a key is the rail afterwards: it falls into the backup band and
         * stays there, where a car with its key turned never sits.  Not the
         * rail at the pulse, which is still at the car's level then (see
         * BACKUP_SETTLE_MS).  Every pulse that is not a key is logged, so one
         * that is ignored still shows on the console.
         *
         * First in the pass, ahead of the tilt and movement checks: with the
         * key on, what they measure is the car being started and driven off.
         * On 2026-10-03 the tilt check ran first, read the car pulling away
         * at 21 km/h as 24.5 deg off its parked attitude and raised a
         * tow/jack alert, and only then did this check find the key.  The
         * same order sent "movement: 17.6deg tilt" as the car set off at
         * 23:37 on 2026-10-01.  A key that comes on later in the pass is
         * caught by ign_on_since_check() before either of them alerts. */
        bool key_wake = false;
        bool ign_int = atomic_clear(&s_ign_int_flag) != 0;
        int ign_now = ignition_read();
        if (ign_int || ign_now != ign_before) {
            uint32_t since = ign_int ? (uint32_t)atomic_get(&s_ign_int_ms)
                                     : k_uptime_get_32();
            int window = IS_ENABLED(CONFIG_APP_BACKUP_SUPPLY)
                             ? BACKUP_PULSE_MAX_MS : IGN_WAKE_DEBOUNCE_MS;
            int on_ms;

            console_resume();
            if (ign_line_held(since, window, &on_ms)) {
                key_wake = true;
            } else {
                hw_power_wake();
                float v = battery_read_voltage();

                if (on_ms >= 0) {
                    LOG_INF("wake: ignition pulse, on for %d ms, rail %.2fV",
                            on_ms, (double)v);
                } else {
                    LOG_INF("wake: ignition pulse, over within %d ms, "
                            "rail %.2fV",
                            (int)(k_uptime_get_32() - since), (double)v);
                }
                if (IS_ENABLED(CONFIG_APP_BACKUP_SUPPLY) && !s_backup_seen &&
                    backup_cut_confirm(&v)) {
                    backup_wake = true;
                    backup_woke = true;
                    s_backup_seen = true;
                    telemetry_remaining = 0;
                } else if (on_ms >= IGN_WAKE_DEBOUNCE_MS) {
                    /* Long enough for a key: a short key cycle, which IDLE
                     * reports once — the same as before the module. */
                    LOG_INF("wake: long enough for a key — reporting it");
                    key_wake = true;
                } else if (ign_back_from_crank()) {
                    /* A key turned straight through to the starter: the
                     * line dropped out for the crank inside the window. */
                    LOG_INF("wake: line back on — a key, cranking at once");
                    key_wake = true;
                } else {
                    LOG_INF("wake: too short for a key — ignored");
                }
                battery_v = v;
                hw_power_shutdown();    /* the report wakes it again */
            }
            ign_before = ignition_read();
        }
        if (key_wake) {
            LOG_INF("wake: ignition ON");
            atomic_set(&s_report_owed, 0);
            ignition = 0;
            s_key_wake = true;
            movement_reset();
            ign_irq_disable();
            accel_irq_disable();
            LOG_INF("wake: INA228 wake");
            hw_power_wake();
            LOG_INF("wake: aux power on");
            hw_aux_power_on();
            led_on();
            nbiot_fallback_end();
            LOG_INF("wake: modem connect");
            modem_connect();
            LOG_INF("wake: GNSS start");
            gnss_start();
            s_state = STATE_IDLE;
            return;
        }

        /* --- slow-tilt check (tow / jack) --- */
        if (TOW_TILT_DEG > 0 && accel_available()) {
            int tilt = accel_tilt_from_ref_tenths();

            /* How long the attitude has held still.  The re-arm below leans
             * on this: re-snapping the reference while the angle is still
             * creeping would swallow the rest of a slow lift. */
            if (tilt >= 0) {
                int drift = (tow_last_tilt < 0) ? -1
                          : (tilt > tow_last_tilt ? tilt - tow_last_tilt
                                                  : tow_last_tilt - tilt);
                if (drift >= 0 && drift <= TOW_STABLE_TENTHS) {
                    tow_stable_secs += elapsed;
                } else {
                    tow_stable_secs = 0;
                }
                tow_last_tilt = tilt;
            }

            if (tilt >= TOW_TILT_DEG * 10) {
                console_resume();
                k_msleep(2000);                       /* debounce */
                tilt = accel_tilt_from_ref_tenths();
                if (ign_on_since_check()) {
                    LOG_INF("tilt %d.%ddeg with the key on — not a tow",
                            tilt / 10, tilt % 10);
                    continue;           /* the next pass takes the key */
                }
                if (tilt >= TOW_TILT_DEG * 10 && !tow_alerted) {
                    tow_alerted = true;
                    tow_stable_secs = 0;
                    LOG_WRN("sustained tilt %d.%ddeg - possible tow/jack",
                            tilt / 10, tilt % 10);
                    led_accel_movement();
                    char msg[56];
                    snprintf(msg, sizeof(msg),
                             "tilt %d.%ddeg - possible tow/jack",
                             tilt / 10, tilt % 10);
                    alert_enqueue(msg, accel_alert_priority());
                    watchdog_kick();
                    int reg = modem_get_network_status();
                    if (reg != 1 && reg != 5) {
                        modem_connect();
                        modem_raised = true;
                    }
                    alert_send_standalone();
                }
            } else if (tilt >= 0 && tilt < TOW_TILT_DEG * 5) {
                tow_alerted = false;   /* re-arm once back near level */
            }

            /* Latched at an attitude it is not coming back from: once that
             * attitude has held still long enough, adopt it as the new
             * normal so a *further* tilt can alert again.  Without the
             * re-snap, clearing the latch would just re-alert on the lift
             * that is already reported. */
            if (tow_alerted && TOW_REARM_S > 0 &&
                tow_stable_secs >= TOW_REARM_S) {
                console_resume();
                accel_snapshot_tilt_ref();
                tow_alerted = false;
                tow_stable_secs = 0;
                tow_last_tilt = -1;
                LOG_INF("tow tilt re-armed at the new resting attitude");
            }
        }

        /* --- 6D orientation tamper (unit flipped / off its mount) --- */
#if SLEEP_D6D_TAMPER
        /* Checked on every wake, not gated on the INT pin: the wake pulse
         * de-asserts before the loop runs, but the zone bits persist. */
        if (accel_available()) {
            uint8_t d6d = 0;
            int changed = accel_d6d_tamper(&d6d);
            if (changed && !tamper_alerted) {
                console_resume();
                tamper_alerted = true;
                LOG_WRN("tamper: orientation changed (D6D_SRC=0x%02x)", d6d);
                alert_enqueue("tamper: orientation changed",
                              accel_alert_priority());
                watchdog_kick();
                int reg = modem_get_network_status();
                if (reg != 1 && reg != 5) {
                    modem_connect();
                    modem_raised = true;
                }
                alert_send_standalone();
            } else if (!changed) {
                tamper_alerted = false;   /* re-arm once back to armed face */
            }
        }
#endif

        /* --- backup rail poll --- */
        /* The module's wake pulse only reaches the sense if nothing else on
         * the ignition wire holds it down, which a car's own ignition loads
         * can, and on the first bench tests it never arrived.  So the rail
         * is read on every pass as well: one INA conversion, silent unless
         * it finds something.  Settled in the band (confirmed a moment
         * later) is the module carrying the tracker, reported once; back
         * above it is the car supply returning, reported once, which also
         * re-arms the poll for the next cut. */
        if (IS_ENABLED(CONFIG_APP_BACKUP_SUPPLY) && !backup_wake &&
            !restore_wake) {
            float v = battery_poll_voltage();

            if (!s_backup_seen && battery_on_backup(v)) {
                k_msleep(BACKUP_CONFIRM_MS);
                v = battery_poll_voltage();
                if (battery_on_backup(v)) {
                    console_resume();
                    LOG_WRN("sleep: rail %.2fV - backup module carrying the "
                            "tracker, reporting the cut", (double)v);
                    backup_wake = true;
                    backup_woke = true;
                    s_backup_seen = true;
                    telemetry_remaining = 0;
                }
            } else if (s_backup_seen && v > BACKUP_SUPPLY_MAX) {
                console_resume();
                LOG_WRN("sleep: rail %.2fV - car supply back, reporting it",
                        (double)v);
                s_backup_seen = false;
                restore_wake = true;
            }
        }

        /* --- movement check (interrupt woke us, confirm sustained) --- */
        /* Take the edge the ISR latched, not the pin level.  The wake-up
         * interrupt is not latched in hardware (LIR clear in TAP_CFG0), so
         * INT1 de-asserts a sample or two after the acceleration falls back
         * under threshold — ~19-40 ms at 52 Hz.  The tilt and ignition
         * checks above outlast that (the tilt debounce alone sleeps 2 s), so
         * a level read here misses every short event: the loop wakes, sees a
         * pin that has already dropped, and silently sleeps again.  That is
         * why an impact was never reported and movement only registered
         * while it was still being moved. */
        bool accel_int = atomic_clear(&s_accel_int_flag) != 0;
        if (accel_available() &&
            (accel_int || gpio_pin_get(hw_gpio0, PIN_ACC_INT1) == 1)) {
            console_resume();
            LOG_INF("accel wake");
            gpio_pin_interrupt_configure(hw_gpio0, PIN_ACC_INT1,
                                         GPIO_INT_DISABLE);

            /* Drain the ring before confirming, not after.  The chip saw the
             * whole transient; the 100 ms confirm polls only ever see the
             * residual.  Draining afterwards also means the window includes
             * up to MOVEMENT_CONFIRM_MS of whatever happened during the
             * confirm, so a hit followed by handling reports the larger of
             * the two rather than the hit.  (The ring itself is safe either
             * way in sleep: accel-only at 26 Hz batching is ~438 samples,
             * about 16 s, so a 10 s confirm does not wrap it.) */
            struct accel_impact imp;
            bool have_imp = (accel_fifo_drain_impact(&imp) == 0 &&
                             imp.samples > 0);
            int peak = have_imp ? imp.peak_delta_mg : 0;
            bool impact_sent = false;

            /* An unambiguous impact is reported straight away rather than
             * waiting out the confirm.  Qualified by duration as well as
             * peak: +/-2 g in sleep clips the deviation at ~1000 mg, so
             * amplitude alone cannot tell a hit from a firm grab, but a hit
             * is a spike where movement is sustained. */
            if (have_imp && IMPACT_IMMEDIATE_MG > 0 &&
                peak >= IMPACT_IMMEDIATE_MG &&
                imp.over_ms <= IMPACT_IMMEDIATE_MAX_MS) {
                LOG_WRN("impact: %d mg over %d ms — reporting now",
                        peak, imp.over_ms);
                led_accel_impact();
                char msg[64];
                snprintf(msg, sizeof(msg), "parked impact %d.%02dg (%dms)",
                         peak / 1000, (peak % 1000) / 10, imp.over_ms);
                alert_enqueue(msg, accel_alert_priority());
                watchdog_kick();
                int reg = modem_get_network_status();
                if (reg != 1 && reg != 5) {
                    modem_connect();
                    modem_raised = true;
                }
                alert_send_standalone();
                impact_sent = true;
                /* Modem deliberately left registered: the confirm below may
                 * have a movement alert to send on the same session. */
            }

            /* The confirm stops as the key comes on.  Then the movement was
             * the driver getting in and setting off, and so was any peak the
             * fallback below would report: no alert, and the next pass takes
             * the key.  The accel is re-armed as after a bump, in case the
             * line was the backup module's pulse rather than a key. */
            int moved = accel_confirm_movement(ign_on_since_check);
            if (ign_on_since_check()) {
                LOG_INF("key on during the movement confirm — not an alarm");
                accel_read_baseline();
                accel_irq_enable();
                continue;
            }
            if (!moved) {
                /* Only fall back to the polled peak if the drain came up
                 * empty — it is the weaker measurement. */
                if (!have_imp) peak = accel_confirm_peak_mg();
                if (!impact_sent) {
                    if (peak >= PARKED_IMPACT_MG) {
                        LOG_WRN("parked impact: %d mg", peak);
                        led_accel_impact();
                        char msg[48];
                        snprintf(msg, sizeof(msg), "parked impact %d.%02dg",
                                 peak / 1000, (peak % 1000) / 10);
                        alert_enqueue(msg, accel_alert_priority());
                        watchdog_kick();
                        int reg = modem_get_network_status();
                        if (reg != 1 && reg != 5) {
                            modem_connect();
                            modem_raised = true;
                        }
                        alert_send_standalone();
                    } else {
                        LOG_INF("transient bump — ignoring (peak %d mg)",
                                peak);
                    }
                }
                /* This branch continues past the bottom-of-loop power-off,
                 * so drop the modem here if anything above raised it —
                 * whether or not the connect actually registered — unless
                 * a timed report is still owed on it, which the next pass
                 * sends first. */
                if (sleep_modem_release(modem_raised, resend_owed)) {
                    transport_teardown();
                    modem_sleep();
                }
                accel_read_baseline();
                accel_irq_enable();
                continue;
            }
            /* Read before accel_irq_disable(), at the ±2 g the confirm ran
             * at.  It sets ±8 g, and a read straight after it comes before the
             * IMU has a valid sample at the new setting: every movement alert
             * from the car said ~3000 mg (3010 on 2026-10-01, 3225-3484
             * before), and on the bench 5 such reads in 8 gave 11-14 g from a
             * unit at rest, against 1-18 mg read at ±2 g first. */
            int mv_tilt, mv_delta;
            accel_get_movement_info(&mv_tilt, &mv_delta);
            accel_irq_disable();
            LOG_INF("movement confirmed");
            /* The unit may have been carried into coverage, so the next
             * timed report goes back to the full cadence rather than
             * whatever a run of dead wakes had stretched it to. */
            network_backoff_reset();
            led_accel_movement();
            s_move_idle_secs = 0;
            s_move_needs_gps = true;

            if (s_move_cooldown_secs <= 0) {
                char msg[80];
                snprintf(msg, sizeof(msg),
                         "movement: %d.%ddeg tilt, %dmg",
                         mv_tilt / 10, mv_tilt % 10, mv_delta);
                alert_enqueue(msg, accel_alert_priority());

                watchdog_kick();
                int reg = modem_get_network_status();
                if (reg != 1 && reg != 5) {
                    modem_connect();
                    modem_raised = true;
                }
                alert_send_standalone();

                s_move_cooldown_secs =
                    s_move_cooldowns[s_move_alert_level];
                if (s_move_alert_level < 3) s_move_alert_level++;

                if (g_settings.loop_interval == 0 ||
                    g_settings.loop_interval >
                        MOVEMENT_TEMPORARY_ENGINE_OFF_INTERVAL) {
                    if (s_saved_loop_interval < 0)
                        s_saved_loop_interval = g_settings.loop_interval;
                    g_settings.loop_interval =
                        MOVEMENT_TEMPORARY_ENGINE_OFF_INTERVAL;
                    telemetry_remaining =
                        MOVEMENT_TEMPORARY_ENGINE_OFF_INTERVAL;
                }
            } else if (alert_count > 0) {
                watchdog_kick();
                int reg = modem_get_network_status();
                if (reg != 1 && reg != 5) {
                    modem_connect();
                    modem_raised = true;
                }
                alert_send();
            }

            accel_read_baseline();
        }

        /* --- owed timed report --- */
        if (resend_owed && modem_is_registered()) {
            report_owed_set(&resend_owed, false);
            if (g_settings.loop_interval > 0) {
                LOG_WRN("registered — sending the timed report owed for "
                        "%lld s",
                        (k_uptime_get() - resend_owed_ms) / 1000);
#if IS_ENABLED(CONFIG_APP_DEBUG_NBIOT_TEST)
                if (modem_nbiot_fallback_on() && !g_debug_key_on_ms) {
                    g_debug_key_on_ms = k_uptime_get() + 90000;
                    LOG_WRN("NB-IoT test: key-on in 90 s");
                }
#endif
                telemetry_remaining = 0;
            } else if (alert_count > 0) {
                /* Timed reports are off, so no report would carry them. */
                LOG_WRN("registered — sending the %d alert%s owed",
                        alert_count, alert_count == 1 ? "" : "s");
                alert_send_standalone();
            }
        } else if (resend_owed && network_search_expired() &&
                   nbiot_fallback_start()) {
            /* Still owed: the same report, over NB-IoT if it gets through
             * before this second window runs out. */
        } else if (resend_owed && network_search_expired()) {
            /* The modem has had its APP_NETWORK_SEARCH_TIMEOUT and is still
             * not registered.  Whatever record there was is in the backlog
             * for the next report that gets through; nothing else keeps
             * the radio up, so it goes off until the next timed wake.  With
             * the NB-IoT fallback this is the second window running out,
             * and the fallback stays on for the timed wakes to come. */
            report_owed_set(&resend_owed, false);
            LOG_WRN("no registration %d s after bringing the radio up — "
                    "modem off until the next timed report",
                    modem_unregistered_s());
            transport_teardown();
            modem_power_off();
        }

        /* --- timer telemetry (and the report a backup or restore owes) --- */
        if ((telemetry_remaining <= 0 && g_settings.loop_interval > 0) ||
            backup_wake || restore_wake) {
            /* The restore report goes out whatever the rail reads: it is one
             * record, and the only way the server hears the car supply is
             * back before the next timed wake. */
            bool forced = restore_wake;

            backup_wake = false;
            restore_wake = false;
            report_owed_set(&resend_owed, false);
            LOG_INF("sleep: INA228 wake for voltage read");
            hw_power_wake();
            float v = battery_read_voltage();
            battery_v = v;

            /* On the backup module the rail reads ~9 V, under both gates
             * below.  Those exist to spare a weak car battery; the pack is
             * there to be spent, and a unit that went quiet the moment its
             * power was cut would defeat it.  data.c raises the alert. */
            bool on_backup = battery_on_backup(v);
            if (on_backup) {
                LOG_INF("battery %.2fV: backup power, reporting", (double)v);
            }
            if (!on_backup && !forced && v > 0 && v < BATTERY_POWEROFF_LEVEL) {
                LOG_WRN("battery %.2fV < poweroff", (double)v);
                hw_power_shutdown();
                telemetry_remaining = BATTERY_CHECK_INTERVAL;
                continue;
            }
            if (!on_backup && !forced && v > 0 && v < SLEEP_SAFETY_VOLTAGE) {
                LOG_WRN("battery %.2fV, skipping send", (double)v);
                hw_power_shutdown();
                telemetry_remaining = telemetry_interval();
                continue;
            }

            /* Past the battery gates, so a wake that skipped the send
             * never starts a measurement.  Only the first pass of a report
             * sets it — a resend keeps the original wake's start. */
            if (wake_at == 0) {
                wake_at = woke_ms;
            }

            watchdog_kick();
            int reg = modem_get_network_status();
            if (reg != 1 && reg != 5) {
                modem_connect();
                /* Whatever the attach cost, or -1 if it never registered.
                 * Read straight after, before anything else can start a
                 * fresh search under it. */
                wake_attach_ms = modem_attach_ms();
            } else {
                /* Already registered — the search was paid for on an
                 * earlier pass of this same wake, so leave what it
                 * measured rather than calling this one free. */
                if (wake_attach_ms < 0) {
                    wake_attach_ms = 0;
                }
            }
            modem_update_cell_info();

            /* No position since boot means no record can be built, so a unit
             * restarted somewhere GNSS cannot reach would stay silent until
             * something moved it.  Search on timed wakes too, backing off so
             * one parked underground does not spend its battery on it.
             *
             * Only with a network to send the fix over.  A wake that could
             * not register used to run the receiver anyway — up to the cold
             * timeout, five minutes — for a record that could only go to
             * the backlog.  The search is left to the next wake that has a
             * network: the backoff counter only advances on those, so the
             * first of them searches, and a movement fix stays wanted. */
            bool registered = modem_is_registered();
            bool search_gps = registered &&
                              (s_move_needs_gps ||
                               (!have_position() && nofix_search_due()));

            if (!registered) {
                LOG_INF("no registration — %s",
                        have_position()
                            ? "recording from the stored position for the backlog"
                            : "no position and no network to search for one");
            }

            if (search_gps) {
                /* the GPS antenna bias tee lives on the AUX domain */
                hw_domain_request(HW_DOMAIN_AUX, HW_DOMAIN_USER_GNSS);
                gnss_start();
                use_cached_gps = false;
            } else {
                use_cached_gps = true;
            }

            ignition = (char)ignition_read();
            read_udp_response = true;
            if (collect_data(ignition) > 0) {
                wake_rec_id = data_last_rec_id();
                send_data();
                /* A wake that found the modem in PSM built this record before
                 * the radio was up, and a sleeping modem has no measurement to
                 * give, so the record went out without a signal reading.  The
                 * send has woken the radio: read it now, while the connection
                 * that carried the reply is still up, for the wake figure to
                 * carry to the server as ws=, filed against this record. */
                if (last_send_ok && !g_cell.signal_valid &&
                    modem_read_signal() == 0) {
                    wake_signal = true;
                    wake_rsrp_dbm = g_cell.rsrp_dbm;
                    wake_snr_db = g_cell.snr_db;
                    wake_band = g_cell.band;
                }
                if (pending_server_cmd[0] != '\0') {
                    cmd_run(pending_server_cmd);
                    pending_server_cmd[0] = '\0';
                    if (alert_count > 0) alert_send();
                }
                if (!last_send_ok) {
                    modem_recover();
                }
            }
            /* Owed whether or not a record could be built.  A unit with no
             * stored position builds none, and used to leave the modem
             * searching with nothing polling for the registration that
             * would let it search for a fix: this is what makes the loop
             * look, every RESEND_POLL_S, for as long as the window allows. */
            if (!modem_is_registered()) {
                report_owed_set(&resend_owed, true);
                resend_owed_ms = k_uptime_get();
                /* Stop counting at the ceiling; the interval is capped
                 * there anyway and the count has nowhere useful to go. */
                if (NO_SIGNAL_MAX_INTERVAL > 0 &&
                    telemetry_interval() < NO_SIGNAL_MAX_INTERVAL) {
                    s_nosignal_wakes++;
                }
            } else {
                network_backoff_reset();
            }
            data_reset();

            if (search_gps) {
                gnss_stop();
                hw_domain_release(HW_DOMAIN_AUX, HW_DOMAIN_USER_GNSS);
                s_move_needs_gps = false;
            }
            use_cached_gps = false;
            /* What the wake still has to deliver goes while the radio is up
             * for it — the replies to its sends, and any backlog — and what
             * goes unanswered is kept for the next one. */
            databuf_deliver(RESPONSE_TIMEOUT_MS);
            transport_close();

            /* An alert that went with the registration — the reject that
             * loses a datagram usually loses the registration too — is owed
             * like a report: the loop keeps the modem searching and sends it
             * the moment the network is back, not an hour on with the next
             * timed report.  On 2026-09-30 the bench's cut alert went at
             * 19:55:19 into exactly that and was never seen again.  One
             * unanswered with the network still up (a reply that never
             * came) waits for the next send, so a server that is not
             * answering is not asked again every RESEND_POLL_S.
             *
             * The same goes for records.  The check above runs before the
             * replies are waited for, so a registration lost while waiting
             * left the requeued record for the next timed wake: on
             * 2026-10-09 the parked car's 23:14 report went into a cause-9
             * reject 6 s after the send and reached the server at 00:15. */
            if ((alert_count > 0 || databuf_count() > 0) &&
                !resend_owed && !modem_is_registered()) {
                if (alert_count > 0) {
                    LOG_WRN("%d alert%s unsent with the network gone — owed "
                            "until it is back", alert_count,
                            alert_count == 1 ? "" : "s");
                } else {
                    LOG_WRN("%d record%s unanswered with the network gone — "
                            "owed until it is back", databuf_count(),
                            databuf_count() == 1 ? "" : "s");
                }
                report_owed_set(&resend_owed, true);
                resend_owed_ms = k_uptime_get();
            }

            /* Tell the server about a reverted update before asking it
             * for another one: a unit that goes straight back to sleep
             * after a revert would otherwise never get the report out. */
            fota_report_flush();

            /* Run a server-indicated update now, while the modem is still
             * registered and GNSS is already stopped: the response that was
             * just processed (cmd_run above) may have advertised a newer
             * version via fota=<ver>.  No-op otherwise — no extra traffic on
             * an ordinary wake. */
            fota_check(FOTA_CTX_ASLEEP);

            hw_power_shutdown();
            telemetry_remaining = telemetry_interval();
            if (s_nosignal_wakes > 0) {
                LOG_WRN("no network on %d consecutive wake%s — next timed "
                        "report in %ds", s_nosignal_wakes,
                        s_nosignal_wakes == 1 ? "" : "s", telemetry_remaining);
            }
        }

        /* power modem back off if any alert or telemetry path woke it —
         * except with a timed report owed on a search still inside its
         * window, or on a registration that has just arrived: the next
         * pass, at most RESEND_POLL_S away, sends it first */
        if (sleep_modem_release(modem_raised, resend_owed)) {
            console_resume();
            transport_teardown();
            modem_sleep();
        }

        /* The report is done with — sent, or given up on — and the modem is
         * down, so this is the whole cost of the wake.  Held for the next
         * record to carry, tagged with the record this one produced; see
         * wake_pending.  A wake that built no record at all (no position
         * since boot, nothing to send) has nothing to attribute the figure
         * to, so it is logged and dropped rather than filed against some
         * other record. */
        if (wake_at != 0 && !resend_owed) {
            int64_t awake_ms = k_uptime_get() - wake_at;

            console_resume();
            if (wake_rec_id != 0) {
                wake_pending.rec_id = wake_rec_id;
                wake_pending.ms = awake_ms;
                wake_pending.attach_ms = wake_attach_ms;
                wake_pending.signal = wake_signal;
                wake_pending.rsrp_dbm = wake_rsrp_dbm;
                wake_pending.snr_db = wake_snr_db;
                wake_pending.band = wake_band;
                LOG_INF("timed wake done: %lld ms awake, %d ms of it "
                        "attaching (record %u)",
                        awake_ms, wake_attach_ms, wake_rec_id);
            } else {
                LOG_INF("timed wake done: %lld ms awake, no record to "
                        "attribute it to", awake_ms);
            }
            wake_at = 0;
            wake_rec_id = 0;
            wake_attach_ms = -1;
            wake_signal = false;
        }

        /* Did the modem we left registered actually go to sleep?  One that
         * stays registered without entering PSM is indistinguishable from a
         * working one in telemetry — same fast wake, same absent attach — and
         * draws milliamps rather than microamps out of a parked vehicle
         * battery.  So it is checked rather than assumed, and the failure
         * falls back to the CFUN=0 this replaced.
         *
         * The window is the active timer the modem has to run down plus a
         * grace period, and it runs from the radio's last activity as
         * modem.c records it, never from an earlier pass: a deadline set
         * while the modem slept used to fall due on the pass of the next
         * timed report, a second after the send had woken it, and so
         * powered off a modem that PSM was working on.  Nothing here infers
         * PSM from being registered — that is exactly the pair this cannot
         * tell apart from outside. */
        if (IS_ENABLED(CONFIG_APP_PSM_SLEEP) && network_ready &&
            modem_psm_granted() && !modem_psm_asleep() &&
            modem_radio_quiet_ms() >=
                (int64_t)(CONFIG_APP_PSM_ACTIVE_S +
                          CONFIG_APP_PSM_SLEEP_GRACE_S) * 1000) {
            console_resume();
            LOG_WRN("PSM granted but the modem is still awake %ds after its "
                    "last radio activity — powering it off instead",
                    CONFIG_APP_PSM_ACTIVE_S + CONFIG_APP_PSM_SLEEP_GRACE_S);
            transport_teardown();
            modem_power_off();
        }

        /* re-read baseline and re-arm accel interrupt before next sleep cycle */
        accel_read_baseline();
        accel_irq_enable();

        /* re-check ignition before going back to sleep.  A backup wake only
         * counts once the line has been off for BACKUP_CONFIRM_MS, so a line
         * on here is a key, whatever woke this pass. */
        ign_now = ignition_read();
        if (ign_now == 0) {
            console_resume();
            LOG_INF("wake: ignition ON");
            atomic_set(&s_report_owed, 0);
            ignition = 0;
            s_key_wake = true;
            movement_reset();
            ign_irq_disable();
            accel_irq_disable();
            LOG_INF("wake: INA228 wake");
            hw_power_wake();
            LOG_INF("wake: aux power on");
            hw_aux_power_on();
            led_on();
            nbiot_fallback_end();
            LOG_INF("wake: modem connect");
            modem_connect();
            LOG_INF("wake: GNSS start");
            gnss_start();
            s_state = STATE_IDLE;
            return;
        }
        ign_before = ign_now;
    }
}

/* ========================================================================= */
/*  STATE_IGNITION_SLEEP — ignition ON, engine OFF, watching for engine      */
/* ========================================================================= */
static void do_ignition_sleep(void)
{
    LOG_INF("ignition sleep (ign=ON, engine=OFF)");
    data_gap_reset();
    led_all_off();
    int64_t last_voltage_ms = k_uptime_get();
    int64_t last_send_ms    = k_uptime_get();

    for (;;) {
        watchdog_kick();
        crash_check();
#if IS_ENABLED(CONFIG_APP_KLINE_OBD)
        /* Ignition on with the engine off: the ECU is still powered, so the
         * session and the fault-code watch stay live here too. */
        obd_service();
#endif

        ignition = (char)ignition_read();
        if (ignition != 0) {
            LOG_INF("ignition OFF — sending final position");
            use_cached_gps = true;
            read_udp_response = false;
            /* Report even with no cached fix — the point of this record is
             * the ignition state, not the position. */
            force_record = true;
            int have_record = collect_data(ignition);
            force_record = false;
            gnss_stop();
            if (have_record > 0) {
                /* The socket stays: do_sleep() collects the reply, which
                 * is what says this record arrived. */
                send_data();
                data_reset();
            }
            previous_ignition = ignition;
            s_state = STATE_SLEEP;
            return;
        }

        int64_t now = k_uptime_get();
        if (now - last_voltage_ms >= VOLTAGE_POLL_INTERVAL * 1000) {
            battery_v = battery_read_voltage();
            if (engine_is_running()) {
                engine_running = true;
                LOG_INF("engine started (%d rpm, %.2fV)",
                        engine_rpm_or_na(), (double)battery_v);
                s_state = STATE_IDLE;
                return;
            }
            last_voltage_ms = now;
        }

        now = k_uptime_get();
        if (now - last_send_ms >= IGNITION_ON_SLEEP_INTERVAL * 1000) {
            /* GNSS is already running continuously — grab a fix if one is
             * available (returns immediately when locked), don't block 60s. */
            struct gnss_fix fix = {0};
            if (gnss_collect(2000, &fix) == 0 && fix.valid) {
                g_gnss = fix;
            }
            use_cached_gps = true;
            read_udp_response = true;
            if (collect_data(ignition) > 0) {
                gnss_stop();
                /* The socket stays open for the replies — see send_data(). */
                send_data();
                data_reset();
                if (pending_server_cmd[0] != '\0') {
                    cmd_run(pending_server_cmd);
                    pending_server_cmd[0] = '\0';
                    if (alert_count > 0) alert_send();
                }
                if (!last_send_ok) modem_recover();
                gnss_resume();

                /* The response just processed may have advertised a newer
                 * firmware (fota=<ver>); no-op otherwise.  GNSS was resumed
                 * above, so the awake variant puts it back on failure. */
                fota_check(FOTA_CTX_AWAKE);

                /* Or switched track mode on: the main loop picks it up. */
                if (IS_ENABLED(CONFIG_APP_TRACK_MODE) && g_settings.track_mode) {
                    s_state = STATE_IDLE;
                    return;
                }
            }
            last_send_ms = k_uptime_get();
        }

        status_delay(1000);
    }
}

/* ========================================================================= */
/*  Track mode — GNSS off, ECU + IMU streamed at a fast cadence              */
/* ========================================================================= */
#if IS_ENABLED(CONFIG_APP_TRACK_MODE)
/* Entered from the main loop whenever the server has switched track mode on
 * and the ignition is on; runs until either changes.
 *
 * The receiver is stopped for the duration: the position is not what this
 * mode is for, and with GNSS out of the way the radio is LTE's outright, so
 * the socket is held and the RRC connection kept up between sends instead
 * of being released after each one (transport_set_streaming).  Each cycle
 * builds one record — last fix, the fast OBD poll, an IMU burst — sends it,
 * and idles out the rest of APP_TRACK_PERIOD_MS.  The K-wire poll is the
 * bulk of a cycle at ~400-500 ms; the send is tens of milliseconds.
 *
 * Only every APP_TRACK_RESP_INTERVAL_S is the server's reply waited for,
 * which costs a round trip and is how track=0 gets back to the device.
 *
 * Key-off ends it the way the other awake loops end: one final record from
 * the cached position with the ignition state, then sleep.  A switch-off
 * from the server hands back to the normal state machine with GNSS
 * resumed, and the next cycle re-acquires. */
static void do_track(void)
{
    const int64_t period_ms = CONFIG_APP_TRACK_PERIOD_MS;
    const int64_t resp_ms   = (int64_t)CONFIG_APP_TRACK_RESP_INTERVAL_S * 1000;
    int64_t last_resp = k_uptime_get() - resp_ms;   /* read the first reply */
    int64_t last_log = 0;
    int sent = 0;

    LOG_INF("track mode: GNSS off, one record per %lld ms", period_ms);
    data_gap_reset();
    gnss_stop();
    transport_set_streaming(true);
    led_idle();
    if (g_cell.mcc == 0) modem_update_cell_info();

    while (g_settings.track_mode) {
        int64_t t0 = k_uptime_get();

        watchdog_kick();
        crash_check();
#if IS_ENABLED(CONFIG_APP_KLINE_OBD)
        obd_service();
#endif
        if (power_reboot) {
            reboot_now();
        }

        ignition = (char)ignition_read();
        if (ignition != 0) {
            LOG_INF("track mode: ignition OFF — sending final position");
            transport_set_streaming(false);
            use_cached_gps = true;
            read_udp_response = false;
            force_record = true;
            int have_record = collect_data(ignition);
            force_record = false;
            if (have_record > 0) {
                /* Not streaming any more, so held until answered for; the
                 * socket stays for do_sleep() to collect the reply. */
                send_data();
            }
            data_reset();
            previous_ignition = ignition;
            engine_running = false;
            s_state = STATE_SLEEP;
            return;
        }

        bool want_resp = (t0 - last_resp) >= resp_ms;

        read_udp_response = want_resp;
        if (collect_track_data() > 0) {
            send_data();
            if (last_send_ok) {
                previous_ignition = ignition;
                s_last_send_ms = k_uptime_get();
                sent++;
            }
            if (want_resp) {
                last_resp = k_uptime_get();
                s_last_resp_ms = last_resp;
            }
            /* From any reply: every one is read now, and the server deletes
             * a command once it has put it in one. */
            if (pending_server_cmd[0] != '\0') {
                cmd_run(pending_server_cmd);
                pending_server_cmd[0] = '\0';
                if (alert_count > 0) alert_send();
            }
            if (!last_send_ok) {
                modem_recover();
            }
        }
        data_reset();

        if (k_uptime_get() - last_log >= 30000) {
            LOG_INF("track: %d records, %d rpm, %.1f km/h, %.2fV, cycle %lld ms",
                    sent, engine_rpm_or_na(), (double)vehicle_speed_kmh(),
                    (double)battery_v, k_uptime_get() - t0);
            last_log = k_uptime_get();
        }

        int64_t spent = k_uptime_get() - t0;
        if (spent < period_ms) {
            status_delay((long)(period_ms - spent));
        }
    }

    LOG_INF("track mode off — resuming GNSS");
    transport_set_streaming(false);
    transport_close();
    gnss_resume();
    s_state = STATE_IDLE;
}
#endif /* CONFIG_APP_TRACK_MODE */

/* ========================================================================= */
/*  main                                                                     */
/* ========================================================================= */
#if IS_ENABLED(CONFIG_APP_KLINE_DISCOVER)
static int kline_boot_res = -ENODATA;
static struct kline_discovery kline_boot_disc;
#endif

#if IS_ENABLED(CONFIG_APP_KLINE_DTC_REPORT)
/* Read the ECU's stored fault codes and queue them for the server, which
 * diffs the set against what it holds and alerts on codes appearing and
 * clearing.  Sent as its own "D," line rather than folded into a telemetry
 * record, because the server treats it as the device's complete current set
 * and must not infer one from a record that happens to lack the field.
 *
 * A failed read sends nothing at all: an empty report means "no codes
 * stored", so reporting one after a timeout would wrongly clear faults that
 * are still there. */
#endif

/* Builds that run a bench harness in place of the tracker.  They wait in loops
 * that never feed the watchdog, and a missed feed resets the SoC (watchdog.c),
 * so main() arms it for the tracker only. */
#define APP_HARNESS_BUILD                         \
    (IS_ENABLED(CONFIG_APP_PROVISION_MODE)   ||   \
     IS_ENABLED(CONFIG_APP_BOARD_TEST)       ||   \
     IS_ENABLED(CONFIG_APP_KLINE_DISCOVER)   ||   \
     IS_ENABLED(CONFIG_APP_KLINE_TEST)       ||   \
     IS_ENABLED(CONFIG_APP_CAN_TEST)         ||   \
     IS_ENABLED(CONFIG_APP_CAN_BENCH)        ||   \
     IS_ENABLED(CONFIG_APP_ACCEL_TEST)       ||   \
     IS_ENABLED(CONFIG_APP_VOLTAGE_TEST)     ||   \
     IS_ENABLED(CONFIG_APP_L_SENSE_TEST)     ||   \
     IS_ENABLED(CONFIG_APP_LTE_POWER_TEST))

/* Every datagram and the update check carry the IMEI.  Reading it needs no
 * network. */
static void read_imei(void)
{
    char imei[32] = {0};

    if (modem_get_imei(imei, sizeof(imei)) == 0) {
        strncpy(g_settings.imei, imei, sizeof(g_settings.imei) - 1);
        LOG_INF("imei=%s", g_settings.imei);
    }
}

int main(void)
{
    LOG_INF("=== l0destar firmware boot (v%s, board %s) ===",
            fota_version(), fota_board_id());

    /* Why this boot happened, on the console, now.  It also rides out with
     * the first record as rst=, but a unit that resets during bring-up
     * never gets that far — which is exactly when the answer matters, and
     * is how a reset loop hides what is resetting it.  Reading it here does
     * not consume it: both accessors cache, so the record still carries it.
     *
     * "on probation" means MCUboot swapped this image in as a test and will
     * take it back out unless the bring-up below completes. */
    {
        const char *rst = dbglog_reset_cause();

        LOG_INF("reset cause: %s%s", rst ? rst : "(none reported)",
                fota_image_on_probation() ? " — image on probation" : "");
    }

    /* Before anything that can take time, and long before the radio.
     *
     * It used to be armed at the end of bring-up, after the modem connect
     * and the A-GNSS fetch, some eleven seconds in.  That is too late in
     * both directions.  The nRF watchdog cannot be stopped once started and
     * survives a soft reset, so a unit that reboots for any reason — a FOTA
     * swap, a fatal error — comes up with the previous image's watchdog
     * still counting down its 12 s window and nothing feeding it until the
     * app takes ownership.  Bring-up is longer than the window, so the boot
     * dies at the same point every time, which is a reset loop that no
     * amount of reflashing over the air can break: the new image never
     * lives long enough to confirm itself and MCUboot reverts it.
     *
     * Taking ownership here instead feeds the stale watchdog within a
     * couple of hundred milliseconds (task_wdt allocates the same hardware
     * channel the previous image did, so the reload lands on the running
     * one), and everything slow below already kicks as it waits. */
    if (!APP_HARNESS_BUILD) {
        watchdog_init();
    }

    /* Before anything tries to update again: work out whether the update
     * staged before the last reboot is the one now running.  It queues an
     * alert and a line for the server if MCUboot reverted it, which is what
     * stops a bad image being downloaded over and over with the engine
     * off — the one failure mode of this that flattens a car battery. */
    fota_verdict_on_boot();

#if defined(CONFIG_APP_PROVISION_MODE)
    /* Provisioning build (prov.conf): bring up the modem library so the AT
     * Host library can bridge nrfcloud-utils <-> modem (AT%KEYGEN, cert
     * install) for nRF Cloud onboarding, then idle.  No LTE needed.  After
     * onboarding, reflash the normal build — the device key/cert persist in
     * modem NVM at CONFIG_NRF_CLOUD_SEC_TAG. */
    printk("\n*** PROVISIONING MODE — AT host ready; run nrfcloud-utils ***\n");
    (void)modem_init();
    for (;;) {
        k_sleep(K_FOREVER);
    }
#endif

    if (crypto_init()) {
        LOG_ERR("crypto init failed — halting");
        return 0;
    }
    settings_load();

    if (hw_gpio_init()) {
        LOG_ERR("gpio init failed — halting");
        return 0;
    }

    hw_domain_init();
    hw_aux_power_on();
    k_msleep(10);

#if IS_ENABLED(CONFIG_APP_LTE_POWER_TEST)
    /* LTE TX power / brown-out rig: idles with LED1 on, blasts uplink
     * traffic on ENTER.  Skips the self-test and every peripheral the
     * radio doesn't need.  Never returns. */
    lte_power_test_run();
#endif

    /* Tests 1 and 2 of the board test walk the same rails interactively, so
     * at boot the self-test would only cycle them a second time and bury the
     * operator's prompt under its own log. */
    if (!IS_ENABLED(CONFIG_APP_BOARD_TEST)) {
        hw_selftest();
    }

    /* A part on the I2C bus that did not answer.
     *
     * The warnings already reach the captured log, but a log line is
     * something you find after going to look, and these are silent losses of
     * what the device is for: with no accelerometer there is no movement,
     * impact or tow detection, and with no INA228 there is no battery
     * voltage — which the low-battery alert, the sleep safety gate and the
     * engine-running fallback all read.  The unit carries on reporting
     * position exactly as though it were whole, so without an alert nothing
     * would ever say otherwise.  Seen on a new bench build on 2026-09-24,
     * where the INA228 did not ACK while the accelerometer on the same bus
     * was fine.
     *
     * Queued, not sent: the radio is not up yet, so these ride out with the
     * first record.  Once per boot — there is no persistent store to
     * remember having said it, so a board with a dead part reports again
     * after every restart, FOTA reboots included. */
    if (hw_power_init()) {
        LOG_WRN("INA228 init failed — voltage unavailable");
        alert_enqueue("INA228 not responding (I2C): no battery voltage",
                      CONFIG_APP_HW_FAULT_PRIORITY);
    }
    if (hw_accel_init()) {
        LOG_WRN("accel init failed — readings unavailable");
        alert_enqueue("accelerometer not responding (I2C): no movement, "
                      "impact or tow detection",
                      CONFIG_APP_HW_FAULT_PRIORITY);
    }
    if (hw_can_init())    LOG_WRN("CAN controller init failed");
    if (IS_ENABLED(CONFIG_APP_BOARD_HAS_L_SENSE) && kline_l_sense_init()) {
        LOG_WRN("L sense init failed — no L-line short detection");
    }

#if IS_ENABLED(CONFIG_APP_BOARD_TEST)
    /* Interactive bring-up rig (board_test.sh): walks the operator through
     * every fitted subsystem over the console, then parks.  Never returns. */
    board_test_run();
#endif

#if IS_ENABLED(CONFIG_APP_ACCEL_TEST)
    if (accel_available()) {
        printk("\n*** ACCEL TEST — streaming at 10 Hz ***\n");
        printk("ax,ay,az,gx,gy,gz,temp\n");
        for (;;) {
            int ax, ay, az, gx = 0, gy = 0, gz = 0;
            float temp = 0;
            accel_read(&ax, &ay, &az);
            accel_read_gyro(&gx, &gy, &gz);
            accel_read_temp(&temp);
            printk("%d,%d,%d,%d,%d,%d,%.1f\n",
                   ax, ay, az, gx, gy, gz, (double)temp);
            k_msleep(100);
        }
    }
#endif

#if IS_ENABLED(CONFIG_APP_L_SENSE_TEST)
    kline_l_sense_test();   /* streams the L-line sense; never returns */
#endif

#if IS_ENABLED(CONFIG_APP_CAN_BENCH)
    can_bench_run();   /* host-driven CAN test target; never returns */
#endif

#if IS_ENABLED(CONFIG_APP_CAN_TEST)
    hw_can_test();
    printk("CAN test complete — halting.\n");
    for (;;) { k_msleep(10000); }
#endif

#if IS_ENABLED(CONFIG_APP_VOLTAGE_TEST)
    printk("\n*** VOLTAGE TEST — streaming INA228 VBUS at 2 Hz ***\n");
    if (!hw_power_available()) {
        printk("INA228 not available — nothing to read.\n");
    }
    for (;;) {
        float v = battery_read_voltage();
        if (v < 0.0f) {
            printk("read failed\n");
        } else {
            printk("VBUS=%.3f V\n", (double)v);
        }
        k_msleep(500);
    }
#endif

#if IS_ENABLED(CONFIG_APP_KLINE_TEST)
    kline_test();
    printk("K-line test complete — halting.\n");
    for (;;) { k_msleep(10000); }
#endif

#if IS_ENABLED(CONFIG_APP_KLINE_DISCOVER)
    /* One-shot investigation of an unknown vehicle: hunt the protocol,
     * rate and ECU addresses, ask each responder what it supports, and
     * finish with a summary and a suggested local.conf.  Slow and noisy by
     * design — run it once per vehicle, then configure the runtime path
     * from what it prints.  Parks afterwards so the console log survives:
     * going on to the modem would let the power-on FOTA check swap this
     * image out mid-investigation. */
    kline_boot_res = kline_discover(&kline_boot_disc);

    while (1) {
        status_delay(1000);
    }
#endif

    LOG_INF("ignition=%s battery=%.2fV",
            ignition_read() == 0 ? "ON" : "OFF",
            (double)battery_read_voltage());

    if (modem_init()) {
        LOG_ERR("modem init failed");
        return 0;
    }
    modem_on_registered(on_registered);
    /* Before connecting: a modem that registers after the start-up wait
     * would otherwise leave the IMEI unset, and every send dropped, for the
     * whole boot. */
    read_imei();
    if (modem_provision_tls()) {
        LOG_ERR("TLS provisioning failed");
        return 0;
    }
    if (gnss_init()) {
        LOG_ERR("gnss init failed");
        return 0;
    }
    if (agnss_init()) {
        LOG_WRN("A-GNSS init failed — will run without assistance");
    }

    led_boot_animation();
    if (modem_connect() == 0) {
        if (g_settings.imei[0] == '\0') {
            read_imei();   /* the read above failed */
        }
        transport_open();
        transport_teardown();
    }

    /* Fetch assistance while the radio is entirely LTE's.  GNSS and LTE
     * share one RF front-end, so doing this during a cold search — which is
     * where it used to happen, from inside gnss_collect() — starves the TLS
     * handshake and times out.  Full assistance rather than a targeted
     * request, because the receiver has not started yet and so has not asked
     * for anything specific. */
    if (modem_is_registered()) {
        if (agnss_fetch(NULL)) {
            LOG_WRN("A-GNSS fetch failed — first fix will take longer");
        }
        /* And the cell context, in the same window and for the same reason.
         * Left to STATE_GPS_COLLECT, where it would otherwise first run, it
         * happens with the receiver already started — which is the "radio
         * busy" a connection evaluation refuses outright and a neighbour
         * scan has to compete with.  So the one reading a boot takes, which
         * after an update is the first thing the server hears, would be the
         * worst of the run.  Costs a few AT commands on an ordinary build;
         * only the diagnostics make it slow, and only where asked for. */
        modem_update_cell_info();
    } else if (ignition_read() != 0) {
        /* Key off and no network: a fix could not be sent, so the receiver
         * is not started and the unit goes to sleep with the modem left
         * searching for what remains of APP_NETWORK_SEARCH_TIMEOUT.  The
         * sleep loop sends the first record, and runs the power-on update
         * check, if the modem registers in that time; otherwise the timed
         * wakes take over, and the first of those with a network searches
         * for the fix.  Before this the main loop idled below with GNSS
         * and the modem both searching until the network came, however
         * long that took. */
        LOG_WRN("no network with the ignition off — sleeping, GNSS not started");
        s_state = STATE_SLEEP;
    } else {
        LOG_INF("no network yet — skipping A-GNSS, GNSS will ask later");
    }

    if (s_state != STATE_SLEEP) {
        gnss_start();
    }

#if IS_ENABLED(CONFIG_APP_KLINE_TELEMETRY) || IS_ENABLED(CONFIG_APP_DEAD_RECKONING)
    /* Poll the ECU about once a second while waiting for a fix, which is
     * where most of a cycle goes: the record built after the fix then
     * costs no bus time, and engine RPM is sampled across the cycle rather
     * than once per record.  And drain the IMU FIFO into the dead
     * reckoning, which a wait as long as LTE can make it would otherwise
     * overrun. */
    gnss_set_tick(fix_wait_tick);
#endif
#if IS_ENABLED(CONFIG_APP_KLINE_TELEMETRY)
    obd_alert_init();
#endif

    crash_irq_enable();

    s_last_send_ms = k_uptime_get();

    /* Everything above got through without hanging or faulting, so a freshly
     * swapped image has proved itself enough to keep.  Until this runs, an
     * update is still on probation and MCUboot reverts it on the next boot. */
    fota_confirm_image();

    /* The one unconditional update check: power-on.  Later checks only run
     * when a telemetry response advertises a newer version (fota=<ver>).
     * GNSS is up, so fota_check restores it if a download fails; on success
     * it reboots and never returns. */
    fota_check(FOTA_CTX_AWAKE);

    LOG_INF("entering main loop");

    //do_sleep();

    for (;;) {
        watchdog_kick();
        crash_check();
        handle_ignition_state();
        /* The fix wait drains the IMU FIFO into the dead reckoning; this is
         * the rest of a cycle — the send, which can take seconds — so the
         * ring never holds more than a few. */
        dr_fifo_service();

        /* A failed-update report waiting for a link.  No-op with nothing
         * pending, which is every iteration but the ones after a revert. */
        if (network_ready) {
            fota_report_flush();
        }
#if IS_ENABLED(CONFIG_APP_KLINE_OBD)
        /* The RPM sampler covers the GPS fix wait; this covers everything
         * else in a cycle — the send and the idle — so the diagnostic
         * session is not dropped and re-initialised every time round, and it
         * is where a fault code raised mid-drive gets reported. */
        obd_service();
#endif

        if (power_reboot) {
            reboot_now();
        }

#if IS_ENABLED(CONFIG_APP_TRACK_MODE)
        /* Track mode takes over every awake state.  Not STATE_SEND: a record
         * is buffered there and goes out first, and with BATCH_SIZE 1 the
         * loop is back here a moment later. */
        if (g_settings.track_mode && ignition == 0 && network_ready &&
            (s_state == STATE_IDLE || s_state == STATE_GPS_COLLECT ||
             s_state == STATE_IGNITION_SLEEP)) {
            do_track();
            continue;
        }
#endif

        switch (s_state) {
        case STATE_IDLE: {
            /* The key is on and there is no registration: carry on below,
             * recording into the backlog, instead of waiting here. */
            bool offline = false;

            if (network_ready) {
                /* Registered — whether the poll below saw it or the LTE
                 * handler flipped the flag back on its own, which is what
                 * happens after every routine blip and skips the reset in
                 * the poll.  The retry timer has to measure the *current*
                 * outage: carried over from an earlier one it declared
                 * "no registration for 300s" five seconds into a cell
                 * change on 2026-09-19 (10:43 and 11:20), and the bring-up
                 * that followed turned each blip into a minute without
                 * the network. */
                s_unregistered_ms = 0;
                /* Also the drive that ends parked somewhere with coverage
                 * after a spell without it: the count is a run of dead
                 * wakes, and this is proof the run is over. */
                network_backoff_reset();
            } else {
                int reg = modem_get_network_status();
                if (reg == 1 || reg == 5) {
                    network_ready = true;
                    s_unregistered_ms = 0;
                    LOG_INF("network ready");
                    fota_report_flush();
                } else if (ignition == 0) {
                    network_retry_tick();

                    /* The key is on: the vehicle is being driven, or is
                     * about to be.  Nothing can be sent, but everything can
                     * be recorded — the backlog holds it and delivers it
                     * once the network is back (send_data() puts it there
                     * rather than hand it to a modem with no registration).
                     * This used to wait here instead, recording nothing: on
                     * 2026-09-27 at 10:57 the receiver kept fixing through
                     * a 16 s outage in the middle of a drive, and a single
                     * record was built. */
                    if (!s_wait_logged) {
                        s_wait_logged = true;
                        LOG_INF("no registration — recording into the "
                                "backlog");
                    }
                    offline = true;
                } else {
                    network_retry_tick();

                    /* Nothing can be sent, but the key turning still has
                     * to be captured now: the modem can spend a quarter of
                     * an hour searching, and building the ignition-off
                     * record only once it is back put the end of a drive
                     * at the wrong time.  GNSS is still running, so take
                     * whatever fix it has and build the record from that
                     * (or from the last known position if it has none).
                     * It waits in the send buffer with the rest of the
                     * batch. */
                    char told = s_buffered_records > 0 ? s_record_ignition
                                                       : previous_ignition;

                    if (previous_ignition != -1 && ignition != told) {
                        LOG_WRN("ignition %s with no registration — "
                                "recording now, sending when it returns",
                                ignition == 0 ? "ON" : "OFF");
                        struct gnss_fix fix = {0};
                        if (gnss_collect(2000, &fix) == 0 && fix.valid) {
                            g_gnss = fix;
                        }
                        use_cached_gps = true;
                        force_record = true;
                        s_record_ignition = ignition;
                        if (collect_data(ignition) > 0) {
                            s_buffered_records++;
                            s_transition_buffered = true;
                        }
                        force_record = false;
                        use_cached_gps = false;
                    }
                    /* With the key off there is nothing to stay awake for
                     * but the network, and that wait is bounded — see
                     * APP_NETWORK_SEARCH_TIMEOUT.  A boot with the key on,
                     * or a key-on wake, that never registered and has had
                     * its key turned off again ends up here, as does a
                     * drive that lost the network and parked; each used to
                     * idle with GNSS running and the modem searching until
                     * the network came.  Anything recorded meanwhile goes
                     * to the backlog for the next send that gets through,
                     * and since that is how the server will hear of the
                     * ignition state it carried, that state is latched as
                     * if sent — the next key-on is then a fresh transition
                     * rather than a repeat of this one. */
                    if (ignition != 0 && !s_coasting &&
                        network_search_expired()) {
                        LOG_WRN("no registration %d s after bringing the "
                                "radio up, ignition off — sleeping",
                                modem_unregistered_s());
                        if (s_buffered_records > 0) {
                            databuf_push_lines(data_current,
                                               (size_t)data_index);
                            data_reset();
                            s_buffered_records = 0;
                            s_transition_buffered = false;
                            previous_ignition = s_record_ignition;
                        }
                        s_wait_logged = false;
                        s_state = STATE_SLEEP;
                        break;
                    }
                    if (!s_wait_logged) {
                        s_wait_logged = true;
                        LOG_INF("waiting for network registration...");
                    }
                    /* One second, not five: the K-line keep-alive runs from
                     * the top of this loop and the ECU drops the session
                     * after P3max (5 s) of silence. */
                    status_delay(1000);
                    break;
                }
            }
            if (!offline) {
                s_wait_logged = false;
            }

            /* The registration handler can flip network_ready on its own,
             * so this is outside the poll above.  Send what was recorded
             * during the outage instead of collecting it again. */
            if (s_transition_buffered) {
                s_transition_buffered = false;
                s_state = STATE_SEND;
                break;
            }

            battery_v = battery_read_voltage();

            bool running_now = engine_is_running();

            if (ignition == 0 && !engine_running && running_now) {
                engine_running = true;
                LOG_INF("engine started (%d rpm, %.2fV)",
                        engine_rpm_or_na(), (double)battery_v);
            } else if (engine_running && !running_now) {
                engine_running = false;
                LOG_INF("engine stopped (%d rpm, %.2fV)",
                        engine_rpm_or_na(), (double)battery_v);
            }

            if (ignition == 0) s_key_wake = false;

            if (ignition == 0 && previous_ignition != 0 &&
                g_gnss.valid) {
                LOG_INF("ignition on — sending cached position");
                use_cached_gps = true;
                read_udp_response = false;
                if (g_cell.mcc == 0) modem_update_cell_info();
                if (collect_data(ignition) > 0) {
                    send_data();
                    data_reset();
                }
                s_last_send_ms = k_uptime_get();
                previous_ignition = ignition;
            }

#if IS_ENABLED(CONFIG_APP_KLINE_DTC_REPORT)
            /* Ignition-on: read what the ECU has persisted from earlier
             * drives.  Deliberately after the cached-position send above,
             * not before it: this costs the ECU's boot delay plus a session
             * open, and the position is the time-sensitive part.
             *
             * Latched on its own state, not previous_ignition, which is only
             * advanced when a position actually goes out — keying off it
             * would repeat the read every second until a fix appeared.
             *
             * And only with a registration: the report goes out at once as
             * its own datagram, and with no network the send would fail and
             * the read be lost.  A key-on without one records into the
             * backlog meanwhile, and reads the codes once the network is
             * back. */
            if (ignition == 0 && s_dtc_last_ign != 0 && network_ready) {
                s_dtc_last_ign = 0;
                k_msleep(CONFIG_APP_KLINE_DTC_ON_DELAY_MS);
                watchdog_kick();
                kline_report_dtcs("ignition on");
            } else if (ignition != 0) {
                s_dtc_last_ign = ignition;
            }
#endif

            /* Server-indicated update (fota=<ver> in a response), manual
             * `fota` command, or a power-on check that hit a dead link and
             * is still pending.  Serviced here so the download happens
             * between sends rather than mid-collection; a no-op (single
             * flag test) when nothing is pending.  Not while the engine runs:
             * a download stops GNSS and telemetry for minutes and ends in a
             * reboot, so a drive keeps its tracking and the update waits for
             * the engine to stop, key-off (STATE_SEND) or a timed wake.
             * Nor without a registration, which it could not download over. */
            if (!engine_running && network_ready) {
                fota_check(FOTA_CTX_AWAKE);
            }

            if (should_send_data()) {
                LOG_INF("collecting GPS fix (%d/%d)",
                        s_buffered_records + 1, BATCH_SIZE);
                use_cached_gps = false;
                s_state = STATE_GPS_COLLECT;
                break;
            }

            /* Ignition off and the server already told: nothing keeps the
             * unit awake, yet sleep is otherwise only entered from
             * STATE_SEND, so this idled with GNSS running until the next
             * timed record.  Reached when a key-on wake's key is off again
             * before the first poll here — the wake needs the line on for
             * 200 ms, but modem_connect() runs for seconds before IDLE
             * looks — and after a routine collection that found no fix.
             * A short key cycle is still an event someone caused, so it
             * gets one record from the cached position before sleeping. */
            if (ignition != 0 && previous_ignition != -1 && !s_coasting &&
                s_buffered_records == 0) {
                /* Off at the first look after a key-on wake is usually the
                 * starter turning, since the car's sense drops out for the
                 * crank (IGN_CRANK_MAX_MS) and the wake gets here a second
                 * or two after the key.  On 2026-10-03 at 10:16 this
                 * reported that as a short key cycle: a record built at the
                 * bottom of the sag, 9.82 V with the ignition "off", which
                 * also went out as "low battery", then a sleep the same key
                 * woke again four seconds later. */
                if (s_key_wake && ign_back_from_crank()) {
                    LOG_INF("ignition back on — that was the crank, not a "
                            "key cycle");
                    break;              /* re-read at the top of the loop */
                }
                if (s_key_wake) {
                    s_key_wake = false;
                    LOG_INF("ignition off again since the wake — reporting "
                            "once");
                    use_cached_gps = true;
                    read_udp_response = false;
                    force_record = true;
                    if (g_cell.mcc == 0) modem_update_cell_info();
                    s_record_ignition = ignition;
                    if (collect_data(ignition) > 0) {
                        send_data();
                        data_reset();
                    }
                    force_record = false;
                    use_cached_gps = false;
                    s_last_send_ms = k_uptime_get();
                    previous_ignition = ignition;
                }
                LOG_INF("ignition off with nothing to send — sleeping");
                s_state = STATE_SLEEP;
                break;
            }
            status_delay(1000);
            break;
        }

        case STATE_GPS_COLLECT: {
            led_gps_searching();
            if (g_cell.mcc == 0) modem_update_cell_info();

            /* An ignition change is the one thing that must not wait for a
             * fix.  Without this the no-fix path below dropped the record and
             * still advanced previous_ignition, so the transition was gone:
             * should_send_data() no longer saw it, nothing retried, and the
             * final ignition-off point never arrived — the common case being
             * switching off indoors, where a fix is least likely. */
            force_record = (previous_ignition != -1 &&
                            ignition != previous_ignition);

            s_record_ignition = ignition;
            int have_record = collect_data(ignition);
            force_record = false;

            if (have_record && !last_record_stale) led_gps_fixed();
            if (!have_record) {
                LOG_WRN("no fix, skipping send");
                led_idle();
                data_reset();
                s_last_send_ms = k_uptime_get();
                /* Coast-to-stop only ends in STATE_SEND, which a collection
                 * with no fix never reaches, and while it lasts nothing lets
                 * the unit sleep.  Without a fix there is no motion to
                 * follow anyway. */
                if (s_coasting) {
                    LOG_INF("coast-to-stop ended: no fix");
                    s_coasting = false;
                }
                /* With a transition pending, force_record gets a record
                 * built from the last known position, so this branch is
                 * normally a routine no-fix collection and the assignment is
                 * a no-op.  It is still reached in two cases, and both want
                 * the assignment: the very first collection (leaving
                 * previous_ignition at -1 would make should_send_data() spin
                 * on 60 s GNSS attempts), and a unit with no last known
                 * position at all, where no valid record can be produced no
                 * matter how often it retries. */
                previous_ignition = ignition;
                /* With the ignition off and no position since boot there is
                 * nothing to send, and sleep is only entered from STATE_SEND,
                 * so idling here would keep the unit awake, GNSS searching,
                 * until a fix turned up.  Sleep instead: do_sleep() searches
                 * again on timed wakes. */
                if (ignition != 0 && !have_position()) {
                    s_state = STATE_SLEEP;
                } else {
                    s_state = STATE_IDLE;
                }
                break;
            }
            s_buffered_records++;
            /* Not counting the motion through gaps (mv=): it is for the
             * fixes the radio cost, and must not cost more of them by
             * making the batch go early.  send_data() leaves it off a batch
             * that would not fit the datagram. */
            if (s_buffered_records >= BATCH_SIZE
                || ignition != 0
                || previous_ignition == -1
                || send_int_to_server
                || !last_send_ok
                || data_index - data_motion_bytes() >= BATCH_FLUSH_BYTES) {
                s_state = STATE_SEND;
            } else {
                s_state = STATE_IDLE;
            }
            break;
        }

        case STATE_SEND:
            read_udp_response =
                (previous_ignition == -1
                 || previous_ignition != ignition
                 || ignition != 0
                 || vehicle_speed_kmh() < 0.005f
                 || (CONFIG_APP_RESP_POLL_S > 0 &&
                     k_uptime_get() - s_last_resp_ms >=
                         (int64_t)CONFIG_APP_RESP_POLL_S * 1000));

            LOG_INF("sending %d records", s_buffered_records);
            led_sending();
            gnss_stop();
            /* The socket is kept after the send, not closed: the server's
             * reply is what says the records arrived, and it lands on it.
             * The release hint set on the send is what frees the radio. */
            send_data();
            k_msleep(200);
            led_sent();
            s_last_send_ms = k_uptime_get();
            s_buffered_records = 0;
            s_transition_buffered = false;
            data_reset();

            if (read_udp_response && last_send_ok) {
                s_last_resp_ms = k_uptime_get();
            }
            /* From any reply, not only one this send waited for: every
             * reply is read now, and the server deletes a command once it
             * has put it in one. */
            if (pending_server_cmd[0] != '\0') {
                cmd_run(pending_server_cmd);
                pending_server_cmd[0] = '\0';
                if (alert_count > 0) alert_send();
            }

            /* A drive recording through an outage cycles between here and
             * STATE_GPS_COLLECT without passing STATE_IDLE's poll, so the
             * radio's retry timer is kept from here too — and reset here,
             * so a later outage is not measured from an earlier one. */
            if (modem_is_registered()) {
                s_unregistered_ms = 0;
            } else {
                network_retry_tick();
            }

            /* network error recovery */
            if (!last_send_ok) {
                modem_recover();
                s_coasting = false;
            }

            /* coast-to-stop: keep sending after ignition off while moving */
            if (!s_coasting &&
                previous_ignition == 0 && ignition != 0 &&
                g_gnss.valid &&
                vehicle_speed_kmh() > COAST_STOP_SPEED_KMH) {
                s_coasting = true;
                s_coast_iters = 0;
                LOG_INF("coast-to-stop started");
            }
            if (s_coasting) {
                s_coast_iters++;
                if (vehicle_speed_kmh() <= COAST_STOP_SPEED_KMH ||
                    s_coast_iters >= COAST_MAX_ITERATIONS) {
                    LOG_INF("coast-to-stop ended (spd=%.1f iter=%d)",
                            (double)vehicle_speed_kmh(), s_coast_iters);
                    s_coasting = false;
                }
            }

            /* previous_ignition is "what the server has been told", so it
             * takes the state the record carried — not a fresh reading.
             *
             * The two differ far more often than it looks.  collect_data()
             * blocks up to GPS_FIX_TIMEOUT_MS waiting for a fix, and
             * handle_ignition_state() re-reads the line at the top of every
             * loop iteration, so with BATCH_SIZE 1 the key routinely turns
             * between the record being built and this line running.  Taking
             * the fresh value here marked the server as having been told
             * "off" when the record it actually received said "on": the
             * transition was consumed without ever being sent, the state
             * machine slept, and the drive ended on an ignition-on point at
             * the parking spot.
             *
             * A send that failed is likewise still owed, so it isn't latched
             * either. */
            if (last_send_ok) {
                previous_ignition = s_record_ignition;
            }
            led_idle();

            /* The key can also turn during send_data() itself. */
            ignition = (char)ignition_read();

            /* state transition */
            if (ignition != 0 && s_record_ignition != 0 && !s_coasting &&
                !last_send_ok && modem_is_registered()) {
                /* Key off, and the record that says so failed to send with
                 * the network up: a dead socket, a data connection gone
                 * under a live registration, a lookup that failed.  Going
                 * round again would only fail the same way, and each pass
                 * pushed another copy of the record into the backlog, with
                 * nothing to stop it short of the modem recovery's restart.
                 * The record is in the backlog already, so the server is as
                 * told as it can be for now: sleep, and let do_sleep()'s
                 * delivery and the timed wakes after it carry the backlog. */
                LOG_WRN("ignition-off record failed to send with the "
                        "network up — held for the next delivery, sleeping");
                previous_ignition = s_record_ignition;
                s_state = STATE_SLEEP;
            } else if (previous_ignition != ignition && ignition != 0 &&
                !s_coasting && !last_send_ok && !modem_is_registered()) {
                /* Key off, and no network to take the record that says so.
                 * The collect-and-send round below is right for a drive
                 * that loses the network — it is what records the drive
                 * into the backlog — but with the key off it repeated with
                 * the same result, once a second, for as long as the outage
                 * lasted, each pass pushing another copy of the ignition-off
                 * record into the backlog and thinning the drive's own
                 * records out of it.  IDLE already knows how to wait out an
                 * outage with the key off: it records the change once,
                 * holds it for the send that follows registration, and
                 * gives up at APP_NETWORK_SEARCH_TIMEOUT. */
                LOG_INF("ignition OFF with no network — waiting in idle");
                gnss_resume();
                s_state = STATE_IDLE;
            } else if (previous_ignition != ignition) {
                /* Ignition changed while that record was being built or sent,
                 * so the server has not been told.  Go round once more to
                 * report it before sleeping; force_record in
                 * STATE_GPS_COLLECT guarantees that pass yields a record even
                 * if GNSS cannot reacquire indoors. */
                LOG_INF("ignition changed during send — reporting %s",
                        ignition == 0 ? "ON" : "OFF");
                gnss_resume();
                s_state = STATE_GPS_COLLECT;
            } else if (ignition != 0 && !s_coasting) {
                /* Key off is where an update gets installed.  The device
                 * already knows whether one is waiting — every response
                 * carries fota=<version> and the last one was the reply to
                 * the ignition-off record just sent — and this is the best
                 * moment to act on it: the radio is still registered, the
                 * battery has just come off the alternator, and the vehicle
                 * is not about to be driven.  Left to the telemetry wake it
                 * would happen an hour later on a colder battery, and only
                 * if the unit still had an engine-off interval to wake on.
                 * A no-op with nothing pending — no traffic on an ordinary
                 * key off. */
                fota_check(FOTA_CTX_AWAKE);
                s_state = STATE_SLEEP;
            } else if (!engine_running && !s_coasting) {
                s_state = STATE_IGNITION_SLEEP;
            } else {
                gnss_resume();
                s_state = STATE_GPS_COLLECT;
            }
            break;

        case STATE_IGNITION_SLEEP:
            do_ignition_sleep();
            break;

        case STATE_SLEEP:
            do_sleep();
            crash_irq_enable();   /* re-arm impact detection for awake */
            break;
        }
    }
    return 0;
}
