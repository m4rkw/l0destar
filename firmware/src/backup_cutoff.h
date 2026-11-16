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
#include <stdio.h>
#include <string.h>

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

/* -- the server's rule for the unit running on the backup module (gx=) ------
 * KEEP: never drop the supply by window or fixed hour (the default, and gx=off)
 * DEF:  the regular rules, gw= window and fixed hour (gx=def)
 * WINDOW: only the gx= window, no gw= window and no fixed hour.
 * Regular power is not affected by any of them. */
enum backup_gx_mode { BACKUP_GX_KEEP = 0, BACKUP_GX_DEF, BACKUP_GX_WINDOW };

/* True if the supply is to be dropped before a sleep of wake_s seconds that
 * starts at now_s: inside a window, entering one during the sleep, or the
 * fixed hour reached.  gw_* is the regular window (-1 = none), fixed_hour the
 * fixed cut-off (-1 = none), gx_* the window of BACKUP_GX_WINDOW.  The server's
 * explicit hold (gb=0) is not decided here. */
static inline bool backup_drop_due(enum backup_gx_mode mode, int gx_from, int gx_to,
				   int gw_from, int gw_to, int fixed_hour,
				   bool on_backup, int64_t now_s, int64_t wake_s)
{
	if (on_backup) {
		if (mode == BACKUP_GX_KEEP) {
			return false;
		}
		if (mode == BACKUP_GX_WINDOW) {
			return backup_window_contains(now_s, gx_from, gx_to) ||
			       (gx_from >= 0 && gx_from != gx_to &&
				backup_cutoff_due(now_s, wake_s, gx_from));
		}
	}
	return backup_window_contains(now_s, gw_from, gw_to) ||
	       (gw_from >= 0 && gw_from != gw_to &&
		backup_cutoff_due(now_s, wake_s, gw_from)) ||
	       backup_cutoff_due(now_s, wake_s, fixed_hour);
}

/* Takes every "gx=" token (at the start or after a comma) out of cmd, so the
 * rest of the reply is what cmd_run always saw.  Returns true and sets mode
 * and the window (-1 unless BACKUP_GX_WINDOW) from the first valid one:
 * "def", "off" or "<from>-<to>" with hours 0..23 and from != to.  An invalid
 * token is removed and ignored. */
static inline bool backup_gx_take(char *cmd, enum backup_gx_mode *mode, int *from, int *to)
{
	bool got = false;
	char *p = cmd;

	while (*p) {
		char *comma = strchr(p, ',');
		size_t len = comma ? (size_t)(comma - p) : strlen(p);

		if (len >= 3 && strncmp(p, "gx=", 3) == 0) {
			char v[8];
			int a, b;
			char tail;

			if (!got && len - 3 < sizeof(v)) {
				memcpy(v, p + 3, len - 3);
				v[len - 3] = '\0';
				if (strcmp(v, "def") == 0) {
					*mode = BACKUP_GX_DEF;
					*from = *to = -1;
					got = true;
				} else if (strcmp(v, "off") == 0) {
					*mode = BACKUP_GX_KEEP;
					*from = *to = -1;
					got = true;
				} else if (sscanf(v, "%d-%d%c", &a, &b, &tail) == 2 &&
					   a >= 0 && a <= 23 && b >= 0 && b <= 23 && a != b) {
					*mode = BACKUP_GX_WINDOW;
					*from = a;
					*to = b;
					got = true;
				}
			}
			if (comma) {
				memmove(p, comma + 1, strlen(comma + 1) + 1);
				continue;
			}
			if (p > cmd) {
				p[-1] = '\0';
			} else {
				*p = '\0';
			}
			break;
		}
		if (!comma) {
			break;
		}
		p = comma + 1;
	}
	return got;
}

#endif
