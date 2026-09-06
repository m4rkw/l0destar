#include <math.h>
#include <stdlib.h>
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
#define ACC_CTRL6_C    0x15
#define ACC_OUT_TEMP_L 0x20
#define ACC_OUTX_L_G   0x22
#define ACC_OUTX_L_A   0x28
#define ACC_D6D_SRC    0x1D
#define ACC_TAP_THS_6D 0x59
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

/* Gyro zero-rate bias (raw LSB), learned at standstill and subtracted from
 * every gyro read. See GYRO_AUTOZERO_* in config.h. */
static int  s_gyro_bias_x, s_gyro_bias_y, s_gyro_bias_z;
static bool s_gyro_bias_valid;

/* mg per LSB for the accel's current full-scale: ±8 g (0.244) while awake so
 * impacts don't clip, ±2 g (0.061) in sleep for fine wake sensitivity.
 * accel_read() converts to milli-g so callers (and telemetry) are
 * FS-independent. */
static float s_mg_per_lsb = 0.244f;

static uint8_t s_d6d_zone;   /* orientation zone captured when 6D was armed */

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

static int gyro_read_raw(int *gx, int *gy, int *gz)
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

int accel_read_gyro(int *gx, int *gy, int *gz)
{
	int rx, ry, rz;
	if (gyro_read_raw(&rx, &ry, &rz) != 0) return -1;

	if (gx) *gx = rx - s_gyro_bias_x;
	if (gy) *gy = ry - s_gyro_bias_y;
	if (gz) *gz = rz - s_gyro_bias_z;
	return 0;
}

/* Re-learn the gyro zero-rate offset from a short burst of raw samples. Call
 * only when the vehicle is known to be stationary (good fix, ~0 speed). The
 * burst is rejected if any axis shows real rotation, so a stale/zero GNSS
 * speed during motion can't corrupt the bias. */
int accel_gyro_autozero(void)
{
	if (!s_ok) return -1;

	int32_t sx = 0, sy = 0, sz = 0;
	int n = 0;

	for (int i = 0; i < GYRO_AUTOZERO_SAMPLES; i++) {
		int rx, ry, rz;
		if (gyro_read_raw(&rx, &ry, &rz) != 0) continue;

		/* Anything well beyond the expected offset is real motion, not
		 * bias — abort rather than learn a wrong zero. */
		if (abs(rx - s_gyro_bias_x) > GYRO_AUTOZERO_REJECT_LSB ||
		    abs(ry - s_gyro_bias_y) > GYRO_AUTOZERO_REJECT_LSB ||
		    abs(rz - s_gyro_bias_z) > GYRO_AUTOZERO_REJECT_LSB) {
			return -1;
		}

		sx += rx; sy += ry; sz += rz;
		n++;
		k_msleep(GYRO_AUTOZERO_GAP_MS);
	}
	if (n == 0) return -1;

	int bx = sx / n, by = sy / n, bz = sz / n;

	if (!s_gyro_bias_valid) {
		s_gyro_bias_x = bx;
		s_gyro_bias_y = by;
		s_gyro_bias_z = bz;
		s_gyro_bias_valid = true;
	} else {
		/* EMA so the bias tracks slow thermal drift but a single noisy
		 * burst can't yank it. */
		s_gyro_bias_x += (bx - s_gyro_bias_x) >> GYRO_AUTOZERO_EMA_SHIFT;
		s_gyro_bias_y += (by - s_gyro_bias_y) >> GYRO_AUTOZERO_EMA_SHIFT;
		s_gyro_bias_z += (bz - s_gyro_bias_z) >> GYRO_AUTOZERO_EMA_SHIFT;
	}

	LOG_DBG("gyro autozero: bias=%d,%d,%d (n=%d)",
	        s_gyro_bias_x, s_gyro_bias_y, s_gyro_bias_z, n);
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

/* Requiring as many hits as there are polls means every single sample has to
 * clear the threshold, which real movement never manages — it passes through
 * ~1 g on every change of direction — so the confirm can never succeed. */
BUILD_ASSERT(MOVEMENT_CONFIRM_HITS < MOVEMENT_CONFIRM_MS / 100,
	     "APP_MOVEMENT_CONFIRM_HITS must be below APP_MOVEMENT_CONFIRM_MS/100 "
	     "(the number of 100 ms polls in the window)");

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

	/* XL_HM_MODE=1: without this the accel stays in high-performance mode
	 * (~170 µA) at every ODR; true low-power at 52 Hz is ~25 µA */
	bb_write_reg(&acc_bus, ACC_ADDR, ACC_CTRL6_C, 0x10);

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

	/* 6D orientation detection: 60° zone threshold — fires when the unit
	 * settles into a different cardinal orientation (tamper / removal). */
	if (!bb_write_reg(&acc_bus, ACC_ADDR, ACC_TAP_THS_6D, 0x40))
		return -EIO;

	/* Route wake-up + 6D to INT1 */
	if (!bb_write_reg(&acc_bus, ACC_ADDR, ACC_MD1_CFG, 0x24))
		return -EIO;

	/* Clear any pending events */
	uint8_t dummy;
	bb_read_regs(&acc_bus, ACC_ADDR, ACC_WAKE_UP_SRC, &dummy, 1);
	bb_read_regs(&acc_bus, ACC_ADDR, ACC_D6D_SRC, &dummy, 1);

	/* The 6D engine classifies the *initial* orientation a few samples
	 * after arming and fires for it — swallow that event and remember the
	 * zone so only a genuine change reads as tamper. */
	k_msleep(80);
	if (bb_read_regs(&acc_bus, ACC_ADDR, ACC_D6D_SRC, &dummy, 1)) {
		s_d6d_zone = dummy & 0x3F;
	}

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

	/* Back to 416 Hz ±8 g in high-performance mode, gyro back on */
	bb_write_reg(&acc_bus, ACC_ADDR, ACC_CTRL6_C, 0x00);
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

int accel_read_d6d_src(uint8_t *src)
{
	if (!s_ok) return -1;
	return bb_read_regs(&acc_bus, ACC_ADDR, ACC_D6D_SRC, src, 1) ? 0 : -1;
}

/* True when the current orientation zone (which face is down) differs from the
 * one captured at arm time.  Uses the static zone bits, not the transient
 * D6D_IA event flag — the no-latch config doesn't hold the event long enough
 * to catch from the sleep loop. */
int accel_d6d_tamper(uint8_t *src)
{
	uint8_t v = 0;
	if (accel_read_d6d_src(&v) != 0) return 0;
	if (src) *src = v;
	uint8_t zone = v & 0x3F;
	if (zone == 0) return 0;          /* no clear orientation classified */
	return zone != s_d6d_zone;        /* changed from the armed orientation */
}

/* -- slow-tilt reference (tow/jack detection) -------------------------------
 * Separate from the movement baseline: that one is re-read after every wake
 * event, which would erase a lift in progress.  This reference is snapped
 * once at sleep entry and persists for the whole parked session. */

static int16_t s_tilt_ref[3];
static bool    s_tilt_ref_ok;

int accel_snapshot_tilt_ref(void)
{
	int x, y, z;
	if (accel_read(&x, &y, &z) != 0) {
		s_tilt_ref_ok = false;
		return -1;
	}
	s_tilt_ref[0] = (int16_t)x;
	s_tilt_ref[1] = (int16_t)y;
	s_tilt_ref[2] = (int16_t)z;
	s_tilt_ref_ok = true;
	return 0;
}

/* Angle between the current gravity vector and the sleep-entry reference,
 * in tenths of a degree.  Negative on error. */
int accel_tilt_from_ref_tenths(void)
{
	if (!s_tilt_ref_ok) return -1;

	int x, y, z;
	if (accel_read(&x, &y, &z) != 0) return -1;

	float rx = s_tilt_ref[0], ry = s_tilt_ref[1], rz = s_tilt_ref[2];
	float cx = x, cy = y, cz = z;
	float dot = rx * cx + ry * cy + rz * cz;
	float mr = sqrtf(rx * rx + ry * ry + rz * rz);
	float mc = sqrtf(cx * cx + cy * cy + cz * cz);
	if (mr <= 0.0f || mc <= 0.0f) return -1;

	float cos_a = dot / (mr * mc);
	if (cos_a > 1.0f)  cos_a = 1.0f;
	if (cos_a < -1.0f) cos_a = -1.0f;
	return (int)(acosf(cos_a) * (1800.0f / 3.14159265f));
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

/* Track mode: hand the ring's contents over as samples rather than as
 * impact statistics.  Reads every word batched since the last drain, so
 * calling it once a cycle turns the FIFO into a 26 Hz stream with no thread
 * of its own and no interrupt.  The impact drain above still works between
 * calls — it just sees the samples since this last ran, which, with the
 * crash check made at the top of every cycle, is the cycle the hit was in.
 *
 * With more samples than `max` the pick is evenly spaced across the whole
 * interval rather than the newest N, so a slow cycle thins the stream
 * instead of leaving a hole at its start.  Gyro words are paired with accel
 * words by arrival order; both are batched at the same rate. */
int accel_fifo_drain_samples(struct accel_sample *out, int max)
{
	if (!s_ok || !out || max <= 0) return -1;

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
		if (tag == 0x02 && nxl < FIFO_MAX_SAMPLES) {
			s_fifo_xl[nxl][0] = x;
			s_fifo_xl[nxl][1] = y;
			s_fifo_xl[nxl][2] = z;
			nxl++;
		} else if (tag == 0x01 && ngy < FIFO_MAX_SAMPLES) {
			s_fifo_gy[ngy][0] = x;
			s_fifo_gy[ngy][1] = y;
			s_fifo_gy[ngy][2] = z;
			ngy++;
		}
		if ((i & 0x3F) == 0) watchdog_kick();
	}
	if (nxl == 0) {
		return 0;
	}

	int count = nxl < max ? nxl : max;
	float mg = s_mg_per_lsb;

	for (int k = 0; k < count; k++) {
		int i = (int)(((int64_t)k * nxl) / count);   /* evenly spaced */
		int j = i < ngy ? i : ngy - 1;

		out[k].ax = (int16_t)(s_fifo_xl[i][0] * mg);
		out[k].ay = (int16_t)(s_fifo_xl[i][1] * mg);
		out[k].az = (int16_t)(s_fifo_xl[i][2] * mg);
		if (j >= 0) {
			out[k].gx = (int16_t)(s_fifo_gy[j][0] - s_gyro_bias_x);
			out[k].gy = (int16_t)(s_fifo_gy[j][1] - s_gyro_bias_y);
			out[k].gz = (int16_t)(s_fifo_gy[j][2] - s_gyro_bias_z);
		} else {
			out[k].gx = out[k].gy = out[k].gz = 0;
		}
	}
	return count;
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
