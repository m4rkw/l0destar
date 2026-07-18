/*
 * Status LEDs.
 *
 * When APP_PIN_LED1/2/3 are set (makerdiary bench), three raw-GPIO LEDs are
 * driven with timer-based flash and animation patterns.  When they are -1
 * (production / DK), falls back to the single DT led0 alias and the multi-LED
 * calls are no-ops.
 *
 * Boot:  1→2→3→2→1 bounce until first GPS fix.
 * Run:   LED1 on, LED2 flash/solid for GPS, LED3 flash/solid for send.
 * Sleep: 3→2→1 sweep during shutdown, then all off.
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "app.h"
#include "pins.h"

LOG_MODULE_REGISTER(led, CONFIG_APP_LOG_LEVEL);

/* --- DT led0 fallback (DK build) ----------------------------------------- */

#define LED0_NODE DT_ALIAS(led0)

#if DT_NODE_HAS_STATUS(LED0_NODE, okay)
#define HAS_DT_LED0 1
static const struct gpio_dt_spec dt_led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);
#else
#define HAS_DT_LED0 0
#endif

/* --- multi-LED state ----------------------------------------------------- */

static const struct device *s_gpio;
static bool s_multi;

/* --- timer callbacks (ISR context) --------------------------------------- */

/* Boot bounce: 1→2→3→2→1→… */
static int s_bounce_step;
static void boot_anim_fn(struct k_timer *t)
{
	static const int seq[] = {0, 1, 2, 1};
	int active = seq[s_bounce_step & 3];
	gpio_pin_set(s_gpio, PIN_LED1, active == 0);
	if (PIN_LED2 >= 0) gpio_pin_set(s_gpio, PIN_LED2, active == 1);
	if (PIN_LED3 >= 0) gpio_pin_set(s_gpio, PIN_LED3, active == 2);
	s_bounce_step++;
}
K_TIMER_DEFINE(s_boot_timer, boot_anim_fn, NULL);

/* LED2 flash (GPS searching) */
static void led2_flash_fn(struct k_timer *t)
{
	gpio_pin_toggle(s_gpio, PIN_LED2);
}
K_TIMER_DEFINE(s_led2_timer, led2_flash_fn, NULL);

/* LED3 flash (sending) */
static void led3_flash_fn(struct k_timer *t)
{
	gpio_pin_toggle(s_gpio, PIN_LED3);
}
K_TIMER_DEFINE(s_led3_timer, led3_flash_fn, NULL);

/* Sleep sweep: 3→2→1, one at a time */
static int s_anim_step;
static void sleep_anim_fn(struct k_timer *t)
{
	if (PIN_LED1 >= 0) gpio_pin_set(s_gpio, PIN_LED1, s_anim_step == 2);
	if (PIN_LED2 >= 0) gpio_pin_set(s_gpio, PIN_LED2, s_anim_step == 1);
	if (PIN_LED3 >= 0) gpio_pin_set(s_gpio, PIN_LED3, s_anim_step == 0);
	s_anim_step = (s_anim_step + 1) % 3;
}
K_TIMER_DEFINE(s_sleep_timer, sleep_anim_fn, NULL);

static void stop_all_timers(void)
{
	k_timer_stop(&s_boot_timer);
	k_timer_stop(&s_led2_timer);
	k_timer_stop(&s_led3_timer);
	k_timer_stop(&s_sleep_timer);
}

/* --- init ---------------------------------------------------------------- */

static int led_init_once(void)
{
	if (PIN_LED1 >= 0) {
		s_gpio = DEVICE_DT_GET(DT_NODELABEL(gpio0));
		if (!device_is_ready(s_gpio)) {
			LOG_ERR("gpio0 not ready");
			return -ENODEV;
		}
		s_multi = true;

		gpio_flags_t flags = GPIO_OUTPUT_INACTIVE;
		if (IS_ENABLED(CONFIG_APP_LED_ACTIVE_LOW))
			flags |= GPIO_ACTIVE_LOW;

		gpio_pin_configure(s_gpio, PIN_LED1, flags);
		if (PIN_LED2 >= 0) gpio_pin_configure(s_gpio, PIN_LED2, flags);
		if (PIN_LED3 >= 0) gpio_pin_configure(s_gpio, PIN_LED3, flags);
		return 0;
	}

#if HAS_DT_LED0
	if (!gpio_is_ready_dt(&dt_led)) {
		LOG_ERR("LED not ready");
		return -ENODEV;
	}
	gpio_pin_configure_dt(&dt_led, GPIO_OUTPUT_INACTIVE);
#endif
	return 0;
}
SYS_INIT(led_init_once, APPLICATION, 50);

/* --- single-LED backward-compat ------------------------------------------ */

void led_on(void)
{
	if (s_multi)
		gpio_pin_set(s_gpio, PIN_LED1, 1);
#if HAS_DT_LED0
	else
		gpio_pin_set_dt(&dt_led, 1);
#endif
}

void led_off(void)
{
	if (s_multi)
		gpio_pin_set(s_gpio, PIN_LED1, 0);
#if HAS_DT_LED0
	else
		gpio_pin_set_dt(&dt_led, 0);
#endif
}

void led_toggle(void)
{
	if (s_multi)
		gpio_pin_toggle(s_gpio, PIN_LED1);
#if HAS_DT_LED0
	else
		gpio_pin_toggle_dt(&dt_led);
#endif
}

/* --- multi-LED status API ------------------------------------------------ */

void led_boot_animation(void)
{
	if (!s_multi) { led_on(); return; }
	stop_all_timers();
	s_bounce_step = 0;
	k_timer_start(&s_boot_timer, K_NO_WAIT, K_MSEC(150));
}

void led_gps_searching(void)
{
	if (!s_multi || PIN_LED2 < 0) return;
	k_timer_stop(&s_boot_timer);
	gpio_pin_set(s_gpio, PIN_LED1, 1);
	if (PIN_LED3 >= 0) gpio_pin_set(s_gpio, PIN_LED3, 0);
	gpio_pin_set(s_gpio, PIN_LED2, 0);
	k_timer_start(&s_led2_timer, K_MSEC(250), K_MSEC(250));
}

void led_gps_fixed(void)
{
	if (!s_multi || PIN_LED2 < 0) return;
	k_timer_stop(&s_led2_timer);
	gpio_pin_set(s_gpio, PIN_LED2, 1);
}

void led_sending(void)
{
	if (!s_multi || PIN_LED3 < 0) return;
	gpio_pin_set(s_gpio, PIN_LED3, 0);
	k_timer_start(&s_led3_timer, K_MSEC(250), K_MSEC(250));
}

void led_sent(void)
{
	if (!s_multi || PIN_LED3 < 0) return;
	k_timer_stop(&s_led3_timer);
	gpio_pin_set(s_gpio, PIN_LED3, 1);
}

void led_idle(void)
{
	if (!s_multi) return;
	k_timer_stop(&s_led2_timer);
	k_timer_stop(&s_led3_timer);
	if (PIN_LED2 >= 0) gpio_pin_set(s_gpio, PIN_LED2, 0);
	if (PIN_LED3 >= 0) gpio_pin_set(s_gpio, PIN_LED3, 0);
}

void led_sleep_enter(void)
{
	if (!s_multi) { led_off(); return; }
	stop_all_timers();
	s_anim_step = 0;
	k_timer_start(&s_sleep_timer, K_MSEC(150), K_MSEC(150));
}

void led_all_off(void)
{
	stop_all_timers();
	if (s_multi) {
		gpio_pin_set(s_gpio, PIN_LED1, 0);
		if (PIN_LED2 >= 0) gpio_pin_set(s_gpio, PIN_LED2, 0);
		if (PIN_LED3 >= 0) gpio_pin_set(s_gpio, PIN_LED3, 0);
	}
#if HAS_DT_LED0
	else {
		gpio_pin_set_dt(&dt_led, 0);
	}
#endif
}

/* --- utility ------------------------------------------------------------- */

void status_delay(long ms)
{
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
