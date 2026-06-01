/*
 * Status LED.  Uses the board's led0 alias (LED1 on the nRF9151 DK).
 * Was active-low on the Polaris; here we let the DTS handle the polarity.
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "app.h"

LOG_MODULE_REGISTER(led, CONFIG_APP_LOG_LEVEL);

#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

static int led_init_once(void)
{
    if (!gpio_is_ready_dt(&led)) {
        LOG_ERR("LED not ready");
        return -ENODEV;
    }
    gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
    return 0;
}
SYS_INIT(led_init_once, APPLICATION, 50);

void led_on(void)     { gpio_pin_set_dt(&led, 1); }
void led_off(void)    { gpio_pin_set_dt(&led, 0); }
void led_toggle(void) { gpio_pin_toggle_dt(&led); }

void status_delay(long ms)
{
    /* Break long sleeps into chunks so the watchdog gets refreshed. */
    while (ms > 1000) {
        k_msleep(1000);
        watchdog_kick();
        ms -= 1000;
    }
    if (ms > 0) {
        k_msleep(ms);
        watchdog_kick();
    }
}
