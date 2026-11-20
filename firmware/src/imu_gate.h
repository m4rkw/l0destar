/*
 * Whether a streamed record carries the accelerometer and gyro (ax..gz).  Pure
 * decision, checked on the host: tests/test_imu_gate.py.
 *
 *   not streaming (lean off)  always
 *   stream, accel on (xia)    every record when the gap (xim) is 0, else once
 *                             the gap has passed since the last reading
 *   stream, accel off         never, except for the first record after an
 *                             impact was detected, if impact capture (xic) is on
 *
 * The impact flag is set by the awake impact detection (crash_check) and is
 * consumed by the record that carries it, so a second impact is a second record.
 */
#ifndef IMU_GATE_H
#define IMU_GATE_H

#include <stdbool.h>
#include <stdint.h>

#include "stream_batch.h"

struct imu_gate {
	int64_t last_ms;	/* when the sensor was last read for a record */
	bool    impact;		/* an impact is waiting to be carried */
};

#define IMU_GATE_INIT { INT64_MIN / 2, false }

static inline void imu_gate_impact(struct imu_gate *g)
{
	g->impact = true;
}

static inline bool imu_gate_due(struct imu_gate *g, bool lean, int64_t now_ms,
				bool accel_on, int32_t gap_ms, bool capture_impact)
{
	if (!lean) {
		g->impact = false;	/* the record carries them anyway */
		return true;
	}
	if (!accel_on) {
		bool take = capture_impact && g->impact;

		g->impact = false;
		if (take) {
			g->last_ms = now_ms;
		}
		return take;
	}
	g->impact = false;
	if (!stream_due(now_ms, g->last_ms, gap_ms)) {
		return false;
	}
	g->last_ms = now_ms;
	return true;
}

#endif /* IMU_GATE_H */
