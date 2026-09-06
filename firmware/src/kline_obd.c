/*
 * OBD-II telemetry and fault codes over the K wire.
 *
 * This is the application layer: hw_kline.c owns the wire (5-baud init,
 * UARTE, request framing) and this file decides what to ask for, converts
 * the answers to engineering units, and formats them for the telemetry
 * record.  Discovery lives in hw_kline.c too and is a separate one-shot
 * operation; nothing here probes.
 *
 * SESSION LIFETIME
 * ----------------
 * Opening a session costs a 5-baud address: 2.4 s with the bus held dominant
 * for 200 ms at a stretch.  Doing that once per telemetry cycle would be the
 * single worst thing this firmware could do to the vehicle's diagnostic bus,
 * so the session is opened once and held for the drive.  Any request resets
 * the ECU's P3 timer (5 s), so a tracker cycling every 2-5 s keeps it alive
 * for free with no TesterPresent.  If the cycle does run long the ECU drops
 * the session, the next request times out, and obd_poll() reopens once and
 * retries — self-correcting, with no keep-alive timer to get wrong.
 *
 * The session is closed on ignition-off and before sleep, which also drops
 * the K rails so a hung or reset MCU can never hold the wire dominant.
 *
 * SUPPORTED PIDS
 * --------------
 * The first poll of each session reads mode 01 PID 00, the support bitmap,
 * and everything after that only asks for PIDs the ECU claims.  On the
 * reference vehicle that is fifteen of them and nothing above 0x15; asking
 * blind would waste ~100 ms per unsupported PID, since an ECU that does not
 * implement a PID simply says nothing.
 *
 * SCALING
 * -------
 * Values go into the telemetry record as integers with a fixed scale, so the
 * packet never carries a decimal point and the firmware needs no float
 * formatting.  The server divides them back (see OBD_FIELDS in main.py).
 */

#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "app.h"

LOG_MODULE_REGISTER(kline_obd, CONFIG_APP_LOG_LEVEL);

#define OBD_NA        INT32_MIN         /* field not read this cycle */
#define OBD_MAX_DTCS  12

/* P3max is 5 s: idle longer than that between requests and the ECU drops the
 * session.  Send something at 3 s to stay clear of it. */
#define OBD_KEEPALIVE_MS       3000

/* Reopening costs a 5-baud init: 2.4 s with the bus held dominant.  If the
 * session keeps dying — a vehicle whose ECU enforces P3 strictly while sends
 * run long, say — reopening every cycle would be exactly the re-init storm
 * this design exists to avoid.  Allow a couple in a minute, then stand off
 * for a minute and simply report no OBD data for those records. */
#define OBD_REOPEN_WINDOW_MS   60000
#define OBD_REOPEN_MAX         2
#define OBD_REOPEN_BACKOFF_MS  60000

/* A fault-code read that fails is retried, because the count watch only fires
 * on a change and would not re-arm on its own.  Space the attempts out: the
 * trigger is checked every time round the main loop, so an unthrottled retry
 * is a request per iteration for as long as the fault persists. */
#define OBD_DTC_RETRY_MS       30000

/* How long engine RPM and vehicle speed stay usable for the tracker's own
 * decisions after the last successful read.  Long enough to ride out a
 * session reopen, short enough that a dead ECU falls back to the GNSS and
 * voltage proxies rather than steering on a stale number. */
#define OBD_STATE_MAX_AGE_MS   15000

/* Mode 01 PIDs this firmware knows how to decode. */
#define PID_STATUS    0x01
#define PID_FUEL_SYS  0x03
#define PID_LOAD      0x04
#define PID_COOLANT   0x05
#define PID_STFT      0x06
#define PID_LTFT      0x07
#define PID_RPM       0x0C
#define PID_SPEED     0x0D
#define PID_TIMING    0x0E
#define PID_INTAKE    0x0F
#define PID_MAF       0x10
#define PID_THROTTLE  0x11

static bool     s_open;                 /* session believed to be up */
static bool     s_pids_known;
static uint8_t  s_pids[4];              /* mode 01 PID 00 support bitmap */
static bool     s_abort;                /* ignition went off mid-poll */

/* RPM accumulated by obd_sample_tick() between telemetry records. */
static int32_t  s_rpm_min = INT32_MAX, s_rpm_max = INT32_MIN;
static int64_t  s_rpm_sum;
static int32_t  s_rpm_n;

/* Stored-code count as last seen in mode 01 PID 01, and whether it has moved
 * since the last report was sent. */
static int32_t  s_dtc_seen = -1;
static bool     s_dtc_pending;
static bool     s_dtc_reported;         /* a report got through this key-on */
static int64_t  s_dtc_retry_after;      /* uptime before which not to retry */

/* Latest engine RPM and vehicle speed, for the tracker's own logic rather
 * than for the telemetry record.  Kept separate from the per-record snapshot
 * because they are read on their own cadence by obd_keepalive(). */
static int32_t  s_state_rpm = OBD_NA;
static int32_t  s_state_speed = OBD_NA;
static int64_t  s_state_ms;

/* Last request of any kind, for the keep-alive; and the reopen rate limit. */
static int64_t  s_last_req_ms;
static int64_t  s_reopen_window_ms;
static int      s_reopen_count;
static int64_t  s_reopen_block_until;
static bool     s_reopened_this_poll;

/* Ignition is active-low: 0 means on.  Nothing on the K wire works with it
 * off — the ECU is unpowered — so every entry point checks this first, and a
 * poll already in flight abandons the rest of its PIDs rather than spending
 * a timeout on each. */
void obd_keepalive(void);   /* defined below; used by the sampler tick */

static bool obd_ignition_on(void)
{
    return ignition_read() == 0;
}

static void obd_rpm_reset(void)
{
    s_rpm_min = INT32_MAX;
    s_rpm_max = INT32_MIN;
    s_rpm_sum = 0;
    s_rpm_n   = 0;
}

static bool pid_supported(uint8_t pid)
{
	if (!s_pids_known || pid < 1 || pid > 32) {
		return false;
	}

	int i = pid - 1;

	return (s_pids[i / 8] & (0x80 >> (i % 8))) != 0;
}

void obd_close(void)
{
	if (s_open) {
		if (obd_ignition_on()) {
			kline_session_close();
		} else {
			/* Ignition off: the ECU will not answer StopCommunication,
			 * so drop the line rather than wait for a reply that
			 * cannot come. */
			kline_session_abort();
		}
		s_open = false;
	}
	s_pids_known = false;
	obd_rpm_reset();
	/* s_abort is deliberately NOT cleared here: obd_close() is part of the
	 * abort path, and clearing it would undo the flag one line after it
	 * was set.  obd_poll() clears it when a new cycle starts. */
	if (!obd_ignition_on()) {
		/* End of a drive: the next key-on wants its own report and its
		 * own count baseline. */
		s_dtc_reported = false;
		s_dtc_seen = -1;
		s_dtc_retry_after = 0;
		s_state_rpm = s_state_speed = OBD_NA;
		s_state_ms = 0;
	}
	/* s_reopened_this_poll deliberately survives: obd_close() is part of
	 * the reopen path and the flag is what records that it happened. */
}

/* Open a session if one is not already up, and learn what the ECU supports. */
static int obd_ensure_session(void)
{
	if (s_open) {
		return 0;
	}
	if (!obd_ignition_on()) {
		return -ESHUTDOWN;
	}

	int64_t now = k_uptime_get();

	if (now < s_reopen_block_until) {
		return -EAGAIN;		/* standing off after repeated drops */
	}
	if (now - s_reopen_window_ms > OBD_REOPEN_WINDOW_MS) {
		s_reopen_window_ms = now;
		s_reopen_count = 0;
	}
	if (++s_reopen_count > OBD_REOPEN_MAX) {
		s_reopen_block_until = now + OBD_REOPEN_BACKOFF_MS;
		LOG_WRN("session dropped %d times in a minute — standing off %d s",
			s_reopen_count, OBD_REOPEN_BACKOFF_MS / 1000);
		return -EAGAIN;
	}

	int err = kline_session_open();

	if (err) {
		return err;
	}
	s_open = true;
	s_last_req_ms = k_uptime_get();

	uint8_t buf[8];
	int n = kline_obd_pid(0x00, buf, sizeof(buf));

	if (n >= 4) {
		memcpy(s_pids, buf, 4);
		s_pids_known = true;
		LOG_INF("OBD session open, PID support %02X %02X %02X %02X",
			s_pids[0], s_pids[1], s_pids[2], s_pids[3]);
	} else {
		/* No support bitmap means we cannot tell what is safe to ask
		 * for.  Rather than guess, treat the session as unusable. */
		LOG_WRN("OBD session open but PID 00 unanswered (%d)", n);
		obd_close();
		return -EPROTO;
	}
	return 0;
}

/* One PID read, reopening the session once if it has timed out.  Returns the
 * number of data bytes, or negative. */
static int obd_read(uint8_t pid, uint8_t *buf, int max)
{
	int n = kline_obd_pid(pid, buf, max);

	s_last_req_ms = k_uptime_get();

	if (n != -ETIMEDOUT && n != -ENOTCONN) {
		return n;
	}

	/* Silence has two causes and they need opposite responses.  If the
	 * ignition is still on, the session has simply expired and reopening
	 * is right.  If it has gone off, the ECU is unpowered and every
	 * remaining PID would burn its full timeout, so abandon the cycle. */
	if (!obd_ignition_on()) {
		LOG_INF("ignition off mid-poll — abandoning OBD cycle");
		s_abort = true;
		obd_close();
		return -ESHUTDOWN;
	}

	LOG_DBG("OBD PID %02X silent — reopening session", pid);
	s_reopened_this_poll = true;
	obd_close();
	if (obd_ensure_session()) {
		return -ENOTCONN;
	}
	return kline_obd_pid(pid, buf, max);
}

/* Record engine RPM and vehicle speed for the tracker's own logic.  Either
 * may be OBD_NA, meaning "not read this time" — the other still refreshes. */
static void obd_note_state(int32_t rpm, int32_t speed)
{
	bool got = false;

	if (rpm != OBD_NA) {
		s_state_rpm = rpm;
		got = true;
	}
	if (speed != OBD_NA) {
		s_state_speed = speed;
		got = true;
	}
	if (got) {
		s_state_ms = k_uptime_get();
	}
}

static bool obd_state_fresh(void)
{
	return s_state_ms != 0 &&
	       (k_uptime_get() - s_state_ms) <= OBD_STATE_MAX_AGE_MS;
}

int obd_rpm(void)
{
	if (s_state_rpm == OBD_NA || !obd_state_fresh()) {
		return -1;
	}
	return s_state_rpm;
}

int obd_speed_kmh(void)
{
	if (s_state_speed == OBD_NA || !obd_state_fresh()) {
		return -1;
	}
	return s_state_speed;
}

/* Read `pid` and convert it, or leave the field as OBD_NA. */
static void obd_get(uint8_t pid, int32_t *out)
{
	uint8_t b[8];

	*out = OBD_NA;
	if (s_abort || !pid_supported(pid)) {
		return;
	}

	int n = obd_read(pid, b, sizeof(b));

	if (n < 1) {
		return;
	}

	switch (pid) {
	case PID_STATUS:                                  /* A: MIL + count */
		*out = b[0];
		break;
	case PID_FUEL_SYS:
		*out = b[0];
		break;
	case PID_LOAD:                                    /* % x10 */
	case PID_THROTTLE:
		*out = (int32_t)b[0] * 1000 / 255;
		break;
	case PID_COOLANT:                                 /* deg C */
	case PID_INTAKE:
		*out = (int32_t)b[0] - 40;
		break;
	case PID_STFT:                                    /* % x10, signed */
	case PID_LTFT:
		*out = ((int32_t)b[0] - 128) * 1000 / 128;
		break;
	case PID_RPM:                                     /* rpm */
		if (n >= 2) {
			*out = ((int32_t)b[0] * 256 + b[1]) / 4;
		}
		break;
	case PID_SPEED:                                   /* km/h */
		*out = b[0];
		break;
	case PID_TIMING:                                  /* deg x10, signed */
		*out = (int32_t)b[0] * 5 - 640;
		break;
	case PID_MAF:                                     /* g/s x100 */
		if (n >= 2) {
			*out = (int32_t)b[0] * 256 + b[1];
		}
		break;
	default:
		break;
	}
}

static void obd_snapshot_clear(struct obd_snapshot *s)
{
	memset(s, 0, sizeof(*s));
	s->rpm = s->speed = s->coolant = s->intake = OBD_NA;
	s->load = s->throttle = s->timing = s->stft = s->ltft = OBD_NA;
	s->maf = s->fuel_status = s->mil = s->dtc_count = OBD_NA;
	s->rpm_min = s->rpm_max = s->rpm_avg = OBD_NA;
}

/* PID 01 packs the malfunction lamp into bit 7 and the stored-code count
 * into the low seven bits of the same byte. */
static void obd_note_status(int32_t status, struct obd_snapshot *s)
{
	if (status == OBD_NA) {
		return;
	}
	s->mil       = (status & 0x80) ? 1 : 0;
	s->dtc_count = status & 0x7F;

	/* This is the fault-code trigger.  The count is already in hand on
	 * every poll, so a code appearing or clearing mid-drive is visible
	 * immediately and for free — which is why there is no periodic mode
	 * 03 re-read.  Only a change fires; the first reading of a session
	 * just establishes the baseline. */
	if (s_dtc_seen >= 0 && s->dtc_count != s_dtc_seen) {
		LOG_INF("stored DTC count %d -> %d — reading codes",
			s_dtc_seen, s->dtc_count);
		s_dtc_pending = true;
	}
	s_dtc_seen = s->dtc_count;

	/* If the ignition-on read never got through — ECU still booting,
	 * session refused — nothing else would ever report the codes it
	 * already had, because the count watch only fires on a *change*.
	 * Keep asking until one lands. */
	if (IS_ENABLED(CONFIG_APP_KLINE_DTC_REPORT) && !s_dtc_reported) {
		s_dtc_pending = true;
	}
}

int obd_poll(struct obd_snapshot *s)
{
	obd_snapshot_clear(s);

	if (!IS_ENABLED(CONFIG_APP_BOARD_HAS_KLINE)) {
		return -ENODEV;
	}
	if (!obd_ignition_on()) {
		obd_close();
		return -ESHUTDOWN;
	}

	s_abort = false;
	s_reopened_this_poll = false;

	int err = obd_ensure_session();

	if (err) {
		return err;
	}

	int32_t status;

	obd_get(PID_RPM,      &s->rpm);
	obd_get(PID_SPEED,    &s->speed);
	obd_note_state(s->rpm, s->speed);
	obd_get(PID_COOLANT,  &s->coolant);
	obd_get(PID_INTAKE,   &s->intake);
	obd_get(PID_LOAD,     &s->load);
	obd_get(PID_THROTTLE, &s->throttle);
	obd_get(PID_MAF,      &s->maf);
	obd_get(PID_TIMING,   &s->timing);
	obd_get(PID_STFT,     &s->stft);
	obd_get(PID_LTFT,     &s->ltft);
	obd_get(PID_FUEL_SYS, &s->fuel_status);
	obd_get(PID_STATUS,   &status);

	obd_note_status(status, s);

	/* Only a poll that never had to reopen proves the session is healthy.
	 * Clearing the count on any successful poll would defeat the limiter
	 * in precisely the case it exists for: a session that dies every
	 * cycle, reopens, and completes — which would then re-initialise the
	 * bus every cycle forever without ever tripping the limit. */
	if (!s_abort && !s_reopened_this_poll) {
		s_reopen_count = 0;
	}

	/* RPM sampled across the cycle by obd_sample_tick(), which sees the
	 * engine between records rather than only at the instant one is
	 * built.  The instantaneous s->rpm above is kept alongside it. */
	if (s_rpm_n > 0) {
		s->rpm_min = s_rpm_min;
		s->rpm_max = s_rpm_max;
		s->rpm_avg = (int32_t)(s_rpm_sum / s_rpm_n);
	}
	obd_rpm_reset();

	s->valid = (s->rpm != OBD_NA) || (s->speed != OBD_NA) ||
		   (s->coolant != OBD_NA);
	return s->valid ? 0 : -ENODATA;
}

/* Track mode.  The bus is asked twice a second here, so what is asked for
 * matters: the four PIDs that move on a timescale a driver can see are read
 * every call, and the eight that move over seconds or minutes take one slot
 * each in turn — coolant on one record, intake on the next, and so on, so
 * each refreshes about every four seconds.  Unsupported PIDs do not use a
 * slot.  The per-cycle RPM min/max/avg accumulator is not reported: at this
 * cadence the instantaneous figure is the resolution. */
int obd_poll_track(struct obd_snapshot *s)
{
	static const uint8_t slow[] = {
		PID_COOLANT, PID_INTAKE, PID_MAF, PID_TIMING,
		PID_STFT, PID_LTFT, PID_FUEL_SYS, PID_STATUS,
	};
	static unsigned int s_slow_idx;

	obd_snapshot_clear(s);

	if (!IS_ENABLED(CONFIG_APP_BOARD_HAS_KLINE)) {
		return -ENODEV;
	}
	if (!obd_ignition_on()) {
		obd_close();
		return -ESHUTDOWN;
	}

	s_abort = false;
	s_reopened_this_poll = false;

	int err = obd_ensure_session();

	if (err) {
		return err;
	}

	obd_get(PID_RPM,      &s->rpm);
	obd_get(PID_SPEED,    &s->speed);
	obd_note_state(s->rpm, s->speed);
	obd_get(PID_THROTTLE, &s->throttle);
	obd_get(PID_LOAD,     &s->load);

	for (unsigned int tries = 0; tries < ARRAY_SIZE(slow) && !s_abort; tries++) {
		uint8_t pid = slow[s_slow_idx];

		s_slow_idx = (s_slow_idx + 1) % ARRAY_SIZE(slow);
		if (!pid_supported(pid)) {
			continue;
		}

		int32_t v;

		obd_get(pid, &v);
		switch (pid) {
		case PID_COOLANT:  s->coolant = v;     break;
		case PID_INTAKE:   s->intake = v;      break;
		case PID_MAF:      s->maf = v;         break;
		case PID_TIMING:   s->timing = v;      break;
		case PID_STFT:     s->stft = v;        break;
		case PID_LTFT:     s->ltft = v;        break;
		case PID_FUEL_SYS: s->fuel_status = v; break;
		case PID_STATUS:   obd_note_status(v, s); break;
		default: break;
		}
		break;
	}

	if (!s_abort && !s_reopened_this_poll) {
		s_reopen_count = 0;
	}
	obd_rpm_reset();

	s->valid = (s->rpm != OBD_NA) || (s->speed != OBD_NA) ||
		   (s->throttle != OBD_NA);
	return s->valid ? 0 : -ENODATA;
}

/* Sample engine RPM only.  Called about once a second from the GNSS fix wait
 * (gnss_set_tick), which is where most of a telemetry cycle is spent.
 *
 * Deliberately never opens or reopens a session: a 5-baud init takes 2.4 s
 * with the bus held dominant, and doing that inside the GPS wait would be
 * both slow and disruptive.  If there is no session the sample is skipped and
 * the next obd_poll() sorts it out.  A successful read also resets the ECU's
 * P3 timer, so sampling keeps the session alive through a long fix. */
void obd_sample_tick(void)
{
	uint8_t b[4];

	if (!s_open || s_abort || !obd_ignition_on()) {
		return;
	}
	if (!pid_supported(PID_RPM)) {
		/* Nothing to sample, but the wait still has to be bridged. */
		obd_keepalive();
		return;
	}

	int n = kline_obd_pid(PID_RPM, b, sizeof(b));

	s_last_req_ms = k_uptime_get();
	if (n < 2) {
		return;
	}

	int32_t rpm = ((int32_t)b[0] * 256 + b[1]) / 4;

	obd_note_state(rpm, OBD_NA);
	if (rpm < s_rpm_min) {
		s_rpm_min = rpm;
	}
	if (rpm > s_rpm_max) {
		s_rpm_max = rpm;
	}
	s_rpm_sum += rpm;
	s_rpm_n++;
}

bool obd_dtc_pending(void)
{
	return s_dtc_pending && k_uptime_get() >= s_dtc_retry_after;
}

/* Bridge the parts of a cycle where nothing else is talking to the ECU.
 *
 * The RPM sampler covers the GPS fix wait, but the poll happens after the
 * fix and the send and idle that follow it can easily run past P3max — so
 * without this the session would drop between cycles and be re-initialised
 * on the next one, every cycle.  Reading PID 01 is the cheapest request the
 * ECU is guaranteed to answer, and it doubles as the stored-code watch, so
 * the keep-alive costs nothing that was not already wanted. */
void obd_keepalive(void)
{
	if (!obd_ignition_on()) {
		/* Key off.  Drop the session now rather than leave a stale one
		 * for the next key-on to find: obd_ensure_session() would
		 * accept it, every request would time out, and a fault-code
		 * read would come back empty — which the server would take as
		 * "all faults cleared". */
		if (s_open) {
			obd_close();
		}
		return;
	}
	if (s_abort) {
		return;
	}
	if (k_uptime_get() - s_last_req_ms < OBD_KEEPALIVE_MS) {
		return;		/* something else has spoken recently */
	}

	if (s_open) {
		/* Refresh what the tracker actually steers on — is the engine
		 * running, is the vehicle moving — as well as the stored-code
		 * count.  Three exchanges, ~250 ms, every 3 s.  Without this
		 * the engine/speed reading would only refresh when a telemetry
		 * record is built, which in the engine-off state is once every
		 * 30 s: far too stale to notice the engine starting. */
		int32_t rpm = OBD_NA, speed = OBD_NA;

		obd_get(PID_RPM, &rpm);
		obd_get(PID_SPEED, &speed);
		obd_note_state(rpm, speed);
		obd_watch_dtc_count();
		return;
	}

	/* No session.  With telemetry on, obd_poll() opens one at collect
	 * time and there is nothing to do here.  With only fault-code
	 * reporting enabled nothing else ever opens one, so this is what
	 * keeps the stored-code watch armed. */
	if (IS_ENABLED(CONFIG_APP_KLINE_DTC_REPORT) &&
	    !IS_ENABLED(CONFIG_APP_KLINE_TELEMETRY)) {
		obd_watch_dtc_count();
	}
}

/* Read mode 01 PID 01 alone to keep the stored-code count under watch when
 * OBD telemetry is off and nothing else is polling.  Costs one exchange. */
int obd_watch_dtc_count(void)
{
	uint8_t b[4];

	if (!IS_ENABLED(CONFIG_APP_BOARD_HAS_KLINE) || !obd_ignition_on()) {
		return -ESHUTDOWN;
	}

	int err = obd_ensure_session();

	if (err) {
		return err;
	}
	if (!pid_supported(PID_STATUS)) {
		return -ENOTSUP;
	}

	int n = obd_read(PID_STATUS, b, sizeof(b));

	if (n < 1) {
		return n < 0 ? n : -ENODATA;
	}

	int32_t count = b[0] & 0x7F;

	if (s_dtc_seen >= 0 && count != s_dtc_seen) {
		LOG_INF("stored DTC count %d -> %d — reading codes",
			s_dtc_seen, count);
		s_dtc_pending = true;
	}
	s_dtc_seen = count;
	if (IS_ENABLED(CONFIG_APP_KLINE_DTC_REPORT) && !s_dtc_reported) {
		s_dtc_pending = true;	/* see obd_poll(): retry until one lands */
	}
	return 0;
}

/* Append the fields that were actually read, as ",k=v;k=v".  Returns the
 * number of bytes written. */
int obd_append(char *buf, int max, const struct obd_snapshot *s)
{
	static const struct {
		const char *key;
		size_t off;
	} map[] = {
		{ "orpm",  offsetof(struct obd_snapshot, rpm)         },
		{ "ormin", offsetof(struct obd_snapshot, rpm_min)     },
		{ "ormax", offsetof(struct obd_snapshot, rpm_max)     },
		{ "oravg", offsetof(struct obd_snapshot, rpm_avg)     },
		{ "ospd",  offsetof(struct obd_snapshot, speed)       },
		{ "ocl",   offsetof(struct obd_snapshot, coolant)     },
		{ "oit",   offsetof(struct obd_snapshot, intake)      },
		{ "old",   offsetof(struct obd_snapshot, load)        },
		{ "oth",   offsetof(struct obd_snapshot, throttle)    },
		{ "omaf",  offsetof(struct obd_snapshot, maf)         },
		{ "otim",  offsetof(struct obd_snapshot, timing)      },
		{ "ostft", offsetof(struct obd_snapshot, stft)        },
		{ "oltft", offsetof(struct obd_snapshot, ltft)        },
		{ "ofs",   offsetof(struct obd_snapshot, fuel_status) },
		{ "omil",  offsetof(struct obd_snapshot, mil)         },
		{ "odtc",  offsetof(struct obd_snapshot, dtc_count)   },
	};
	int len = 0;
	bool first = true;

	if (!s->valid || max <= 0) {
		return 0;
	}

	for (size_t i = 0; i < ARRAY_SIZE(map); i++) {
		int32_t v = *(const int32_t *)((const char *)s + map[i].off);

		if (v == OBD_NA) {
			continue;
		}

		int n = snprintf(&buf[len], max - len, "%c%s=%d",
				 first ? ',' : ';', map[i].key, v);

		if (n <= 0 || n >= max - len) {
			break;		/* out of room: emit what fits */
		}
		len += n;
		first = false;
	}
	return len;
}

/* Build the fault-code report line, "D,P0133,P0420", or "D," when the ECU
 * has none.  The server treats it as the device's complete current set, so
 * an empty one is meaningful and clears anything it still has active.
 *
 * Returns the line length, or negative if no session could be opened — in
 * which case nothing must be sent, since an empty report would wrongly clear
 * codes that are still stored. */
int obd_dtc_report(char *buf, int max)
{
	uint16_t codes[OBD_MAX_DTCS];

	if (!IS_ENABLED(CONFIG_APP_BOARD_HAS_KLINE)) {
		return -ENODEV;
	}
	if (!obd_ignition_on()) {
		/* The ECU is unpowered, so there is nothing to read and an
		 * empty report would wrongly clear live faults. */
		return -ESHUTDOWN;
	}

	int err = obd_ensure_session();

	if (err) {
		return err;
	}

	/* The stored-code count from mode 01 PID 01 is what makes a silent
	 * mode 03 readable, so make sure one has been taken. */
	if (s_dtc_seen < 0) {
		obd_watch_dtc_count();
	}

	int n = kline_obd_dtcs(0x03, codes, ARRAY_SIZE(codes));

	if (n < 0) {
		/* Silence on mode 03 is ambiguous on its own, which is why it
		 * is treated as a failure by default.  It stops being
		 * ambiguous once the ECU has said how many codes it holds:
		 * some — the reference Toyota among them — simply do not
		 * answer mode 03 when the answer would be "none", and taking
		 * that as a failure means fault reporting never works on them
		 * and retries forever.
		 *
		 * So a timeout counts as an empty set only when PID 01 has
		 * independently reported zero stored codes.  With a non-zero
		 * count, silence is a real read failure and nothing is sent. */
		if (n == -ETIMEDOUT && s_dtc_seen == 0) {
			n = 0;
		} else {
			s_dtc_retry_after = k_uptime_get() + OBD_DTC_RETRY_MS;
			return n;
		}
	}

	int len = snprintf(buf, max, "D,");

	if (len < 0 || len >= max) {
		return -ENOMEM;
	}
	for (int i = 0; i < n; i++) {
		char code[6];

		kline_dtc_string(codes[i], code);

		int w = snprintf(&buf[len], max - len, "%s%s",
				 i ? "," : "", code);

		if (w <= 0 || w >= max - len) {
			break;
		}
		len += w;
	}
	/* Only cleared once the codes have actually been read: a failed read
	 * leaves the trigger armed so the next cycle tries again. */
	s_dtc_pending = false;
	s_dtc_reported = true;
	s_dtc_retry_after = 0;
	LOG_INF("DTC report: %s", buf);
	return len;
}
