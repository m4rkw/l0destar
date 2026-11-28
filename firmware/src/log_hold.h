/*
 * Holding captured log lines back while the signal is weak
 * (CONFIG_APP_LOG_HOLD).  Pure decisions, so they are checked on the host:
 * tests/test_log_hold.py.
 *
 * Every byte costs far more radio energy from a weak cell (bench: a parked
 * report took 60-90 mC above -113 dBm RSRP, 150-1000 mC at -116 and below),
 * so at or below the threshold a record goes without the warnings waiting in
 * dbglog.c.  Errors are never held.  Everything goes once the store is near
 * full, so nothing is lost to the hold, or once its oldest line has waited
 * too long.
 */
#ifndef LOG_HOLD_H
#define LOG_HOLD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The threshold turns the hold on from here down; 0 is off. */
#define LOG_HOLD_RSRP_MAX_DBM (-90)

/* The store is near full from this share of its two rings. */
#define LOG_HOLD_FULL_PCT 75

/* Weak at or below the threshold, on the reading of this moment, else on the
 * one from the last report of an unmoved parked unit (ref).  With neither
 * there is no reading, and nothing is held. */
static inline bool log_hold_weak(int thr_dbm, bool now_valid, int now_dbm,
				 bool ref_valid, int ref_dbm)
{
	if (thr_dbm > LOG_HOLD_RSRP_MAX_DBM) {
		return false;
	}
	if (now_valid) {
		return now_dbm <= thr_dbm;
	}
	return ref_valid && ref_dbm <= thr_dbm;
}

/* Whether the store can still wait: not near full, and its oldest line
 * (oldest_age_ms) younger than max_ms. */
static inline bool log_hold_can_wait(size_t used, size_t cap, int64_t oldest_age_ms,
				     int64_t max_ms)
{
	return used * 100 < cap * LOG_HOLD_FULL_PCT && oldest_age_ms < max_ms;
}

#endif /* LOG_HOLD_H */
