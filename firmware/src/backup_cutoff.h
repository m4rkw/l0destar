/*
 * When a parked unit drops the GNSS receiver's backup supply
 * (APP_GNSS_BACKUP_OFF_UTC_HOUR).  Pure arithmetic, so it is checked on the
 * host: tests/test_backup_cutoff.py.
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

#endif
