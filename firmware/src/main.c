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

#include <modem/lte_lc.h>

LOG_MODULE_REGISTER(main, CONFIG_APP_LOG_LEVEL);

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
bool     engine_running;
float    battery_v;

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

static void ign_isr(const struct device *dev, struct gpio_callback *cb,
                    uint32_t pins)
{
    k_sem_give(&s_wake_sem);
}

static void ign_irq_enable(void)
{
    if (!s_ign_cb_installed) {
        gpio_init_callback(&s_ign_cb, ign_isr, BIT(PIN_IGN_SENSE));
        gpio_add_callback(hw_gpio0, &s_ign_cb);
        s_ign_cb_installed = true;
    }
    gpio_pin_interrupt_configure(hw_gpio0, PIN_IGN_SENSE, GPIO_INT_EDGE_BOTH);
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
 * harmlessly: with the device suspended the driver's tx path is a no-op. */
#if DT_HAS_CHOSEN(zephyr_console)
static const struct device *const s_console_dev =
    DEVICE_DT_GET_OR_NULL(DT_CHOSEN(zephyr_console));
#else
static const struct device *const s_console_dev = NULL;
#endif

static void console_suspend(void)
{
    if (s_console_dev == NULL || !device_is_ready(s_console_dev)) {
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
    }
}

static void console_resume(void)
{
    if (s_console_dev == NULL || !device_is_ready(s_console_dev)) {
        return;
    }
    (void)pm_device_action_run(s_console_dev, PM_DEVICE_ACTION_RESUME);
}

/* -- accelerometer wake interrupt ------------------------------------------ */
static struct gpio_callback s_accel_cb;
static bool s_accel_cb_installed;
static atomic_t s_accel_int_flag;

static void accel_isr(const struct device *dev, struct gpio_callback *cb,
                      uint32_t pins)
{
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
    gpio_pin_interrupt_configure(hw_gpio0, PIN_ACC_INT1,
                                 GPIO_INT_EDGE_TO_ACTIVE);
}

static void accel_irq_disable(void)
{
    if (!accel_available()) return;
    gpio_pin_interrupt_configure(hw_gpio0, PIN_ACC_INT1, GPIO_INT_DISABLE);
    accel_disable_wake_int();
}

/* -- crash (impact) interrupt while awake ----------------------------------- */
static void crash_irq_enable(void)
{
    if (!accel_available()) return;
    accel_cb_install();
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

/* Called from the awake loops.  The FIFO ring holds ~9 s of accel+gyro
 * history, so the impact profile is intact even with loop-cadence latency. */
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
 * but for "are we moving or not" the zero is what matters. */
static float vehicle_speed_kmh(void)
{
#if IS_ENABLED(CONFIG_APP_KLINE_OBD)
    int obd = obd_speed_kmh();

    if (obd >= 0) {
        return (float)obd;
    }
#endif
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

static void do_sleep(void)
{
    LOG_INF("entering sleep");
    /* No fault-code read here: sleep is only ever entered with the ignition
     * off, and the engine ECU is unpowered then, so the read would time out
     * and an empty report would wrongly clear live faults.  Codes raised
     * during a drive are caught while it is still running, by the stored-code
     * count in mode 01 PID 01 (see obd_dtc_pending). */
    crash_irq_disable();
    led_sleep_enter();
    LOG_INF("sleep: GNSS stop");
    gnss_stop();
    LOG_INF("sleep: transport close");
    transport_close();
    LOG_INF("sleep: modem power off");
    lte_lc_power_off();
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
    network_ready = false;
    led_all_off();
    LOG_INF("sleep: all peripherals off");

    accel_read_baseline();
    accel_snapshot_tilt_ref();
    accel_irq_enable();

    bool tow_alerted = false;
    bool tamper_alerted = false;
    int  tow_last_tilt = -1;      /* tenths, for the "still" test */
    int  tow_stable_secs = 0;

    if (s_saved_loop_interval < 0) {
        s_saved_loop_interval = g_settings.loop_interval;
    }

    int telemetry_remaining = g_settings.loop_interval;

    ign_irq_enable();
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
        if (sleep_secs < 1) sleep_secs = 1;

        k_sem_reset(&s_wake_sem);
        int64_t t0 = k_uptime_get();
        console_suspend();
        k_sem_take(&s_wake_sem, K_SECONDS(sleep_secs));
        console_resume();
        int elapsed = (int)((k_uptime_get() - t0) / 1000);
        if (elapsed < 1) elapsed = 1;

        /* Set by every path below that brings the radio up.  network_ready is
         * not enough on its own: modem_connect() sets it only on success, but
         * lte_lc_connect() has already taken the modem out of offline mode by
         * the time it fails, so a failed connect leaves the radio powered and
         * searching with the flag still false. */
        bool modem_raised = false;

        /* update timers */
        if (telemetry_remaining > 0) telemetry_remaining -= elapsed;
        if (s_move_cooldown_secs > 0) s_move_cooldown_secs -= elapsed;
        s_move_idle_secs += elapsed;
        if (s_move_idle_secs >= MOVEMENT_INACTIVITY_RESET)
            s_move_alert_level = 0;

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
                k_msleep(2000);                       /* debounce */
                tilt = accel_tilt_from_ref_tenths();
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
                accel_snapshot_tilt_ref();
                tow_alerted = false;
                tow_stable_secs = 0;
                tow_last_tilt = -1;
                LOG_INF("tow tilt re-armed at the new resting attitude");
            }
        }

        /* --- 6D orientation tamper (unit flipped / off its mount) --- */
        /* TODO: re-enable once unit is permanently mounted */
#if 0
        /* Checked on every wake, not gated on the INT pin: the wake pulse
         * de-asserts before the loop runs, but the zone bits persist. */
        if (accel_available()) {
            uint8_t d6d = 0;
            int changed = accel_d6d_tamper(&d6d);
            if (changed && !tamper_alerted) {
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

        /* --- ignition check --- */
        int ign_now = ignition_read();
        if (ign_now != ign_before) {
            k_msleep(200);
            ign_now = ignition_read();
            if (ign_now == 0) {
                LOG_INF("wake: ignition ON");
                ignition = 0;
                movement_reset();
                ign_irq_disable();
                accel_irq_disable();
                LOG_INF("wake: INA228 wake");
                hw_power_wake();
                LOG_INF("wake: aux power on");
                hw_aux_power_on();
                led_on();
                LOG_INF("wake: modem connect");
                modem_connect();
                LOG_INF("wake: GNSS start");
                gnss_start();
                s_state = STATE_IDLE;
                return;
            }
            ign_before = ign_now;
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

            if (!accel_confirm_movement()) {
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
                 * whether or not the connect actually registered. */
                if (modem_raised || network_ready) {
                    lte_lc_power_off();
                    network_ready = false;
                }
                accel_read_baseline();
                accel_irq_enable();
                continue;
            }
            accel_irq_disable();
            LOG_INF("movement confirmed");
            led_accel_movement();
            s_move_idle_secs = 0;
            s_move_needs_gps = true;

            if (s_move_cooldown_secs <= 0) {
                int tilt, delta;
                accel_get_movement_info(&tilt, &delta);
                char msg[80];
                snprintf(msg, sizeof(msg),
                         "movement: %d.%ddeg tilt, %dmg",
                         tilt / 10, tilt % 10, delta);
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

        /* --- timer telemetry --- */
        if (telemetry_remaining <= 0 && g_settings.loop_interval > 0) {
            LOG_INF("sleep: INA228 wake for voltage read");
            hw_power_wake();
            float v = battery_read_voltage();
            battery_v = v;

            if (v > 0 && v < BATTERY_POWEROFF_LEVEL) {
                LOG_WRN("battery %.2fV < poweroff", (double)v);
                hw_power_shutdown();
                telemetry_remaining = BATTERY_CHECK_INTERVAL;
                continue;
            }
            if (v > 0 && v < SLEEP_SAFETY_VOLTAGE) {
                LOG_WRN("battery %.2fV, skipping send", (double)v);
                hw_power_shutdown();
                telemetry_remaining = g_settings.loop_interval;
                continue;
            }

            watchdog_kick();
            int reg = modem_get_network_status();
            if (reg != 1 && reg != 5) modem_connect();
            modem_update_cell_info();

            if (s_move_needs_gps) {
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
                send_data();
                if (pending_server_cmd[0] != '\0') {
                    cmd_run(pending_server_cmd);
                    pending_server_cmd[0] = '\0';
                    if (alert_count > 0) alert_send();
                }
                if (!last_send_ok) modem_recover();
            }
            data_reset();

            if (s_move_needs_gps) {
                gnss_stop();
                hw_domain_release(HW_DOMAIN_AUX, HW_DOMAIN_USER_GNSS);
                s_move_needs_gps = false;
            }
            use_cached_gps = false;
            transport_close();

            /* Run a server-indicated update now, while the modem is still
             * registered and GNSS is already stopped: the response that was
             * just processed (cmd_run above) may have advertised a newer
             * version via fota=<ver>.  No-op otherwise — no extra traffic on
             * an ordinary wake. */
            fota_check(FOTA_CTX_ASLEEP);

            hw_power_shutdown();
            telemetry_remaining = g_settings.loop_interval;
        }

        /* power modem back off if any alert or telemetry path woke it */
        if (modem_raised || network_ready) {
            lte_lc_power_off();
            network_ready = false;
        }

        /* re-read baseline and re-arm accel interrupt before next sleep cycle */
        accel_read_baseline();
        accel_irq_enable();

        /* re-check ignition before going back to sleep */
        ign_now = ignition_read();
        if (ign_now == 0) {
            LOG_INF("wake: ignition ON");
            ignition = 0;
            movement_reset();
            ign_irq_disable();
            accel_irq_disable();
            LOG_INF("wake: INA228 wake");
            hw_power_wake();
            LOG_INF("wake: aux power on");
            hw_aux_power_on();
            led_on();
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
                send_data();
                transport_close();
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
                send_data();
                transport_close();
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
                send_data();
            }
            transport_close();
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
                if (pending_server_cmd[0] != '\0') {
                    cmd_run(pending_server_cmd);
                    pending_server_cmd[0] = '\0';
                    if (alert_count > 0) alert_send();
                }
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

int main(void)
{
    LOG_INF("=== l0destar firmware boot (v%s, board %s) ===",
            fota_version(), fota_board_id());

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

    if (hw_power_init())  LOG_WRN("INA228 init failed — voltage unavailable");
    if (hw_accel_init())  LOG_WRN("accel init failed — readings unavailable");
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
        char imei[32] = {0};
        if (modem_get_imei(imei, sizeof(imei)) == 0) {
            strncpy(g_settings.imei, imei, sizeof(g_settings.imei) - 1);
            LOG_INF("imei=%s", g_settings.imei);
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
    } else {
        LOG_INF("no network yet — skipping A-GNSS, GNSS will ask later");
    }

    gnss_start();

#if IS_ENABLED(CONFIG_APP_KLINE_TELEMETRY)
    /* Sample engine RPM about once a second while waiting for a fix, which
     * is where most of a cycle goes.  One reading per record would say
     * nothing about how the car was driven. */
    gnss_set_tick(obd_sample_tick);
#endif

    crash_irq_enable();

    watchdog_init();
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
        case STATE_IDLE:

            if (!network_ready) {
                LOG_INF("waiting for network registration...");
                int reg = modem_get_network_status();
                if (reg == 1 || reg == 5) {
                    network_ready = true;
                    LOG_INF("network ready");
                } else {
                    status_delay(5000);
                    break;
                }
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
             * would repeat the read every second until a fix appeared. */
            if (ignition == 0 && s_dtc_last_ign != 0) {
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
             * flag test) when nothing is pending. */
            fota_check(FOTA_CTX_AWAKE);

            if (should_send_data()) {
                LOG_INF("collecting GPS fix (%d/%d)",
                        s_buffered_records + 1, BATCH_SIZE);
                use_cached_gps = false;
                s_state = STATE_GPS_COLLECT;
                break;
            }
            status_delay(1000);
            break;

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
                s_state = STATE_IDLE;
                break;
            }
            s_buffered_records++;
            if (s_buffered_records >= BATCH_SIZE
                || ignition != 0
                || previous_ignition == -1
                || send_int_to_server
                || !last_send_ok
                || data_index >= DATA_LIMIT - BATCH_HEADROOM) {
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
            send_data();
            transport_close();
            k_msleep(200);
            led_sent();
            s_last_send_ms = k_uptime_get();
            s_buffered_records = 0;
            data_reset();

            if (read_udp_response && last_send_ok) {
                s_last_resp_ms = k_uptime_get();
            }
            if (pending_server_cmd[0] != '\0' && read_udp_response) {
                cmd_run(pending_server_cmd);
                pending_server_cmd[0] = '\0';
                if (alert_count > 0) alert_send();
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
            if (previous_ignition != ignition) {
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
