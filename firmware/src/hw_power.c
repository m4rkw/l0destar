#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "app.h"
#include "hw_common.h"
#include "hw_domain.h"
#include "pins.h"

LOG_MODULE_REGISTER(hw_power, CONFIG_APP_LOG_LEVEL);

static const struct bb_i2c ina_bus = {
	.scl_pin = PIN_INA_SCL,
	.sda_pin = PIN_INA_SDA
};

#define INA228_ADDR     0x40
#define INA228_ADC_CFG  0x01
#define INA228_ADC_CONT 0xB920
#define INA228_ADC_SHUT 0x0920

static bool s_ok;

int hw_power_init(void)
{
	bb_init(&ina_bus);
	bb_pin_test(&ina_bus, "INA228");
	gpio_pin_configure(hw_gpio0, PIN_INA_ALRT, GPIO_INPUT | GPIO_PULL_UP);
	/* The PCBs sense ignition through a 2N7002 open-drain with an external
	 * 56K pull-up to the always-on 3.3V rail; adding the internal pull-up
	 * would roughly triple the sense current whenever ignition is on. */
	if (IS_ENABLED(CONFIG_APP_BOARD_IGN_EXT_PULLUP)) {
		gpio_pin_configure(hw_gpio0, PIN_IGN_SENSE, GPIO_INPUT);
	} else {
		gpio_pin_configure(hw_gpio0, PIN_IGN_SENSE,
				   GPIO_INPUT | GPIO_PULL_UP);
	}

	if (!bb_write16(&ina_bus, INA228_ADDR, 0x00, 0x8000)) {
		LOG_ERR("INA228 reset NACK");
		return -EIO;
	}
	k_msleep(5);

	uint8_t id[2];
	if (!bb_read_regs(&ina_bus, INA228_ADDR, 0x3E, id, 2)) {
		LOG_ERR("INA228 MFR_ID NACK");
		return -EIO;
	}
	uint16_t mfr = (id[0] << 8) | id[1];
	if (mfr != 0x5449) {
		LOG_ERR("INA228 MFR_ID=0x%04X (expect 0x5449)", mfr);
		return -EIO;
	}

	bb_write16(&ina_bus, INA228_ADDR, INA228_ADC_CFG, INA228_ADC_CONT);
	bb_write16(&ina_bus, INA228_ADDR, 0x0B, 0x0000);
	k_msleep(10);

	s_ok = true;
	float v = battery_read_voltage();
	LOG_INF("INA228 ready — VBUS=%.3f V", (double)v);
	return 0;
}

/* The AUX domain (GPS bias tee; on v2.x also the OBD/aux rails) is
 * reference-counted in hw_domain so subsystem users can't fight the main
 * state machine.  These wrappers hold/release the main state's claim, with
 * all domain-pin park/release sequencing handled inside hw_domain. */
void hw_aux_power_on(void)
{
    hw_domain_request(HW_DOMAIN_AUX, HW_DOMAIN_USER_MAIN);
}

void hw_aux_power_off(void)
{
    hw_domain_release(HW_DOMAIN_AUX, HW_DOMAIN_USER_MAIN);
}

bool hw_power_available(void)
{
	return s_ok;
}

float battery_read_voltage(void)
{
#if CONFIG_APP_DEBUG_BATTERY_MV > 0
	/* bench override from local.conf */
	return CONFIG_APP_DEBUG_BATTERY_MV / 1000.0f;
#else
	if (!s_ok) return -1.0f;
	uint8_t buf[3];
	if (!bb_read_regs(&ina_bus, INA228_ADDR, 0x05, buf, 3)) {
		return -1.0f;
	}
	uint32_t raw = ((uint32_t)buf[0] << 16 | (uint32_t)buf[1] << 8 | buf[2]) >> 4;
	return (float)raw * 195.3125e-6f;
#endif
}

void hw_power_shutdown(void)
{
	if (!s_ok) return;
	bb_write16(&ina_bus, INA228_ADDR, INA228_ADC_CFG, INA228_ADC_SHUT);
}

void hw_power_wake(void)
{
	if (!s_ok) return;
	bb_write16(&ina_bus, INA228_ADDR, INA228_ADC_CFG, INA228_ADC_CONT);
	k_msleep(2);
}

int ignition_read(void)
{
#if CONFIG_APP_DEBUG_IGNITION >= 0
	/* bench override from local.conf: 0 = ON, 1 = OFF */
	return CONFIG_APP_DEBUG_IGNITION;
#else
	/* Active-low sense (pulled up): pin low = ignition present = 0 (ON) */
	return gpio_pin_get(hw_gpio0, PIN_IGN_SENSE);
#endif
}
