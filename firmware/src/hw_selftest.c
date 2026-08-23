/*
 * Boot-time switched-rail self-test (v3.1+, needs the rail-sense inputs).
 *
 * hw_domain_request() already verifies that a rail comes UP before it lets
 * anything drive into the domain, and alerts if it does not.  What it cannot
 * check on its own is the opposite fault — a load switch that stays on, or a
 * sense line stuck at the "rail present" level — because that only shows up
 * after the enable is dropped.  This runs each fitted domain through one
 * on/off cycle at boot and reports what the request path would not.
 *
 * Timing note: none of these rails is actively discharged, so the fall time
 * is set by the load.  PP3V3_CAN carries 10.2 uF and drains mainly through
 * the parked CAN_CS/CAN_INT pulldowns in series with their 10K pull-ups once
 * the MCP2518FD and MAX33041 drop out of regulation (~130 ms); PP12V_K is
 * ~100 nF against the 280K sense divider plus the TJA1027T's sleep current
 * (tens of ms).  The off-check therefore polls with a generous timeout
 * instead of sampling once after a fixed settle — a fixed 20 ms sample
 * reports a healthy board as "stuck".
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "app.h"
#include "hw_common.h"
#include "hw_domain.h"
#include "pins.h"

LOG_MODULE_REGISTER(hw_selftest, CONFIG_APP_LOG_LEVEL);

#if IS_ENABLED(CONFIG_APP_BOARD_HAS_RAIL_SENSE)

#define RAIL_OFF_TIMEOUT_MS 500
#define RAIL_POLL_MS        5

/* Rail-status polarity: the 3.3V senses are plain dividers off their rail
 * (high = up); the 12V sense is inverted by a 2N7002 with a pull-up to the
 * always-on rail (low = up).  rail_level() takes the logical "rail should be
 * up" state and applies the inversion. */
static int rail_level(int up)
{
	return IS_ENABLED(CONFIG_APP_BOARD_RAIL_ST_12V_ACTIVE_LOW) ? !up : up;
}

/* Poll one sense line until it reads `expect`.  Returns 0 on success. */
static int wait_rail(int pin, int expect, const char *label)
{
	if (pin < 0) {
		return 0;
	}
	for (int waited = 0;; waited += RAIL_POLL_MS) {
		int val = gpio_pin_get(hw_gpio0, pin);
		if (val < 0) {
			LOG_ERR("%s: gpio read error %d", label, val);
			return -EIO;
		}
		if (val == expect) {
			LOG_INF("%s: OK", label);
			return 0;
		}
		if (waited >= RAIL_OFF_TIMEOUT_MS) {
			LOG_ERR("%s: expected %d, still %d after %d ms",
				label, expect, val, waited);
			return -EFAULT;
		}
		k_msleep(RAIL_POLL_MS);
	}
}

int hw_selftest(void)
{
	int fails = 0;

	LOG_INF("=== power rail self-test ===");

	/* The GPS/AUX domain is already up (hw_aux_power_on at boot) and is
	 * held there for the whole session, so only its present state can be
	 * checked — powering it down here would drop the bias tee.  A failure
	 * to come up was already logged and alerted by hw_domain_request(),
	 * so don't spend a second slot in the alert queue on it. */
	if (!hw_domain_is_on(HW_DOMAIN_AUX)) {
		LOG_ERR("PP3V3_GPS: domain off (rail fault at boot)");
		fails++;
	} else if (wait_rail(PIN_GPS_RAIL_ST, 1, "PP3V3_GPS")) {
		alert_enqueue("SELFTEST:GPS rail fail", 1);
		fails++;
	}

	if (CONFIG_APP_OBD_MODE == 1) {
		/* CAN: PP3V3_CAN only. */
		if (hw_domain_request(HW_DOMAIN_CAN, HW_DOMAIN_USER_MAIN)) {
			fails++;   /* logged and alerted by hw_domain */
		} else {
			hw_domain_release(HW_DOMAIN_CAN, HW_DOMAIN_USER_MAIN);
			if (wait_rail(PIN_CAN_RAIL_ST, 0, "PP3V3_CAN off")) {
				alert_enqueue("SELFTEST:CAN 3V3 stuck", 1);
				fails++;
			}
		}
	} else if (CONFIG_APP_OBD_MODE == 2) {
		/* K-line: PP3V3_K and PP12V_K share K_EN — hw_domain_request
		 * only reports success when both sense lines confirm. */
		if (hw_domain_request(HW_DOMAIN_K, HW_DOMAIN_USER_MAIN)) {
			fails++;
		} else {
			hw_domain_release(HW_DOMAIN_K, HW_DOMAIN_USER_MAIN);
			if (wait_rail(PIN_K3V3_RAIL_ST, 0, "PP3V3_K off")) {
				alert_enqueue("SELFTEST:K 3V3 stuck", 1);
				fails++;
			}
			if (wait_rail(PIN_K12V_RAIL_ST, rail_level(0),
				      "PP12V_K off")) {
				alert_enqueue("SELFTEST:K 12V stuck", 1);
				fails++;
			}
		}
	}

	if (fails) {
		LOG_ERR("self-test: %d rail failure(s)", fails);
	} else {
		LOG_INF("self-test: all rails OK");
	}
	return fails ? -EFAULT : 0;
}

#else

int hw_selftest(void)
{
	return 0;
}

#endif
