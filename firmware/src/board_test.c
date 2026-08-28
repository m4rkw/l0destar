/*
 * Interactive board bring-up test (CONFIG_APP_BOARD_TEST, driven by
 * board_test.sh).  Runs instead of the main state machine and walks the
 * operator through every piece of fitted hardware in a fixed order:
 *
 *   1. GPS/AUX rail switching (left ON afterwards)
 *   2. OBD rail switching        (v3.1+ with an OBD interface fitted)
 *   3. Ignition sense            (3 on/off toggles)
 *   4. Ignition wake from sleep  (once)
 *   5. Battery voltage           (up >= 1 V, then down >= 1 V)
 *   6. Accelerometer             (live roll/pitch readout; continues on the
 *                                 first impact, reported with its metrics)
 *   7. Movement wake from sleep  (once)
 *   8. Raw GPS fix               (no A-GNSS assistance, coords optionally
 *                                 hidden via APP_BOARD_TEST_HIDE_COORDS)
 *   9. Modem + DNS               (needs SIM + APN; resolves www.google.com)
 *  10. K-line loopback           (kline_test(): TX -> transceiver -> RX)
 *  11. CAN loopback              (hw_can_selftest(): internal + external)
 *
 * A failed step is reported and the run continues; steps whose prerequisite
 * hardware failed are skipped (no fix attempt on a dead GPS rail, no OBD rail
 * test on a board without one, no modem test without SIM/APN, no loopbacks
 * on rails that never came up).  Nothing here touches the watchdog, so a
 * stalled step can sit at its prompt forever minus the per-step timeout.
 *
 * Bench-only code, kept out of production images by the Kconfig gate.
 */

#include <math.h>
#include <string.h>
#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>

#include <modem/lte_lc.h>

#include "app.h"
#include "hw_common.h"
#include "hw_domain.h"
#include "pins.h"

LOG_MODULE_REGISTER(board_test, CONFIG_APP_LOG_LEVEL);

/* -- result book-keeping ---------------------------------------------------- */

enum tres { R_PASS, R_FAIL, R_SKIP };

#define N_TESTS 11
static enum tres s_res[N_TESTS];
static const char *const s_name[N_TESTS] = {
	"GPS rail switching",
	"OBD rail switching",
	"ignition sense (3 toggles)",
	"ignition wake from sleep",
	"battery voltage up/down 1V",
	"accel tilt + impact",
	"movement wake from sleep",
	"raw GPS fix",
	"modem + DNS",
	"K-line loopback",
	"CAN loopback",
};

static void record(int idx, enum tres r)
{
	s_res[idx] = r;
	printk("[TEST %2d] %-28s %s\n", idx + 1, s_name[idx],
	       r == R_PASS ? "PASS" : r == R_FAIL ? "FAIL" : "SKIP");
}

/* -- start gate --------------------------------------------------------------
 * board_test.sh resets the target and then attaches screen; everything before
 * the operator's first keypress would otherwise scroll past unseen, so hold
 * here until any byte arrives on the console. */

#if DT_HAS_CHOSEN(zephyr_console)
static const struct device *const s_console =
	DEVICE_DT_GET_OR_NULL(DT_CHOSEN(zephyr_console));
#else
static const struct device *const s_console = NULL;
#endif

static void wait_start(void)
{
	unsigned char c;

	if (s_console == NULL || !device_is_ready(s_console)) {
		printk("no console input device -- starting in 5 s\n");
		k_msleep(5000);
		return;
	}
	while (uart_poll_in(s_console, &c) == 0) {
		/* drain anything stale so only a fresh keypress starts us */
	}
	printk("\n>>> press ENTER to start <<<\n");
	for (;;) {
		if (uart_poll_in(s_console, &c) == 0) {
			return;
		}
		k_msleep(20);
	}
}

/* -- shared wake plumbing (ignition edge + accel INT1) ---------------------- */

static K_SEM_DEFINE(s_evt_sem, 0, 1);
static struct gpio_callback s_ign_cb, s_acc_cb;
static bool s_ign_cb_in, s_acc_cb_in;
static atomic_t s_acc_flag;

static void ign_isr(const struct device *dev, struct gpio_callback *cb,
		    uint32_t pins)
{
	k_sem_give(&s_evt_sem);
}

static void acc_isr(const struct device *dev, struct gpio_callback *cb,
		    uint32_t pins)
{
	atomic_set(&s_acc_flag, 1);
	k_sem_give(&s_evt_sem);
}

static void ign_irq(bool on)
{
	if (on) {
		if (!s_ign_cb_in) {
			gpio_init_callback(&s_ign_cb, ign_isr,
					   BIT(PIN_IGN_SENSE));
			gpio_add_callback(hw_gpio0, &s_ign_cb);
			s_ign_cb_in = true;
		}
		gpio_pin_interrupt_configure(hw_gpio0, PIN_IGN_SENSE,
					     GPIO_INT_EDGE_BOTH);
	} else {
		gpio_pin_interrupt_configure(hw_gpio0, PIN_IGN_SENSE,
					     GPIO_INT_DISABLE);
	}
}

static void acc_irq(bool on)
{
	if (PIN_ACC_INT1 < 0) {
		return;
	}
	if (on) {
		if (!s_acc_cb_in) {
			gpio_pin_configure(hw_gpio0, PIN_ACC_INT1, GPIO_INPUT);
			gpio_init_callback(&s_acc_cb, acc_isr,
					   BIT(PIN_ACC_INT1));
			gpio_add_callback(hw_gpio0, &s_acc_cb);
			s_acc_cb_in = true;
		}
		gpio_pin_interrupt_configure(hw_gpio0, PIN_ACC_INT1,
					     GPIO_INT_EDGE_TO_ACTIVE);
	} else {
		gpio_pin_interrupt_configure(hw_gpio0, PIN_ACC_INT1,
					     GPIO_INT_DISABLE);
	}
}

/* -- rail-sense helpers (mirrors hw_selftest.c, verbose form) --------------- */

/* 12V K sense is inverted by its 2N7002 (low = rail up). */
static int lvl12(int up)
{
	return IS_ENABLED(CONFIG_APP_BOARD_RAIL_ST_12V_ACTIVE_LOW) ? !up : up;
}

/* Poll a sense pin until it reads `expect`; -1 pin = "no sense fitted" and
 * counts as success (there is nothing to check against). */
static int wait_sense(int pin, int expect, const char *what)
{
	if (pin < 0) {
		printk("        %s: no sense line fitted (visual check only)\n",
		       what);
		return 0;
	}
	for (int waited = 0;; waited += 5) {
		int v = gpio_pin_get(hw_gpio0, pin);
		if (v < 0) {
			printk("        %s: GPIO read error %d\n", what, v);
			return -EIO;
		}
		if (v == expect) {
			printk("        %s: OK\n", what);
			return 0;
		}
		if (waited >= 500) {
			printk("        %s: expected %d, still %d after %d ms\n",
			       what, expect, v, waited);
			return -EFAULT;
		}
		k_msleep(5);
	}
}

/* -- mini sleep (rails down, wake on whatever IRQ the caller armed) --------- */

static void rails_down(void)
{
	hw_power_shutdown();
	hw_domain_release(HW_DOMAIN_AUX, HW_DOMAIN_USER_MAIN);
}

static void rails_up(void)
{
	hw_aux_power_on();
	hw_power_wake();
}

/* -- small accel helpers ---------------------------------------------------- */

static bool accel_sample(float *ax_g, float *ay_g, float *az_g)
{
	int ax, ay, az;

	if (accel_read(&ax, &ay, &az)) {
		return false;
	}
	*ax_g = ax / 1000.0f;
	*ay_g = ay / 1000.0f;
	*az_g = az / 1000.0f;
	return true;
}

/* Wait for |a| to sit within +-80 mg of 1 g for 2 s.  Returns false only on
 * timeout (the caller proceeds anyway; a noisy bench is reported, not fatal). */
static bool wait_still(int timeout_s)
{
	int still = 0;

	for (int i = 0; i < timeout_s * 10; i++) {
		float x, y, z;

		if (!accel_sample(&x, &y, &z)) {
			return false;
		}
		float mag = sqrtf(x * x + y * y + z * z);

		still = (fabsf(mag - 1.0f) < 0.08f) ? still + 1 : 0;
		if (still >= 20) {
			return true;
		}
		k_msleep(100);
	}
	return false;
}

/* -- 1: GPS/AUX rail switching ---------------------------------------------- */

static bool test_gps_rail(void)
{
	bool ok = true;

	printk("\n=== TEST 1: GPS rail switching ===\n");

	/* Booted with AUX up; prove the sense agrees, then a full off/on. */
	ok &= wait_sense(PIN_GPS_RAIL_ST, 1, "PP3V3_GPS on (boot)") == 0;

	hw_domain_release(HW_DOMAIN_AUX, HW_DOMAIN_USER_MAIN);
	ok &= wait_sense(PIN_GPS_RAIL_ST, 0, "PP3V3_GPS off") == 0;

	hw_aux_power_on();
	if (!hw_domain_is_on(HW_DOMAIN_AUX)) {
		printk("        AUX domain refused to come back up\n");
		ok = false;
	}
	ok &= wait_sense(PIN_GPS_RAIL_ST, 1, "PP3V3_GPS back on") == 0;

	printk("        rail left ON\n");
	record(0, ok ? R_PASS : R_FAIL);
	return ok;
}

/* -- 2: OBD rail switching --------------------------------------------------- */

/* Returns 1 on pass, 0 on fail, -1 when there is no OBD rail to test — the
 * loopback tests (11/12) use it to skip when their rail is already known bad. */
static int test_obd_rails(void)
{
	printk("\n=== TEST 2: OBD rail switching ===\n");

	if (CONFIG_APP_OBD_MODE == 1) {
		bool ok = true;

		printk("        CAN fitted: cycling PP3V3_CAN\n");
		if (hw_domain_request(HW_DOMAIN_CAN, HW_DOMAIN_USER_MAIN)) {
			printk("        CAN rail did not come up\n");
			ok = false;
		} else {
			ok &= wait_sense(PIN_CAN_RAIL_ST, 1, "PP3V3_CAN on") == 0;
			hw_domain_release(HW_DOMAIN_CAN, HW_DOMAIN_USER_MAIN);
			ok &= wait_sense(PIN_CAN_RAIL_ST, 0, "PP3V3_CAN off") == 0;
		}
		record(1, ok ? R_PASS : R_FAIL);
		return ok ? 1 : 0;
	} else if (CONFIG_APP_OBD_MODE == 2) {
		bool ok = true;

		printk("        K-line fitted: cycling PP3V3_K + PP12V_K\n");
		if (hw_domain_request(HW_DOMAIN_K, HW_DOMAIN_USER_MAIN)) {
			printk("        K rails did not come up\n");
			ok = false;
		} else {
			ok &= wait_sense(PIN_K3V3_RAIL_ST, 1, "PP3V3_K on") == 0;
			ok &= wait_sense(PIN_K12V_RAIL_ST, lvl12(1),
					 "PP12V_K on") == 0;
			hw_domain_release(HW_DOMAIN_K, HW_DOMAIN_USER_MAIN);
			ok &= wait_sense(PIN_K3V3_RAIL_ST, 0, "PP3V3_K off") == 0;
			ok &= wait_sense(PIN_K12V_RAIL_ST, lvl12(0),
					 "PP12V_K off") == 0;
		}
		record(1, ok ? R_PASS : R_FAIL);
		return ok ? 1 : 0;
	}

	printk("        no OBD interface on this build\n");
	record(1, R_SKIP);
	return -1;
}

/* -- 3: ignition toggles ----------------------------------------------------- */

/* Debounced read: 5 consecutive identical 20 ms samples. */
static int ign_stable(void)
{
	int last = -2, run = 0;

	for (;;) {
		int v = ignition_read();   /* 0 = ON (active low) */

		run = (v == last) ? run + 1 : 1;
		last = v;
		if (run >= 5) {
			return v;
		}
		k_msleep(20);
	}
}

static void test_ignition(void)
{
	int state = ign_stable();
	int toggles = 0;
	int64_t t0 = k_uptime_get();

	printk("\n=== TEST 3: ignition sense ===\n");
	printk("        ignition is %s -- toggle on/off 3 times (180 s)\n",
	       state == 0 ? "ON" : "OFF");

	while (toggles < 6) {
		if (k_uptime_get() - t0 > 180 * 1000) {
			printk("        timeout after %d edge(s)\n", toggles);
			record(2, R_FAIL);
			return;
		}
		int v = ign_stable();

		if (v != state) {
			state = v;
			toggles++;
			printk("        ignition %s  (%d/6 edges)\n",
			       state == 0 ? "ON" : "OFF", toggles);
		}
		k_msleep(20);
	}
	record(2, R_PASS);
}

/* -- 4: ignition wake from sleep --------------------------------------------- */

static void test_ign_wake(void)
{
	printk("\n=== TEST 4: ignition wake from sleep ===\n");

	if (ign_stable() == 0) {
		printk("        turn ignition OFF to arm the test (60 s)\n");
		int64_t t0 = k_uptime_get();

		while (ign_stable() == 0) {
			if (k_uptime_get() - t0 > 60 * 1000) {
				printk("        ignition never went off\n");
				record(3, R_FAIL);
				return;
			}
			k_msleep(100);
		}
	}

	rails_down();
	k_sem_reset(&s_evt_sem);
	ign_irq(true);
	printk("        *** SLEEPING -- turn ignition ON to wake (180 s) ***\n");

	int err = k_sem_take(&s_evt_sem, K_SECONDS(180));

	ign_irq(false);
	rails_up();

	if (err) {
		printk("        no wake within timeout\n");
		record(3, R_FAIL);
		return;
	}
	k_msleep(200);   /* debounce */
	if (ignition_read() == 0) {
		printk("        woke on ignition edge, ignition reads ON\n");
		record(3, R_PASS);
	} else {
		printk("        woke, but ignition reads OFF (glitch?)\n");
		record(3, R_FAIL);
	}
}

/* -- 5: battery voltage up/down ---------------------------------------------- */

static void test_voltage(void)
{
	printk("\n=== TEST 5: battery voltage ===\n");

	if (!hw_power_available()) {
		printk("        INA228 not available\n");
		record(4, R_SKIP);
		return;
	}

	float base = 0;

	for (int i = 0; i < 3; i++) {
		base += battery_read_voltage();
		k_msleep(200);
	}
	base /= 3.0f;
	printk("        baseline %.2f V -- raise supply by >= 1 V, then lower by >= 1 V (300 s)\n",
	       (double)base);

	int64_t t0 = k_uptime_get();
	float high = base;
	bool up_done = false;

	for (;;) {
		if (k_uptime_get() - t0 > 300 * 1000) {
			printk("        timeout (%s)\n",
			       up_done ? "up seen, down not" : "no rise seen");
			record(4, R_FAIL);
			return;
		}
		float v = battery_read_voltage();

		printk("        %.2f V  [%s]\n", (double)v,
		       up_done ? "now lower >= 1 V" : "raise >= 1 V");

		if (!up_done) {
			if (v >= base + 1.0f) {
				up_done = true;
				high = v;
				printk("        UP OK (%.2f V)\n", (double)v);
			}
		} else {
			if (v > high) {
				high = v;
			}
			if (v <= high - 1.0f) {
				printk("        DOWN OK (%.2f V)\n", (double)v);
				record(4, R_PASS);
				return;
			}
		}
		k_msleep(1000);
	}
}

/* -- 6: accelerometer, live tilt + impact -------------------------------------
 * No attitude target to hit: roll/pitch are streamed so the operator can wave
 * the board around and watch the readout follow it.  The impact interrupt is
 * armed for the whole readout, so desk bangs are reported with their metrics
 * as they happen, and one impact is required to move on -- that is what
 * proves the INT1 path and the FIFO capture, which tilt alone does not. */

static void test_accel(void)
{
	printk("\n=== TEST 6: accelerometer (tilt + impact) ===\n");

	if (!accel_available()) {
		printk("        accelerometer not available\n");
		record(5, R_SKIP);
		return;
	}

	printk("        place the board flat and still...\n");
	if (!wait_still(30)) {
		printk("        never settled -- calibrating anyway\n");
	}

	float bx = 0, by = 0, bz = 0;

	for (int i = 0; i < 10; i++) {
		float x, y, z;

		if (accel_sample(&x, &y, &z)) {
			bx += x; by += y; bz += z;
		}
		k_msleep(50);
	}
	bx /= 10; by /= 10; bz /= 10;

	float r0 = atan2f(by, bz) * 57.2958f;
	float p0 = atan2f(-bx, sqrtf(by * by + bz * bz)) * 57.2958f;

	printk("        calibrated flat (roll %.1f, pitch %.1f)\n",
	       (double)r0, (double)p0);

	atomic_clear(&s_acc_flag);
	accel_fifo_enable();
	accel_crash_int_enable(CRASH_THRESHOLD_MG);
	acc_irq(true);

	printk("        tilt and rotate the board -- the angles below should\n"
	       "        follow it.  Then BANG THE DESK at least once (impact\n"
	       "        threshold %d mg); the test continues on the first\n"
	       "        impact detected (300 s).\n", CRASH_THRESHOLD_MG);

	int hits = 0;
	int64_t t0 = k_uptime_get();
	int64_t last_print = 0;
	float lr = 1e6f, lp = 1e6f;   /* forces the first line out */

	while (hits == 0) {
		if (k_uptime_get() - t0 > 300 * 1000) {
			printk("        timeout -- no impact detected\n");
			break;
		}

		float x, y, z;

		if (accel_sample(&x, &y, &z)) {
			float roll  = atan2f(y, z) * 57.2958f - r0;
			float pitch = atan2f(-x, sqrtf(y * y + z * z)) * 57.2958f
				      - p0;
			float mag = sqrtf(x * x + y * y + z * z);
			int64_t now = k_uptime_get();

			/* Print on movement, plus a 2 s heartbeat so a board
			 * left still still shows the readout is alive. */
			if (fabsf(roll - lr) > 2.0f || fabsf(pitch - lp) > 2.0f
			    || now - last_print > 2000) {
				printk("        roll %6.1f  pitch %6.1f  "
				       "|a| %.2f g\n",
				       (double)roll, (double)pitch, (double)mag);
				lr = roll;
				lp = pitch;
				last_print = now;
			}
		}

		if (!atomic_clear(&s_acc_flag)) {
			k_msleep(100);
			continue;
		}
		k_msleep(400);   /* let the FIFO ring capture the tail */

		uint8_t src = 0;

		accel_read_wake_src(&src);

		struct accel_impact imp;

		if (accel_fifo_drain_impact(&imp) == 0 && imp.samples > 0) {
			hits++;
			printk("        IMPACT: peak %d.%02d g "
			       "(x=%d y=%d z=%d mg)  gyro %d.%d dps  "
			       "duration %d ms  [src 0x%02x]\n",
			       imp.peak_mg / 1000, (imp.peak_mg % 1000) / 10,
			       imp.pax, imp.pay, imp.paz,
			       imp.peak_gyro_dps10 / 10,
			       imp.peak_gyro_dps10 % 10,
			       imp.over_ms, src);
		} else {
			printk("        interrupt but no FIFO data (src 0x%02x)"
			       " -- hit harder?\n", src);
		}
		/* Angles are re-baselined against the last print, not the
		 * bang, so the readout resumes cleanly if we loop again. */
		lr = 1e6f;
		lp = 1e6f;
	}

	acc_irq(false);
	accel_crash_int_disable();
	accel_fifo_disable();
	record(5, hits ? R_PASS : R_FAIL);
}

/* -- 7: movement wake from sleep ---------------------------------------------- */

static void test_move_wake(void)
{
	printk("\n=== TEST 7: movement wake from sleep ===\n");

	if (!accel_available()) {
		printk("        accelerometer not available\n");
		record(6, R_SKIP);
		return;
	}

	printk("        leave the board still...\n");
	wait_still(15);

	rails_down();
	atomic_clear(&s_acc_flag);
	k_sem_reset(&s_evt_sem);
	accel_enable_wake_int();
	acc_irq(true);

	printk("        *** SLEEPING -- move or tap the board to wake (180 s) ***\n");

	int err = k_sem_take(&s_evt_sem, K_SECONDS(180));
	uint8_t src = 0;

	if (!err) {
		accel_read_wake_src(&src);
	}
	acc_irq(false);
	accel_disable_wake_int();
	rails_up();

	if (err) {
		printk("        no wake within timeout\n");
		record(6, R_FAIL);
	} else {
		printk("        woke on movement (wake src 0x%02x)\n", src);
		record(6, R_PASS);
	}
}

/* -- 8: raw GPS fix ------------------------------------------------------------ */

static void test_gps_fix(bool rail_ok, bool modem_ok)
{
	printk("\n=== TEST 8: raw GPS fix (no assistance) ===\n");

	if (!rail_ok) {
		printk("        GPS rail failed earlier -- no point searching\n");
		record(7, R_SKIP);
		return;
	}
	if (!modem_ok) {
		printk("        modem library init failed -- GNSS unavailable\n");
		record(7, R_SKIP);
		return;
	}

	/* GNSS-only functional mode: no LTE, so no clock, no A-GNSS, nothing
	 * assisting -- exactly the cold unaided fix this test wants.  agnss.c
	 * is never initialised in this build.  A factory-fresh modem may not
	 * have GNSS in %XSYSTEMMODE yet, so set it first (only settable while
	 * the modem is offline, which it still is here). */
	int err = lte_lc_system_mode_set(LTE_LC_SYSTEM_MODE_LTEM_NBIOT_GPS,
					 LTE_LC_SYSTEM_MODE_PREFER_LTEM);

	if (err) {
		printk("        system mode set failed: %d (continuing)\n", err);
	}
	err = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_ACTIVATE_GNSS);

	if (err) {
		printk("        GNSS activate failed: %d\n", err);
		record(7, R_FAIL);
		return;
	}
	if (gnss_init() || gnss_start()) {
		printk("        GNSS start failed\n");
		record(7, R_FAIL);
		return;
	}

	memset(&g_gnss, 0, sizeof(g_gnss));

	int timeout_s = GPS_COLD_FIX_TIMEOUT_MS / 1000;

	printk("        searching (cold, unaided, up to %d s)...\n", timeout_s);

	for (int t = 0; t < timeout_s; t++) {
		if (g_gnss.valid) {
			break;
		}
		if (t && (t % 15) == 0) {
			printk("        still searching (%d s)\n", t);
		}
		k_msleep(1000);
	}
	gnss_stop();

	if (!g_gnss.valid) {
		printk("        TIMEOUT -- no fix after %d s\n", timeout_s);
		record(7, R_FAIL);
		return;
	}

	if (IS_ENABLED(CONFIG_APP_BOARD_TEST_HIDE_COORDS)) {
		printk("        FIX OK: sats=%ld hdop=%ld.%ld alt=%.0fm "
		       "time=%s (coords hidden)\n",
		       g_gnss.sats, g_gnss.hdop_x10 / 10, g_gnss.hdop_x10 % 10,
		       (double)g_gnss.altitude_m, g_gnss.time_iso);
	} else {
		printk("        FIX OK: %s,%s  sats=%ld hdop=%ld.%ld alt=%.0fm"
		       " time=%s\n",
		       g_gnss.lat_str, g_gnss.lon_str,
		       g_gnss.sats, g_gnss.hdop_x10 / 10, g_gnss.hdop_x10 % 10,
		       (double)g_gnss.altitude_m, g_gnss.time_iso);
	}
	record(7, R_PASS);
}

/* -- 9: modem + DNS ------------------------------------------------------------ */

static void test_modem(bool modem_ok)
{
	printk("\n=== TEST 9: modem + DNS ===\n");

	if (IS_ENABLED(CONFIG_APP_BOARD_TEST_SKIP_MODEM)) {
		printk("        no APN configured (skipped at build time)\n");
		record(8, R_SKIP);
		return;
	}
	if (!modem_ok) {
		printk("        modem library init failed\n");
		record(8, R_SKIP);
		return;
	}

	/* SIM presence: power the UICC and ask for the IMSI.  The UICC can
	 * take a moment after activation, so give it a few tries before
	 * declaring the slot empty. */
	lte_lc_func_mode_set(LTE_LC_FUNC_MODE_ACTIVATE_UICC);

	char resp[64] = {0};
	bool sim_ok = false;

	for (int i = 0; i < 5 && !sim_ok; i++) {
		k_msleep(1000);
		sim_ok = modem_at("AT+CIMI", resp, sizeof(resp)) == 0 &&
			 resp[0] >= '0' && resp[0] <= '9';
	}
	if (!sim_ok) {
		printk("        no SIM detected (AT+CIMI: %.20s)\n", resp);
		record(8, R_SKIP);
		return;
	}
	printk("        SIM OK, connecting with APN \"%s\" (can take 30 s+)...\n",
	       g_settings.apn);

	if (modem_connect()) {
		printk("        LTE registration failed\n");
		record(8, R_FAIL);
		return;
	}
	printk("        registered (%s) -- resolving www.google.com\n",
	       modem_rat());

	struct zsock_addrinfo hints = {
		.ai_family   = AF_INET,
		.ai_socktype = SOCK_DGRAM,
	};
	struct zsock_addrinfo *res = NULL;
	int err = zsock_getaddrinfo("www.google.com", NULL, &hints, &res);

	if (err || !res) {
		printk("        DNS FAILED: %d\n", err);
		record(8, R_FAIL);
		return;
	}

	char ip[INET_ADDRSTRLEN] = "?";

	zsock_inet_ntop(AF_INET,
			&((struct sockaddr_in *)res->ai_addr)->sin_addr,
			ip, sizeof(ip));
	zsock_freeaddrinfo(res);
	printk("        DNS OK: www.google.com -> %s\n", ip);
	record(8, R_PASS);
}

/* -- 10: K-line loopback -------------------------------------------------------- */

static void test_kline_loop(int obd_res)
{
	printk("\n=== TEST 10: K-line loopback ===\n");

	if (!IS_ENABLED(CONFIG_APP_BOARD_HAS_KLINE) || PIN_K1_TX < 0) {
		printk("        no K-line interface on this build\n");
		record(9, R_SKIP);
		return;
	}
	if (IS_ENABLED(CONFIG_APP_BOARD_SPLIT_OBD_DOMAIN) && obd_res == 0) {
		printk("        K rails failed earlier -- no point driving them\n");
		record(9, R_SKIP);
		return;
	}
	/* kline_test(): powers the K rails, static TX->RX check through the
	 * transceiver, a 256-byte stream over the loopback path (both
	 * transceivers on the bench build), the L-line FET where fitted,
	 * then powers back down. */
	record(9, kline_test() == 0 ? R_PASS : R_FAIL);
}

/* -- 11: CAN loopback ------------------------------------------------------------ */

static void test_can_loop(int obd_res)
{
	printk("\n=== TEST 11: CAN loopback ===\n");

	if (!IS_ENABLED(CONFIG_APP_BOARD_HAS_CAN) || PIN_CAN_SCK < 0) {
		printk("        no CAN interface on this build\n");
		record(10, R_SKIP);
		return;
	}
	if (IS_ENABLED(CONFIG_APP_BOARD_SPLIT_OBD_DOMAIN) && obd_res == 0) {
		printk("        CAN rail failed earlier -- no point driving it\n");
		record(10, R_SKIP);
		return;
	}
	if (!hw_can_available()) {
		/* Boot-time probe failed; the rail has been cycled since, so
		 * give the controller one more chance before calling it dead. */
		printk("        MCP2518FD failed boot init -- retrying probe\n");
		hw_can_init();
	}
	if (!hw_can_available()) {
		printk("        MCP2518FD not responding\n");
		record(10, R_FAIL);
		return;
	}
	record(10, hw_can_selftest() == 0 ? R_PASS : R_FAIL);
}

/* -- driver --------------------------------------------------------------------- */

void board_test_run(void)
{
	printk("\n");
	printk("==============================================\n");
	printk("  l0destar BOARD TEST -- board %s, fw %s\n",
	       fota_board_id(), fota_version());
	printk("==============================================\n");

	wait_start();

	for (int i = 0; i < N_TESTS; i++) {
		s_res[i] = R_SKIP;
	}

	bool gps_rail_ok = test_gps_rail();
	int obd_res = test_obd_rails();

	test_ignition();
	test_ign_wake();
	test_voltage();
	test_accel();
	test_move_wake();

	bool modem_ok = (modem_init() == 0);

	test_gps_fix(gps_rail_ok, modem_ok);
	test_modem(modem_ok);
	test_kline_loop(obd_res);
	test_can_loop(obd_res);

	int pass = 0, fail = 0, skip = 0;

	printk("\n==============================================\n");
	printk("  BOARD TEST SUMMARY\n");
	printk("==============================================\n");
	for (int i = 0; i < N_TESTS; i++) {
		printk("  %2d. %-28s %s\n", i + 1, s_name[i],
		       s_res[i] == R_PASS ? "PASS" :
		       s_res[i] == R_FAIL ? "FAIL" : "SKIP");
		if (s_res[i] == R_PASS) pass++;
		else if (s_res[i] == R_FAIL) fail++;
		else skip++;
	}
	printk("----------------------------------------------\n");
	printk("  %d pass, %d fail, %d skip -- %s\n",
	       pass, fail, skip, fail ? "BOARD NOT OK" : "BOARD OK");
	printk("==============================================\n");

	/* Slow blink forever: distinguishable from a hung boot at a glance. */
	for (;;) {
		led_toggle();
		k_msleep(fail ? 200 : 1000);
	}
}
