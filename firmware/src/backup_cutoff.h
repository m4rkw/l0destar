/*
 * When a unit drops the GNSS receiver's backup supply: at a fixed hour while
 * parked (APP_GNSS_BACKUP_OFF_UTC_HOUR), and for the whole of a window the
 * server sets (gw=).  Pure arithmetic, so it is checked on the host:
 * tests/test_backup_cutoff.py.
 */
#ifndef BACKUP_CUTOFF_H
#define BACKUP_CUTOFF_H

#include <stdbool.h>
#include <stdint.h>

/* True if the cut-off, hour_utc:00 UTC on any day, falls between now and the
 * end of a sleep of wake_s seconds.  The unit is not woken at the cut-off
 * itself, so the supply is dropped before that sleep starts.  An hour outside
 * 0..23 means never. */
static inline bool backup_cutoff_due(int64_t now_s, int64_t wake_s, int hour_utc)
{
	if (hour_utc < 0 || hour_utc > 23 || now_s < 0 || wake_s < 0) {
		return false;
	}

	int64_t cut = now_s - now_s % 86400 + (int64_t)hour_utc * 3600;

	if (cut < now_s) {
		cut += 86400;
	}
	return now_s + wake_s >= cut;
}

/* True if now falls inside the window from_utc:00 up to (not including)
 * to_utc:00 UTC, which may wrap midnight (22 to 6).  An hour outside 0..23,
 * or from == to, is no window. */
static inline bool backup_window_contains(int64_t now_s, int from_utc, int to_utc)
{
	if (from_utc < 0 || from_utc > 23 || to_utc < 0 || to_utc > 23 ||
	    from_utc == to_utc || now_s < 0) {
		return false;
	}

	int hour = (int)(now_s % 86400 / 3600);

	return from_utc < to_utc ? (hour >= from_utc && hour < to_utc)
				 : (hour >= from_utc || hour < to_utc);
}

#endif
