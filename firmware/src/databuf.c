/*
 * Backlog buffer for telemetry records that could not be sent — or were
 * sent and never answered for.
 *
 * WHY
 * ---
 * A record whose send fails used to be discarded, so a radio outage cost the
 * position data for its whole duration rather than merely delaying it.  On
 * 2026-09-05 that turned a twelve-minute coverage drop into twelve minutes of
 * a hundred-mile drive with no track at all.  The device was up the whole
 * time — uptime advanced normally — so the data existed and was simply thrown
 * away.  This keeps it until the link comes back.
 *
 * A send that "succeeds" can lose it just the same: the modem took the
 * datagram, and then threw it away.  So each send's records are also held
 * here until the server's reply says they arrived — see UNANSWERED below.
 *
 * MEMORY
 * ------
 * Statically allocated, deliberately.  A fixed array is counted by the linker,
 * so a size that does not fit fails the build rather than the device; there is
 * no allocation to fail in a tunnel, and no fragmentation.  Nothing here grows
 * at runtime and there is no path that can consume more than the array.
 *
 * THREADS
 * -------
 * Main thread only.  Nothing here is locked, and no other thread calls it: a
 * thread that has something to say sets a flag for the main thread to act on
 * (gnss_ext.c does this for its alerts).
 *
 * WHEN IT FILLS
 * -------------
 * Dropping the newest record loses the recovery, and dropping the oldest loses
 * the start of the outage.  Neither is what you want from a journey log.  So
 * a full buffer is decimated instead: every second record is discarded and the
 * effective sample interval doubles.  The buffer then covers an outage of any
 * length at progressively coarser resolution rather than truncating it, which
 * for a track is much the better trade.  With the default slot count:
 *
 *     outage      resolution
 *     2.7 min     5 s   (as recorded)
 *     5.3 min     10 s
 *     10.7 min    20 s
 *     21 min      40 s
 *
 * The drive stream builds a record every few hundred milliseconds, and at that
 * rate 32 slots are full in seconds and then thinned again and again.  While
 * it runs a record is kept only if none held was built within
 * APP_DATABUF_STREAM_GAP_S of it, by its own up= second (so records requeued
 * together are not mistaken for dense ones), and that gap doubles with each
 * thinning, as the interval does above.  At 3 s the slots hold 96 s: the
 * car's longest registration losses so far were 76 s and 93 s.
 *
 * DRAINING
 * --------
 * The backlog must never delay live telemetry, so it is drained a datagram or
 * two per cycle alongside the current record rather than in one burst, and
 * each datagram is packed to stay inside the transport's packet limit.
 *
 * FLASH
 * -----
 * With CONFIG_APP_FLASH_BACKLOG the ring above is the front of a longer queue.
 * When it is full the oldest records are moved to the external flash instead of
 * being thinned away, so a long outage keeps every record the stream's spacing
 * asks for (an hour of it is about 1 MB of flash; the log's 2016 sectors hold
 * four and a half to twelve hours).  Draining takes the flash first, oldest
 * first, and then the ring, so the order holds.  The queue in the flash lasts
 * one boot (see flashlog.h for why, and for how the chip is kept from wearing
 * out).
 *
 * Anything that goes wrong with the flash leaves the backlog as it is without
 * one: a push that cannot be spilled thins the ring, and records already in
 * the flash are sent for as long as they can be read.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "app.h"
#include "config.h"

#if IS_ENABLED(CONFIG_APP_FLASH_BACKLOG)
#include "extflash.h"
#include "flashlog.h"
#endif

LOG_MODULE_REGISTER(databuf, CONFIG_APP_LOG_LEVEL);

#define SLOTS    CONFIG_APP_DATABUF_SLOTS
#define REC_MAX  CONFIG_APP_DATABUF_REC_MAX

/* Leave room for the transport envelope (length byte, IMEI, nonce, tag) so a
 * packed batch always fits without transport_send() rejecting it. */
#define BATCH_MAX (UDP_PACKET_SIZE - 64)

static char     s_rec[SLOTS][REC_MAX];
static uint16_t s_len[SLOTS];
static int      s_head;                 /* oldest occupied slot */
static int      s_count;
static int      s_interval_mult = 1;    /* 1, 2, 4 ... after each decimation */
static int      s_push_seq;             /* for decimated sampling */
static uint32_t s_dropped;              /* records lost to decimation */

#if IS_ENABLED(CONFIG_APP_DRIVE_STREAM)
#define STREAM_GAP_S CONFIG_APP_DATABUF_STREAM_GAP_S
static int32_t  s_up[SLOTS];            /* the second each held record was built; -1 unknown */
static bool     s_stream;               /* the drive stream is running */
#define STREAMING s_stream
#else
#define STREAMING false
#endif

/* -- ages -------------------------------------------------------------------
 *
 * The server files a record at the moment it arrives.  For one sent late —
 * from the backlog, or again after its first datagram went unanswered — that
 * is not when it was built, and it landed on the track, and in the journey,
 * after records built later than it.  So a record sent AGE_MARK_S or more
 * after it was built says how long ago that was (age=<s>): the server files
 * it at its own time, and as history rather than as the vehicle's latest
 * state.  The build time is the record's own up= field.  The age is worked
 * out afresh at every send, so a record that goes round twice is never
 * stamped with its first attempt's.  Fresh records carry none, so the live
 * track still reads the time each point reached the server. */
#define AGE_MARK_S 10

/* Where `key` starts inside the first `len` bytes of `s`, or NULL. */
static const char *field_find(const char *s, size_t len, const char *key)
{
    size_t klen = strlen(key);

    for (size_t i = 0; i + klen <= len; i++) {
        if (memcmp(s + i, key, klen) == 0) {
            return s + i;
        }
    }
    return NULL;
}

#if IS_ENABLED(CONFIG_APP_DRIVE_STREAM)
/* The second a record was built, from its up= field; -1 if it has none. */
static int32_t record_up(const char *rec, size_t len)
{
    const char *p = field_find(rec, len, ",up=");
    const char *stop = rec + len;
    int64_t v = 0;
    int digits = 0;

    if (!p) {
        return -1;
    }
    for (p += 4; p < stop && *p >= '0' && *p <= '9' && v <= INT32_MAX; p++) {
        v = v * 10 + (*p - '0');
        digits++;
    }
    return (digits > 0 && v <= INT32_MAX) ? (int32_t)v : -1;
}

/* Whether a held record was built less than `gap` seconds from `up`.  All of
 * them are looked at, not just the newest: records requeued after an unanswered
 * send are older than ones the outage has since put there. */
static bool near_held(int32_t up, int32_t gap)
{
    for (int i = 0; i < s_count; i++) {
        int32_t u = s_up[(s_head + i) % SLOTS];

        if (u >= 0 && (up >= u ? up - u : u - up) < gap) {
            return true;
        }
    }
    return false;
}

void databuf_stream(bool on)
{
    s_stream = on;
}
#endif

/* One record into out: without the age= it may already carry, and with a
 * fresh one when `mark` and it was built AGE_MARK_S or more before now_s.
 * Returns the bytes written, or -1 if they do not fit in cap. */
static int stamp_record(const char *rec, size_t len, char *out, size_t cap,
                        int64_t now_s, bool mark)
{
    size_t skip_at = len, skip_len = 0;
    const char *old = field_find(rec, len, ",age=");

    if (old) {
        const char *end = old + 1;
        const char *stop = rec + len;

        while (end < stop && *end != ',') {
            end++;
        }
        skip_at = (size_t)(old - rec);
        skip_len = (size_t)(end - old);
    }

    size_t body = len - skip_len;

    if (body > cap) {
        return -1;
    }
    memcpy(out, rec, skip_at);
    memcpy(out + skip_at, rec + skip_at + skip_len, len - skip_at - skip_len);

    const char *up = mark ? field_find(rec, len, ",up=") : NULL;

    if (up) {
        /* Bounded by hand: a stored record is not NUL-terminated. */
        const char *q = up + 4;
        const char *stop = rec + len;
        long long built = 0;

        while (q < stop && *q >= '0' && *q <= '9') {
            built = built * 10 + (*q - '0');
            q++;
        }

        long long age = now_s - built;

        if (built > 0 && age >= AGE_MARK_S) {
            char tag[24];
            int n = snprintf(tag, sizeof(tag), ",age=%lld", age);

            if (n <= 0 || body + (size_t)n > cap) {
                return -1;
            }
            memcpy(out + body, tag, (size_t)n);
            body += (size_t)n;
        }
    }
    return (int)body;
}

int databuf_stamp(const char *in, size_t len, char *out, size_t cap)
{
    const char *p = in;
    const char *end = in + len;
    int64_t now_s = k_uptime_get() / 1000;
    size_t used = 0;

    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t rec_len = nl ? (size_t)(nl - p) : (size_t)(end - p);

        if (used > 0) {
            if (used >= cap) {
                return -1;
            }
            out[used++] = '\n';
        }

        int n = stamp_record(p, rec_len, out + used, cap - used, now_s, true);

        if (n < 0) {
            return -1;
        }
        used += (size_t)n;
        if (!nl) {
            break;
        }
        p = nl + 1;
    }
    return (int)used;
}

#if IS_ENABLED(CONFIG_APP_FLASH_BACKLOG)

/* -- the flash tier ---------------------------------------------------------
 *
 * Opened the first time the ring is full: a boot that never has an outage long
 * enough to need it reads nothing from the chip but its ID, once, when it is
 * parked at boot, and a chip that does not answer that is said at once
 * (databuf_flash_report) rather than when it matters. */

#define FL_SECTORS       CONFIG_APP_FLASH_BACKLOG_SECTORS
/* Records moved in one go when the ring is full.  A session has a wake from
 * deep power-down and a pin set-up around it, so it is worth a handful. */
#define FL_SPILL_BATCH   MAX(1, SLOTS / 4)
#define FL_WARN_EVERY_MS 60000

enum fl_state {
    FL_NEW,                 /* not used yet this boot */
    FL_UP,                  /* the log is open */
    FL_NO,                  /* not usable this boot; said why when found out */
};

static enum fl_state   s_fl_state;
static bool            s_fl_noappend;   /* nothing more goes in; what is there comes out */
static bool            s_fl_unreachable;/* the last try found the bus not free */
static bool            s_fl_unreach_said;
static bool            s_fl_gone_said;  /* the chip stopped answering the ID check */
static bool            s_fl_alerted;
static int64_t         s_fl_warn_ms = -FL_WARN_EVERY_MS;
static struct flashlog s_fl;
static uint16_t        s_fl_cnt[FL_SECTORS];
static char            s_fl_rec[REC_MAX];

/* What the last pack took from the flash, for the drop after its send. */
static struct flashlog_cur s_fl_cur;
static bool                s_fl_cur_valid;
static int                 s_fl_packed;

static int fl_pending(void)
{
    return s_fl_state == FL_UP ? (int)flashlog_pending(&s_fl) : 0;
}

/* The same complaint at most once a minute: an outage can be hours. */
static bool fl_warn_due(void)
{
    int64_t now = k_uptime_get();

    if (now - s_fl_warn_ms < FL_WARN_EVERY_MS) {
        return false;
    }
    s_fl_warn_ms = now;
    return true;
}

#if IS_ENABLED(CONFIG_APP_DRIVE_STREAM)
/* The spacing the backlog keeps records at while streaming: the flash's, while
 * there is a flash to hold them, and the ring's own when there is not. */
static int stream_gap_now(void)
{
    const bool flash = s_fl_state != FL_NO && !s_fl_noappend && !s_fl_unreachable;

    return flash ? CONFIG_APP_FLASH_BACKLOG_GAP_S : STREAM_GAP_S;
}
#define GAP_NOW stream_gap_now()
#endif

/* alert.c's message size (private to it); a longer message is cut there. */
#define FL_ALERT_MSG 120

/* Tells the operator, once per boot, that the flash is not being used.  Not
 * for a bus that is only busy, nor for limits that are working as designed. */
static void fl_alert(const char *what)
{
    char msg[FL_ALERT_MSG];

    if (s_fl_alerted) {
        return;
    }
    s_fl_alerted = true;
    snprintf(msg, sizeof(msg), "flash backlog: %s, RAM backlog only", what);
    alert_enqueue(msg, 0);
}

/* The chip's ID, asked for once at boot by extflash_init() and said here, from
 * main.c's startup in the main thread, as the other fitted parts that did not
 * answer are.  A chip that is not there is the same loss as one found missing
 * at the first spill, found at once instead of an outage later, so it is a
 * fault of a fitted part: APP_HW_FAULT_PRIORITY, queued to ride out with the
 * first record.  It counts as the boot's one flash alert, so the first spill
 * does not say it again.  The tier is left as it is: that spill asks the chip
 * again and decides, and a chip that has come back is used.
 *
 * A chip that answers gets one line, so that the console shows the part is
 * there.  Nothing is said when the ID could not be read yet (a bus shared with
 * the CAN controller whose rail has not come up): the first spill finds out, as
 * it always did. */
void databuf_flash_report(void)
{
    uint8_t id[3];
    const int err = extflash_boot_check(id);

    if (err == -EAGAIN) {
        return;
    }
    if (err == 0) {
        LOG_INF("flash backlog: chip ready, ID %02X %02X %02X (MX25R6435F)",
                id[0], id[1], id[2]);
        return;
    }

    LOG_WRN("flash backlog: the chip returned ID %02X %02X %02X at startup, "
            "not C2 28 17 (MX25R6435F)", id[0], id[1], id[2]);
    if (!s_fl_alerted) {
        char msg[FL_ALERT_MSG];

        s_fl_alerted = true;
        snprintf(msg, sizeof(msg), "flash chip not responding (SPI): outage "
                 "backlog is RAM only (ID %02X %02X %02X)", id[0], id[1], id[2]);
        alert_enqueue(msg, CONFIG_APP_HW_FAULT_PRIORITY);
    }
}

/* Inside a session, the first time: finds the chip and the place the ring has
 * got to.  False if the flash is not to be used this boot. */
static bool fl_open(void)
{
    uint32_t sectors = 0;
    int err = extflash_probe(&sectors);

    if (!err) {
        const struct flashlog_cfg cfg = {
            .max_cycles = CONFIG_APP_FLASH_BACKLOG_MAX_CYCLES,
            .max_opens_per_hour = CONFIG_APP_FLASH_BACKLOG_MAX_OPENS_PER_HOUR,
        };

        err = flashlog_init(&s_fl, extflash_dev(sectors), s_fl_cnt, &cfg);
    }
    if (err) {
        s_fl_state = FL_NO;
        LOG_WRN("flash backlog unavailable (%d), RAM only", err);

        char what[40];

        snprintf(what, sizeof(what), "chip not usable (%d)", err);
        fl_alert(what);
        return false;
    }
    s_fl_state = FL_UP;
    /* The one line that says the feature works end to end. */
    LOG_INF("flash backlog: %u sectors, %u erase cycles used so far",
            (unsigned)sectors, (unsigned)flashlog_cycles(&s_fl));
    return true;
}

/* Starts a session on the flash, opening the log the first time.  False, and
 * the chip untouched, when it is not to be used or cannot be reached just now
 * (a bus shared with the CAN controller whose rail is down).  Not reaching it
 * is no verdict on it: it is asked again next time. */
static bool fl_session(void)
{
    if (s_fl_state == FL_NO) {
        return false;
    }

    int err = extflash_begin();

    if (err) {
        s_fl_unreachable = true;
        if (!s_fl_unreach_said) {
            s_fl_unreach_said = true;
            LOG_WRN("flash backlog: the flash cannot be reached now (%d), RAM "
                    "only until it can; on a bus shared with the CAN "
                    "controller that means the CAN rail is down", err);
        }
        return false;
    }
    if (s_fl_state == FL_NEW) {
        s_fl_unreachable = false;
        if (!fl_open()) {
            extflash_end();
            return false;
        }
        return true;
    }
    /* Not a verdict: the chip may be back next time, with its records. */
    if (extflash_check() != 0) {
        extflash_end();
        s_fl_unreachable = true;
        if (!s_fl_gone_said) {
            s_fl_gone_said = true;
            LOG_WRN("flash backlog: the chip does not answer, RAM only until "
                    "it does");
        }
        fl_alert("chip stopped answering");
        return false;
    }
    s_fl_unreachable = false;
    return true;
}

static void fl_error(int err)
{
    switch (err) {
    case FLASHLOG_ERR_RATE:
        if (fl_warn_due()) {
            LOG_WRN("flash backlog: hourly limit on sectors opened reached, "
                    "keeping to RAM until it is up");
        }
        break;
    case FLASHLOG_ERR_WORN:
        s_fl_noappend = true;
        LOG_WRN("flash backlog: erase cycle limit reached, no more written");
        break;
    case FLASHLOG_ERR_DEAD:
        s_fl_noappend = true;
        LOG_WRN("flash backlog: writing stopped after repeated failures");
        fl_alert("writes stopped after repeated failures");
        break;
    default:
        if (fl_warn_due()) {
            LOG_WRN("flash backlog: write failed (%d)", err);
        }
        break;
    }
}

/* Moves the oldest records of the ring into the flash to make room.  True if
 * at least one slot is free; false leaves the caller to thin the ring. */
static bool fl_spill(void)
{
    if (s_fl_noappend || !fl_session()) {
        return false;
    }

    const uint32_t now_s = (uint32_t)(k_uptime_get() / 1000);
    const uint32_t dropped0 = s_fl.st.dropped;
    int moved = 0;
    int err = 0;

    while (moved < FL_SPILL_BATCH && s_count > 0) {
        err = flashlog_append(&s_fl, s_rec[s_head], s_len[s_head], now_s);
        if (err) {
            break;
        }
        s_head = (s_head + 1) % SLOTS;
        s_count--;
        moved++;
        watchdog_kick();
    }
    extflash_end();

    if (s_fl.st.dropped != dropped0) {
        /* The ring of sectors is full: the oldest were overwritten unsent. */
        s_dropped += s_fl.st.dropped - dropped0;
        if (fl_warn_due()) {
            LOG_WRN("flash backlog full: %u oldest records overwritten",
                    (unsigned)(s_fl.st.dropped - dropped0));
        }
    }
    if (err) {
        fl_error(err);
    }
    return moved > 0;
}

/* The oldest records in the flash into out, as databuf_pack() does for the
 * ring.  *drained is whether they are all taken: false when the datagram is
 * full, and the ring's records then wait their turn so that the order holds.
 * A flash that cannot be read just now (the CAN rail it shares a bus with is
 * down) is left for later and counts as drained: the server files a record by
 * its age, so a late one is no worse than one held back behind it. */
static size_t fl_pack(char *out, size_t max, int64_t now_s, int *packed_out,
                      bool *drained)
{
    struct flashlog_cur cur;
    size_t used = 0;
    int packed = 0;

    *packed_out = 0;
    *drained = true;
    s_fl_cur_valid = false;
    s_fl_packed = 0;

    if (s_fl_state != FL_UP || flashlog_pending(&s_fl) == 0) {
        return 0;
    }
    if (!fl_session()) {
        return 0;
    }

    flashlog_cursor(&s_fl, &cur);
    for (;;) {
        struct flashlog_cur next = cur;
        int n = flashlog_next(&s_fl, &next, s_fl_rec, sizeof(s_fl_rec));

        if (n == FLASHLOG_ERR_CRC) {
            cur = next;                     /* unreadable: passed over, counted at commit */
            continue;
        }
        if (n <= 0) {
            break;                          /* none left, or not readable */
        }

        size_t sep = packed ? 1 : 0;

        if (used + sep >= max) {
            *drained = false;
            break;
        }

        int m = stamp_record(s_fl_rec, (size_t)n, &out[used + sep],
                             max - used - sep - 1, now_s, true);

        if (m < 0) {
            *drained = false;
            break;
        }
        if (sep) {
            out[used] = '\n';
        }
        used += sep + (size_t)m;
        packed++;
        cur = next;
    }
    extflash_end();

    if (packed == 0 && cur.n > 0) {
        /* Only records that could not be read: nothing to send, so nothing
         * for a send to commit.  Take them out now or they are read again for
         * ever. */
        const uint32_t lost0 = s_fl.st.lost;

        (void)flashlog_commit(&s_fl, &cur);
        s_dropped += s_fl.st.lost - lost0;
        if (fl_warn_due()) {
            LOG_WRN("flash backlog: %u records could not be read back",
                    (unsigned)(s_fl.st.lost - lost0));
        }
        return 0;
    }

    s_fl_cur = cur;
    s_fl_cur_valid = true;
    s_fl_packed = packed;
    *packed_out = packed;
    return used;
}

/* The records the last pack took from the flash have been sent: out of the
 * queue.  Returns how many of the sent records they were.  A commit that is
 * refused (the ring overwrote the place meanwhile) leaves them to go again,
 * which the server tolerates. */
static int fl_commit(void)
{
    const int sent = s_fl_packed;

    if (s_fl_cur_valid) {
        const uint32_t lost0 = s_fl.st.lost;

        (void)flashlog_commit(&s_fl, &s_fl_cur);
        if (s_fl.st.lost != lost0) {
            s_dropped += s_fl.st.lost - lost0;
            if (fl_warn_due()) {
                LOG_WRN("flash backlog: %u records could not be read back",
                        (unsigned)(s_fl.st.lost - lost0));
            }
        }
        s_fl_cur_valid = false;
    }
    s_fl_packed = 0;
    return sent;
}

/* Everything waiting to be sent: the ring and the flash. */
#define BACKLOG_COUNT() (s_count + fl_pending())

#else

#define BACKLOG_COUNT() s_count

#endif /* CONFIG_APP_FLASH_BACKLOG */

#if IS_ENABLED(CONFIG_APP_DRIVE_STREAM) && !IS_ENABLED(CONFIG_APP_FLASH_BACKLOG)
#define GAP_NOW STREAM_GAP_S
#endif

int databuf_count(void)
{
    return BACKLOG_COUNT();
}

int databuf_ring_count(void)
{
    return s_count;
}

uint32_t databuf_dropped(void)
{
    return s_dropped;
}

void databuf_reset(void)
{
    s_head = 0;
    s_count = 0;
    s_interval_mult = 1;
    s_push_seq = 0;
}

/* Halve the contents, keeping every second record starting with the oldest.
 * Order is preserved and the span covered is unchanged; only the resolution
 * drops. */
static void databuf_decimate(void)
{
    int kept = 0;

    for (int i = 0; i < s_count; i += 2) {
        int src = (s_head + i) % SLOTS;
        int dst = (s_head + kept) % SLOTS;

        if (src != dst) {
            memcpy(s_rec[dst], s_rec[src], s_len[src]);
            s_len[dst] = s_len[src];
#if IS_ENABLED(CONFIG_APP_DRIVE_STREAM)
            s_up[dst] = s_up[src];
#endif
        }
        kept++;
    }

    s_dropped += (uint32_t)(s_count - kept);
    s_count = kept;
    s_interval_mult *= 2;
    LOG_WRN("backlog full — thinned to %d records, now every %d th",
            s_count, s_interval_mult);
}

/* 0 if the record was taken (or thinned away by the sample interval, which
 * counts as taken), 1 if the stream's spacing left it out, negative if it does
 * not fit a slot. */
int databuf_push(const char *rec, size_t len)
{
    if (len == 0 || len >= REC_MAX) {
        /* Too long to store is a bug rather than a condition to handle
         * quietly: REC_MAX is meant to exceed the longest record. */
        LOG_ERR("record of %u bytes does not fit a %d byte slot — dropped",
                (unsigned)len, REC_MAX);
        s_dropped++;
        return -EMSGSIZE;
    }

#if IS_ENABLED(CONFIG_APP_DRIVE_STREAM)
    int32_t up = record_up(rec, len);

    /* Streaming: records come every few hundred milliseconds, so they are kept
     * a gap apart by when they were built, the gap doubling with each thinning.
     * Not counted in s_push_seq, which is for pushes at a steady rate. */
    if (s_stream && GAP_NOW > 0 && up >= 0) {
        if (near_held(up, GAP_NOW * s_interval_mult)) {
            s_dropped++;
            return 1;                   /* left out, which is not stored */
        }
    } else
#endif
    /* Once thinned, only keep one record in every s_interval_mult so the
     * stored sample interval stays uniform across the whole outage instead of
     * being dense at the start and sparse later. */
    if (s_interval_mult > 1 && (s_push_seq++ % s_interval_mult) != 0) {
        s_dropped++;
        return 0;
    }

    if (s_count == SLOTS) {
#if IS_ENABLED(CONFIG_APP_FLASH_BACKLOG)
        /* The flash takes the oldest; the ring is thinned only if it cannot. */
        if (!fl_spill()) {
            databuf_decimate();
        }
#else
        databuf_decimate();
#endif
    }

    int slot = (s_head + s_count) % SLOTS;

    memcpy(s_rec[slot], rec, len);
    s_len[slot] = (uint16_t)len;
#if IS_ENABLED(CONFIG_APP_DRIVE_STREAM)
    s_up[slot] = up;
#endif
    s_count++;
    return 0;
}

/* Split a newline-separated send buffer into records and stash each one. */
int databuf_push_lines(const char *buf, size_t len)
{
    const char *p = buf;
    const char *end = buf + len;
    int stored = 0;

    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t rec_len = nl ? (size_t)(nl - p) : (size_t)(end - p);

        if (rec_len > 0 && databuf_push(p, rec_len) == 0) {
            stored++;
        }
        if (!nl) {
            break;
        }
        p = nl + 1;
    }

    /* Nothing to say about a streamed record the spacing left out: that is
     * most of them. */
    if (stored > 0 || !STREAMING) {
        LOG_INF("backlog: +%d record%s (%d held, %u dropped)",
                stored, stored == 1 ? "" : "s", BACKLOG_COUNT(), s_dropped);
    }
    return stored;
}

/* Pack as many of the oldest records as fit into one datagram, each stamped
 * with its age.  Returns the byte count and, via n_recs, how many records it
 * covers so the caller can drop exactly those once they are sent. */
static int databuf_pack(char *out, size_t max, int *n_recs)
{
    size_t used = 0;
    int packed = 0;
    int64_t now_s = k_uptime_get() / 1000;
#if IS_ENABLED(CONFIG_APP_FLASH_BACKLOG)
    int fl_packed = 0;
    bool fl_drained = true;
#endif

    if (max > BATCH_MAX) {
        max = BATCH_MAX;
    }

#if IS_ENABLED(CONFIG_APP_FLASH_BACKLOG)
    /* What the flash holds is older than anything in the ring: it goes first,
     * and the ring's records only once it has been taken whole. */
    used = fl_pack(out, max, now_s, &fl_packed, &fl_drained);
    if (!fl_drained) {
        *n_recs = fl_packed;
        return (int)used;
    }
#endif

    while (packed < s_count) {
        int slot = (s_head + packed) % SLOTS;
#if IS_ENABLED(CONFIG_APP_FLASH_BACKLOG)
        size_t sep = (packed || fl_packed) ? 1 : 0;   /* newline separator */
#else
        size_t sep = packed ? 1 : 0;           /* newline separator */
#endif

        if (used + sep >= max) {
            break;
        }

        int n = stamp_record(s_rec[slot], s_len[slot], &out[used + sep],
                             max - used - sep - 1, now_s, true);

        if (n < 0) {
            break;
        }
        if (sep) {
            out[used] = '\n';
        }
        used += sep + (size_t)n;
        packed++;
    }

#if IS_ENABLED(CONFIG_APP_FLASH_BACKLOG)
    *n_recs = packed + fl_packed;
#else
    *n_recs = packed;
#endif
    return (int)used;
}

static void databuf_drop(int n)
{
#if IS_ENABLED(CONFIG_APP_FLASH_BACKLOG)
    /* The flash's records were first in the datagram. */
    n -= fl_commit();
#endif
    if (n >= s_count) {
        databuf_reset();
        return;
    }
    s_head = (s_head + n) % SLOTS;
    s_count -= n;
}

/* -- UNANSWERED: sent, but not yet known to have arrived --------------------
 *
 * transport_send() returning 0 means the modem took the datagram.  The
 * server's reply is the only proof it arrived, so each send's records are
 * kept here under the transport's id for the send until that reply comes in
 * (databuf_ack) — then dropped — or until APP_ACK_TIMEOUT_S passes without
 * one, when they go back into the backlog to be sent again.  The server
 * drops a record it already holds, so a reply that was merely late costs a
 * few hundred bytes, where a datagram that really was lost used to cost the
 * track.
 *
 * On 2026-09-27 the network rejected a tracking-area update twice in one
 * short drive (EMM cause 9, TAC 4096 to 12296 and back).  Each time the modem
 * dropped its registration, re-attached, and discarded what it had queued:
 * three records at 10:57, and at 11:50 the last position and the
 * ignition-off record, so the journey ended a minute late.  The firmware
 * had counted every one of them as sent. */
#define UNANSWERED       CONFIG_APP_DATABUF_UNACKED_SLOTS
#define ACK_TIMEOUT_MS   ((int64_t)CONFIG_APP_ACK_TIMEOUT_S * 1000)

/* -- FLOW CONTROL (drive stream) --------------------------------------------
 *
 * The slots are also a window.  At six or seven sends a second they are full
 * whenever a reply is a second late, and on a stalled uplink the modem took
 * about nine datagrams before its own buffer was full and send() sat out its
 * ten seconds, then reconnected and lost the replies (2026-10-31, three times
 * in eight minutes).  So while the stream runs it sends no more than the slots
 * hold: databuf_window_wait() gives a reply a moment to free one, and when none
 * does the record is held in the backlog, as an outage's would be.
 *
 * A slot is freed by its reply; by a later send's reply, once it is
 * WINDOW_LOST_MS old (it was lost, or its reply was not recognised); by the
 * socket it went out on being closed (its reply cannot come); or after
 * APP_ACK_TIMEOUT_S.  Were it only the last, a few lost datagrams would shut
 * the window for half a minute each.  With nothing answered at all, one send
 * in WINDOW_PROBE_MS goes anyway, so a link that has come back is found. */
#if IS_ENABLED(CONFIG_APP_DRIVE_STREAM) && CONFIG_APP_DATABUF_UNACKED_SLOTS > 0 && \
    CONFIG_APP_DRIVE_STREAM_WINDOW_MS > 0
#define FLOW_CONTROL 1
#define WINDOW_LOST_MS   1500
#define WINDOW_PROBE_MS  3000
#endif

#if UNANSWERED > 0
static struct {
    uint32_t id;                  /* transport's id for the send; 0 = free */
    int64_t  sent_ms;
    uint16_t len;
#ifdef FLOW_CONTROL
    uint32_t gen;                 /* transport_generation() it went out on */
#endif
    char     recs[UDP_PACKET_SIZE];
} s_out[UNANSWERED];
#endif

#ifdef FLOW_CONTROL
static uint32_t s_acked_high;     /* the highest send id a reply has come for */
static int64_t  s_probe_ms;       /* when a send last went out on a full window */
#endif

/* When a reply last came in, from any send: the evidence that the link is
 * delivering right now, which is what the backlog waits for. */
static int64_t s_last_ack_ms;

int64_t databuf_last_ack_ms(void)
{
    return s_last_ack_ms;
}

#if UNANSWERED > 0
/* The records of one unanswered send back into the backlog, their ages
 * stripped: the next send works them out again. */
static void requeue(int i)
{
    static char rec[REC_MAX];
    const char *p = s_out[i].recs;
    const char *end = p + s_out[i].len;
    int stored = 0;

    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t rec_len = nl ? (size_t)(nl - p) : (size_t)(end - p);
        int n = stamp_record(p, rec_len, rec, sizeof(rec), 0, false);

        if (n > 0 && databuf_push(rec, (size_t)n) == 0) {
            stored++;
        }
        if (!nl) {
            break;
        }
        p = nl + 1;
    }

    if (stored > 0 || !STREAMING) {
        LOG_WRN("datagram #%u unanswered after %lld s — %d record%s back in "
                "the backlog", s_out[i].id,
                (k_uptime_get() - s_out[i].sent_ms) / 1000,
                stored, stored == 1 ? "" : "s");
    }
    s_out[i].id = 0;
}

/* The unanswered send that went out first, optionally only among those at
 * least `min_age_ms` old; -1 if none. */
static int oldest(int64_t min_age_ms)
{
    int64_t now = k_uptime_get();
    int best = -1;

    for (int i = 0; i < UNANSWERED; i++) {
        if (s_out[i].id == 0 || now - s_out[i].sent_ms < min_age_ms) {
            continue;
        }
        if (best < 0 || s_out[i].sent_ms < s_out[best].sent_ms) {
            best = i;
        }
    }
    return best;
}

#ifdef FLOW_CONTROL
/* The unanswered send that went out first, among those whose reply cannot be
 * coming (see FLOW CONTROL); -1 if none. */
static int lost(void)
{
    int64_t now = k_uptime_get();
    uint32_t gen = transport_generation();
    int best = -1;

    for (int i = 0; i < UNANSWERED; i++) {
        if (s_out[i].id == 0) {
            continue;
        }

        bool gone = s_out[i].gen != gen ||
                    (s_out[i].id < s_acked_high &&
                     now - s_out[i].sent_ms >= WINDOW_LOST_MS);

        if (gone && (best < 0 || s_out[i].sent_ms < s_out[best].sent_ms)) {
            best = i;
        }
    }
    return best;
}
#endif
#endif

void databuf_sent(uint32_t id, const char *recs, size_t len)
{
#if UNANSWERED > 0
    if (id != 0) {
        if (len == 0) {
            return;
        }
        if (len > sizeof(s_out[0].recs)) {
            /* Cannot happen: the transport refuses anything larger. */
            LOG_ERR("datagram #%u: %u bytes of records not held", id,
                    (unsigned)len);
            return;
        }

        int slot = -1;

        for (int i = 0; i < UNANSWERED; i++) {
            if (s_out[i].id == 0) {
                slot = i;
                break;
            }
        }
        if (slot < 0) {
            /* All waiting: the oldest has waited longest, and makes room. */
            slot = oldest(0);
            requeue(slot);
        }
        s_out[slot].id = id;
        s_out[slot].sent_ms = k_uptime_get();
        s_out[slot].len = (uint16_t)len;
#ifdef FLOW_CONTROL
        s_out[slot].gen = transport_generation();
#endif
        memcpy(s_out[slot].recs, recs, len);
        return;
    }
#else
    ARG_UNUSED(id);
#endif
    ARG_UNUSED(recs);
    ARG_UNUSED(len);
    /* Nothing to wait for: the send was its own receipt (Traccar's 2xx), or
     * nothing is held, in which case a send is taken at its word. */
    s_last_ack_ms = k_uptime_get();
}

void databuf_ack(uint32_t id)
{
    s_last_ack_ms = k_uptime_get();
    /* Every reply comes through here, and an alert may be what it answers. */
    alert_ack(id);
    dbglog_answered(id);
#ifdef FLOW_CONTROL
    if (id > s_acked_high) {
        s_acked_high = id;
    }
#endif
#if UNANSWERED > 0
    for (int i = 0; i < UNANSWERED; i++) {
        if (s_out[i].id == id) {
            LOG_DBG("datagram #%u answered after %lld ms", id,
                    k_uptime_get() - s_out[i].sent_ms);
            s_out[i].id = 0;
            break;
        }
    }
#else
    ARG_UNUSED(id);
#endif
}

int databuf_unacked(void)
{
    int n = 0;

#if UNANSWERED > 0
    for (int i = 0; i < UNANSWERED; i++) {
        if (s_out[i].id != 0) {
            n++;
        }
    }
#endif
    return n;
}

int databuf_expire(void)
{
    int n = 0;

#if UNANSWERED > 0
    int i;

    /* Oldest first, so the backlog takes them back in the order they were
     * built. */
    while ((i = oldest(ACK_TIMEOUT_MS)) >= 0) {
        requeue(i);
        n++;
    }
#ifdef FLOW_CONTROL
    /* The stream does not wait half a minute for what cannot be coming. */
    while (s_stream && (i = lost()) >= 0) {
        requeue(i);
        n++;
    }
#endif
#endif
    return n;
}

#if IS_ENABLED(CONFIG_APP_DRIVE_STREAM)
bool databuf_window_wait(int max_ms)
{
#ifndef FLOW_CONTROL
    ARG_UNUSED(max_ms);
    return true;
#else
    if (!s_stream || max_ms <= 0) {
        return true;
    }

    int64_t deadline = k_uptime_get() + max_ms;

    for (;;) {
        if (transport_poll() < 0) {
            return true;        /* no socket: the send opens one, or says why not */
        }
        databuf_expire();
        if (databuf_unacked() < UNANSWERED) {
            return true;
        }
        if (k_uptime_get() >= deadline) {
            break;
        }
        k_msleep(5);
    }

    /* Full, and nothing freed a slot.  If replies are coming this is only a
     * slow link and the record waits in the backlog; if none has come for a
     * while, a send now and then finds out whether the link is back. */
    int64_t now = k_uptime_get();

    if (now - s_last_ack_ms >= WINDOW_PROBE_MS && now - s_probe_ms >= WINDOW_PROBE_MS) {
        s_probe_ms = now;
        return true;
    }
    return false;
#endif
}
#endif

int databuf_settle(int timeout_ms)
{
    int n = 0;

#if UNANSWERED > 0
    int64_t deadline = k_uptime_get() + timeout_ms;

    /* Alerts and log lines are held the same way (alert.c, dbglog.c), so
     * their replies are waited for too: one requeued here that had only been
     * slow is sent again. */
    while (databuf_unacked() > 0 || alert_unanswered() > 0 ||
           dbglog_unanswered() > 0) {
        /* No socket: nothing more can arrive. */
        if (transport_poll() < 0 ||
            (databuf_unacked() == 0 && alert_unanswered() == 0 &&
             dbglog_unanswered() == 0) ||
            k_uptime_get() >= deadline) {
            break;
        }
        k_msleep(100);
    }

    int i;

    while ((i = oldest(0)) >= 0) {
        requeue(i);
        n++;
    }
    alert_settle();
    dbglog_settle();
#else
    ARG_UNUSED(timeout_ms);
#endif
    return n;
}

void databuf_deliver(int timeout_ms, int budget_ms, bool (*stop)(void))
{
    int64_t end = k_uptime_get() + budget_ms;

    /* The replies to what is already out are always waited for. */
    databuf_settle(timeout_ms);

    while (BACKLOG_COUNT() > 0 && k_uptime_get() < end &&
           !(stop && stop())) {
        watchdog_kick();
        if (databuf_flush(CONFIG_APP_DATABUF_FLUSH_PER_CYCLE) == 0) {
            break;
        }
        /* Anything that went unanswered says the link is not taking it:
         * the rest waits for the next wake. */
        if (databuf_settle(timeout_ms) > 0) {
            break;
        }
    }
}

int databuf_flush(int max_datagrams)
{
    static char batch[BATCH_MAX];
    int sent = 0;

    if (BACKLOG_COUNT() == 0 || !modem_is_registered()) {
        return 0;
    }

    for (int i = 0; i < max_datagrams && BACKLOG_COUNT() > 0; i++) {
#ifdef FLOW_CONTROL
        /* The window is for the stream's own records first. */
        if (s_stream && databuf_unacked() >= UNANSWERED) {
            break;
        }
#endif
        int n_recs = 0;
        int len = databuf_pack(batch, sizeof(batch), &n_recs);

        if (len <= 0 || n_recs == 0) {
            break;
        }

        watchdog_kick();
        if (transport_send((const uint8_t *)batch, (size_t)len) != 0) {
            LOG_WRN("backlog flush failed — %d records still held",
                    BACKLOG_COUNT());
            break;
        }
        /* Out of the ring before they are held: holding can push an
         * unanswered send back in, and a thinning then would shuffle the
         * ring under the records just packed. */
        databuf_drop(n_recs);
        databuf_sent(transport_sent_id(), batch, (size_t)len);
        sent += n_recs;
    }

    if (sent) {
        LOG_INF("backlog: sent %d record%s, %d still held",
                sent, sent == 1 ? "" : "s", BACKLOG_COUNT());
        if (BACKLOG_COUNT() == 0 && s_dropped) {
            LOG_INF("backlog drained (%u thinned away during the outage)",
                    s_dropped);
            s_dropped = 0;
        }
    }
    return sent;
}
