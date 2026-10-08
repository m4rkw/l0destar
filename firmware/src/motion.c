/*
 * Dead reckoning: the motion through a gap between GNSS fixes, carried on the
 * record that ends the gap so the server can fill it in.
 *
 * WHY
 * ---
 * GNSS and LTE share the nRF9151's radio, and the receiver gets nothing while
 * the modem holds an RRC connection.  Every send costs one.  On most cells
 * here the network lets go about 2.5 s after the reply, but the TAC 12296
 * cells hold on for about 10 s, and no cell here honours release assistance:
 * every %RAI report the bench has logged reads as_rai=0, cp_rai=0.  So a
 * drive on those cells is three one-second fixes every 14 s joined by
 * straight lines 100-180 m long — the whole of the 22:49 drive on 2026-10-03
 * — and a rejected tracking-area update blinds the receiver for 25 s or more
 * (22:37 the same evening, 214 m in one straight line).  The firmware cannot
 * shorten either.
 *
 * What it can do is measure the motion it cannot see.  The ECU's road speed
 * is polled about once a second through the fix wait, and the IMU batches
 * its gyro at 26 Hz into a FIFO that runs whenever the unit is awake.
 * Projected onto gravity, the gyro is the vehicle's yaw rate however the
 * unit is mounted; integrated, it is the change of heading.  Speed and
 * heading change, second by second, are the shape of the path — everything
 * but its rotation and scale, which the fixes at either end pin down.  So
 * the record that ends a gap says how the vehicle moved through it (mv=),
 * and the server rebuilds the path and fits it between the two fixes.
 *
 * WHAT IS MEASURED
 * ----------------
 * Heading change, from the gyro.  Every FIFO drain (hw_accel.c) passes its
 * samples here.  Each is projected onto gravity — the accel words batched
 * with them, averaged over about twenty seconds — and integrated into the
 * heading change since the log began, which is noted at every whole second
 * of uptime.  Checked against the car's own records before this was
 * written: over 2,084 one-second pairs from 2026-09-25 to 10-04 the GNSS
 * heading change was -1.0 times the projected gyro (r = -0.63 on single
 * instantaneous gyro readings), the sign a right-handed gyro gives for a
 * clockwise turn with gravity pointing up.  The car's unit sits 19 degrees
 * off level with Y up, so its Z axis alone would have been no use.
 *
 * Speed, from the ECU.  Every reading the K-line poll takes is noted with
 * its time, and a reading for any moment is interpolated between the ones
 * either side.  The poll pauses for a send, so there are holes of a few
 * seconds, which interpolation bridges.  Without an ECU the field carries
 * headings alone and the server takes speeds from the fixes.
 *
 * THE FIELD
 * ---------
 *     mv=<gap>:<v0>:<steps>:<end>
 *
 * gap    tenths of a second between the two fixes, by the device's clock
 * v0     km/h at the first fix, or empty when there is no ECU speed
 * steps  one entry for every whole second after the first fix and before
 *        the second: a speed change and a heading change when v0 is
 *        given, the heading change alone when it is not
 * end    the heading change from the last whole second to the second fix
 *
 * Each change is one character of the base64url alphabet, the value plus
 * 32: "A" is -32, "g" is 0, "_" is 31.  -32 is reserved, and in a speed
 * slot means not measured.  Speeds are whole km/h and headings whole
 * degrees, clockwise.  Each step is the change in the rounded running
 * value, so rounding never accumulates; a change beyond +/-31 in one second
 * (a hard stop, a tight corner) carries into the next.  Two bytes a second:
 * the 3-4 s gap between batches on most cells costs 15-17 bytes and a 12 s
 * one 33, about 1.5 KB per ten minutes of driving either way.  The batch's
 * byte budget leaves them out (data_motion_bytes), so they never make a
 * batch go early, and send_data() drops them before it would drop a record.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "app.h"

LOG_MODULE_REGISTER(motion, CONFIG_APP_LOG_LEVEL);

/* The gyro at +/-250 dps (hw_accel.c). */
#define DPS_PER_LSB      0.00875f
/* The FIFO's batch rate, 26 Hz, as a period in ms.  Each gyro word is the
 * rate at one batch instant, so each stands for one period of rotation. */
#define SAMPLE_MS        (1000.0f / 26.0f)
/* More time since the last drain than this many periods per word, plus one,
 * and some words were never batched or never read: the log has a hole. */
#define SPACING_MAX      1.6f
/* Gravity is a running mean of the accel words: a plain mean until it has
 * this many, an exponential one after (about 20 s at 26 Hz).  A sustained
 * 0.2 g of braking tilts it by a degree or two, which costs nothing. */
#define GRAVITY_N        512
/* Heading change at each whole second of uptime, newest last.  A minute
 * covers the longest gap a field is built for. */
#define HEAD_SLOTS       64
/* ECU speed readings: the poll takes about one a second, the keep-alive one
 * every three. */
#define SPEED_SLOTS      96
/* How far a speed reading may be stretched to a moment it was not taken at:
 * between two readings, and past the last one or before the first. */
#define SPEED_SPAN_MS    6000
#define SPEED_HOLD_MS    2500

static float   s_grav[3];        /* accel LSB */
static int     s_grav_n;

static int64_t s_last_ms;        /* newest sample; 0 while there is no log */
static float   s_psi;            /* heading change at s_last_ms, degrees */
static int64_t s_from_ms;        /* the log is unbroken from here */
static float   s_from_psi;

static float   s_head[HEAD_SLOTS];
static int64_t s_head_sec;       /* the newest second held */
static int     s_head_n;

static struct {
	int64_t ms;
	int16_t kmh;
} s_spd[SPEED_SLOTS];
static int s_spd_n;
static int s_spd_next;

/* base64url, value + 32; s_alpha[0] (-32) is a speed not measured. */
static const char s_alpha[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static char enc(int v)
{
	return s_alpha[v + 32];
}

static int clamp31(int v)
{
	return v > 31 ? 31 : (v < -31 ? -31 : v);
}

void motion_reset(void)
{
	s_last_ms = 0;
	s_head_n = 0;
}

static bool have_second(int64_t sec)
{
	return s_head_n > 0 && sec <= s_head_sec &&
	       s_head_sec - sec < s_head_n;
}

static float head(int64_t sec)
{
	return s_head[sec % HEAD_SLOTS];
}

static void push_second(int64_t sec, float psi)
{
	if (s_head_n > 0 && sec != s_head_sec + 1) {
		s_head_n = 0;           /* cannot happen inside an unbroken log */
	}
	s_head[sec % HEAD_SLOTS] = psi;
	s_head_sec = sec;
	if (s_head_n < HEAD_SLOTS) {
		s_head_n++;
	}
	if (s_head_n == HEAD_SLOTS) {
		/* The oldest second held is as far back as the log now reaches. */
		int64_t oldest = s_head_sec - HEAD_SLOTS + 1;

		s_from_ms = oldest * 1000;
		s_from_psi = head(oldest);
	}
}

void motion_feed(const int16_t (*xl)[3], int nxl,
		 const int16_t (*gy)[3], int ngy,
		 const int bias[3], int64_t now_ms, bool lost)
{
	if (ngy <= 0) {
		return;
	}

	for (int i = 0; i < nxl; i++) {
		if (s_grav_n < GRAVITY_N) {
			s_grav_n++;
		}
		float k = 1.0f / (float)s_grav_n;

		for (int a = 0; a < 3; a++) {
			s_grav[a] += ((float)xl[i][a] - s_grav[a]) * k;
		}
	}

	float gn = sqrtf(s_grav[0] * s_grav[0] + s_grav[1] * s_grav[1] +
			 s_grav[2] * s_grav[2]);

	if (s_grav_n == 0 || gn < 1.0f) {
		return;                 /* no accel yet: nothing to project onto */
	}

	float u[3] = { s_grav[0] / gn, s_grav[1] / gn, s_grav[2] / gn };

	/* Unbroken unless the FIFO overran (or more words were batched than
	 * the drain could keep), or more time has passed since the last drain
	 * than these words account for: then something stopped the batching,
	 * and the log starts again from them. */
	bool unbroken = s_last_ms != 0 && !lost &&
		(float)(now_ms - s_last_ms) <=
			SAMPLE_MS * ((float)ngy * SPACING_MAX + 1.0f);

	if (!unbroken) {
		LOG_DBG("heading log starts again: %s, %d words, %lld ms since "
			"the last drain", lost ? "FIFO overran" : "batching stopped",
			ngy, s_last_ms ? now_ms - s_last_ms : -1LL);
		s_last_ms = now_ms - (int64_t)lroundf(SAMPLE_MS * (float)ngy);
		s_from_ms = s_last_ms;
		s_from_psi = s_psi;
		s_head_n = 0;
	}

	/* When each word was batched, for placing the whole seconds: spread
	 * evenly up to now, the newest having gone in at most a period ago.
	 * A few milliseconds either way; the angle does not depend on it. */
	int64_t base = s_last_ms;
	int64_t span = now_ms > base ? now_ms - base : ngy;

	for (int i = 0; i < ngy; i++) {
		int64_t t = base + span * (i + 1) / ngy;

		float wx = (float)(gy[i][0] - bias[0]);
		float wy = (float)(gy[i][1] - bias[1]);
		float wz = (float)(gy[i][2] - bias[2]);
		/* About gravity, which points up: positive is anticlockwise
		 * seen from above, so a clockwise heading change is minus. */
		float rate = -(wx * u[0] + wy * u[1] + wz * u[2]) * DPS_PER_LSB;
		float dpsi = rate * SAMPLE_MS / 1000.0f;

		if (t > s_last_ms) {
			float dt = (float)(t - s_last_ms);

			for (int64_t sec = s_last_ms / 1000 + 1; sec * 1000 <= t;
			     sec++) {
				push_second(sec, s_psi + dpsi *
					    (float)(sec * 1000 - s_last_ms) / dt);
			}
			s_last_ms = t;
		}
		s_psi += dpsi;
	}
}

void motion_note_speed(int kmh)
{
	s_spd[s_spd_next].ms = k_uptime_get();
	s_spd[s_spd_next].kmh = (int16_t)kmh;
	s_spd_next = (s_spd_next + 1) % SPEED_SLOTS;
	if (s_spd_n < SPEED_SLOTS) {
		s_spd_n++;
	}
}

/* Heading change since the log began at uptime t. */
static bool heading_at(int64_t t, float *psi)
{
	if (s_last_ms == 0 || t < s_from_ms) {
		return false;
	}
	if (t >= s_last_ms) {
		/* The drain that brought the log up to date came after the fix
		 * it is asked about, so this is a few milliseconds at most. */
		if (t - s_last_ms > 250) {
			return false;
		}
		*psi = s_psi;
		return true;
	}

	int64_t sec = t / 1000;
	int64_t lt, rt;
	float lp, rp;

	if (have_second(sec) && sec * 1000 >= s_from_ms) {
		lt = sec * 1000;
		lp = head(sec);
	} else {
		lt = s_from_ms;
		lp = s_from_psi;
	}
	if (have_second(sec + 1)) {
		rt = (sec + 1) * 1000;
		rp = head(sec + 1);
	} else {
		rt = s_last_ms;
		rp = s_psi;
	}
	if (rt <= lt) {
		*psi = lp;
		return true;
	}
	*psi = lp + (rp - lp) * (float)(t - lt) / (float)(rt - lt);
	return true;
}

/* ECU road speed at uptime t, in km/h. */
static bool speed_at(int64_t t, float *kmh)
{
	bool before = false, after = false;
	int64_t t0 = 0, t1 = 0;
	int v0 = 0, v1 = 0;

	/* Newest first: the first reading at or before t ends the search, and
	 * the last one seen after it is the earliest after it. */
	for (int i = 0; i < s_spd_n; i++) {
		int k = (s_spd_next - 1 - i + SPEED_SLOTS) % SPEED_SLOTS;

		if (s_spd[k].ms > t) {
			t1 = s_spd[k].ms;
			v1 = s_spd[k].kmh;
			after = true;
		} else {
			t0 = s_spd[k].ms;
			v0 = s_spd[k].kmh;
			before = true;
			break;
		}
	}

	if (before && after && t1 - t0 <= SPEED_SPAN_MS) {
		*kmh = (float)v0 + (float)(v1 - v0) * (float)(t - t0) /
		       (float)(t1 - t0);
	} else if (before && t - t0 <= SPEED_HOLD_MS) {
		*kmh = (float)v0;
	} else if (after && t1 - t <= SPEED_HOLD_MS) {
		*kmh = (float)v1;
	} else {
		return false;
	}
	return true;
}

int motion_field(char *out, int max, int64_t from_ms, int64_t to_ms,
		 bool moving)
{
	int64_t gap = to_ms - from_ms;

	if (gap < DR_MIN_GAP_MS || gap > DR_MAX_GAP_MS) {
		return 0;
	}

	/* The whole seconds after the first fix and before the second. */
	int n = (int)((gap - 1) / 1000);
	float p0, p, vf;

	if (!heading_at(from_ms, &p0)) {
		return 0;
	}

	char steps[2 * (DR_MAX_GAP_MS / 1000)];
	int len = 0;
	bool speeds = speed_at(from_ms, &vf);
	int v0 = speeds ? (int)lroundf(vf) : 0;
	int ev = v0;            /* the speed as the field has it so far */
	int eh = 0;             /* and the heading change */

	if (speeds && v0 >= DR_MOVING_KMH) {
		moving = true;
	}

	for (int k = 1; k <= n; k++) {
		int64_t t = from_ms + (int64_t)k * 1000;

		if (!heading_at(t, &p)) {
			return 0;
		}
		if (speeds) {
			if (speed_at(t, &vf)) {
				int v = (int)lroundf(vf);
				int d = clamp31(v - ev);

				ev += d;
				steps[len++] = enc(d);
				if (v >= DR_MOVING_KMH) {
					moving = true;
				}
			} else {
				steps[len++] = s_alpha[0];
			}
		}

		int d = clamp31((int)lroundf(p - p0) - eh);

		eh += d;
		steps[len++] = enc(d);
	}
	if (!heading_at(to_ms, &p)) {
		return 0;
	}
	if (!moving) {
		/* Standing still: the two fixes are the whole story. */
		return 0;
	}

	char head_txt[24];
	int hl = speeds
		? snprintf(head_txt, sizeof(head_txt), ",mv=%d:%d:",
			   (int)((gap + 50) / 100), v0)
		: snprintf(head_txt, sizeof(head_txt), ",mv=%d::",
			   (int)((gap + 50) / 100));
	int total = hl + len + 2;

	if (hl <= 0 || total > max) {
		return 0;
	}
	memcpy(out, head_txt, (size_t)hl);
	memcpy(out + hl, steps, (size_t)len);
	out[hl + len] = ':';
	out[hl + len + 1] = enc(clamp31((int)lroundf(p - p0) - eh));
	return total;
}

int motion_strip(char *buf, int len)
{
	int w = 0;

	for (int r = 0; r < len;) {
		if (buf[r] == ',' && r + 4 <= len &&
		    memcmp(&buf[r], ",mv=", 4) == 0) {
			r += 4;
			while (r < len && buf[r] != ',' && buf[r] != '\n') {
				r++;
			}
			continue;
		}
		buf[w++] = buf[r++];
	}
	return w;
}
