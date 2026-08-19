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

	if (CONFIG_APP_OBD_MODE > 0) {
		hw_domain_request(HW_DOMAIN_OBD, HW_DOMAIN_USER_MAIN);
		k_msleep(SETTLE_MS);

		if (check_rail(PIN_OBD3V3_RAIL_ST, 1, "PP3V3_OBD on")) {
			alert_enqueue("SELFTEST:OBD 3V3 fail", 1);
			fails++;
		}
		if (CONFIG_APP_OBD_MODE == 2 &&
		    check_rail(PIN_OBD12V_RAIL_ST, 1, "PP12V_OBD on")) {
			alert_enqueue("SELFTEST:OBD 12V fail", 1);
			fails++;
		}

		hw_domain_release(HW_DOMAIN_OBD, HW_DOMAIN_USER_MAIN);
		k_msleep(SETTLE_MS);

		if (check_rail(PIN_OBD3V3_RAIL_ST, 0, "PP3V3_OBD off")) {
			alert_enqueue("SELFTEST:OBD 3V3 stuck", 1);
			fails++;
		}
		if (CONFIG_APP_OBD_MODE == 2 &&
		    check_rail(PIN_OBD12V_RAIL_ST, 0, "PP12V_OBD off")) {
			alert_enqueue("SELFTEST:OBD 12V stuck", 1);
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
