#include <math.h>
#include <string.h>
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
#define ACC_FIFO_CTRL1 0x07
#define ACC_FIFO_CTRL2 0x08
#define ACC_FIFO_CTRL3 0x09
#define ACC_FIFO_CTRL4 0x0A
#define ACC_CTRL1_XL   0x10
#define ACC_CTRL2_G    0x11
#define ACC_CTRL3_C    0x12
#define ACC_OUT_TEMP_L 0x20
#define ACC_OUTX_L_G   0x22
#define ACC_OUTX_L_A   0x28
#define ACC_FIFO_STATUS1 0x3A
#define ACC_FIFO_STATUS2 0x3B
#define ACC_FIFO_DATA_TAG 0x78
#define ACC_TAP_CFG0   0x56
#define ACC_TAP_CFG2   0x58
#define ACC_WAKE_UP_THS 0x5B
#define ACC_WAKE_UP_DUR 0x5C
#define ACC_WAKE_UP_SRC 0x1B
#define ACC_MD1_CFG    0x5E

static int16_t s_base_x, s_base_y, s_base_z;

/* mg per LSB for the accel's current full-scale: ±8 g (0.244) while awake so
 * impacts don't clip, ±2 g (0.061) in sleep for fine wake sensitivity.
 * accel_read() converts to milli-g so callers (and telemetry) are
 * FS-independent. */
static float s_mg_per_lsb = 0.244f;

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
	/* 416 Hz, ±8 g — headroom for impact detection without clipping */
	if (!bb_write_reg(&acc_bus, ACC_ADDR, ACC_CTRL1_XL, 0x6C)) {
		LOG_ERR("ACC CTRL1_XL NACK");
		return -EIO;
	}
	s_mg_per_lsb = 0.244f;
	/* Gyro on: 104 Hz, ±250 dps (8.75 mdps/LSB) — telemetry snapshots */
	if (!bb_write_reg(&acc_bus, ACC_ADDR, ACC_CTRL2_G, 0x40)) {
		LOG_ERR("ACC CTRL2_G NACK");
		return -EIO;
	}
	k_msleep(80);   /* gyro turn-on time ~70 ms */

	s_ok = true;
	LOG_INF("ACC ready (WHO_AM_I=0x%02X, XL 416 Hz +/-8g, G 104 Hz +/-250dps)", who);
	return 0;
}

bool accel_available(void)
{
	return s_ok;
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

	/* Convert raw LSB to milli-g at the current full-scale. */
	if (ax) *ax = (int)((int16_t)((b[1] << 8) | b[0]) * s_mg_per_lsb);
	if (ay) *ay = (int)((int16_t)((b[3] << 8) | b[2]) * s_mg_per_lsb);
	if (az) *az = (int)((int16_t)((b[5] << 8) | b[4]) * s_mg_per_lsb);
	return 0;
}

int accel_read_gyro(int *gx, int *gy, int *gz)
{
	if (!s_ok) return -1;

	uint8_t b[6];
	if (!bb_read_regs(&acc_bus, ACC_ADDR, ACC_OUTX_L_G, b, 6)) {
		return -1;
	}

	if (gx) *gx = (int)(int16_t)((b[1] << 8) | b[0]);
	if (gy) *gy = (int)(int16_t)((b[3] << 8) | b[2]);
	if (gz) *gz = (int)(int16_t)((b[5] << 8) | b[4]);
	return 0;
}

int accel_read_temp(float *temp_c)
{
	if (!s_ok) return -1;

	uint8_t b[2];
	if (!bb_read_regs(&acc_bus, ACC_ADDR, ACC_OUT_TEMP_L, b, 2)) {
		return -1;
	}

	/* 256 LSB/°C, 0 LSB = 25 °C */
	int16_t raw = (int16_t)((b[1] << 8) | b[0]);
	if (temp_c) *temp_c = 25.0f + (float)raw / 256.0f;
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

	/* accel_read already returns milli-g */
	float dx = (float)(ax - s_base_x);
	float dy = (float)(ay - s_base_y);
	float dz = (float)(az - s_base_z);
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

static int s_confirm_peak_mg;

int accel_confirm_movement(void)
{
	if (!s_ok) return 0;

	int hits = 0;
	int polls = MOVEMENT_CONFIRM_MS / 100;

	s_confirm_peak_mg = 0;
	for (int i = 0; i < polls; i++) {
		int tilt, delta;
		accel_get_movement_info(&tilt, &delta);
		if (delta > s_confirm_peak_mg) s_confirm_peak_mg = delta;
		if (delta > ACC_MOVEMENT_THRESHOLD) {
			hits++;
			if (hits >= MOVEMENT_CONFIRM_HITS) return 1;
		}
		k_msleep(100);
	}
	return 0;
}

/* Peak delta (mg) observed during the last confirm window — lets the sleep
 * path tell a car-park ding from wind/a passing truck. */
int accel_confirm_peak_mg(void)
{
	return s_confirm_peak_mg;
}

int accel_enable_wake_int(void)
{
	if (!s_ok) return -1;

	/* Gyro off for sleep (it draws ~0.6 mA and wake-up only needs accel) */
	bb_write_reg(&acc_bus, ACC_ADDR, ACC_CTRL2_G, 0x00);

	/* Switch to low-power 52 Hz ±2 g for sleep (fine wake sensitivity) */
	if (!bb_write_reg(&acc_bus, ACC_ADDR, ACC_CTRL1_XL, 0x30))
		return -EIO;
	s_mg_per_lsb = 0.061f;

	/* The FS/ODR change makes the raw output step ~4x and resets the slope
	 * filter — without settling, that internal step fires a spurious wake
	 * interrupt ~26 ms after arming and the first reads are garbage.
	 * 100 ms ≈ 5 samples at 52 Hz. */
	k_msleep(100);

	/* Keep the FIFO ring running in sleep, accel-only (gyro is off), so a
	 * parked impact's true waveform is captured for drain on wake. */
	bb_write_reg(&acc_bus, ACC_ADDR, ACC_FIFO_CTRL3, 0x02);
	bb_write_reg(&acc_bus, ACC_ADDR, ACC_FIFO_CTRL4, 0x06);

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

	/* Back to 416 Hz ±8 g, gyro back on */
	bb_write_reg(&acc_bus, ACC_ADDR, ACC_CTRL1_XL, 0x6C);
	s_mg_per_lsb = 0.244f;
	bb_write_reg(&acc_bus, ACC_ADDR, ACC_CTRL2_G, 0x40);

	LOG_INF("ACC wake-up interrupt disabled");
	return 0;
}

/* -- crash (high-g) interrupt, used while awake ----------------------------- */

int accel_crash_int_enable(int threshold_mg)
{
	if (!s_ok) return -1;

	/* Slope filter + master interrupt enable (same machinery as the sleep
	 * wake-up, but at impact threshold and ±8 g FS: 1 LSB = FS/64 = 125 mg) */
	if (!bb_write_reg(&acc_bus, ACC_ADDR, ACC_TAP_CFG0, 0x10))
		return -EIO;
	if (!bb_write_reg(&acc_bus, ACC_ADDR, ACC_TAP_CFG2, 0x80))
		return -EIO;

	int ths = threshold_mg / 125;
	if (ths < 1)  ths = 1;
	if (ths > 63) ths = 63;
	if (!bb_write_reg(&acc_bus, ACC_ADDR, ACC_WAKE_UP_THS, (uint8_t)ths))
		return -EIO;
	if (!bb_write_reg(&acc_bus, ACC_ADDR, ACC_WAKE_UP_DUR, 0x00))
		return -EIO;
	if (!bb_write_reg(&acc_bus, ACC_ADDR, ACC_MD1_CFG, 0x20))
		return -EIO;

	/* Clear any pending event */
	uint8_t dummy;
	bb_read_regs(&acc_bus, ACC_ADDR, ACC_WAKE_UP_SRC, &dummy, 1);

	LOG_INF("crash interrupt armed (%d mg, reg=%d)", ths * 125, ths);
	return 0;
}

int accel_crash_int_disable(void)
{
	if (!s_ok) return -1;
	bb_write_reg(&acc_bus, ACC_ADDR, ACC_MD1_CFG, 0x00);
	bb_write_reg(&acc_bus, ACC_ADDR, ACC_TAP_CFG2, 0x00);
	bb_write_reg(&acc_bus, ACC_ADDR, ACC_TAP_CFG0, 0x00);
	return 0;
}

int accel_read_wake_src(uint8_t *src)
{
	if (!s_ok) return -1;
	return bb_read_regs(&acc_bus, ACC_ADDR, ACC_WAKE_UP_SRC, src, 1) ? 0 : -1;
}

/* -- FIFO ring buffer: pre-impact forensics --------------------------------- */
/* Accel+gyro batched at 26 Hz into the 3 KB FIFO in continuous (ring) mode
 * ≈ 9 s of history.  On an impact interrupt the ring is drained and the
 * profile around the peak summarised for the alert. */

#define FIFO_MAX_SAMPLES 512
#define FIFO_SAMPLE_MS   38              /* 26 Hz */

static int16_t s_fifo_xl[FIFO_MAX_SAMPLES][3];   /* raw LSB */
static int16_t s_fifo_gy[FIFO_MAX_SAMPLES][3];

int accel_fifo_enable(void)
{
	if (!s_ok) return -1;
	/* BDR 26 Hz for both sensors (gyro[7:4]=0010, xl[3:0]=0010) */
	if (!bb_write_reg(&acc_bus, ACC_ADDR, ACC_FIFO_CTRL3, 0x22))
		return -EIO;
	/* Continuous (ring) mode */
	if (!bb_write_reg(&acc_bus, ACC_ADDR, ACC_FIFO_CTRL4, 0x06))
		return -EIO;
	return 0;
}

int accel_fifo_disable(void)
{
	if (!s_ok) return -1;
	/* Bypass mode stops batching and clears the FIFO */
	bb_write_reg(&acc_bus, ACC_ADDR, ACC_FIFO_CTRL4, 0x00);
	bb_write_reg(&acc_bus, ACC_ADDR, ACC_FIFO_CTRL3, 0x00);
	return 0;
}

int accel_fifo_drain_impact(struct accel_impact *out)
{
	if (!s_ok || !out) return -1;

	uint8_t s1 = 0, s2 = 0;
	if (!bb_read_regs(&acc_bus, ACC_ADDR, ACC_FIFO_STATUS1, &s1, 1) ||
	    !bb_read_regs(&acc_bus, ACC_ADDR, ACC_FIFO_STATUS2, &s2, 1)) {
		return -EIO;
	}
	int words = (((int)(s2 & 0x03)) << 8) | s1;

	int nxl = 0, ngy = 0;
	for (int i = 0; i < words; i++) {
		uint8_t w[7];
		if (!bb_read_regs(&acc_bus, ACC_ADDR, ACC_FIFO_DATA_TAG, w, 7))
			break;
		uint8_t tag = w[0] >> 3;
		int16_t x = (int16_t)((w[2] << 8) | w[1]);
		int16_t y = (int16_t)((w[4] << 8) | w[3]);
		int16_t z = (int16_t)((w[6] << 8) | w[5]);
		if (tag == 0x02 && nxl < FIFO_MAX_SAMPLES) {        /* accel NC */
			s_fifo_xl[nxl][0] = x;
			s_fifo_xl[nxl][1] = y;
			s_fifo_xl[nxl][2] = z;
			nxl++;
		} else if (tag == 0x01 && ngy < FIFO_MAX_SAMPLES) { /* gyro NC */
			s_fifo_gy[ngy][0] = x;
			s_fifo_gy[ngy][1] = y;
			s_fifo_gy[ngy][2] = z;
			ngy++;
		}
		if ((i & 0x3F) == 0) watchdog_kick();   /* drain takes ~0.5 s */
	}

	memset(out, 0, sizeof(*out));
	out->samples = nxl;

	/* Convert at the CURRENT full-scale: ±8 g awake, ±2 g in sleep */
	float mg = s_mg_per_lsb;

	/* Peak |a| over the ring (mg) and the sample index */
	int peak_idx = 0;
	float peak_sq = 0.0f;
	for (int i = 0; i < nxl; i++) {
		float x = s_fifo_xl[i][0] * mg;
		float y = s_fifo_xl[i][1] * mg;
		float z = s_fifo_xl[i][2] * mg;
		float sq = x * x + y * y + z * z;
		if (sq > peak_sq) {
			peak_sq = sq;
			peak_idx = i;
		}
	}
	out->peak_mg = (int)sqrtf(peak_sq);
	out->pax = (int)(s_fifo_xl[peak_idx][0] * mg);
	out->pay = (int)(s_fifo_xl[peak_idx][1] * mg);
	out->paz = (int)(s_fifo_xl[peak_idx][2] * mg);

	/* Peak gyro magnitude (dps ×10, ±250 dps scale: 8.75 mdps/LSB) */
	float gpeak_sq = 0.0f;
	for (int i = 0; i < ngy; i++) {
		float x = s_fifo_gy[i][0] * 0.00875f;
		float y = s_fifo_gy[i][1] * 0.00875f;
		float z = s_fifo_gy[i][2] * 0.00875f;
		float sq = x * x + y * y + z * z;
		if (sq > gpeak_sq) gpeak_sq = sq;
	}
	out->peak_gyro_dps10 = (int)(sqrtf(gpeak_sq) * 10.0f);

	/* Disturbance duration + peak deviation from 1 g (the impact metric:
	 * spikes go above, free-fall dips below) */
	int over = 0;
	float peak_dev = 0.0f;
	for (int i = 0; i < nxl; i++) {
		float x = s_fifo_xl[i][0] * mg;
		float y = s_fifo_xl[i][1] * mg;
		float z = s_fifo_xl[i][2] * mg;
		float magn = sqrtf(x * x + y * y + z * z);
		float dev = (magn > 1000.0f) ? magn - 1000.0f : 1000.0f - magn;
		if (dev > peak_dev) peak_dev = dev;
		if (dev > 250.0f) over++;
	}
	out->over_ms = over * FIFO_SAMPLE_MS;
	out->peak_delta_mg = (int)peak_dev;

	/* Dump ±0.5 s around the peak to the log for bench analysis */
	int lo = peak_idx - 13;
	int hi = peak_idx + 13;
	if (lo < 0) lo = 0;
	if (hi >= nxl) hi = nxl - 1;
	for (int i = lo; i <= hi; i++) {
		LOG_INF("fifo[%+d] %d/%d/%d mg", i - peak_idx,
			(int)(s_fifo_xl[i][0] * mg),
			(int)(s_fifo_xl[i][1] * mg),
			(int)(s_fifo_xl[i][2] * mg));
	}
	return 0;
}
