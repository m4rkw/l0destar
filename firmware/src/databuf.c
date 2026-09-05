/*
 * Backlog buffer for telemetry records that could not be sent.
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

/* Pack as many of the oldest records as fit into one datagram.  Returns the
 * byte count and, via n_recs, how many records it covers so the caller can
 * drop exactly those once they are acknowledged. */
static int databuf_pack(char *out, size_t max, int *n_recs)
{
    size_t used = 0;
    int packed = 0;

    if (max > BATCH_MAX) {
        max = BATCH_MAX;
    }

    while (packed < s_count) {
        int slot = (s_head + packed) % SLOTS;
        size_t need = s_len[slot] + (packed ? 1 : 0);   /* newline separator */

        if (used + need >= max) {
            break;
        }
        if (packed) {
            out[used++] = '\n';
        }
        memcpy(&out[used], s_rec[slot], s_len[slot]);
        used += s_len[slot];
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

int databuf_flush(int max_datagrams)
{
    static char batch[BATCH_MAX];
    int sent = 0;

    if (s_count == 0 || !modem_is_registered()) {
        return 0;
    }

    /* The backlog is history; nothing in it needs a reply, and asking for one
     * would keep the radio awake for no reason. */
    bool saved_read = read_udp_response;

    read_udp_response = false;

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
        databuf_drop(n_recs);
        sent += n_recs;
    }

    read_udp_response = saved_read;

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
