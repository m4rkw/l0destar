#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "hw_common.h"

LOG_MODULE_REGISTER(hw_common, CONFIG_APP_LOG_LEVEL);

const struct device *hw_gpio0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));

int hw_gpio_init(void)
{
	if (!device_is_ready(hw_gpio0)) {
		LOG_ERR("gpio0 not ready");
		return -ENODEV;
	}
	return 0;
}

static inline void bb_dly(void)        { k_busy_wait(5); }

static inline void bb_sda_hi(const struct bb_i2c *b)
{ gpio_pin_configure(hw_gpio0, b->sda_pin, GPIO_INPUT | GPIO_PULL_UP); }

static inline void bb_sda_lo(const struct bb_i2c *b)
{ gpio_pin_configure(hw_gpio0, b->sda_pin, GPIO_OUTPUT_LOW); }

static inline void bb_scl_hi(const struct bb_i2c *b)
{ gpio_pin_configure(hw_gpio0, b->scl_pin, GPIO_INPUT | GPIO_PULL_UP); }

static inline void bb_scl_lo(const struct bb_i2c *b)
{ gpio_pin_configure(hw_gpio0, b->scl_pin, GPIO_OUTPUT_LOW); }

static inline int bb_sda_rd(const struct bb_i2c *b)
{
	gpio_pin_configure(hw_gpio0, b->sda_pin, GPIO_INPUT | GPIO_PULL_UP);
	return gpio_pin_get(hw_gpio0, b->sda_pin);
}

static void bb_start(const struct bb_i2c *b)
{
	bb_sda_hi(b); bb_scl_hi(b); bb_dly();
	bb_sda_lo(b); bb_dly();
	bb_scl_lo(b); bb_dly();
}

static void bb_stop(const struct bb_i2c *b)
{
	bb_sda_lo(b); bb_dly();
	bb_scl_hi(b); bb_dly();
	bb_sda_hi(b); bb_dly();
}

static int bb_wr(const struct bb_i2c *b, uint8_t byte)
{
	for (int i = 0; i < 8; i++) {
		if (byte & 0x80) bb_sda_hi(b); else bb_sda_lo(b);
		byte <<= 1;
		bb_dly(); bb_scl_hi(b); bb_dly(); bb_scl_lo(b); bb_dly();
	}
	bb_sda_hi(b); bb_dly();
	bb_scl_hi(b); bb_dly();
	int ack = bb_sda_rd(b);
	bb_scl_lo(b); bb_dly();
	return ack;
}

static uint8_t bb_rd(const struct bb_i2c *b, int ack)
{
	uint8_t byte = 0;
	bb_sda_hi(b);
	for (int i = 0; i < 8; i++) {
		bb_dly(); bb_scl_hi(b); bb_dly();
		byte = (byte << 1) | (bb_sda_rd(b) & 1);
		bb_scl_lo(b);
	}
	if (ack) bb_sda_lo(b); else bb_sda_hi(b);
	bb_dly(); bb_scl_hi(b); bb_dly(); bb_scl_lo(b); bb_dly();
	bb_sda_hi(b);
	return byte;
}

void bb_init(const struct bb_i2c *b)
{
	bb_sda_hi(b);
	bb_scl_hi(b);
	k_msleep(10);
}

void bb_pin_test(const struct bb_i2c *b, const char *name)
{
	bb_sda_hi(b); bb_scl_hi(b);
	k_busy_wait(50);
	int scl = gpio_pin_get(hw_gpio0, b->scl_pin);
	int sda = gpio_pin_get(hw_gpio0, b->sda_pin);
	LOG_INF("%s pins: SCL(P0.%d)=%d SDA(P0.%d)=%d %s",
		name, b->scl_pin, scl, b->sda_pin, sda,
		(scl && sda) ? "OK" : "FAIL");
}

bool bb_read_regs(const struct bb_i2c *b, uint8_t addr, uint8_t reg,
		  uint8_t *buf, int n)
{
	bb_start(b);
	if (bb_wr(b, (addr << 1) | 0)) { bb_stop(b); return false; }
	if (bb_wr(b, reg))              { bb_stop(b); return false; }
	bb_start(b);
	if (bb_wr(b, (addr << 1) | 1)) { bb_stop(b); return false; }
	for (int i = 0; i < n; i++) buf[i] = bb_rd(b, i < n - 1);
	bb_stop(b);
	return true;
}

bool bb_write_reg(const struct bb_i2c *b, uint8_t addr, uint8_t reg,
		  uint8_t val)
{
	bb_start(b);
	if (bb_wr(b, (addr << 1) | 0)) { bb_stop(b); return false; }
	if (bb_wr(b, reg))              { bb_stop(b); return false; }
	if (bb_wr(b, val))              { bb_stop(b); return false; }
	bb_stop(b);
	return true;
}

bool bb_write16(const struct bb_i2c *b, uint8_t addr, uint8_t reg,
		uint16_t val)
{
	bb_start(b);
	if (bb_wr(b, (addr << 1) | 0)) { bb_stop(b); return false; }
	if (bb_wr(b, reg))              { bb_stop(b); return false; }
	if (bb_wr(b, (uint8_t)(val >> 8)))   { bb_stop(b); return false; }
	if (bb_wr(b, (uint8_t)(val & 0xFF))) { bb_stop(b); return false; }
	bb_stop(b);
	return true;
}
