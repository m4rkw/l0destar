#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "app.h"
#include "hw_common.h"
#include "pins.h"

LOG_MODULE_REGISTER(hw_relay, CONFIG_APP_LOG_LEVEL);

#if RELAY_CONNECTED

static bool s_connected = true;

int relay_init(void)
{
	if (PIN_RLY_SET < 0 || PIN_RLY_RST < 0) {
		LOG_INF("no relay on this board");
		s_connected = false;
		return 0;
	}
	gpio_pin_configure(hw_gpio0, PIN_RLY_SET, GPIO_OUTPUT_LOW);
	gpio_pin_configure(hw_gpio0, PIN_RLY_RST, GPIO_OUTPUT_LOW);
	gpio_pin_configure(hw_gpio0, PIN_RLY_SET_FB, GPIO_INPUT);
	gpio_pin_configure(hw_gpio0, PIN_RLY_RST_FB, GPIO_INPUT);
	int set_fb = gpio_pin_get(hw_gpio0, PIN_RLY_SET_FB);
	int rst_fb = gpio_pin_get(hw_gpio0, PIN_RLY_RST_FB);
	LOG_INF("relay init: SET_FB=%d RST_FB=%d", set_fb, rst_fb);
	if (set_fb == 0 && rst_fb == 0) {
		LOG_WRN("relay not detected (both FB low)");
		s_connected = false;
	}
	return 0;
}

int relay_set(void)
{
	if (!s_connected) return 0;
	if (gpio_pin_get(hw_gpio0, PIN_RLY_SET_FB) == 1 &&
	    gpio_pin_get(hw_gpio0, PIN_RLY_RST_FB) == 0) {
		return 0;
	}

	gpio_pin_set(hw_gpio0, PIN_RLY_SET, 1);
	k_msleep(350);
	gpio_pin_set(hw_gpio0, PIN_RLY_SET, 0);
	k_msleep(150);

	int set_fb = gpio_pin_get(hw_gpio0, PIN_RLY_SET_FB);
	int rst_fb = gpio_pin_get(hw_gpio0, PIN_RLY_RST_FB);
	LOG_INF("relay SET: SET_FB=%d RST_FB=%d", set_fb, rst_fb);

	if (set_fb != 1 || rst_fb != 0) {
		LOG_ERR("relay SET feedback mismatch");
		return -EIO;
	}
	return 0;
}

bool relay_available(void)
{
	return s_connected;
}

int relay_reset(void)
{
	if (!s_connected) return 0;
	if (gpio_pin_get(hw_gpio0, PIN_RLY_SET_FB) == 0 &&
	    gpio_pin_get(hw_gpio0, PIN_RLY_RST_FB) == 1) {
		return 0;
	}

	gpio_pin_set(hw_gpio0, PIN_RLY_RST, 1);
	k_msleep(350);
	gpio_pin_set(hw_gpio0, PIN_RLY_RST, 0);
	k_msleep(150);

	int set_fb = gpio_pin_get(hw_gpio0, PIN_RLY_SET_FB);
	int rst_fb = gpio_pin_get(hw_gpio0, PIN_RLY_RST_FB);
	LOG_INF("relay RST: SET_FB=%d RST_FB=%d", set_fb, rst_fb);

	if (set_fb != 0 || rst_fb != 1) {
		LOG_ERR("relay RST feedback mismatch");
		return -EIO;
	}
	return 0;
}

#else /* !RELAY_CONNECTED — no relay on this board; pins reused for I2C/INA */

int  relay_init(void)      { return 0; }
int  relay_set(void)       { return 0; }
int  relay_reset(void)     { return 0; }
bool relay_available(void) { return false; }

#endif /* RELAY_CONNECTED */
