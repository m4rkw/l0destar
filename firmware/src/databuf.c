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
 * DRAINING
 * --------
 * The backlog must never delay live telemetry, so it is drained a datagram or
 * two per cycle alongside the current record rather than in one burst, and
 * each datagram is packed to stay inside the transport's packet limit.
 */

#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "app.h"
#include "config.h"

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

int databuf_count(void)
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
        }
        kept++;
    }

    s_dropped += (uint32_t)(s_count - kept);
    s_count = kept;
    s_interval_mult *= 2;
    LOG_WRN("backlog full — thinned to %d records, now every %d th",
            s_count, s_interval_mult);
}

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

    /* Once thinned, only keep one record in every s_interval_mult so the
     * stored sample interval stays uniform across the whole outage instead of
     * being dense at the start and sparse later. */
    if (s_interval_mult > 1 && (s_push_seq++ % s_interval_mult) != 0) {
        s_dropped++;
        return 0;
    }

    if (s_count == SLOTS) {
        databuf_decimate();
    }

    int slot = (s_head + s_count) % SLOTS;

    memcpy(s_rec[slot], rec, len);
    s_len[slot] = (uint16_t)len;
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

    LOG_INF("backlog: +%d record%s (%d held, %u dropped)",
            stored, stored == 1 ? "" : "s", s_count, s_dropped);
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

    if (max > BATCH_MAX) {
        max = BATCH_MAX;
    }

    while (packed < s_count) {
        int slot = (s_head + packed) % SLOTS;
        size_t sep = packed ? 1 : 0;           /* newline separator */

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

    *n_recs = packed;
    return (int)used;
}

static void databuf_drop(int n)
{
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
/* Flush-and-settle rounds databuf_deliver() will go through before sleep:
 * enough for a full backlog at two datagrams a round. */
#define DELIVER_ROUNDS   8

#if UNANSWERED > 0
static struct {
    uint32_t id;                  /* transport's id for the send; 0 = free */
    int64_t  sent_ms;
    uint16_t len;
    char     recs[UDP_PACKET_SIZE];
} s_out[UNANSWERED];
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

    LOG_WRN("datagram #%u unanswered after %lld s — %d record%s back in "
            "the backlog", s_out[i].id,
            (k_uptime_get() - s_out[i].sent_ms) / 1000,
            stored, stored == 1 ? "" : "s");
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
#endif
    return n;
}

int databuf_settle(int timeout_ms)
{
    int n = 0;

#if UNANSWERED > 0
    int64_t deadline = k_uptime_get() + timeout_ms;

    while (databuf_unacked() > 0) {
        /* No socket: nothing more can arrive. */
        if (transport_poll() < 0 || databuf_unacked() == 0 ||
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
#else
    ARG_UNUSED(timeout_ms);
#endif
    return n;
}

void databuf_deliver(int timeout_ms)
{
    databuf_settle(timeout_ms);

    for (int round = 0; round < DELIVER_ROUNDS && s_count > 0; round++) {
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

    if (s_count == 0 || !modem_is_registered()) {
        return 0;
    }

    for (int i = 0; i < max_datagrams && s_count > 0; i++) {
        int n_recs = 0;
        int len = databuf_pack(batch, sizeof(batch), &n_recs);

        if (len <= 0 || n_recs == 0) {
            break;
        }

        watchdog_kick();
        if (transport_send((const uint8_t *)batch, (size_t)len) != 0) {
            LOG_WRN("backlog flush failed — %d records still held", s_count);
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
                sent, sent == 1 ? "" : "s", s_count);
        if (s_count == 0 && s_dropped) {
            LOG_INF("backlog drained (%u thinned away during the outage)",
                    s_dropped);
            s_dropped = 0;
        }
    }
    return sent;
}
