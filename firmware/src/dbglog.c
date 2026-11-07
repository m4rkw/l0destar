/*
 * Debug log capture (CONFIG_APP_DEBUG_LOG).
 *
 * WHY
 * ---
 * A deployed unit has no console.  When telemetry stops for a while the
 * only evidence is the gap itself; whatever the firmware logged about it
 * went out of the UART into nothing.  This keeps every warning and error
 * in RAM and sends them with the next telemetry record that gets through,
 * so an outage can be read back from the server afterwards.
 *
 * HOW
 * ---
 * A second Zephyr log backend, alongside the UART one, sees every message
 * and keeps those at WRN or ERR.  Each is formatted once into a line
 *
 *     <uptime_ms>,<E|W>,<module>: <text>
 *
 * and stored in one of two rings:
 *
 *   head   append-only until full, never overwritten.  Holds the onset of
 *          an incident: the first errors are usually the ones that matter.
 *   tail   oldest line evicted when full.  Holds the most recent lines, so
 *          the recovery is there too.
 *
 * Lines evicted from the tail are counted and reported as one marker line
 * when the buffer drains.  Consecutive identical lines (same level, module
 * and text) are collapsed into the first plus a "repeated N times" marker,
 * since a stuck link says the same thing every cycle.
 *
 * In deferred mode the log core can also throw messages away itself when
 * its buffer overflows before the log thread drains it.  That count is
 * global and level-blind: it is almost entirely INF traffic bound for the
 * console (a send logs every record in the batch back to back), so it is
 * kept apart from ours and reported as "console lines dropped" rather than
 * being mistaken for warnings that never made it here.
 *
 * DRAINING
 * --------
 * send_data() appends as many lines as fit its budget to the outgoing record
 * as "L,<line>" records (dbglog_take).  The modem accepting the datagram only
 * means it is queued, and a lost one takes with it the lines that describe
 * the fault, so they are held like the records are (databuf.c, alert.c):
 * dbglog_sent(id) marks them as riding datagram `id`, and dbglog_answered(id),
 * called for each reply, frees them.  Nothing is copied: a held line stays in
 * its ring, only counted as taken.
 *
 * One batch is outstanding at a time.  While it is held dbglog_take() hands
 * out nothing and new lines wait in the rings, so no line ever rides two
 * datagrams that may both be answered.  A hold ends by
 *
 *   - the reply (dbglog_answered): the held lines are popped from the front
 *     of each ring and the dropped counts that were reported are subtracted;
 *   - APP_ACK_TIMEOUT_S passing (dbglog_expire, also run by dbglog_take);
 *   - the socket closing (dbglog_settle).
 *
 * The last two only drop the flag.  The lines never left the rings, so the
 * next take sends them again, "N lines dropped" markers included (their count
 * is subtracted only when a batch is answered, so reverting never subtracts
 * twice).  A reply that comes in after that finds nothing held and frees
 * nothing; the server may then show the lines twice, which is better than
 * losing them.  With no hold configured (APP_DATABUF_UNACKED_SLOTS = 0), or a
 * send that was its own receipt (id 0), the lines are freed at once.  A
 * failed send is dbglog_ack(false) and leaves them for next time.  Nothing
 * here sends on its own and nothing here logs: a backend that logged would
 * recurse.  A reply that never comes is reported by the records' own
 * "datagram #N unanswered".
 *
 * MEMORY AND RETENTION
 * --------------------
 * Both rings, their positions and the two dropped counters live in one
 * __noinit block, so a warm reset (watchdog, fatal error, sys_reboot) keeps
 * the lines that had not gone out: the ones about the problem that caused the
 * reset.  The cost is still HEAD + TAIL bytes, fixed at link time, plus a line
 * buffer on the stack of whichever thread is logging (immediate mode) or the
 * log thread (deferred mode).  Both rings use a byte layout of [len][text]
 * per line, so nothing is wasted on fixed-width slots.
 *
 * A power cut leaves RAM undefined, and so does a pin reset (measured on the
 * nRF9151: the block read back as random bits), and a new firmware may have
 * other sizes, so the block carries a magic word, the buffer sizes and a
 * checksum of its metadata (everything but the two buffers; sealed whenever
 * the lock is released).  The first use after boot validates it: magic,
 * layout, checksum, consistent ring positions, and a bounded walk over every
 * line checking that it is whole, printable and starts "<digits>,<E|W>,".
 * Anything less and both rings start empty; nothing is half-restored.
 *
 * A restored line's uptime belongs to the previous run's clock, which restarted
 * at 0, so its digits are overwritten with the same number of '0's (the server
 * shows up=0.000: "from before this boot").  There is no marker line.  What is
 * not kept: the hold and what was taken (those lines come back as pending, and
 * a datagram that did arrive may show them twice), and the repeat collapsing.
 *
 * CONTEXT
 * -------
 * In immediate mode the backend runs on the caller's context, which can be
 * any thread or an ISR, and two callers can be in process() at once.  The
 * formatting uses only the stack; the rings are guarded by a spinlock.
 */

#include <string.h>
#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log_backend.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/logging/log_msg.h>
#include <zephyr/sys/cbprintf.h>
#include <zephyr/drivers/hwinfo.h>

#include "app.h"
#include "holdbreak.h"

#define HEAD_SIZE  CONFIG_APP_DEBUG_LOG_HEAD
#define TAIL_SIZE  CONFIG_APP_DEBUG_LOG_TAIL
#define DBG_LINE_MAX   CONFIG_APP_DEBUG_LOG_LINE_MAX

BUILD_ASSERT(DBG_LINE_MAX <= 255, "line length is stored in one byte");
BUILD_ASSERT(HEAD_SIZE > DBG_LINE_MAX + 1 && TAIL_SIZE > DBG_LINE_MAX + 1,
             "each ring must hold at least one full line");

/* -- byte ring of [len][text] entries -------------------------------------- */

/* The part of a ring that survives a reset. */
struct ring_pos {
    uint16_t  rd;       /* offset of the oldest entry */
    uint16_t  wr;       /* offset where the next entry goes */
    uint16_t  used;     /* bytes, including the length bytes */
    uint16_t  lines;
};

struct ring {
    char            *buf;
    uint16_t         size;
    struct ring_pos *pos;
};

#define STORE_MAGIC 0x4C4F4731u     /* "LOG1" */

/* Everything that must outlive a warm reset, in one block.  `sum` covers all
 * of it but the two buffers and is sealed whenever the lock is released.
 * No padding: all fields are 32 or 16 bits and the 16-bit ones come in fours. */
struct dbg_store {
    uint32_t        magic;
    uint32_t        head_size;      /* the layout this was written with */
    uint32_t        tail_size;
    uint32_t        line_max;
    uint32_t        block_size;
    struct ring_pos head;
    struct ring_pos tail;
    /* Lines lost from the tail ring (evicted, or refused during a send).
     * Reported as one marker line on the next drain. */
    uint32_t        dropped;
    /* Messages of any level the log core discarded before any backend saw
     * them (deferred mode only).  Reported separately: see the header. */
    uint32_t        core_dropped;
    uint32_t        sum;
    char            head_buf[HEAD_SIZE];
    char            tail_buf[TAIL_SIZE];
};

static __noinit struct dbg_store s_store;

BUILD_ASSERT(offsetof(struct dbg_store, head_buf) ==
             offsetof(struct dbg_store, sum) + sizeof(uint32_t),
             "the checksummed part has no padding");

#define s_dropped       s_store.dropped
#define s_core_dropped  s_store.core_dropped

static struct ring s_head = { .buf = s_store.head_buf, .size = HEAD_SIZE,
                              .pos = &s_store.head };
static struct ring s_tail = { .buf = s_store.tail_buf, .size = TAIL_SIZE,
                              .pos = &s_store.tail };

static struct k_spinlock s_lock;

/* The first use after boot checks the retained block (store_open).  Zero at
 * every boot, unlike the block. */
static bool s_ready;

/* Duplicate collapsing: the last line stored, minus its timestamp. */
static char     s_last[DBG_LINE_MAX + 1];
static uint32_t s_repeat;
static int64_t  s_repeat_ms;

/* What the last dbglog_take() handed out.  They stay counted here while the
 * batch is in flight and while it is held, so store() leaves the tail alone
 * for both. */
static uint16_t s_take_head;
static uint16_t s_take_tail;
static uint32_t s_take_dropped;
static uint32_t s_take_core;

/* The batch is held: sent as datagram s_held_id at s_held_ms, no reply yet. */
static bool     s_held;
static uint32_t s_held_id;
static int64_t  s_held_ms;

#define DBG_HOLD        (CONFIG_APP_DATABUF_UNACKED_SLOTS > 0)
#define ACK_TIMEOUT_MS  ((int64_t)CONFIG_APP_ACK_TIMEOUT_S * 1000)

static bool ring_fits(const struct ring *r, uint8_t len)
{
    return (uint32_t)r->pos->used + 1U + len <= r->size;
}

static void ring_put_byte(struct ring *r, char c)
{
    r->buf[r->pos->wr] = c;
    r->pos->wr = (r->pos->wr + 1U) % r->size;
}

static void ring_push(struct ring *r, const char *s, uint8_t len)
{
    ring_put_byte(r, (char)len);
    for (uint8_t i = 0; i < len; i++) {
        ring_put_byte(r, s[i]);
    }
    r->pos->used  += 1U + len;
    r->pos->lines += 1U;
}

static void ring_pop(struct ring *r)
{
    uint8_t len = (uint8_t)r->buf[r->pos->rd];

    r->pos->rd     = (r->pos->rd + 1U + len) % r->size;
    r->pos->used  -= 1U + len;
    r->pos->lines -= 1U;
}

/* Copy the entry at `off` into `out` (no terminator); returns its length and
 * advances `*off` to the next entry. */
static uint8_t ring_read(const struct ring *r, uint16_t *off, char *out)
{
    uint16_t o = *off;
    uint8_t len = (uint8_t)r->buf[o];

    o = (o + 1U) % r->size;
    for (uint8_t i = 0; i < len; i++) {
        out[i] = r->buf[o];
        o = (o + 1U) % r->size;
    }
    *off = o;
    return len;
}

/* -- retained block -------------------------------------------------------- */

/* FNV-1a: small, and enough to tell a block we wrote from garbage. */
static uint32_t fnv1a(const void *p, size_t n)
{
    const uint8_t *b = p;
    uint32_t h = 2166136261u;

    while (n--) {
        h = (h ^ *b++) * 16777619u;
    }
    return h;
}

static uint32_t store_sum(void)
{
    return fnv1a(&s_store, offsetof(struct dbg_store, sum));
}

/* Every critical section that changes the metadata ends here, so the block is
 * always sealed when the lock is free (a reset mid-section is a reset with an
 * invalid checksum, which starts empty). */
static void unlock_sealed(k_spinlock_key_t key)
{
    s_store.sum = store_sum();
    k_spin_unlock(&s_lock, key);
}

/* Is the ring's metadata consistent and does it hold exactly `lines` whole
 * entries?  Reads only inside the buffer and never more than `used` bytes of
 * entries, whatever the bytes are.  Each text must be printable and begin
 * "<digits>,<E|W>,". */
static bool ring_valid(const struct ring *r)
{
    const struct ring_pos *p = r->pos;
    uint32_t total = 0;
    uint16_t off;

    if (p->rd >= r->size || p->used > r->size ||
        p->wr != (uint16_t)(((uint32_t)p->rd + p->used) % r->size) ||
        (uint32_t)p->lines * 5U > p->used) {      /* 5 = shortest entry */
        return false;
    }
    off = p->rd;
    for (uint16_t i = 0; i < p->lines; i++) {
        uint8_t len = (uint8_t)r->buf[off];
        int state = 0;      /* 0 first digit, 1 digits, 2 level, 3 comma, 4 text */

        if (len < 4 || len > DBG_LINE_MAX || total + 1U + len > p->used) {
            return false;
        }
        total += 1U + len;
        off = (off + 1U) % r->size;
        for (uint8_t j = 0; j < len; j++) {
            uint8_t c = (uint8_t)r->buf[off];
            bool digit = c >= '0' && c <= '9';

            off = (off + 1U) % r->size;
            if (c < 0x20 || c > 0x7e) {
                return false;
            }
            if (state == 0 && digit) {
                state = 1;
            } else if (state == 1 && c == ',') {
                state = 2;
            } else if (state == 2 && (c == 'E' || c == 'W')) {
                state = 3;
            } else if (state == 3 && c == ',') {
                state = 4;
            } else if (!(state == 1 && digit) && state != 4) {
                return false;
            }
        }
        if (state != 4) {
            return false;
        }
    }
    return total == p->used;
}

/* The uptimes in a restored ring are the previous run's.  ring_valid() has
 * already vouched for the structure. */
static void ring_restamp(struct ring *r)
{
    uint16_t off = r->pos->rd;

    for (uint16_t i = 0; i < r->pos->lines; i++) {
        uint8_t len = (uint8_t)r->buf[off];
        bool digits = true;

        off = (off + 1U) % r->size;
        for (uint8_t j = 0; j < len; j++) {
            if (digits && r->buf[off] >= '0' && r->buf[off] <= '9') {
                r->buf[off] = '0';
            } else {
                digits = false;
            }
            off = (off + 1U) % r->size;
        }
    }
}

/* First use after boot, under the lock: keep the retained block if it is
 * plainly ours, else start both rings empty.  Never half of each. */
static void store_open(void)
{
    if (s_ready) {
        return;
    }
    s_ready = true;

    if (s_store.magic == STORE_MAGIC &&
        s_store.head_size == HEAD_SIZE && s_store.tail_size == TAIL_SIZE &&
        s_store.line_max == DBG_LINE_MAX &&
        s_store.block_size == sizeof(s_store) &&
        s_store.sum == store_sum() &&
        ring_valid(&s_head) && ring_valid(&s_tail)) {
        ring_restamp(&s_head);
        ring_restamp(&s_tail);
        return;
    }
    s_store.magic = STORE_MAGIC;
    s_store.head_size = HEAD_SIZE;
    s_store.tail_size = TAIL_SIZE;
    s_store.line_max = DBG_LINE_MAX;
    s_store.block_size = sizeof(s_store);
    memset(&s_store.head, 0, sizeof(s_store.head));
    memset(&s_store.tail, 0, sizeof(s_store.tail));
    s_store.dropped = 0;
    s_store.core_dropped = 0;
    s_store.sum = store_sum();
}

/* -- capture --------------------------------------------------------------- */

/* Store one formatted line.  Called with the lock held. */
static void store(const char *line, uint8_t len)
{
    if (ring_fits(&s_head, len)) {
        ring_push(&s_head, line, len);
        return;
    }
    while (!ring_fits(&s_tail, len)) {
        /* Never pull a line out from under a batch that is in flight or held
         * (s_take_tail stays counted for both): dropping the new one instead
         * keeps the counts that will be freed honest.  The window is one
         * datagram long, or until its reply or the ack timeout. */
        if (s_tail.pos->lines == 0 || s_take_tail > 0) {
            s_dropped++;
            return;
        }
        ring_pop(&s_tail);
        s_dropped++;
    }
    ring_push(&s_tail, line, len);
}

/* Flush a pending "repeated N times" marker.  Called with the lock held. */
static void flush_repeat(void)
{
    char marker[DBG_LINE_MAX + 1];
    int n;

    if (s_repeat == 0) {
        return;
    }
    n = snprintf(marker, sizeof(marker),
                 "%lld,W,dbglog: previous line repeated %u more times",
                 (long long)s_repeat_ms, (unsigned)s_repeat);
    if (n > DBG_LINE_MAX) {
        n = DBG_LINE_MAX;
    }
    s_repeat = 0;
    if (n > 0) {
        store(marker, (uint8_t)n);
    }
}

static void capture(const char *line, uint8_t len, int64_t ms)
{
    /* Everything after the timestamp identifies the message for collapsing. */
    const char *text = memchr(line, ',', len);
    size_t text_len;

    if (text == NULL) {
        text = line;
    } else {
        text++;
    }
    text_len = (size_t)len - (size_t)(text - line);

    k_spinlock_key_t key = k_spin_lock(&s_lock);

    store_open();
    if (text_len == strlen(s_last) && memcmp(text, s_last, text_len) == 0) {
        s_repeat++;
        s_repeat_ms = ms;
    } else {
        flush_repeat();
        memcpy(s_last, text, text_len);
        s_last[text_len] = '\0';
        store(line, len);
    }

    unlock_sealed(key);
}

/* Uptime at which the message was logged, in ms.  The default log timestamp
 * on this SoC is k_cycle_get_32() (the 32 kHz RTC: hw cycles/sec is below
 * the 1 MHz threshold where the core switches to uptime), so the message's
 * age is the cycle delta from now.  Right until the counter wraps at 36 h,
 * and a message is never that old, but treat an implausible age as "now"
 * rather than produce a negative time. */
static int64_t msg_uptime_ms(struct log_msg *m)
{
#if IS_ENABLED(CONFIG_LOG_TIMESTAMP_64BIT)
    return k_ticks_to_ms_floor64((int64_t)log_msg_get_timestamp(m));
#else
    uint32_t age = k_cycle_get_32() - (uint32_t)log_msg_get_timestamp(m);
    uint64_t age_ms = (uint64_t)age * 1000ULL / sys_clock_hw_cycles_per_sec();
    int64_t now = k_uptime_get();

    if (age_ms > 60000ULL || (int64_t)age_ms > now) {
        return now;
    }
    return now - (int64_t)age_ms;
#endif
}

struct out_ctx {
    char   *buf;
    size_t  max;
    size_t  len;
};

/* cbpprintf sink: appends to the line, sanitised for the wire.  The record
 * is split on newlines by the transport and the server, so none may
 * survive; other control characters would only garble the log file. */
static int out_char(int c, void *ctx)
{
    struct out_ctx *o = ctx;

    if (o->len < o->max) {
        if (c == '\n' || c == '\r' || c == '\t') {
            c = ' ';
        } else if (c < 0x20 || c > 0x7e) {
            c = '?';
        }
        o->buf[o->len++] = (char)c;
    }
    return c;
}

/* Lines not worth the airtime: known, periodic, and not a problem.  Each
 * entry is a module name and the start of the message text.  Our own
 * modules should log such things at INF instead; this is for messages the
 * firmware does not own, or a modem reply that is a warning on the bench
 * but a fixture in the field.  The console still shows them. */
static const struct {
    const char *module;
    const char *prefix;
} s_ignore[] = {
    /* nrf_cloud retries the JWT every second until the clock is set; the
     * agnss module already reports the wait and the eventual failure. */
    { "nrf_cloud_jwt", "Modem does not have valid date/time" },
    /* The modem firmware answers ERROR to AT%REL14FEAT on every boot. */
    { "modem",         "%REL14FEAT: " },
};

static bool ignored(const char *module, const char *text)
{
    if (module == NULL) {
        return false;
    }
    for (size_t i = 0; i < ARRAY_SIZE(s_ignore); i++) {
        if (strcmp(module, s_ignore[i].module) == 0 &&
            strncmp(text, s_ignore[i].prefix, strlen(s_ignore[i].prefix)) == 0) {
            return true;
        }
    }
    return false;
}

static void process(const struct log_backend *const backend,
                    union log_msg_generic *msg)
{
    ARG_UNUSED(backend);

    struct log_msg *m = &msg->log;
    uint8_t level = log_msg_get_level(m);

    if (level != LOG_LEVEL_ERR && level != LOG_LEVEL_WRN) {
        return;
    }

    char line[DBG_LINE_MAX + 1];
    struct out_ctx o = { .buf = line, .max = DBG_LINE_MAX, .len = 0 };
    int64_t ms = msg_uptime_ms(m);
    int16_t sid = log_msg_get_source_id(m);
    const char *src = (sid >= 0)
        ? log_source_name_get(log_msg_get_domain(m), (uint32_t)sid) : NULL;
    int n;

    n = snprintf(line, sizeof(line), "%lld,%c,%s: ",
                 (long long)ms, level == LOG_LEVEL_ERR ? 'E' : 'W',
                 src ? src : "?");
    if (n < 0) {
        return;
    }
    o.len = (size_t)n < o.max ? (size_t)n : o.max;

    size_t prefix_len = o.len;
    size_t plen;
    uint8_t *pkg = log_msg_get_package(m, &plen);

    if (pkg != NULL && plen > 0) {
        (void)cbpprintf(out_char, &o, pkg);
    }
    line[o.len] = '\0';
    if (ignored(src, line + prefix_len)) {
        return;
    }

    size_t dlen;
    (void)log_msg_get_data(m, &dlen);
    if (dlen > 0 && o.len < o.max) {
        char tail[24];

        n = snprintf(tail, sizeof(tail), " +%u bytes", (unsigned)dlen);
        for (int i = 0; i < n; i++) {
            out_char(tail[i], &o);
        }
    }

    capture(line, (uint8_t)o.len, ms);
}

/* Deferred mode only: the core ran out of message buffer and threw some
 * away before any backend saw them.  Counted apart from our own losses:
 * the figure covers every level, and in practice it is console INF output. */
static void dropped(const struct log_backend *const backend, uint32_t cnt)
{
    ARG_UNUSED(backend);

    k_spinlock_key_t key = k_spin_lock(&s_lock);

    store_open();
    s_core_dropped += cnt;
    unlock_sealed(key);
}

static void panic(const struct log_backend *const backend)
{
    ARG_UNUSED(backend);
    /* Nothing to flush: the rings live in RAM and this build reboots on a
     * fault, so there is no way to get them out.  A fault's cause is
     * reported through rst= on the first record after the restart. */
}

static const struct log_backend_api s_api = {
    .process = process,
    .dropped = IS_ENABLED(CONFIG_LOG_MODE_IMMEDIATE) ? NULL : dropped,
    .panic   = panic,
};

LOG_BACKEND_DEFINE(log_backend_dbglog, s_api, true);

/* -- draining -------------------------------------------------------------- */

size_t dbglog_pending(void)
{
    k_spinlock_key_t key = k_spin_lock(&s_lock);

    store_open();
    size_t n = (size_t)s_head.pos->used + s_tail.pos->used;
    k_spin_unlock(&s_lock, key);
    return n;
}

/* Append one "\nL,<uptime>,W,dbglog: <text>" marker if it fits.  Returns
 * bytes written, or 0 when there was no room.  Called with the lock held. */
static size_t put_marker(char *out, size_t max, const char *fmt, uint32_t n)
{
    char marker[DBG_LINE_MAX];
    int len = snprintf(marker, sizeof(marker), "\nL,%lld,W,dbglog: ",
                       (long long)k_uptime_get());

    if (len < 0 || (size_t)len >= sizeof(marker)) {
        return 0;
    }
    int m = snprintf(marker + len, sizeof(marker) - (size_t)len, fmt,
                     (unsigned)n);

    if (m < 0) {
        return 0;
    }
    len += m;
    if ((size_t)len >= sizeof(marker)) {
        len = (int)sizeof(marker) - 1;
    }
    if ((size_t)len > max) {
        return 0;
    }
    memcpy(out, marker, (size_t)len);
    return (size_t)len;
}

/* Append lines from `r` as "\nL,<line>" until `max` is exhausted.  Returns
 * bytes written; `*taken` counts the lines.  Called with the lock held. */
static size_t drain_ring(const struct ring *r, char *out, size_t max,
                         uint16_t *taken)
{
    uint16_t off = r->pos->rd;
    size_t used = 0;
    char line[DBG_LINE_MAX];

    for (uint16_t i = 0; i < r->pos->lines; i++) {
        uint8_t len = ring_read(r, &off, line);
        size_t need = 3U + len;          /* "\nL," + text */

        if (used + need > max) {
            break;
        }
        out[used++] = '\n';
        out[used++] = 'L';
        out[used++] = ',';
        memcpy(out + used, line, len);
        used += len;
        (*taken)++;
    }
    return used;
}

static void taken_clear(void)
{
    s_take_head = 0;
    s_take_tail = 0;
    s_take_dropped = 0;
    s_take_core = 0;
}

static bool taken_any(void)
{
    return s_take_head > 0 || s_take_tail > 0 || s_take_dropped > 0 ||
           s_take_core > 0;
}

/* The taken batch is delivered: pop exactly what it counted from the front
 * of each ring and take off the dropped counts it reported.  Lines logged
 * since sit behind them.  Called with the lock held. */
static void taken_free(void)
{
    while (s_take_head > 0 && s_head.pos->lines > 0) {
        ring_pop(&s_head);
        s_take_head--;
    }
    while (s_take_tail > 0 && s_tail.pos->lines > 0) {
        ring_pop(&s_tail);
        s_take_tail--;
    }
    s_dropped -= s_take_dropped;
    s_core_dropped -= s_take_core;
    taken_clear();
    s_held = false;
}

/* The hold ends without a reply.  The lines never left the rings and the
 * dropped counts were not subtracted, so only the flag and the counts of
 * what was taken go.  Called with the lock held. */
static void hold_revert(void)
{
    s_held = false;
    taken_clear();
}

static void hold_expire(void)
{
    if (s_held && k_uptime_get() - s_held_ms >= ACK_TIMEOUT_MS) {
        hold_revert();
    }
}

int dbglog_take(char *out, size_t max)
{
    size_t used = 0;

    k_spinlock_key_t key = k_spin_lock(&s_lock);

    store_open();
    hold_expire();
    /* One batch outstanding: what is held may still be answered, so the
     * lines behind it wait rather than ride a second datagram. */
    if (s_held) {
        k_spin_unlock(&s_lock, key);
        return 0;
    }

    /* Anything handed out before and never acked goes again. */
    taken_clear();

    flush_repeat();

    used += drain_ring(&s_head, out + used, max - used, &s_take_head);

    /* Only once the head has gone out entirely: the markers sit between
     * the onset and the recent lines, which is where the gap is.  The tail
     * follows only if every pending marker fitted, so the order on the
     * server is always onset, losses, recovery. */
    if (s_take_head == s_head.pos->lines) {
        bool markers_out = true;

        if (s_dropped > 0) {
            size_t n = put_marker(out + used, max - used,
                                  "%u lines dropped", s_dropped);

            if (n > 0) {
                used += n;
                s_take_dropped = s_dropped;
            } else {
                markers_out = false;
            }
        }
        if (markers_out && s_core_dropped > 0) {
            size_t n = put_marker(out + used, max - used,
                                  "%u console lines dropped (log buffer "
                                  "overflow)", s_core_dropped);

            if (n > 0) {
                used += n;
                s_take_core = s_core_dropped;
            } else {
                markers_out = false;
            }
        }
        if (markers_out) {
            used += drain_ring(&s_tail, out + used, max - used,
                               &s_take_tail);
        }
    }

    unlock_sealed(key);
    return (int)used;
}

void dbglog_sent(uint32_t id)
{
    k_spinlock_key_t key = k_spin_lock(&s_lock);

    store_open();
    if (s_held || !taken_any()) {
        /* Nothing was handed out (or a batch is already held): nothing to
         * hold. */
    } else if (id == 0 || !DBG_HOLD) {
        taken_free();           /* no reply to wait for */
    } else {
        s_held = true;
        s_held_id = id;
        s_held_ms = k_uptime_get();
    }
    unlock_sealed(key);
}

/* A reply that comes after its hold was dropped finds nothing here: the lines
 * went out again, so the server may show them twice. */
void dbglog_answered(uint32_t id)
{
    k_spinlock_key_t key = k_spin_lock(&s_lock);

    store_open();
    if (s_held && s_held_id == id) {
        taken_free();
    }
    unlock_sealed(key);
}

int dbglog_unanswered(void)
{
    k_spinlock_key_t key = k_spin_lock(&s_lock);

    store_open();
    int n = s_held ? 1 : 0;
    k_spin_unlock(&s_lock, key);
    return n;
}

void dbglog_expire(void)
{
    k_spinlock_key_t key = k_spin_lock(&s_lock);

    store_open();
    hold_expire();
    k_spin_unlock(&s_lock, key);
}

void dbglog_settle(void)
{
    k_spinlock_key_t key = k_spin_lock(&s_lock);

    store_open();
    if (s_held) {
        hold_revert();
    }
    k_spin_unlock(&s_lock, key);
}

/* For a caller with no datagram id: the batch taken just now went out
 * (true) or did not (false).  Leaves a held batch alone. */
void dbglog_ack(bool sent)
{
    k_spinlock_key_t key = k_spin_lock(&s_lock);

    store_open();
    if (!s_held) {
        if (sent) {
            taken_free();
        } else {
            taken_clear();
        }
    }
    unlock_sealed(key);
}

/* -- reset cause ----------------------------------------------------------- */

/* Read once and cleared, so the next boot reports only its own cause: the
 * RESETREAS bits are sticky and would otherwise accumulate across restarts.
 * NULL when the register is unreadable or says nothing (a plain power-on on
 * some parts). */
const char *dbglog_reset_cause(void)
{
    static char s_cause[64];
    static bool s_read;

    if (!s_read) {
        static const struct { uint32_t bit; const char *name; } names[] = {
            { RESET_PIN,            "pin"    },
            { RESET_SOFTWARE,       "sw"     },
            { RESET_BROWNOUT,       "bor"    },
            { RESET_POR,            "por"    },
            { RESET_WATCHDOG,       "wdt"    },
            { RESET_DEBUG,          "dbg"    },
            { RESET_SECURITY,       "sec"    },
            { RESET_LOW_POWER_WAKE, "lpwake" },
            { RESET_CPU_LOCKUP,     "lockup" },
        };
        uint32_t cause = 0;
        size_t pos = 0;
#if IS_ENABLED(CONFIG_APP_HOLD_BREAK)
        uint32_t raw = 0;
#endif

        s_read = true;
        if (hwinfo_get_reset_cause(&cause) == 0 && cause != 0) {
#if IS_ENABLED(CONFIG_APP_HOLD_BREAK)
            raw = cause;
#endif
            for (size_t i = 0; i < ARRAY_SIZE(names); i++) {
                if (!(cause & names[i].bit)) {
                    continue;
                }
                pos += snprintf(s_cause + pos, sizeof(s_cause) - pos, "%s%s",
                                pos ? "+" : "", names[i].name);
                cause &= ~names[i].bit;
                if (pos >= sizeof(s_cause)) {
                    break;
                }
            }
            if (cause != 0 && pos < sizeof(s_cause)) {
                snprintf(s_cause + pos, sizeof(s_cause) - pos, "%s0x%x",
                         pos ? "+" : "", (unsigned)cause);
            }
            (void)hwinfo_clear_reset_cause();
        }

        /* A crash reboots through sys_reboot(), so the register only ever
         * says "sw" for it.  The handler's note says which one it was. */
        const char *crash = fatal_last_crash();

        if (crash) {
            pos += snprintf(s_cause + pos, sizeof(s_cause) - pos, "%s%s",
                            pos ? "+" : "", crash);
        }

        /* And a watchdog reset says where the loop was stuck. */
        const char *stall = watchdog_last_stall();

#if IS_ENABLED(CONFIG_APP_HOLD_BREAK)
        /* A reset nobody asked for says where the hold breaker was, too
         * (crumb.c).  rst= is varchar(48) on the server, in strict mode, and a
         * longer value loses the record: the parts are fitted in order of what
         * each is worth (holdbreak.h). */
        const bool unasked = (raw & (RESET_WATCHDOG | RESET_CPU_LOCKUP)) != 0 ||
                             crash != NULL;
        char head[sizeof(s_cause)];

        memcpy(head, s_cause, sizeof(head));
        (void)hb_rst_compose(s_cause, sizeof(s_cause), head,
                             unasked ? crumb_previous() : NULL, stall);
#else
        if (stall && pos < sizeof(s_cause)) {
            snprintf(s_cause + pos, sizeof(s_cause) - pos, "%s%s",
                     pos ? "+" : "", stall);
        }
#endif
    }
    return s_cause[0] ? s_cause : NULL;
}
