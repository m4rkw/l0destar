"""Host tests for src/dbglog.c (CONFIG_APP_DEBUG_LOG): the log backend's rings.

The real dbglog.c is #included into a harness translation unit built with the
host compiler against stub Zephyr headers, so the tests can call the static
capture() directly and reach the statics to simulate a reset: every plain
static goes back to its boot value, the __noinit block is kept (__noinit is
empty on the host).  What is checked: lines are held until the server answers
and go again when it does not, the dropped-line counts stay honest through a
hold, and the rings come back after a warm reset only when the retained block
is plainly ours.

    uv run --python 3.12 --with pytest pytest -q firmware/tests/test_dbglog.py
"""
import ctypes as C
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile

import pytest

SRC = os.environ.get("DB_SRC", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src"))

KERNEL_H = r"""
#ifndef STUB_KERNEL_H
#define STUB_KERNEL_H
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define Z_IS_ENABLED1(m) Z_IS_ENABLED2(_XXXX##m)
#define _XXXX1 _YYYY,
#define Z_IS_ENABLED2(a) Z_IS_ENABLED3(a 1, 0)
#define Z_IS_ENABLED3(ignore, val, ...) val
#define IS_ENABLED(m) Z_IS_ENABLED1(m)
#define ARG_UNUSED(x) (void)(x)
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define BUILD_ASSERT(c, msg) _Static_assert(c, msg)

int64_t k_uptime_get(void);
static inline uint32_t k_cycle_get_32(void) { return 0; }
static inline uint32_t sys_clock_hw_cycles_per_sec(void) { return 32768; }
static inline int64_t k_ticks_to_ms_floor64(int64_t t) { return t; }

/* A spinlock that counts: the harness checks it is never taken twice and that
 * the retained block is sealed each time it is released. */
struct k_spinlock { int locked; };
typedef struct { int key; } k_spinlock_key_t;
extern int g_lock_depth, g_lock_bad;
void h_unlocking(void);
static inline k_spinlock_key_t k_spin_lock(struct k_spinlock *l)
{
    (void)l;
    if (++g_lock_depth != 1) g_lock_bad++;
    return (k_spinlock_key_t){0};
}
static inline void k_spin_unlock(struct k_spinlock *l, k_spinlock_key_t k)
{
    (void)l; (void)k;
    h_unlocking();
    g_lock_depth--;
}
#endif
"""

LOG_BACKEND_H = r"""
#ifndef STUB_LOG_BACKEND_H
#define STUB_LOG_BACKEND_H
#include <stdint.h>
#include <stdbool.h>
union log_msg_generic;
struct log_backend;
struct log_backend_api {
    void (*process)(const struct log_backend *const, union log_msg_generic *);
    void (*dropped)(const struct log_backend *const, uint32_t);
    void (*panic)(const struct log_backend *const);
};
struct log_backend { const struct log_backend_api *api; bool autostart; };
#define LOG_BACKEND_DEFINE(name, api, autostart) \
    const struct log_backend name = { &api, autostart }
#endif
"""

LOG_CTRL_H = r"""
#ifndef STUB_LOG_CTRL_H
#define STUB_LOG_CTRL_H
#include <stdint.h>
static inline const char *log_source_name_get(uint8_t d, uint32_t s) { (void)d; (void)s; return "stub"; }
#endif
"""

LOG_MSG_H = r"""
#ifndef STUB_LOG_MSG_H
#define STUB_LOG_MSG_H
#include <stdint.h>
#include <stddef.h>
#define LOG_LEVEL_ERR 1
#define LOG_LEVEL_WRN 2
struct log_msg { int level; };
union log_msg_generic { struct log_msg log; };
static inline uint8_t log_msg_get_level(struct log_msg *m) { return (uint8_t)m->level; }
static inline uint32_t log_msg_get_timestamp(struct log_msg *m) { (void)m; return 0; }
static inline int16_t log_msg_get_source_id(struct log_msg *m) { (void)m; return 0; }
static inline uint8_t log_msg_get_domain(struct log_msg *m) { (void)m; return 0; }
static inline uint8_t *log_msg_get_package(struct log_msg *m, size_t *l) { (void)m; *l = 0; return NULL; }
static inline uint8_t *log_msg_get_data(struct log_msg *m, size_t *l) { (void)m; *l = 0; return NULL; }
#endif
"""

CBPRINTF_H = r"""
#ifndef STUB_CBPRINTF_H
#define STUB_CBPRINTF_H
#include <stdint.h>
typedef int (*cbprintf_cb)(int c, void *ctx);
static inline int cbpprintf(cbprintf_cb out, void *ctx, uint8_t *pkg) { (void)out; (void)ctx; (void)pkg; return 0; }
#endif
"""

HWINFO_H = r"""
#ifndef STUB_HWINFO_H
#define STUB_HWINFO_H
#include <stdint.h>
#define RESET_PIN (1u << 0)
#define RESET_SOFTWARE (1u << 1)
#define RESET_BROWNOUT (1u << 2)
#define RESET_POR (1u << 3)
#define RESET_WATCHDOG (1u << 4)
#define RESET_DEBUG (1u << 5)
#define RESET_SECURITY (1u << 6)
#define RESET_LOW_POWER_WAKE (1u << 7)
#define RESET_CPU_LOCKUP (1u << 8)
static inline int hwinfo_get_reset_cause(uint32_t *c) { *c = 0; return 0; }
static inline int hwinfo_clear_reset_cause(void) { return 0; }
#endif
"""

HARNESS = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dbglog.c"

/* -- the stubs dbglog.c links against -------------------------------------- */
int g_lock_depth, g_lock_bad, g_unsealed;
static int64_t g_ms;
int64_t k_uptime_get(void) { return g_ms; }
const char *fatal_last_crash(void) { return NULL; }
const char *watchdog_last_stall(void) { return NULL; }
size_t hb_rst_compose(char *out, size_t out_sz, const char *cause,
                      const char *crumb, const char *stall)
{
    (void)out; (void)out_sz; (void)cause; (void)crumb; (void)stall;
    return 0;
}

/* Every unlock: once the module is in use the block must be sealed. */
void h_unlocking(void)
{
    if (s_ready && s_store.sum != store_sum()) {
        g_unsealed++;
    }
}

/* -- what the tests drive --------------------------------------------------- */
void h_set_ms(int64_t ms) { g_ms = ms; }
int  h_lock_bad(void) { return g_lock_bad + (g_lock_depth != 0 ? 1000 : 0); }
int  h_unsealed(void) { return g_unsealed; }

void h_capture(const char *line, int len, int64_t ms) { capture(line, (uint8_t)len, ms); }
int  h_take(char *out, int max) { return dbglog_take(out, (size_t)max); }
void h_sent(uint32_t id) { dbglog_sent(id); }
void h_answered(uint32_t id) { dbglog_answered(id); }
int  h_unanswered(void) { return dbglog_unanswered(); }
void h_expire(void) { dbglog_expire(); }
void h_settle(void) { dbglog_settle(); }
void h_ack(int sent) { dbglog_ack(sent != 0); }
long h_pending(void) { return (long)dbglog_pending(); }
void h_core_dropped(uint32_t n) { dropped(NULL, n); }
uint32_t h_dropped(void) { return s_dropped; }
uint32_t h_core(void) { return s_core_dropped; }

/* A warm reset: every plain static back to its boot value, the retained
 * block (and nothing else) kept. */
void h_reset(void)
{
    s_ready = false;
    memset(s_last, 0, sizeof(s_last));
    s_repeat = 0;
    s_repeat_ms = 0;
    s_take_head = s_take_tail = 0;
    s_take_dropped = s_take_core = 0;
    s_held = false;
    s_held_id = 0;
    s_held_ms = 0;
}

uint8_t *h_store(void) { return (uint8_t *)&s_store; }
int h_store_size(void) { return (int)sizeof(s_store); }
void h_reseal(void) { s_store.sum = store_sum(); }
int h_off(const char *what)
{
    if (!strcmp(what, "magic"))      return (int)offsetof(struct dbg_store, magic);
    if (!strcmp(what, "head_size"))  return (int)offsetof(struct dbg_store, head_size);
    if (!strcmp(what, "tail_size"))  return (int)offsetof(struct dbg_store, tail_size);
    if (!strcmp(what, "line_max"))   return (int)offsetof(struct dbg_store, line_max);
    if (!strcmp(what, "block_size")) return (int)offsetof(struct dbg_store, block_size);
    if (!strcmp(what, "head"))       return (int)offsetof(struct dbg_store, head);
    if (!strcmp(what, "tail"))       return (int)offsetof(struct dbg_store, tail);
    if (!strcmp(what, "dropped"))    return (int)offsetof(struct dbg_store, dropped);
    if (!strcmp(what, "core"))       return (int)offsetof(struct dbg_store, core_dropped);
    if (!strcmp(what, "sum"))        return (int)offsetof(struct dbg_store, sum);
    if (!strcmp(what, "head_buf"))   return (int)offsetof(struct dbg_store, head_buf);
    if (!strcmp(what, "tail_buf"))   return (int)offsetof(struct dbg_store, tail_buf);
    return -1;
}
int h_sizes(int which) { return which == 0 ? HEAD_SIZE : which == 1 ? TAIL_SIZE : DBG_LINE_MAX; }

#ifdef FUZZ_MAIN
/* Garbage and damaged blocks, under the sanitisers when the toolchain has them:
 * whatever the bytes, the first use must not read out of range and what it
 * keeps must be whole lines. */
static uint64_t rng_state;
static uint32_t rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state >> 11);
}

static void fill(int n)
{
    char line[DBG_LINE_MAX + 1];

    for (int i = 0; i < n; i++) {
        int len = snprintf(line, sizeof(line), "%d,%c,mod: message number %d xx",
                           1000 + i * 7, (i & 1) ? 'E' : 'W', i);
        capture(line, (uint8_t)len, 1000 + i * 7);
    }
}

static int check_output(const char *out, int n)
{
    /* \nL,<digits>,<E|W>,<printable>... repeated */
    int i = 0;

    while (i < n) {
        if (out[i] != '\n' || i + 2 >= n || out[i + 1] != 'L' || out[i + 2] != ',') return 0;
        i += 3;
        int d = 0;
        while (i < n && out[i] >= '0' && out[i] <= '9') { i++; d++; }
        if (!d || i + 3 > n || out[i] != ',' || (out[i + 1] != 'E' && out[i + 1] != 'W') ||
            out[i + 2] != ',') return 0;
        while (i < n && out[i] != '\n') {
            if ((unsigned char)out[i] < 0x20 || (unsigned char)out[i] > 0x7e) return 0;
            i++;
        }
    }
    return 1;
}

int main(int argc, char **argv)
{
    int runs = argc > 1 ? atoi(argv[1]) : 2000;
    static char out[HEAD_SIZE + TAIL_SIZE + 1024];

    for (int seed = 1; seed <= runs; seed++) {
        int mode = seed % 5;

        rng_state = 0x9E3779B97F4A7C15ull * (uint64_t)seed + 1;
        h_reset();
        memset(&s_store, 0, sizeof(s_store));
        if (mode == 0) {
            for (size_t i = 0; i < sizeof(s_store); i++) ((uint8_t *)&s_store)[i] = (uint8_t)rnd();
        } else {
            fill(10 + (int)(rnd() % 90));
            dropped(NULL, rnd() % 5);
            h_reset();
            if (mode == 1) {                /* random buffers, intact header */
                for (int i = 0; i < HEAD_SIZE; i++) s_store.head_buf[i] = (char)rnd();
                for (int i = 0; i < TAIL_SIZE; i++) s_store.tail_buf[i] = (char)rnd();
            } else if (mode == 2) {         /* a few damaged bytes anywhere */
                for (int k = 0, n = 1 + (int)(rnd() % 4); k < n; k++) {
                    ((uint8_t *)&s_store)[rnd() % sizeof(s_store)] ^= (uint8_t)(1 + rnd() % 255);
                }
            } else if (mode == 3) {         /* random positions */
                s_store.head.rd = (uint16_t)rnd(); s_store.head.wr = (uint16_t)rnd();
                s_store.head.used = (uint16_t)rnd(); s_store.head.lines = (uint16_t)rnd();
                s_store.tail.rd = (uint16_t)rnd(); s_store.tail.wr = (uint16_t)rnd();
                s_store.tail.used = (uint16_t)rnd(); s_store.tail.lines = (uint16_t)rnd();
            } else {                        /* positions plausible, bytes damaged */
                s_store.head.lines = (uint16_t)(rnd() % 40);
                s_store.tail.used = (uint16_t)(rnd() % TAIL_SIZE);
                s_store.tail.wr = (uint16_t)((s_store.tail.rd + s_store.tail.used) % TAIL_SIZE);
                ((uint8_t *)s_store.head_buf)[rnd() % HEAD_SIZE] = (uint8_t)rnd();
            }
            if (mode != 2 || (rnd() & 1)) {
                h_reseal();                 /* so the structural checks are what decide */
            }
        }

        switch (seed % 3) {                 /* whichever entry point comes first */
        case 0: (void)dbglog_pending(); break;
        case 1: h_core_dropped(1); break;
        default: dbglog_ack(true); break;
        }
        int n = dbglog_take(out, sizeof(out));
        if (mode == 0 && (n != 0 && seed % 3 != 1)) { printf("seed %d: garbage kept\n", seed); return 1; }
        if (!check_output(out, n)) { printf("seed %d: bad output\n", seed); return 1; }
        dbglog_ack(true);
        if (g_lock_bad || g_unsealed) { printf("seed %d: lock or seal\n", seed); return 1; }

        /* normal operation afterwards, and the new line is not zero-stamped */
        capture("123456,W,mod: after", 19, 123456);
        n = dbglog_take(out, sizeof(out));
        if (!check_output(out, n) || !strstr(out, "L,123456,W,mod: after")) {
            printf("seed %d: no normal operation\n", seed); return 1;
        }
        dbglog_ack(true);
    }
    printf("ok\n");
    return 0;
}
#endif
"""

HEAD, TAIL, LINE_MAX = 256, 384, 64
CFLAGS = [
    "-std=gnu11", "-Wall", "-Wextra", "-Wno-unused-parameter", "-O1", "-D__noinit=",
    f"-DCONFIG_APP_DEBUG_LOG_HEAD={HEAD}", f"-DCONFIG_APP_DEBUG_LOG_TAIL={TAIL}",
    f"-DCONFIG_APP_DEBUG_LOG_LINE_MAX={LINE_MAX}",
    "-DCONFIG_APP_ACK_TIMEOUT_S=30", "-DCONFIG_APP_DEBUG_LOG=1",
]
ACK_MS = 30_000


def write_stubs(d):
    for rel, text in (("zephyr/kernel.h", KERNEL_H),
                      ("zephyr/logging/log_backend.h", LOG_BACKEND_H),
                      ("zephyr/logging/log_ctrl.h", LOG_CTRL_H),
                      ("zephyr/logging/log_msg.h", LOG_MSG_H),
                      ("zephyr/sys/cbprintf.h", CBPRINTF_H),
                      ("zephyr/drivers/hwinfo.h", HWINFO_H)):
        path = os.path.join(d, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as f:
            f.write(text)
    with open(os.path.join(d, "h.c"), "w") as f:
        f.write(HARNESS)


def build(slots, tag):
    d = tempfile.mkdtemp()
    write_stubs(d)
    so = os.path.join(d, f"libdbg_{tag}.so")
    cmd = ["cc", *CFLAGS, f"-DCONFIG_APP_DATABUF_UNACKED_SLOTS={slots}", "-shared", "-fPIC",
           "-o", so, "-I", d, "-I", SRC, os.path.join(d, "h.c")]
    r = subprocess.run(cmd, capture_output=True, text=True)
    assert r.returncode == 0 and r.stderr == "", r.stderr          # clean under -Wall -Wextra
    return so


_copies = [0]


def load(so):
    # A fresh copy for each test: the rings and the retained block are statics.
    _copies[0] += 1
    fresh = os.path.join(os.path.dirname(so), f"copy{_copies[0]}_{os.path.basename(so)}")
    shutil.copyfile(so, fresh)
    L = C.CDLL(fresh)
    L.h_set_ms.argtypes = [C.c_int64]
    L.h_capture.argtypes = [C.c_char_p, C.c_int, C.c_int64]
    L.h_take.argtypes = [C.c_char_p, C.c_int]
    L.h_sent.argtypes = [C.c_uint32]
    L.h_answered.argtypes = [C.c_uint32]
    L.h_ack.argtypes = [C.c_int]
    L.h_pending.restype = C.c_long
    L.h_core_dropped.argtypes = [C.c_uint32]
    L.h_dropped.restype = C.c_uint32
    L.h_core.restype = C.c_uint32
    L.h_store.restype = C.POINTER(C.c_uint8)
    L.h_off.argtypes = [C.c_char_p]
    L.h_sizes.argtypes = [C.c_int]
    return L


@pytest.fixture(scope="module")
def so_hold():
    return build(6, "hold")


@pytest.fixture(scope="module")
def so_nohold():
    return build(0, "nohold")


def fuzz_exe():
    """The garbage run as a program, with the sanitisers if the toolchain has them."""
    d = tempfile.mkdtemp()
    write_stubs(d)
    exe = os.path.join(d, "fuzz")
    base = ["cc", *CFLAGS, "-DCONFIG_APP_DATABUF_UNACKED_SLOTS=6", "-DFUZZ_MAIN", "-g",
            "-I", d, "-I", SRC, os.path.join(d, "h.c"), "-o", exe]
    san = subprocess.run([*base, "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined"],
                         capture_output=True, text=True)
    if san.returncode == 0:
        assert san.stderr == "", san.stderr
        return exe, True
    r = subprocess.run(base, capture_output=True, text=True)
    assert r.returncode == 0 and r.stderr == "", r.stderr
    return exe, False


class Log:
    def __init__(self, so):
        self.L = load(so)
        self.n = 0
        self.ms = 1000
        self.L.h_set_ms(self.ms)

    # -- feeding it ------------------------------------------------------------
    def add(self, k=1, ms=None, level="W", body=None):
        out = []
        for _ in range(k):
            self.ms += 7 if ms is None else 0
            t = self.ms if ms is None else ms
            line = ("%d,%s,mod: %s" % (t, level, body or "message number %04d" % self.n)).encode()
            self.n += 1
            self.L.h_capture(line, len(line), t)
            out.append(line)
        self.L.h_set_ms(self.ms)
        return out

    def at(self, ms):
        self.L.h_set_ms(ms)

    # -- reading it back -------------------------------------------------------
    def take(self, max=4096):
        buf = C.create_string_buffer(max + 1)
        n = self.L.h_take(buf, max)
        return buf.raw[:n]

    def take_lines(self, max=4096):
        raw = self.take(max)
        assert raw == b"" or raw.startswith(b"\nL,")
        return [x[2:] for x in raw.split(b"\n")[1:]] if raw else []

    def check(self):
        assert self.L.h_lock_bad() == 0                   # never nested, always released
        assert self.L.h_unsealed() == 0                   # the block sealed at every unlock

    # -- the retained block ----------------------------------------------------
    def block(self):
        return bytearray(C.string_at(self.L.h_store(), self.L.h_store_size()))

    def put_block(self, b):
        C.memmove(self.L.h_store(), bytes(b), len(b))

    def off(self, what):
        return self.L.h_off(what.encode())

    def reset(self):
        self.L.h_reset()


@pytest.fixture
def log(so_hold):
    lg = Log(so_hold)
    yield lg
    lg.check()


def u16(b, o):
    return b[o] | (b[o + 1] << 8)


def w16(b, o, v):
    b[o] = v & 255
    b[o + 1] = (v >> 8) & 255


def w32(b, o, v):
    for i in range(4):
        b[o + i] = (v >> (8 * i)) & 255


def zeroed(line):
    """The restored form: the leading digit run replaced by zeros of the same length."""
    m = re.match(rb"\d+", line)
    return b"0" * len(m.group(0)) + line[len(m.group(0)):]


def is_marker(x):
    return b"lines dropped" in x


def lines_only(taken):
    return [x for x in taken if b"dbglog: " not in x]


def marker_count(taken, what=b" lines dropped"):
    for x in taken:
        m = re.search(rb"dbglog: (\d+)" + re.escape(what), x)
        if m and (what != b" lines dropped" or b"console" not in x):
            return int(m.group(1))
    return None


# --------------------------------------------------------------------------- holding

def test_lines_taken_stay_held_until_the_reply_to_that_datagram_arrives_and_then_are_gone(log):
    sent = log.add(3)
    assert log.take_lines() == sent
    log.L.h_sent(41)
    assert log.L.h_unanswered() == 1
    held = log.L.h_pending()
    assert held > 0                                        # still counted as pending
    log.L.h_answered(40)                                   # another datagram's reply
    assert log.L.h_unanswered() == 1 and log.L.h_pending() == held
    log.L.h_answered(41)
    assert log.L.h_unanswered() == 0 and log.L.h_pending() == 0
    assert log.take() == b""


def test_a_send_with_id_zero_frees_the_lines_at_once(log):
    log.add(3)
    assert log.take() != b""
    log.L.h_sent(0)                                        # the send was its own receipt
    assert log.L.h_unanswered() == 0 and log.L.h_pending() == 0


def test_with_no_hold_configured_a_sent_batch_is_freed_at_once(so_nohold):
    lg = Log(so_nohold)
    lg.add(3)
    assert lg.take() != b""
    lg.L.h_sent(41)
    assert lg.L.h_unanswered() == 0 and lg.L.h_pending() == 0
    lg.check()


def test_a_take_that_handed_out_nothing_holds_nothing(log):
    assert log.take() == b""
    log.L.h_sent(5)
    assert log.L.h_unanswered() == 0
    log.add(1)                                             # a line that was never taken
    log.L.h_sent(6)
    assert log.L.h_unanswered() == 0 and log.L.h_pending() > 0


def test_while_a_batch_is_held_nothing_is_handed_out_and_new_lines_wait_in_order(log):
    first = log.add(3)
    assert log.take_lines() == first
    log.L.h_sent(7)
    later = log.add(4)
    assert log.take() == b""                               # one batch outstanding
    assert log.take() == b""
    log.L.h_answered(7)
    got = log.take_lines()
    assert got == later                                    # exactly the new ones, in order
    log.L.h_sent(8)
    log.L.h_answered(8)
    assert log.take() == b"" and log.L.h_pending() == 0


def test_the_taken_lines_are_what_was_captured_in_order(log):
    lines = log.add(6)
    assert log.take_lines() == lines


# --------------------------------------------------------------------------- reverting

def test_a_hold_older_than_the_ack_timeout_is_dropped_and_the_same_lines_go_again_once(log):
    lines = log.add(3)
    t0 = log.ms
    assert log.take_lines() == lines
    log.L.h_sent(9)
    log.at(t0 + ACK_MS - 1)
    log.L.h_expire()
    assert log.L.h_unanswered() == 1                       # not before the timeout
    assert log.take() == b""
    log.at(t0 + ACK_MS)
    log.L.h_expire()
    assert log.L.h_unanswered() == 0
    assert log.take_lines() == lines                       # the same lines, again
    log.L.h_sent(10)
    log.L.h_answered(9)                                    # the late reply to the old datagram
    assert log.L.h_unanswered() == 1                       # frees nothing
    log.L.h_answered(10)
    assert log.take() == b"" and log.L.h_pending() == 0    # and they went once more only


def test_taking_after_the_timeout_applies_the_expiry_itself(log):
    lines = log.add(2)
    t0 = log.ms
    log.take()
    log.L.h_sent(9)
    log.at(t0 + ACK_MS - 1)
    assert log.take() == b""
    log.at(t0 + ACK_MS)
    assert log.take_lines() == lines


def test_settling_drops_a_hold_at_once_and_unanswered_follows(log):
    lines = log.add(2)
    log.take()
    log.L.h_sent(9)
    assert log.L.h_unanswered() == 1
    log.L.h_settle()
    assert log.L.h_unanswered() == 0
    assert log.take_lines() == lines
    log.L.h_settle()                                       # nothing held: nothing happens
    assert log.L.h_unanswered() == 0


def test_a_reply_after_the_hold_was_dropped_finds_nothing_to_free(log):
    lines = log.add(2)
    log.take()
    log.L.h_sent(9)
    log.L.h_settle()
    log.L.h_answered(9)
    assert log.L.h_pending() > 0
    assert log.take_lines() == lines


# --------------------------------------------------------------------------- dropped counts

def overflow(log, k=60):
    """More lines than the two rings hold: the tail evicts, the head keeps the onset."""
    return log.add(k)


def test_a_full_tail_refuses_new_lines_while_its_lines_are_held_and_counts_them(log):
    overflow(log)
    dropped0 = log.L.h_dropped()
    assert dropped0 > 0
    log.take()
    log.L.h_sent(1)
    log.add(5)                                             # nothing may be evicted now
    assert log.L.h_dropped() == dropped0 + 5
    log.L.h_answered(1)                                    # the held tail is freed
    assert log.L.h_dropped() == 5                          # what the marker reported is gone
    taken = log.take_lines()
    assert marker_count(taken) == 5                        # the refused lines, reported next
    log.L.h_sent(2)
    log.L.h_answered(2)
    assert log.L.h_dropped() == 0 and log.L.h_pending() == 0


def test_a_reverted_batch_reports_the_dropped_count_again_without_subtracting_twice(log):
    overflow(log)
    first = log.take_lines()
    n0 = marker_count(first)
    assert n0 and n0 == log.L.h_dropped()
    log.L.h_sent(1)
    log.add(5)                                             # refused: counted
    log.L.h_settle()                                       # no reply: back to pending
    assert log.L.h_dropped() == n0 + 5                     # a revert subtracts nothing
    again = log.take_lines()
    assert marker_count(again) == n0 + 5
    assert lines_only(again) == lines_only(first)          # the same lines
    log.L.h_sent(2)
    log.L.h_answered(2)
    assert log.L.h_dropped() == 0                          # and the final count is right
    assert log.take() == b""


def test_the_console_dropped_count_is_held_reverted_and_freed_the_same_way(log):
    log.add(2)
    log.L.h_core_dropped(3)
    taken = log.take_lines()
    assert marker_count(taken, b" console lines dropped") == 3
    log.L.h_sent(1)
    log.L.h_core_dropped(2)                                # more while held
    log.L.h_settle()
    assert log.L.h_core() == 5                             # nothing subtracted by the revert
    assert marker_count(log.take_lines(), b" console lines dropped") == 5
    log.L.h_sent(2)
    log.L.h_core_dropped(1)
    log.L.h_answered(2)
    assert log.L.h_core() == 1                             # only what the batch reported
    assert marker_count(log.take_lines(), b" console lines dropped") == 1
    log.L.h_sent(3)
    log.L.h_answered(3)
    assert log.L.h_core() == 0


def test_the_markers_sit_between_the_head_and_the_tail(log):
    overflow(log)
    log.L.h_core_dropped(2)
    taken = log.take_lines()
    kinds = ["m" if b"dbglog: " in x else "l" for x in taken]
    first_m = kinds.index("m")
    assert kinds[:first_m].count("l") == first_m and first_m > 0
    assert kinds[first_m:first_m + 2] == ["m", "m"]
    assert "m" not in kinds[first_m + 2:] and "l" in kinds[first_m + 2:]


# --------------------------------------------------------------------------- id-less callers

def test_acking_true_frees_what_was_taken_and_its_dropped_count(log):
    overflow(log)
    log.take()
    log.L.h_ack(1)
    assert log.L.h_pending() == 0 and log.L.h_dropped() == 0
    assert log.take() == b""


def test_acking_false_leaves_everything_for_the_next_take(log):
    overflow(log)
    first = log.take()
    log.L.h_ack(0)
    assert log.L.h_dropped() > 0
    assert log.take() == first


def test_a_take_replaced_by_another_before_any_ack_is_not_double_counted(log):
    lines = log.add(4)
    log.take()
    assert log.take_lines() == lines
    log.L.h_ack(1)
    assert log.L.h_pending() == 0


def test_acking_does_not_touch_a_batch_that_is_held(log):
    lines = log.add(3)
    log.take()
    log.L.h_sent(5)
    log.L.h_ack(1)
    log.L.h_ack(0)
    assert log.L.h_unanswered() == 1 and log.L.h_pending() > 0
    log.L.h_answered(5)
    assert log.L.h_pending() == 0
    assert lines


# --------------------------------------------------------------------------- partial batches

def test_a_batch_cut_short_by_the_buffer_continues_where_it_left_off_with_nothing_lost_or_doubled(log):
    lines = log.add(18)                                    # fits the two rings without any drop
    assert log.L.h_dropped() == 0
    got = []
    for i in range(1, 100):
        part = log.take_lines(max=120)
        if not part:
            break
        assert len(part) < len(lines)                      # really in pieces
        got += part
        log.L.h_sent(i)
        log.L.h_answered(i)
    assert got == lines                                    # head before tail, each once
    assert log.L.h_pending() == 0


def test_a_cut_short_batch_that_is_never_answered_comes_back_whole_next_time(log):
    lines = log.add(18)
    part = log.take_lines(max=120)
    log.L.h_sent(1)
    log.L.h_settle()
    again = log.take_lines(max=4096)
    assert again == lines and again[:len(part)] == part


# --------------------------------------------------------------------------- reset

def fill_both(log, k=100):
    """Head full, tail wrapped many times, some lines dropped."""
    return log.add(k)


def test_a_warm_reset_keeps_the_lines_in_order_with_the_uptime_digits_replaced_by_zeros(log):
    fill_both(log)
    before = log.take_lines()
    before_dropped = marker_count(before)
    assert before_dropped
    log.reset()
    after = log.take_lines()
    assert [zeroed(x) for x in before if not is_marker(x)] == [x for x in after if not is_marker(x)]
    assert marker_count(after) == before_dropped           # the counter survived
    assert all(re.match(rb"0+,[EW],", x) for x in after if not is_marker(x))


def test_the_text_after_the_digits_is_left_exactly_as_it_was(log):
    log.add(1, ms=5, body="text with 123 digits, and commas")
    log.add(1, ms=123456789, level="E", body="x")
    log.reset()
    got = log.take_lines()
    assert got == [b"0,W,mod: text with 123 digits, and commas", b"000000000,E,mod: x"]


def test_lines_that_were_only_taken_at_the_reset_come_back_pending(log):
    lines = log.add(5)
    log.take()                                             # in flight, never sent
    log.reset()
    assert log.take_lines() == [zeroed(x) for x in lines]


def test_lines_that_were_held_at_the_reset_come_back_pending(log):
    lines = log.add(5)
    log.take()
    log.L.h_sent(12)
    log.reset()
    assert log.L.h_unanswered() == 0                       # the hold is not retained
    assert log.take_lines() == [zeroed(x) for x in lines]
    log.L.h_answered(12)                                   # the old datagram's reply: no effect


def test_the_dropped_counts_held_at_the_reset_are_reported_again(log):
    overflow(log)
    log.L.h_core_dropped(4)
    first = log.take_lines()
    n = marker_count(first)
    log.L.h_sent(1)
    log.reset()
    again = log.take_lines()
    assert marker_count(again) == n
    assert marker_count(again, b" console lines dropped") == 4


def test_a_second_reset_straight_after_keeps_them_again_unchanged(log):
    fill_both(log)
    log.reset()
    log.L.h_pending()                                      # touched, nothing sent
    snap = log.block()
    log.reset()
    again = log.take_lines()
    log.reset()
    third = log.take_lines()
    assert again == third and again
    assert bytes(log.block()[:log.off("head_buf")]) == bytes(snap[:log.off("head_buf")])


def test_the_restored_rings_take_new_lines_normally_and_those_are_not_stamped_zero(log):
    fill_both(log)
    old = log.take_lines()
    log.reset()
    log.at(50)
    new = log.add(3, ms=55)
    got = log.take_lines()
    kept = [x for x in got if not is_marker(x)]
    assert kept[-3:] == new                                # new lines last, as captured
    older = iter(zeroed(x) for x in old if not is_marker(x))
    assert all(x in older for x in kept[:-3])              # the restored ones, in order, minus evictions
    assert all(re.match(rb"0+,", x) for x in kept[:-3])
    assert all(x.startswith(b"55,") for x in kept[-3:])


def test_a_restored_tail_still_refuses_to_evict_while_its_lines_are_held(log):
    fill_both(log)
    log.reset()
    log.take()
    log.L.h_sent(1)
    before = log.L.h_dropped()
    log.add(3)
    assert log.L.h_dropped() == before + 3


def test_a_message_arriving_before_anything_else_touched_the_module_is_kept(log):
    lines = log.add(1)                                     # capture is the very first call
    assert log.take_lines() == lines


def test_a_message_arriving_first_after_a_reset_is_kept_beside_the_restored_lines(log):
    old = log.add(4)
    log.reset()
    log.at(77)
    new = log.add(1, ms=77)                                # capture before any other entry point
    assert log.take_lines() == [zeroed(x) for x in old] + new


@pytest.mark.parametrize("first", ["pending", "dropped_cb", "ack", "take", "settle", "answered"])
def test_whichever_entry_point_comes_first_after_a_reset_restores_the_lines(log, first):
    old = log.add(4)
    log.reset()
    {"pending": lambda: log.L.h_pending(),
     "dropped_cb": lambda: log.L.h_core_dropped(0),
     "ack": lambda: log.L.h_ack(0),
     "take": lambda: log.take(),
     "settle": lambda: log.L.h_settle(),
     "answered": lambda: log.L.h_answered(3)}[first]()
    if first == "take":
        log.reset()
    assert log.take_lines() == [zeroed(x) for x in old]


def test_a_block_never_written_in_this_run_of_the_machine_gives_empty_rings(so_hold):
    lg = Log(so_hold)                                      # statics start zero: not our block
    assert lg.L.h_pending() == 0 and lg.take() == b""
    lg.add(1)
    assert len(lg.take_lines()) == 1
    lg.check()


# --------------------------------------------------------------------------- rejection

def seeded(log):
    """Head and tail both holding lines (tail wrapped), then the block as bytes."""
    fill_both(log, 80)
    return log.block()


def damage_cases():
    """(name, mutation(block, log), needs_reseal): each leaves an invalid block."""
    def at(b, log, ring, field):
        return log.off(ring) + {"rd": 0, "wr": 2, "used": 4, "lines": 6}[field]

    cases = []

    def case(name, reseal=True):
        def deco(fn):
            cases.append((name, fn, reseal))
            return fn
        return deco

    @case("wrong magic")
    def _(b, log): w32(b, log.off("magic"), 0x12345678)

    @case("wrong head size in the layout")
    def _(b, log): w32(b, log.off("head_size"), log.L.h_sizes(0) + 8)

    @case("wrong tail size in the layout")
    def _(b, log): w32(b, log.off("tail_size"), log.L.h_sizes(1) - 8)

    @case("wrong line max in the layout")
    def _(b, log): w32(b, log.off("line_max"), log.L.h_sizes(2) + 1)

    @case("wrong block size in the layout")
    def _(b, log): w32(b, log.off("block_size"), log.L.h_store_size() + 4)

    @case("a flipped byte in the checksummed metadata", reseal=False)
    def _(b, log): b[log.off("dropped")] ^= 0x01

    @case("a flipped byte in the head position", reseal=False)
    def _(b, log): b[at(b, log, "head", "rd")] ^= 0x10

    @case("a wrong checksum", reseal=False)
    def _(b, log): b[log.off("sum")] ^= 0xFF

    @case("head wr inconsistent with rd and used")
    def _(b, log): w16(b, at(b, log, "head", "wr"), (u16(b, at(b, log, "head", "wr")) + 1) % HEAD)

    @case("tail wr inconsistent with rd and used")
    def _(b, log): w16(b, at(b, log, "tail", "wr"), (u16(b, at(b, log, "tail", "wr")) + 3) % TAIL)

    @case("head rd beyond the ring")
    def _(b, log): w16(b, at(b, log, "head", "rd"), HEAD)

    @case("tail used beyond the ring")
    def _(b, log): w16(b, at(b, log, "tail", "used"), TAIL + 1)

    @case("head line count too high")
    def _(b, log): w16(b, at(b, log, "head", "lines"), u16(b, at(b, log, "head", "lines")) + 1)

    @case("tail line count too low")
    def _(b, log): w16(b, at(b, log, "tail", "lines"), u16(b, at(b, log, "tail", "lines")) - 1)

    @case("a huge line count")
    def _(b, log): w16(b, at(b, log, "tail", "lines"), 0xFFFF)

    def first_len_off(b, log, ring):
        base = log.off(ring + "_buf")
        return base + u16(b, log.off(ring) + 0)

    @case("a head length byte too long")
    def _(b, log): b[first_len_off(b, log, "head")] = LINE_MAX + 1

    @case("a head length byte of 255")
    def _(b, log): b[first_len_off(b, log, "head")] = 255

    @case("a head length byte too short")
    def _(b, log): b[first_len_off(b, log, "head")] = 3

    @case("a tail length byte too short")
    def _(b, log): b[first_len_off(b, log, "tail")] = 2

    @case("a tail length byte longer than the line by one")
    def _(b, log): b[first_len_off(b, log, "tail")] += 1

    @case("a tail length byte shorter than the line by one")
    def _(b, log): b[first_len_off(b, log, "tail")] -= 1

    @case("a zero length byte")
    def _(b, log): b[first_len_off(b, log, "head")] = 0

    @case("a non-printable byte in a head text")
    def _(b, log): b[first_len_off(b, log, "head") + 12] = 0x07

    @case("a byte above 0x7e in a tail text")
    def _(b, log): b[first_len_off(b, log, "tail") + 12] = 0x80

    @case("a newline in a tail text")
    def _(b, log): b[first_len_off(b, log, "tail") + 9] = 0x0A

    @case("a line that does not start with digits")
    def _(b, log): b[first_len_off(b, log, "head") + 1] = ord("x")

    @case("a line whose digits are not followed by a comma")
    def _(b, log): b[first_len_off(b, log, "head") + 5] = ord(";")

    @case("a line with a level that is not E or W")
    def _(b, log): b[first_len_off(b, log, "tail") + 6] = ord("I")

    @case("a line with no comma after the level")
    def _(b, log): b[first_len_off(b, log, "tail") + 7] = ord(" ")

    @case("a line that is all digits")
    def _(b, log):
        o = first_len_off(b, log, "head")
        n = b[o]
        for i in range(n):
            b[o + 1 + i] = ord("1")

    @case("a well-formed line longer than the line max, in otherwise consistent rings")
    def _(b, log):
        text = b"1000,W,mod: " + b"x" * (LINE_MAX + 6 - 12)
        h = log.off("head")
        for i in range(16):
            b[h + i] = 0                                   # both rings empty ...
        w16(b, h + 2, 1 + len(text))                       # ... but for one oversized line in the head
        w16(b, h + 4, 1 + len(text))
        w16(b, h + 6, 1)
        o = log.off("head_buf")
        b[o] = len(text)
        b[o + 1:o + 1 + len(text)] = text

    @case("every byte zero", reseal=False)
    def _(b, log):
        for i in range(len(b)):
            b[i] = 0

    @case("every byte 0xFF", reseal=False)
    def _(b, log):
        for i in range(len(b)):
            b[i] = 0xFF

    return cases


CASES = damage_cases()


def test_the_undamaged_block_is_restored_so_the_damage_cases_mean_something(log):
    blk = seeded(log)
    log.reset()
    log.put_block(blk)
    log.L.h_reseal()
    assert log.take_lines() != []


@pytest.mark.parametrize("name,fn,reseal", CASES, ids=[c[0] for c in CASES])
def test_a_damaged_retained_block_gives_empty_rings_and_normal_operation_afterwards(log, name, fn, reseal):
    blk = seeded(log)
    fn(blk, log)
    log.reset()
    log.put_block(blk)
    if reseal:
        log.L.h_reseal()
    assert log.L.h_pending() == 0, name
    assert log.take() == b""
    assert log.L.h_dropped() == 0 and log.L.h_core() == 0
    new = log.add(3, ms=99999)
    assert log.take_lines() == new                         # new lines are not zero-stamped
    log.L.h_sent(0)
    log.reset()                                            # and the next reset keeps what is valid
    assert log.L.h_pending() == 0


def test_a_few_hundred_blocks_of_random_garbage_all_give_empty_rings_and_no_crash(log):
    rng = random.Random(20260930)
    size = log.L.h_store_size()
    for i in range(400):
        blk = bytearray(rng.getrandbits(8) for _ in range(size))
        if i % 3 == 1:
            for j in range(size):
                blk[j] &= 0x7F                             # mostly printable-looking
        log.reset()
        log.put_block(blk)
        assert log.L.h_pending() == 0, i
        assert log.take() == b""
    new = log.add(2)
    assert log.take_lines() == new


def test_random_damage_to_a_valid_block_never_leaves_a_half_restored_state(log):
    blk = seeded(log)
    rng = random.Random(7)
    for i in range(300):
        b = bytearray(blk)
        for _ in range(rng.randint(1, 3)):
            b[rng.randrange(len(b))] = rng.getrandbits(8)
        log.reset()
        log.put_block(b)
        if rng.random() < 0.7:
            log.L.h_reseal()
        got = log.take_lines()
        # either everything was kept or nothing; what is kept is whole lines
        assert all(re.match(rb"\d+,[EW],[\x20-\x7e]*$", x) for x in got), i
        log.L.h_ack(0)


def test_garbage_under_the_sanitisers_where_the_toolchain_has_them():
    exe, sanitised = fuzz_exe()
    r = subprocess.run([exe, "3000"], capture_output=True, text=True, timeout=300,
                       env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
    sys.stderr.write("dbglog garbage run: %s\n" %
                     ("with address,undefined sanitisers" if sanitised else "WITHOUT sanitisers"))
    assert r.returncode == 0, r.stdout + r.stderr
    assert r.stdout.strip() == "ok"
