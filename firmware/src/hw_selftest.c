#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "app.h"
#include "hw_common.h"
#include "hw_domain.h"
#include "pins.h"

LOG_MODULE_REGISTER(hw_selftest, CONFIG_APP_LOG_LEVEL);

#define SETTLE_MS 20

#if IS_ENABLED(CONFIG_APP_BOARD_HAS_RAIL_SENSE)

/* Rail-status polarity: the 3.3V senses are plain dividers off their rail
 * (high = up); the 12V sense is inverted by a 2N7002 with a pull-up to the
 * always-on rail (low = up).  check_rail() takes the logical "rail should be
 * up" state and applies the inversion. */
static int rail_level(int up)
{
	return IS_ENABLED(CONFIG_APP_BOARD_RAIL_ST_12V_ACTIVE_LOW) ? !up : up;
}

static int check_rail(int pin, int expect, const char *label)
{
	if (pin < 0) {
		return 0;
	}
	int val = gpio_pin_get(hw_gpio0, pin);
	if (val < 0) {
		LOG_ERR("%s: gpio read error %d", label, val);
		return -EIO;
	}
	if (val != expect) {
		LOG_ERR("%s: expected %d, read %d", label, expect, val);
		return -EFAULT;
	}
	LOG_INF("%s: OK", label);
	return 0;
}

int hw_selftest(void)
{
	int fails = 0;

	LOG_INF("=== power rail self-test ===");

	/* GPS domain is already enabled (hw_aux_power_on at boot). */
	k_msleep(SETTLE_MS);
	if (check_rail(PIN_GPS_RAIL_ST, 1, "PP3V3_GPS")) {
		alert_enqueue("SELFTEST:GPS rail fail", 1);
		fails++;
	}

	if (CONFIG_APP_OBD_MODE == 1) {
		/* CAN: PP3V3_CAN only. */
		hw_domain_request(HW_DOMAIN_CAN, HW_DOMAIN_USER_MAIN);
		k_msleep(SETTLE_MS);
		if (check_rail(PIN_CAN_RAIL_ST, 1, "PP3V3_CAN on")) {
			alert_enqueue("SELFTEST:CAN 3V3 fail", 1);
			fails++;
		}

		hw_domain_release(HW_DOMAIN_CAN, HW_DOMAIN_USER_MAIN);
		k_msleep(SETTLE_MS);
		if (check_rail(PIN_CAN_RAIL_ST, 0, "PP3V3_CAN off")) {
			alert_enqueue("SELFTEST:CAN 3V3 stuck", 1);
			fails++;
		}
	} else if (CONFIG_APP_OBD_MODE == 2) {
		/* K-line: PP3V3_K and PP12V_K share K_EN. */
		hw_domain_request(HW_DOMAIN_K, HW_DOMAIN_USER_MAIN);
		k_msleep(SETTLE_MS);
		if (check_rail(PIN_K3V3_RAIL_ST, 1, "PP3V3_K on")) {
			alert_enqueue("SELFTEST:K 3V3 fail", 1);
			fails++;
		}
		if (check_rail(PIN_K12V_RAIL_ST, rail_level(1), "PP12V_K on")) {
			alert_enqueue("SELFTEST:K 12V fail", 1);
			fails++;
		}

		hw_domain_release(HW_DOMAIN_K, HW_DOMAIN_USER_MAIN);
		k_msleep(SETTLE_MS);
		if (check_rail(PIN_K3V3_RAIL_ST, 0, "PP3V3_K off")) {
			alert_enqueue("SELFTEST:K 3V3 stuck", 1);
			fails++;
		}
		if (check_rail(PIN_K12V_RAIL_ST, rail_level(0), "PP12V_K off")) {
			alert_enqueue("SELFTEST:K 12V stuck", 1);
			fails++;
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
