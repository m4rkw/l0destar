/*
 * The changes of the GNSS receiver's backup supply (V_BCKP) not yet reported
 * to the server.  A change happens while the unit sleeps (the overnight
 * cut-off) and is only reported by the next record, hours later, so each is
 * kept with when it happened and sent as its age: bk=<state>:<age_s>, several
 * joined by '|', oldest first.  The server files each at the record's time
 * minus the age.  Pure code, checked on the host: tests/test_backup_log.py.
 */
#ifndef BACKUP_LOG_H
#define BACKUP_LOG_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Enough for a cut-off and its return across a few unsent days. */
#define BKLOG_MAX 6

/* "1:4294967|" is the longest an entry gets. */
#define BKLOG_BUF (BKLOG_MAX * 12 + 1)

struct bklog {
	uint8_t n;
	uint8_t state[BKLOG_MAX];
	int64_t at_ms[BKLOG_MAX];
};

/* Note a change.  The same state as the newest entry adds nothing; a full
 * list loses its oldest. */
static inline void bklog_add(struct bklog *l, int state, int64_t now_ms)
{
	if (l->n > 0 && l->state[l->n - 1] == (state != 0)) {
		return;
	}
	if (l->n == BKLOG_MAX) {
		for (int i = 1; i < BKLOG_MAX; i++) {
			l->state[i - 1] = l->state[i];
			l->at_ms[i - 1] = l->at_ms[i];
		}
		l->n--;
	}
	l->state[l->n] = (state != 0);
	l->at_ms[l->n] = now_ms;
	l->n++;
}

/* Write the entries as "<state>:<age_s>|..." into buf.  Returns how many were
 * written, which is how many to bklog_drop() once the text is in a record
 * that is on its way; 0 leaves buf empty.  An entry that does not fit stops
 * the list there, to go with the next record. */
static inline int bklog_format(const struct bklog *l, int64_t now_ms,
			       char *buf, size_t size)
{
	size_t used = 0;
	int done = 0;

	if (size > 0) {
		buf[0] = '\0';
	}
	for (int i = 0; i < l->n; i++) {
		int64_t age = (now_ms - l->at_ms[i]) / 1000;
		char item[24];
		int len;

		if (age < 0) {
			age = 0;
		}
		len = snprintf(item, sizeof(item), "%s%d:%lld", i ? "|" : "",
			       l->state[i], (long long)age);
		if (len < 0 || used + len + 1 > size) {
			break;
		}
		memcpy(buf + used, item, len + 1);
		used += len;
		done++;
	}
	return done;
}

/* Forget the first n entries: they went out. */
static inline void bklog_drop(struct bklog *l, int n)
{
	if (n <= 0) {
		return;
	}
	if (n >= l->n) {
		l->n = 0;
		return;
	}
	for (int i = n; i < l->n; i++) {
		l->state[i - n] = l->state[i];
		l->at_ms[i - n] = l->at_ms[i];
	}
	l->n -= n;
}

#endif
