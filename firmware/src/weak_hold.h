/*
 * Weak-signal send hold.  Pure decisions, so they are checked on the host:
 * tests/test_weak_hold.py.
 *
 * The car's firmware has frozen inside a socket send (hardware watchdog,
 * rst=wdt+mc:send) while the serving cell was very weak (-119 dBm RSRP, SNR
 * -8, band 20), just before a cell change or loss of registration.  While the
 * signal is that poor a live send is not handed to the modem: its records wait
 * in the backlog, as those of a shut flow-control window or a send the modem
 * did not take do, and go with the first send after the signal recovers.
 *
 * Weak is RSRP at or below the RSRP threshold (xwr) OR SNR at or below the SNR
 * threshold (xws).  An RSRP threshold of 0 (or anything above -100 dBm) turns
 * the whole hold off, SNR included.  A reading that is not fresh is no reading:
 * not weak, so no hold.  An SNR of 0 is also the modem giving none (app.h), so
 * it never counts as weak by itself.
 *
 * The hold never lasts: after xwm seconds of it one send goes out anyway, so
 * the server is not starved and a different failure is not masked, and for a
 * short cooldown after that the hold stays off so that send can be answered.
 */
#ifndef WEAK_HOLD_H
#define WEAK_HOLD_H

#include <stdbool.h>
#include <stdint.h>

/* The RSRP threshold turns the hold on from here up to this. */
#define WEAK_HOLD_RSRP_MAX_DBM (-100)

/* How old a signal reading may be and still hold a send, ms.  Fresher than the
 * record field's limit (SIGNAL_FRESH_MS): the hold acts on the reading, so a
 * stale "weak" must not keep records back after the vehicle has moved on. */
#define WEAK_HOLD_FRESH_MS 20000

/* Hold off after the one send the cap lets through, ms. */
#define WEAK_HOLD_COOLDOWN_MS 5000

/* While the hold is on, how often the signal is read again, ms (an AT%XMONITOR
 * a record builds, never one per send). */
#define WEAK_HOLD_SAMPLE_MS 10000

struct weak_hold {
	bool    holding;
	int64_t since_ms;	/* the first record held in this spell */
	int64_t free_until_ms;	/* no hold until then (0 = none) */
};

static inline bool weak_hold_enabled(int rsrp_thr_dbm)
{
	return rsrp_thr_dbm <= WEAK_HOLD_RSRP_MAX_DBM;
}

/* True if the reading (valid, taken age_ms ago) is at or below a threshold. */
static inline bool weak_hold_is_weak(bool valid, int64_t age_ms, int rsrp_dbm, int snr_db,
				     int rsrp_thr_dbm, int snr_thr_db)
{
	if (!weak_hold_enabled(rsrp_thr_dbm) || !valid ||
	    age_ms < 0 || age_ms > WEAK_HOLD_FRESH_MS) {
		return false;
	}
	return rsrp_dbm <= rsrp_thr_dbm || (snr_db != 0 && snr_db <= snr_thr_db);
}

/* A send is about to be made at now_ms.  True to hold it back.  `weak` is
 * weak_hold_is_weak(); max_ms is the cap on a hold. */
static inline bool weak_hold_decide(struct weak_hold *h, int64_t now_ms, bool weak, int64_t max_ms)
{
	if (!weak) {
		h->holding = false;
		return false;
	}
	if (h->free_until_ms != 0 && now_ms < h->free_until_ms) {
		return false;
	}
	if (!h->holding) {
		h->holding = true;
		h->since_ms = now_ms;
		return true;
	}
	if (now_ms - h->since_ms >= max_ms) {
		/* The cap: this one goes out. */
		h->holding = false;
		h->free_until_ms = now_ms + WEAK_HOLD_COOLDOWN_MS;
		return false;
	}
	return true;
}

#endif /* WEAK_HOLD_H */
