/*
 * The parked unit's slow-tilt watch done by the IMU (APP_TOW_IMU_WAKE).  Pure
 * decisions, so they are checked on the host: tests/test_tow_wake.py.
 *
 * The ASM330LHHX has one wake-up engine.  With USR_OFF_ON_WU set it is fed the
 * low-pass output less the user offsets instead of the high-pass output, and
 * fires while any axis is further than the threshold from the stored value.
 * With the tilt reference as that value, one threshold catches knocks and slow
 * tilts alike, and the sleep loop need not wake to read the angle.
 *
 * CENTRED: that comparator is armed, centred on the tilt reference, and there
 * is no tilt poll.  OFF-CENTRE: the unit sits beyond it, or a tow alert is
 * latched, so INT1 would stay high: the high-pass wake and the APP_TOW_POLL_S
 * poll are used until the unit is back within half the threshold of the
 * reference.  The reference only ever moves with the tow re-arm, never to the
 * latest reading, or a slow lift would be absorbed.
 */
#ifndef TOW_WAKE_H
#define TOW_WAKE_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

/* X/Y/Z_OFS_USR weight with USR_OFF_W set: 2^-6 g, in micro-g. */
#define TOW_WAKE_OFS_UG 15625
/* WK_THS weight with WAKE_THS_W set at +/-2 g: 2 g / 256, in micro-g
 * (7812.5, rounded up). */
#define TOW_WAKE_THS_UG 7813

/* The threshold's limits in WK_THS steps: 47 and 102 mg.  Below the floor the
 * comparator sits too near the noise and the offsets' rounding.  The ceiling is
 * well under the 156 mg high-pass wake it replaces, so it is never deafer to a
 * knock, and a slow tilt's own swing at the trip cannot pass for one. */
#define TOW_WAKE_THS_MIN 6
#define TOW_WAKE_THS_MAX 13

/* The high-pass wake's threshold (WK_THS 5 of 31.25 mg).  A wake of the
 * absolute comparator with less than this in the ring would not have woken the
 * unit before: a tilt, or something too light to look into. */
#define TOW_WAKE_KNOCK_MG 156

/* From an absolute wake to the drain that judges it. */
#define TOW_WAKE_RING_MS 500

struct tow_wake {
	bool off_centre;
	bool hold;		/* stay off-centre at the next arm */
};

/* The user offset for a reference axis, mg.  The comparator SUBTRACTS the
 * offset from the measured acceleration (ST AN5192, "accelerometer offset
 * registers"), so the register holds the reference with its own sign: at rest
 * on the reference the difference is nil. */
static inline int8_t tow_wake_ofs(int mg)
{
	int ug = mg * 1000;
	int n = (ug + (ug < 0 ? -TOW_WAKE_OFS_UG / 2 : TOW_WAKE_OFS_UG / 2)) /
		TOW_WAKE_OFS_UG;

	return (int8_t)(n > 127 ? 127 : n < -127 ? -127 : n);
}

/* The largest per-axis distance, micro-g, of a reading from the offsets: what
 * the comparator compares with its threshold. */
static inline int tow_wake_dev_ug(const int mg[3], const int8_t ofs[3])
{
	int dev = 0;

	for (int i = 0; i < 3; i++) {
		int d = abs(mg[i] * 1000 - ofs[i] * TOW_WAKE_OFS_UG);

		if (d > dev) {
			dev = d;
		}
	}
	return dev;
}

/* The threshold, in WK_THS steps, that a tilt of tilt_deg from ref_mg trips
 * whatever its direction and however the unit is mounted; 0 where there is
 * none (no tilt watch, no usable reference, or an angle too small for the
 * comparator), and the caller polls instead.
 *
 * Tilting g by that angle moves the vector by 2 |g| sin(angle / 2), and the
 * largest of its three components is at least that over sqrt(3).  The offsets'
 * rounding can take back up to its largest per-axis error. */
static inline int tow_wake_ths(const int ref_mg[3], int tilt_deg)
{
	if (tilt_deg <= 0) {
		return 0;
	}

	float x = ref_mg[0], y = ref_mg[1], z = ref_mg[2];
	float g = sqrtf(x * x + y * y + z * z);

	if (g < 800.0f || g > 1200.0f) {
		return 0;	/* not an accelerometer at rest */
	}

	int8_t ofs[3] = { tow_wake_ofs(ref_mg[0]), tow_wake_ofs(ref_mg[1]),
			  tow_wake_ofs(ref_mg[2]) };
	float chord_ug = 2000.0f * g *
			 sinf((float)tilt_deg * (3.14159265f / 360.0f));
	float room_ug = chord_ug / 1.7320508f -
			(float)tow_wake_dev_ug(ref_mg, ofs);
	int n = (int)(room_ug / (float)TOW_WAKE_THS_UG);

	if (n < TOW_WAKE_THS_MIN) {
		return 0;
	}
	return n > TOW_WAKE_THS_MAX ? TOW_WAKE_THS_MAX : n;
}

/* The wake is being re-armed with the unit reading now_mg: true for the
 * absolute comparator, false for the high-pass wake and the poll.  Leaving it
 * takes the threshold, coming back half of it. */
static inline bool tow_wake_centred(struct tow_wake *s, bool alerted,
				    const int now_mg[3], const int8_t ofs[3],
				    int ths)
{
	int limit_ug = ths * TOW_WAKE_THS_UG / (s->off_centre ? 2 : 1);

	s->off_centre = alerted || s->hold ||
			tow_wake_dev_ug(now_mg, ofs) > limit_ug;
	s->hold = false;
	return !s->off_centre;
}

/* The absolute comparator woke the unit for something short of a knock.  It
 * stays off-centre through this pass's re-arm, so the soonest it can be armed
 * again is a poll later: a light vibration costs one wake per poll, no more. */
static inline void tow_wake_trip(struct tow_wake *s)
{
	s->off_centre = true;
	s->hold = true;
}

/* From the FIFO ring of an absolute wake: peak | |a| - 1g | and the largest
 * per-axis swing.  The swing is what sees a sideways knock, which barely moves
 * the magnitude. */
static inline bool tow_wake_knock(int peak_delta_mg, int swing_mg)
{
	return peak_delta_mg >= TOW_WAKE_KNOCK_MG ||
	       swing_mg >= TOW_WAKE_KNOCK_MG;
}

#endif /* TOW_WAKE_H */
