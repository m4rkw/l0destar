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
    alert_enqueue(msg, 2);
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
static void do_sleep(void)
{
    LOG_INF("entering sleep");
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
    kline_power_off();
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

    if (s_saved_loop_interval < 0) {
        s_saved_loop_interval = g_settings.loop_interval;
    }

    int telemetry_remaining = g_settings.loop_interval;
    if (telemetry_remaining == 0 && RELAY_CONNECTED) {
        telemetry_remaining = BATTERY_CHECK_INTERVAL;
    }

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

        /* update timers */
        if (telemetry_remaining > 0) telemetry_remaining -= elapsed;
        if (s_move_cooldown_secs > 0) s_move_cooldown_secs -= elapsed;
        s_move_idle_secs += elapsed;
        if (s_move_idle_secs >= MOVEMENT_INACTIVITY_RESET)
            s_move_alert_level = 0;

        /* --- slow-tilt check (tow / jack) --- */
        if (TOW_TILT_DEG > 0 && accel_available()) {
            int tilt = accel_tilt_from_ref_tenths();
            if (tilt >= TOW_TILT_DEG * 10) {
                k_msleep(2000);                       /* debounce */
                tilt = accel_tilt_from_ref_tenths();
                if (tilt >= TOW_TILT_DEG * 10 && !tow_alerted) {
                    tow_alerted = true;
                    LOG_WRN("sustained tilt %d.%ddeg - possible tow/jack",
                            tilt / 10, tilt % 10);
                    char msg[56];
                    snprintf(msg, sizeof(msg),
                             "tilt %d.%ddeg - possible tow/jack",
                             tilt / 10, tilt % 10);
                    alert_enqueue(msg, 2);
                    watchdog_kick();
                    int reg = modem_get_network_status();
                    if (reg != 1 && reg != 5) modem_connect();
                    alert_send_standalone();
                }
            } else if (tilt >= 0 && tilt < TOW_TILT_DEG * 5) {
                tow_alerted = false;   /* re-arm once back near level */
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
                alert_enqueue("tamper: orientation changed", 2);
                watchdog_kick();
                int reg = modem_get_network_status();
                if (reg != 1 && reg != 5) modem_connect();
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
                LOG_INF("wake: relay set");
                relay_set();
                s_state = STATE_IDLE;
                return;
            }
            ign_before = ign_now;
        }

        /* --- movement check (interrupt woke us, confirm sustained) --- */
        if (accel_available() && gpio_pin_get(hw_gpio0, PIN_ACC_INT1) == 1) {
            LOG_INF("accel wake — confirming movement");
            gpio_pin_interrupt_configure(hw_gpio0, PIN_ACC_INT1,
                                         GPIO_INT_DISABLE);
            if (!accel_confirm_movement()) {
                /* True peak comes from the FIFO ring (the chip saw the
                 * whole transient); the 100 ms confirm polls only see the
                 * residual.  Fall back to the polled peak if drain fails. */
                struct accel_impact imp;
                int peak = (accel_fifo_drain_impact(&imp) == 0 && imp.samples)
                         ? imp.peak_delta_mg : accel_confirm_peak_mg();
                if (peak >= PARKED_IMPACT_MG) {
                    LOG_WRN("parked impact: %d mg", peak);
                    char msg[48];
                    snprintf(msg, sizeof(msg), "parked impact %d.%02dg",
                             peak / 1000, (peak % 1000) / 10);
                    alert_enqueue(msg, 2);
                    watchdog_kick();
                    int reg = modem_get_network_status();
                    if (reg != 1 && reg != 5) modem_connect();
                    alert_send_standalone();
                    lte_lc_power_off();
                    network_ready = false;
                } else {
                    LOG_INF("transient bump — ignoring (peak %d mg)", peak);
                }
                accel_read_baseline();
                accel_irq_enable();
                continue;
            }
            accel_irq_disable();
            LOG_INF("movement confirmed");
            s_move_idle_secs = 0;
            s_move_needs_gps = true;

            if (s_move_cooldown_secs <= 0) {
                int tilt, delta;
                accel_get_movement_info(&tilt, &delta);
                char msg[80];
                snprintf(msg, sizeof(msg),
                         "movement: %d.%ddeg tilt, %dmg",
                         tilt / 10, tilt % 10, delta);
                alert_enqueue(msg, 2);

                watchdog_kick();
                int reg = modem_get_network_status();
                if (reg != 1 && reg != 5) modem_connect();
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
                if (reg != 1 && reg != 5) modem_connect();
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
                relay_reset();
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
                if (!last_send_ok) modem_recover(gsm_send_failures);
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
        if (network_ready) {
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
            LOG_INF("wake: relay set");
            relay_set();
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

        ignition = (char)ignition_read();
        if (ignition != 0) {
            LOG_INF("ignition OFF — sending final position");
            use_cached_gps = true;
            read_udp_response = false;
            if (collect_data(ignition) > 0) {
                gnss_stop();
                send_data();
                transport_close();
                data_reset();
            } else {
                gnss_stop();
            }
            previous_ignition = ignition;
            s_state = STATE_SLEEP;
            return;
        }

        int64_t now = k_uptime_get();
        if (now - last_voltage_ms >= VOLTAGE_POLL_INTERVAL * 1000) {
            battery_v = battery_read_voltage();
            if (battery_v >= ENGINE_RUNNING_VOLTAGE) {
                engine_running = true;
                LOG_INF("engine started (%.2fV)", (double)battery_v);
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
                if (!last_send_ok) modem_recover(gsm_send_failures);
                gnss_resume();

                /* The response just processed may have advertised a newer
                 * firmware (fota=<ver>); no-op otherwise.  GNSS was resumed
                 * above, so the awake variant puts it back on failure. */
                fota_check(FOTA_CTX_AWAKE);
            }
            last_send_ms = k_uptime_get();
        }

        status_delay(1000);
    }
}

/* ========================================================================= */
/*  main                                                                     */
/* ========================================================================= */
int main(void)
{
    LOG_INF("=== l0destar firmware boot (v%s) ===", fota_version());

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

    hw_selftest();

    if (hw_power_init())  LOG_WRN("INA228 init failed — voltage unavailable");
    if (hw_accel_init())  LOG_WRN("accel init failed — readings unavailable");
    if (hw_can_init())    LOG_WRN("CAN controller init failed");

    relay_init();

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

#if IS_ENABLED(CONFIG_APP_CAN_TEST)
    hw_can_test();
    printk("CAN test complete — halting.\n");
    for (;;) { k_msleep(10000); }
#endif

#if IS_ENABLED(CONFIG_APP_KLINE_TEST)
    kline_test();
    printk("K-line test complete — halting.\n");
    for (;;) { k_msleep(10000); }
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

    gnss_start();

    if (ignition_read() == 0 || g_settings.always_on) {
        relay_set();
    }

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

        if (power_reboot) {
            reboot_now();
        }

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
            if (ignition == 0 && !engine_running &&
                battery_v >= ENGINE_RUNNING_VOLTAGE) {
                engine_running = true;
                LOG_INF("engine started (%.2fV)", (double)battery_v);
            } else if (engine_running &&
                       battery_v < ENGINE_RUNNING_VOLTAGE) {
                engine_running = false;
                LOG_INF("engine stopped (%.2fV)", (double)battery_v);
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
            int have_fix = collect_data(ignition);
            if (have_fix) led_gps_fixed();
            if (!have_fix) {
                LOG_WRN("no fix, skipping send");
                led_idle();
                data_reset();
                s_last_send_ms = k_uptime_get();
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
                 || g_gnss.speed_kmh < 0.005f);

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

            if (pending_server_cmd[0] != '\0' && read_udp_response) {
                cmd_run(pending_server_cmd);
                pending_server_cmd[0] = '\0';
                if (alert_count > 0) alert_send();
            }

            /* network error recovery */
            if (!last_send_ok) {
                modem_recover(gsm_send_failures);
                s_coasting = false;
            }

            /* coast-to-stop: keep sending after ignition off while moving */
            if (!s_coasting &&
                previous_ignition == 0 && ignition != 0 &&
                g_gnss.speed_kmh > COAST_STOP_SPEED_KMH) {
                s_coasting = true;
                s_coast_iters = 0;
                LOG_INF("coast-to-stop started");
            }
            if (s_coasting) {
                s_coast_iters++;
                if (g_gnss.speed_kmh <= COAST_STOP_SPEED_KMH ||
                    s_coast_iters >= COAST_MAX_ITERATIONS) {
                    LOG_INF("coast-to-stop ended (spd=%.1f iter=%d)",
                            (double)g_gnss.speed_kmh, s_coast_iters);
                    s_coasting = false;
                }
            }

            previous_ignition = ignition;
            led_idle();

            /* state transition */
            if (ignition != 0 && !s_coasting) {
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
