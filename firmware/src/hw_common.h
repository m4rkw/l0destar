#ifndef HW_COMMON_H_
#define HW_COMMON_H_

#include <stdint.h>
#include <stdbool.h>
#include <zephyr/device.h>

struct bb_i2c {
	uint8_t scl_pin;
	uint8_t sda_pin;
};

extern const struct device *hw_gpio0;

int  hw_gpio_init(void);
void bb_init(const struct bb_i2c *b);
void bb_pin_test(const struct bb_i2c *b, const char *name);
bool bb_read_regs(const struct bb_i2c *b, uint8_t addr, uint8_t reg,
		  uint8_t *buf, int n);
bool bb_write_reg(const struct bb_i2c *b, uint8_t addr, uint8_t reg,
		  uint8_t val);
bool bb_write16(const struct bb_i2c *b, uint8_t addr, uint8_t reg,
		uint16_t val);

#endif
