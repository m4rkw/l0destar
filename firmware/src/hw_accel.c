#include <math.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "app.h"
#include "hw_common.h"
#include "pins.h"

LOG_MODULE_REGISTER(hw_accel, CONFIG_APP_LOG_LEVEL);

static const struct bb_i2c acc_bus = {
	.scl_pin = PIN_ACC_SCL,
	.sda_pin = PIN_ACC_SDA
};

static bool s_ok;

#define ACC_ADDR       0x6A
#define ACC_WHOAMI     0x0F
#define ACC_CTRL1_XL   0x10
#define ACC_CTRL3_C    0x12
#define ACC_OUTX_L_A   0x28
#define ACC_TAP_CFG0   0x56
#define ACC_TAP_CFG2   0x58
#define ACC_WAKE_UP_THS 0x5B
#define ACC_WAKE_UP_DUR 0x5C
#define ACC_WAKE_UP_SRC 0x1B
#define ACC_MD1_CFG    0x5E

static int16_t s_base_x, s_base_y, s_base_z;

int hw_accel_init(void)
{
	bb_init(&acc_bus);
	bb_pin_test(&acc_bus, "ACC");

	uint8_t who = 0;
	int retries = 5;
	while (retries-- > 0) {
		if (bb_read_regs(&acc_bus, ACC_ADDR, ACC_WHOAMI, &who, 1)) {
			break;
		}
		k_msleep(100);
	}
	if (who == 0) {
		LOG_ERR("ACC WHO_AM_I NACK (no response after retries)");
		return -EIO;
	}
	if (who != 0x6B) {
		LOG_ERR("ACC WHO_AM_I=0x%02X (expect 0x6B)", who);
		return -EIO;
	}

	if (!bb_write_reg(&acc_bus, ACC_ADDR, ACC_CTRL3_C, 0x44)) {
		LOG_ERR("ACC CTRL3_C NACK");
		return -EIO;
	}
	if (!bb_write_reg(&acc_bus, ACC_ADDR, ACC_CTRL1_XL, 0x60)) {
		LOG_ERR("ACC CTRL1_XL NACK");
		return -EIO;
	}
	k_msleep(20);

	s_ok = true;
	LOG_INF("ACC ready (WHO_AM_I=0x%02X, 416 Hz, +/-2g)", who);
	return 0;
}

int accel_read(int *ax, int *ay, int *az)
{
	if (!s_ok) {
		if (ax) *ax = 0;
		if (ay) *ay = 0;
		if (az) *az = 0;
		return -1;
	}

	uint8_t b[6];
	if (!bb_read_regs(&acc_bus, ACC_ADDR, ACC_OUTX_L_A, b, 6)) {
		if (ax) *ax = 0;
		if (ay) *ay = 0;
		if (az) *az = 0;
		return -1;
	}

	if (ax) *ax = (int)(int16_t)((b[1] << 8) | b[0]);
	if (ay) *ay = (int)(int16_t)((b[3] << 8) | b[2]);
	if (az) *az = (int)(int16_t)((b[5] << 8) | b[4]);
	return 0;
}

int accel_read_baseline(void)
{
	int ax, ay, az;
	if (accel_read(&ax, &ay, &az) != 0) return -1;
	s_base_x = (int16_t)ax;
	s_base_y = (int16_t)ay;
	s_base_z = (int16_t)az;
	return 0;
}

void accel_get_movement_info(int *tilt_tenths, int *delta_mg)
{
	int ax, ay, az;
	if (accel_read(&ax, &ay, &az) != 0) {
		*tilt_tenths = 0;
		*delta_mg = 0;
		return;
	}

	float dx = (float)(ax - s_base_x) * 0.061f;
	float dy = (float)(ay - s_base_y) * 0.061f;
	float dz = (float)(az - s_base_z) * 0.061f;
	*delta_mg = (int)sqrtf(dx * dx + dy * dy + dz * dz);

	float bx = (float)s_base_x, by = (float)s_base_y, bz = (float)s_base_z;
	float cx = (float)ax, cy = (float)ay, cz = (float)az;
	float dot = bx * cx + by * cy + bz * cz;
	float mag_b = sqrtf(bx * bx + by * by + bz * bz);
	float mag_c = sqrtf(cx * cx + cy * cy + cz * cz);

	if (mag_b > 0.0f && mag_c > 0.0f) {
		float cos_a = dot / (mag_b * mag_c);
		if (cos_a > 1.0f) cos_a = 1.0f;
		if (cos_a < -1.0f) cos_a = -1.0f;
		*tilt_tenths = (int)(acosf(cos_a) * (1800.0f / 3.14159265f));
	} else {
		*tilt_tenths = 0;
	}
}

int accel_confirm_movement(void)
{
	if (!s_ok) return 0;

	int hits = 0;
	int polls = MOVEMENT_CONFIRM_MS / 100;

	for (int i = 0; i < polls; i++) {
		int tilt, delta;
		accel_get_movement_info(&tilt, &delta);
		if (delta > ACC_MOVEMENT_THRESHOLD) {
			hits++;
			if (hits >= MOVEMENT_CONFIRM_HITS) return 1;
		}
		k_msleep(100);
	}
	return 0;
}

int accel_enable_wake_int(void)
{
	if (!s_ok) return -1;

	/* Switch to low-power 52 Hz for sleep */
	if (!bb_write_reg(&acc_bus, ACC_ADDR, ACC_CTRL1_XL, 0x30))
		return -EIO;

	/* Enable slope filter for wake-up */
	if (!bb_write_reg(&acc_bus, ACC_ADDR, ACC_TAP_CFG0, 0x10))
		return -EIO;

	/* Master enable for interrupts */
	if (!bb_write_reg(&acc_bus, ACC_ADDR, ACC_TAP_CFG2, 0x80))
		return -EIO;

	/* Threshold: ~156mg (5 * 31.25mg at ±2g, LSB = FS/64) */
	if (!bb_write_reg(&acc_bus, ACC_ADDR, ACC_WAKE_UP_THS, 0x05))
		return -EIO;

	/* Duration: 0 (single sample) */
	if (!bb_write_reg(&acc_bus, ACC_ADDR, ACC_WAKE_UP_DUR, 0x00))
		return -EIO;

	/* Route wake-up to INT1 */
	if (!bb_write_reg(&acc_bus, ACC_ADDR, ACC_MD1_CFG, 0x20))
		return -EIO;

	/* Clear any pending interrupt */
	uint8_t dummy;
	bb_read_regs(&acc_bus, ACC_ADDR, ACC_WAKE_UP_SRC, &dummy, 1);

	LOG_DBG("ACC wake-up interrupt enabled on INT1");
	return 0;
}

int accel_disable_wake_int(void)
{
	if (!s_ok) return -1;

	/* Disable INT1 routing */
	bb_write_reg(&acc_bus, ACC_ADDR, ACC_MD1_CFG, 0x00);

	/* Disable interrupts */
	bb_write_reg(&acc_bus, ACC_ADDR, ACC_TAP_CFG2, 0x00);
	bb_write_reg(&acc_bus, ACC_ADDR, ACC_TAP_CFG0, 0x00);

	/* Back to 416 Hz */
	bb_write_reg(&acc_bus, ACC_ADDR, ACC_CTRL1_XL, 0x60);

	LOG_INF("ACC wake-up interrupt disabled");
	return 0;
}
