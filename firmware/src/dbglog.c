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
 * DRAINING
 * --------
 * send_data() appends as many lines as fit its budget to the outgoing record
 * as "L,<line>" records (dbglog_take), and only frees them once the datagram
 * has left (dbglog_ack).  A failed send leaves them for next time.  Nothing
 * here sends on its own and nothing here logs: a backend that logged would
 * recurse.
 *
 * MEMORY
 * ------
 * Static, so the cost is fixed at link time: HEAD + TAIL bytes plus a line
 * buffer on the stack of whichever thread is logging (immediate mode) or the
 * log thread (deferred mode).  Both rings use a byte layout of [len][text]
 * per line, so nothing is wasted on fixed-width slots.
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

#define HEAD_SIZE  CONFIG_APP_DEBUG_LOG_HEAD
#define TAIL_SIZE  CONFIG_APP_DEBUG_LOG_TAIL
#define DBG_LINE_MAX   CONFIG_APP_DEBUG_LOG_LINE_MAX

BUILD_ASSERT(DBG_LINE_MAX <= 255, "line length is stored in one byte");
BUILD_ASSERT(HEAD_SIZE > DBG_LINE_MAX + 1 && TAIL_SIZE > DBG_LINE_MAX + 1,
             "each ring must hold at least one full line");

/* -- byte ring of [len][text] entries -------------------------------------- */

struct ring {
    char     *buf;
    uint16_t  size;
    uint16_t  rd;       /* offset of the oldest entry */
    uint16_t  wr;       /* offset where the next entry goes */
    uint16_t  used;     /* bytes, including the length bytes */
    uint16_t  lines;
};

static char s_head_buf[HEAD_SIZE];
static char s_tail_buf[TAIL_SIZE];

static struct ring s_head = { .buf = s_head_buf, .size = HEAD_SIZE };
static struct ring s_tail = { .buf = s_tail_buf, .size = TAIL_SIZE };

static struct k_spinlock s_lock;

/* Lines lost: evicted from the tail, or dropped by the log core itself in
 * deferred mode.  Reported as one marker line on the next drain. */
static uint32_t s_dropped;

/* Duplicate collapsing: the last line stored, minus its timestamp. */
static char     s_last[DBG_LINE_MAX + 1];
static uint32_t s_repeat;
static int64_t  s_repeat_ms;

/* In flight: what the last dbglog_take() handed out, freed by dbglog_ack(). */
static uint16_t s_take_head;
static uint16_t s_take_tail;
static uint32_t s_take_dropped;

static bool ring_fits(const struct ring *r, uint8_t len)
{
    return (uint32_t)r->used + 1U + len <= r->size;
}

static void ring_put_byte(struct ring *r, char c)
{
    r->buf[r->wr] = c;
    r->wr = (r->wr + 1U) % r->size;
}

static void ring_push(struct ring *r, const char *s, uint8_t len)
{
    ring_put_byte(r, (char)len);
    for (uint8_t i = 0; i < len; i++) {
        ring_put_byte(r, s[i]);
    }
    r->used  += 1U + len;
    r->lines += 1U;
}

static void ring_pop(struct ring *r)
{
    uint8_t len = (uint8_t)r->buf[r->rd];

    r->rd     = (r->rd + 1U + len) % r->size;
    r->used  -= 1U + len;
    r->lines -= 1U;
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

/* -- capture --------------------------------------------------------------- */

/* Store one formatted line.  Called with the lock held. */
static void store(const char *line, uint8_t len)
{
    if (ring_fits(&s_head, len)) {
        ring_push(&s_head, line, len);
        return;
    }
    while (!ring_fits(&s_tail, len)) {
        /* Never pull a line out from under a send in progress: dropping the
         * new one instead keeps dbglog_ack()'s counts honest, and the window
         * is one datagram long. */
        if (s_tail.lines == 0 || s_take_tail > 0) {
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

    if (text_len == strlen(s_last) && memcmp(text, s_last, text_len) == 0) {
        s_repeat++;
        s_repeat_ms = ms;
    } else {
        flush_repeat();
        memcpy(s_last, text, text_len);
        s_last[text_len] = '\0';
        store(line, len);
    }

    k_spin_unlock(&s_lock, key);
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
 * away before any backend saw them.  Count them with ours. */
static void dropped(const struct log_backend *const backend, uint32_t cnt)
{
    ARG_UNUSED(backend);

    k_spinlock_key_t key = k_spin_lock(&s_lock);
    s_dropped += cnt;
    k_spin_unlock(&s_lock, key);
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
    size_t n = (size_t)s_head.used + s_tail.used;
    k_spin_unlock(&s_lock, key);
    return n;
}

/* Append lines from `r` as "\nL,<line>" until `max` is exhausted.  Returns
 * bytes written; `*taken` counts the lines.  Called with the lock held. */
static size_t drain_ring(const struct ring *r, char *out, size_t max,
                         uint16_t *taken)
{
    uint16_t off = r->rd;
    size_t used = 0;
    char line[DBG_LINE_MAX];

    for (uint16_t i = 0; i < r->lines; i++) {
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

int dbglog_take(char *out, size_t max)
{
    size_t used = 0;

    k_spinlock_key_t key = k_spin_lock(&s_lock);

    /* Anything handed out before and never acked goes again. */
    s_take_head = 0;
    s_take_tail = 0;
    s_take_dropped = 0;

    flush_repeat();

    used += drain_ring(&s_head, out + used, max - used, &s_take_head);

    /* Only once the head has gone out entirely: the marker sits between
     * the onset and the recent lines, which is where the gap is. */
    if (s_take_head == s_head.lines) {
        if (s_dropped > 0) {
            char marker[DBG_LINE_MAX];
            int n = snprintf(marker, sizeof(marker),
                             "\nL,%lld,W,dbglog: %u lines dropped",
                             (long long)k_uptime_get(), (unsigned)s_dropped);

            if (n > 0 && used + (size_t)n <= max) {
                memcpy(out + used, marker, (size_t)n);
                used += (size_t)n;
                s_take_dropped = s_dropped;
            }
        }
        if (s_take_dropped == s_dropped) {
            used += drain_ring(&s_tail, out + used, max - used,
                               &s_take_tail);
        }
    }

    k_spin_unlock(&s_lock, key);
    return (int)used;
}

void dbglog_ack(bool sent)
{
    k_spinlock_key_t key = k_spin_lock(&s_lock);

    if (sent) {
        while (s_take_head > 0 && s_head.lines > 0) {
            ring_pop(&s_head);
            s_take_head--;
        }
        while (s_take_tail > 0 && s_tail.lines > 0) {
            ring_pop(&s_tail);
            s_take_tail--;
        }
        s_dropped -= s_take_dropped;
    }
    s_take_head = 0;
    s_take_tail = 0;
    s_take_dropped = 0;

    k_spin_unlock(&s_lock, key);
}

/* -- reset cause ----------------------------------------------------------- */

/* Read once and cleared, so the next boot reports only its own cause: the
 * RESETREAS bits are sticky and would otherwise accumulate across restarts.
 * NULL when the register is unreadable or says nothing (a plain power-on on
 * some parts). */
const char *dbglog_reset_cause(void)
{
    static char s_cause[40];
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

        s_read = true;
        if (hwinfo_get_reset_cause(&cause) == 0 && cause != 0) {
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
    }
    return s_cause[0] ? s_cause : NULL;
}
