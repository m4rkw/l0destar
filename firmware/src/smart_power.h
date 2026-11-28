/*
 * Intelligent power saving: whether a parked unit puts off a timed report.
 * Pure decisions, so they are checked on the host: tests/test_smart_power.py.
 *
 * A report from a weak cell costs several times a normal one: on the bench a
 * parked PSM-resume report took 53-126 mC at -102..-113 dBm RSRP, 146-595 mC
 * at -116..-120 and up to 1051 mC below that.  A timed report is put off to
 * the next wake when all of these hold: the signal is that weak, the car
 * battery is low enough for the saving to matter, the unit has not moved since
 * the last report it sent, nothing else is waiting to go, and the next wake
 * still falls inside the longest time allowed between reports.
 *
 * The signal: a modem in PSM has no AT%XMONITOR reading to give, so the figure
 * is the stored one, read with the last report sent from where the unit stands.
 * Where that alone would put the report off, one neighbour-cell measurement is
 * made first (APP_SMART_POWER_NCELL), so that a signal that has improved sends
 * it.  A reading taken on this wake replaces the stored one, here and for later
 * wakes; with none, the stored one stands.
 *
 * A report also goes when the battery reads a set amount below what it did at
 * the last one sent, which keeps a draining battery visible to the server and
 * its parked drain alert.
 *
 * Anything unknown (no report sent yet, no signal with it, no accelerometer, no
 * battery reading) means send.  Only the drop rule is skipped where the reading
 * at the last report is unknown.
 */
#ifndef SMART_POWER_H
#define SMART_POWER_H

#include <stdbool.h>
#include <stdint.h>

/* What is waiting to go, any of which sends the report. */
#define SMART_PENDING_ALERT   (1u << 0)  /* queued, unanswered or about to be raised */
#define SMART_PENDING_RECORDS (1u << 1)  /* backlog, unanswered or unsent records */
#define SMART_PENDING_LOG     (1u << 2)  /* captured lines not held for signal (log_hold.h) */
#define SMART_PENDING_FIELDS  (1u << 3)  /* boot fields, a command ack, a change to report */
#define SMART_PENDING_WORK    (1u << 4)  /* an owed or failed send, a command, FOTA, a fix wanted */

/* Why: SMART_DEFER, or the first reason found to send. */
enum smart_power_why {
	SMART_DEFER = 0,
	SMART_SEND_OFF,		/* the feature is off */
	SMART_SEND_NOT_TIMED,	/* not a plain interval report */
	SMART_SEND_PENDING,	/* in.pending */
	SMART_SEND_NO_REFERENCE,/* no report sent in this stay, or no signal with it */
	SMART_SEND_MOVED,	/* moved since, or no accelerometer to say */
	SMART_SEND_BATTERY,	/* battery above the threshold, unread, or on backup */
	SMART_SEND_BATTERY_DROP,/* battery down by in.batt_drop_mv since the report */
	SMART_SEND_SIGNAL,	/* signal better than the threshold */
	SMART_SEND_CAP,		/* the next wake would be too long after the last report */
};

/* One parked stay. */
struct smart_power {
	bool    have_ref;	/* the last report went, and a signal is known */
	bool    moved;		/* the accelerometer has woken the unit since */
	int64_t sent_ms;	/* when it went */
	int     rsrp_dbm;	/* the signal it went at, or measured since */
	int     ref_batt_mv;	/* the battery it went at; 0 or less: unknown */
};

struct smart_power_in {
	bool     enabled;
	bool     timed_only;	/* the interval timer and nothing else */
	uint32_t pending;	/* SMART_PENDING_* */
	bool     imu_ok;
	bool     on_backup;
	int      batt_mv;	/* 0 or less: no reading */
	bool     fresh_valid;	/* a signal reading taken on this wake */
	int      fresh_rsrp_dbm;
	int64_t  now_ms;
	int      next_interval_s;	/* the wait if this one is put off */
	int      rsrp_thr_dbm;	/* weak at or below */
	int      batt_thr_mv;	/* low at or below */
	int      batt_drop_mv;	/* send once this far down; 0: off */
	int      max_defer_s;	/* longest between reports */
};

/* A new parked stay, or one the accelerometer has not watched throughout. */
static inline void smart_power_reset(struct smart_power *s)
{
	*s = (struct smart_power){ 0 };
}

/* A report is about to be built: what was known of the last one is over.
 * Movement from here on counts against the new one. */
static inline void smart_power_report_start(struct smart_power *s)
{
	smart_power_reset(s);
}

/* The modem took it, and these were the signal and the battery.  (Whether the
 * server answered is seen later, in the pending records.) */
static inline void smart_power_report_sent(struct smart_power *s, int64_t now_ms,
					   int rsrp_dbm, int batt_mv)
{
	s->have_ref = true;
	s->sent_ms = now_ms;
	s->rsrp_dbm = rsrp_dbm;
	s->ref_batt_mv = batt_mv;
}

static inline void smart_power_moved(struct smart_power *s)
{
	s->moved = true;
}

static inline enum smart_power_why
smart_power_decide(const struct smart_power *s, const struct smart_power_in *in)
{
	if (!in->enabled) {
		return SMART_SEND_OFF;
	}
	if (!in->timed_only || in->next_interval_s <= 0) {
		return SMART_SEND_NOT_TIMED;
	}
	if (in->pending != 0) {
		return SMART_SEND_PENDING;
	}
	if (!s->have_ref) {
		return SMART_SEND_NO_REFERENCE;
	}
	if (s->moved || !in->imu_ok) {
		return SMART_SEND_MOVED;
	}
	if (in->on_backup || in->batt_mv <= 0 || in->batt_mv > in->batt_thr_mv) {
		return SMART_SEND_BATTERY;
	}
	if (in->batt_drop_mv > 0 && s->ref_batt_mv > 0 &&
	    s->ref_batt_mv - in->batt_mv >= in->batt_drop_mv) {
		return SMART_SEND_BATTERY_DROP;
	}
	if ((in->fresh_valid ? in->fresh_rsrp_dbm : s->rsrp_dbm) >
	    in->rsrp_thr_dbm) {
		return SMART_SEND_SIGNAL;
	}
	if (in->now_ms - s->sent_ms + (int64_t)in->next_interval_s * 1000 >
	    (int64_t)in->max_defer_s * 1000) {
		return SMART_SEND_CAP;
	}
	return SMART_DEFER;
}

/* Whether the report is put off on the stored signal alone, so that a reading
 * taken now could change that: it is weak and everything else says defer.  Log
 * lines waiting are set aside here: whether a weak reading would hold them is
 * for the caller (log_hold.h). */
static inline bool smart_power_hangs_on_signal(const struct smart_power *s,
					       const struct smart_power_in *in)
{
	struct smart_power_in stored = *in;

	stored.fresh_valid = false;
	stored.pending &= ~SMART_PENDING_LOG;
	return smart_power_decide(s, &stored) == SMART_DEFER;
}

/* A reading taken between reports, from where the last one went: it stands for
 * later wakes.  The longest gap still counts from the report. */
static inline void smart_power_measured(struct smart_power *s, int rsrp_dbm)
{
	if (s->have_ref && !s->moved) {
		s->rsrp_dbm = rsrp_dbm;
	}
}

#endif /* SMART_POWER_H */
