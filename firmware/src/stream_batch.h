/*
 * How the drive stream batches its records and paces its accelerometer.  Pure
 * decisions, so they are checked on the host: tests/test_stream_batch.py.
 *
 * Every streamed record used to be a datagram of its own, and on a metered SIM
 * the datagram, not the record, was most of the cost: an envelope and the IP
 * and UDP headers on the way up, and a reply on the way back.  So the stream
 * that runs as fast as it can (no interval limit) puts up to
 * APP_DRIVE_STREAM_BATCH records in a datagram.  A stream under an interval, a
 * second or slower, stays at one record a datagram: there is nothing to save
 * and the live view would only get later.
 *
 * What must not change is how often the live view moves.  A batch is therefore
 * sent as soon as waiting for one more record would stretch the gap between
 * datagrams past APP_DRIVE_STREAM_BATCH_GAP_MS (900 ms), however many records it
 * holds.  At 6 records a second or more that is four records and a datagram
 * every 0.7 s; at the 4 a second the car manages it is two or three, a datagram
 * every 0.6 to 0.8 s; and below 2 a second every record goes as it is built, so a
 * record that takes a second to build is not held back for the next.  The live
 * view moves at least 1.1 times a second in all of them (measured on the bench
 * at 3.8 records a second: 1.5 times).
 */
#ifndef STREAM_BATCH_H
#define STREAM_BATCH_H

#include <stdbool.h>
#include <stdint.h>

/* Records a streamed datagram may hold: the configured number when the stream
 * has no interval limit, else one. */
static inline int stream_batch_max(int interval_s, int configured)
{
	if (interval_s > 0 || configured < 2) {
		return 1;
	}
	return configured;
}

/* A guess at how long the next record will take to build: the longest one in
 * the last second or two.  Some records take much longer than the rest, such as
 * the one that reads the modem's temperature and supply once a second, and
 * nothing says beforehand which one comes next.  On the bench that was 425 ms
 * against 216 for the others, and a guess from the last record alone let a batch
 * wait for it and go a fifth late.  Two buckets of STREAM_PERIOD_WINDOW_MS: the
 * current one and the one before, so a slow record that comes round once a
 * second is always remembered, and a single stall is forgotten within two. */
#define STREAM_PERIOD_WINDOW_MS  1000

struct stream_period {
	int64_t cur, prev;
	int64_t start_ms;
};

/* Notes a record that came `period_ms` after the one before, at `now_ms`. */
static inline void stream_period_note(struct stream_period *s, int64_t now_ms, int64_t period_ms)
{
	if (now_ms - s->start_ms >= STREAM_PERIOD_WINDOW_MS) {
		s->prev = s->cur;
		s->cur = 0;
		s->start_ms = now_ms;
	}
	if (period_ms > s->cur) {
		s->cur = period_ms;
	}
}

static inline int64_t stream_period_est(const struct stream_period *s)
{
	return s->cur > s->prev ? s->cur : s->prev;
}

/* After a record has been built: whether to send what is held, rather than build
 * another first.  `count` records are held, `max` is the most a datagram takes,
 * `since_send_ms` is the time since the last send was decided on (so it holds
 * the send itself and all that goes with it), `period_ms` is a guess at how long
 * the next record will take (stream_period_est), and `max_gap_ms` is the longest
 * the gap between datagrams may be.  Send when the datagram is full, or when the
 * next record would arrive after the gap has run out. */
static inline bool stream_batch_flush(int count, int max, int64_t since_send_ms,
				      int64_t period_ms, int64_t max_gap_ms)
{
	if (count >= max) {
		return true;
	}
	if (period_ms < 1) {
		period_ms = 1;
	}
	return since_send_ms + period_ms > max_gap_ms;
}

/* Whether a reading that is wanted about every `every_ms` is due: `last_ms` is
 * when it was last taken (a large negative number for never).  Zero means every
 * time. */
static inline bool stream_due(int64_t now_ms, int64_t last_ms, int64_t every_ms)
{
	return every_ms <= 0 || now_ms - last_ms >= every_ms;
}

#endif /* STREAM_BATCH_H */
