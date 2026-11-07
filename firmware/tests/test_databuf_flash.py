"""Host tests for the flash tier of src/databuf.c (CONFIG_APP_FLASH_BACKLOG).

The real databuf.c and flashlog.c are compiled with the host compiler against
stand-ins for the rest of the firmware: a transport that records what it is
sent, a modem that is registered or not, a clock the test moves, and a flash
that counts erases and records any use of it outside a session.  What is checked
here is the part flashlog's own tests cannot see: that the ring spills into the
flash instead of thinning, that what comes back is in order and whole, and that
every way the flash can be missing or fail leaves the backlog as it was without it.

    uv run --python 3.12 --with pytest pytest -q firmware/tests/test_databuf_flash.py
"""
import ctypes as C
import os
import re
import shutil
import subprocess
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
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
int64_t k_uptime_get(void);
int32_t k_msleep(int32_t ms);
#endif
"""

LOG_H = r"""
#ifndef STUB_LOG_H
#define STUB_LOG_H
void h_log(int level, const char *fmt, ...);
#define LOG_MODULE_REGISTER(...)
#define LOG_ERR(...) h_log(1, __VA_ARGS__)
#define LOG_WRN(...) h_log(2, __VA_ARGS__)
#define LOG_INF(...) h_log(3, __VA_ARGS__)
#define LOG_DBG(...) h_log(4, __VA_ARGS__)
#endif
"""

HARNESS = r"""
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#if IS_ENABLED(CONFIG_APP_FLASH_BACKLOG)
#include "extflash.h"
#endif

/* -- clock, log ------------------------------------------------------------ */
static int64_t g_ms;
int64_t k_uptime_get(void) { return g_ms; }
int32_t k_msleep(int32_t ms) { g_ms += ms; return 0; }
void h_set_ms(int64_t ms) { g_ms = ms; }

#define LOGMAX 4000
static char g_log[LOGMAX][200];
static int  g_lvl[LOGMAX];
static int  g_nlog;
void h_log(int level, const char *fmt, ...)
{
    va_list ap;
    if (g_nlog >= LOGMAX) return;
    va_start(ap, fmt);
    vsnprintf(g_log[g_nlog], sizeof(g_log[0]), fmt, ap);
    va_end(ap);
    g_lvl[g_nlog++] = level;
}
int h_log_n(void) { return g_nlog; }
int h_log_level(int i) { return g_lvl[i]; }
const char *h_log_line(int i) { return g_log[i]; }

/* -- transport, modem, watchdog, alerts ------------------------------------ */
static int      g_registered = 1;
static int      g_send_fail;
static int      g_answer;          /* a poll brings the replies in */
static int      g_send_cost_ms;    /* what one datagram takes to send */
static int      g_stop_after = -1;
static int      g_stop_calls;
static uint32_t g_sent_id;
#define DGMAX 20000
static char    *g_dg[DGMAX];
static size_t   g_dglen[DGMAX];
static int      g_ndg;
static int      g_kicks;
bool modem_is_registered(void) { return g_registered; }
int transport_send(const uint8_t *p, size_t n)
{
    if (g_send_fail || g_ndg >= DGMAX) return -1;
    g_dg[g_ndg] = malloc(n + 1);
    memcpy(g_dg[g_ndg], p, n);
    g_dg[g_ndg][n] = 0;
    g_dglen[g_ndg++] = n;
    g_sent_id++;
    g_ms += g_send_cost_ms;
    return 0;
}
uint32_t transport_sent_id(void) { return g_sent_id; }
int transport_poll(void)
{
    if (g_answer) for (uint32_t i = 1; i <= g_sent_id; i++) databuf_ack(i);
    return 0;
}
uint32_t transport_generation(void) { return 1; }
void watchdog_kick(void) { g_kicks++; }
void alert_ack(uint32_t id) { (void)id; }
int alert_unanswered(void) { return 0; }
void alert_settle(void) {}
/* dbglog.c is not compiled here: only that databuf.c calls into it is checked. */
static uint32_t g_dbg_answered[16];
static int      g_dbg_answered_n;
static int      g_dbg_unanswered;
static int      g_dbg_settles;
void dbglog_answered(uint32_t id)
{
    if (g_dbg_answered_n < 16) g_dbg_answered[g_dbg_answered_n++] = id;
}
int dbglog_unanswered(void) { return g_dbg_unanswered; }
void dbglog_settle(void) { g_dbg_settles++; g_dbg_unanswered = 0; }
void h_dbg_set_unanswered(int v) { g_dbg_unanswered = v; }
int h_dbg_answered_n(void) { return g_dbg_answered_n; }
uint32_t h_dbg_answered(int i) { return g_dbg_answered[i]; }
int h_dbg_settles(void) { return g_dbg_settles; }
#define ALERTS 16
static char g_alert_msg[ALERTS][200];
static int  g_alert_prio[ALERTS];
static int  g_nalert;
void alert_enqueue(const char *msg, int priority)
{
    if (g_nalert >= ALERTS) return;
    snprintf(g_alert_msg[g_nalert], sizeof(g_alert_msg[0]), "%s", msg);
    g_alert_prio[g_nalert++] = priority;
}
int h_alert_n(void) { return g_nalert; }
const char *h_alert_msg(int i) { return g_alert_msg[i]; }
int h_alert_prio(int i) { return g_alert_prio[i]; }
void h_set_registered(int v) { g_registered = v; }
void h_set_send_fail(int v) { g_send_fail = v; }
int h_ndg(void) { return g_ndg; }
const char *h_dg(int i) { return g_dg[i]; }
uint32_t h_last_id(void) { return g_sent_id; }
int h_kicks(void) { return g_kicks; }

/* -- the flash and its sessions -------------------------------------------- */
#if !IS_ENABLED(CONFIG_APP_FLASH_BACKLOG)
void h_sim_reset(void) {}
void h_sim_set(int a, int b, int c, int d) { (void)a; (void)b; (void)c; (void)d; }
int h_sim_violations(void) { return 0; }
int h_sim_sessions(void) { return 0; }
uint32_t h_sim_erases(int i) { (void)i; return 0; }
void h_sim_corrupt(uint32_t addr) { (void)addr; }
int h_sim_progs(void) { return 0; }
void h_sim_set_id_ok(int ok) { (void)ok; }
#else
#define SIM_SECTORS 32
static uint8_t  sim_mem[SIM_SECTORS * 4096];
static uint32_t sim_erases[SIM_SECTORS];
static int      sim_violations;
static int      sim_in_session;
static int      sim_sessions;
static int      sim_present = 1;
static int      sim_busy;
static int      sim_fail_prog_after = -1;
static int      sim_progs;
static int      sim_unreadable;
static int      sim_id_ok = 1;      /* what RDID answers, for the check after the first session */
static struct flashlog_dev sim_dev;

static int sim_read(void *c, uint32_t a, void *b, size_t n)
{
    (void)c;
    if (!sim_in_session) sim_violations++;
    if (sim_unreadable) return -5;
    if (a + n > sizeof(sim_mem)) { sim_violations++; return -1; }
    memcpy(b, sim_mem + a, n);
    return 0;
}
static int sim_prog(void *c, uint32_t a, const void *b, size_t n)
{
    (void)c;
    if (!sim_in_session) sim_violations++;
    if (n == 0 || n > 256 || (a % 256) + n > 256 || a + n > sizeof(sim_mem)) { sim_violations++; return -1; }
    for (size_t i = 0; i < n; i++) if (sim_mem[a + i] != 0xFF) { sim_violations++; break; }
    if (sim_fail_prog_after >= 0 && sim_progs >= sim_fail_prog_after) return -1;
    sim_progs++;
    for (size_t i = 0; i < n; i++) sim_mem[a + i] &= ((const uint8_t *)b)[i];
    return 0;
}
static int sim_erase(void *c, uint32_t a)
{
    (void)c;
    if (!sim_in_session) sim_violations++;
    if (a % 4096 || a >= sizeof(sim_mem)) { sim_violations++; return -1; }
    memset(sim_mem + a, 0xFF, 4096);
    sim_erases[a / 4096]++;
    return 0;
}
int extflash_begin(void)
{
    if (sim_busy) return -EBUSY;
    if (sim_in_session) sim_violations++;
    sim_in_session = 1;
    sim_sessions++;
    return 0;
}
void extflash_end(void)
{
    if (!sim_in_session) sim_violations++;
    sim_in_session = 0;
}
int extflash_check(void)
{
    if (!sim_in_session) sim_violations++;
    return sim_id_ok ? 0 : -ENODEV;
}
int extflash_probe(uint32_t *sectors)
{
    if (!sim_in_session) sim_violations++;
    if (!sim_present) return -ENODEV;
    *sectors = SIM_SECTORS;
    return 0;
}
const struct flashlog_dev *extflash_dev(uint32_t sectors)
{
    sim_dev.read = sim_read; sim_dev.prog = sim_prog; sim_dev.erase = sim_erase;
    sim_dev.sectors = sectors;
    return &sim_dev;
}
void h_sim_reset(void)
{
    memset(sim_mem, 0xFF, sizeof(sim_mem));
    memset(sim_erases, 0, sizeof(sim_erases));
    sim_violations = sim_in_session = sim_sessions = sim_busy = sim_progs = sim_unreadable = 0;
    sim_present = 1; sim_fail_prog_after = -1; sim_id_ok = 1;
}
void h_sim_set_id_ok(int ok) { sim_id_ok = ok; }
void h_sim_set(int present, int busy, int fail_prog_after, int unreadable)
{
    sim_present = present; sim_busy = busy; sim_fail_prog_after = fail_prog_after;
    sim_unreadable = unreadable;
}
int h_sim_violations(void) { return sim_violations + (sim_in_session ? 1000 : 0); }
int h_sim_sessions(void) { return sim_sessions; }
uint32_t h_sim_erases(int i) { return sim_erases[i]; }
void h_sim_corrupt(uint32_t addr) { sim_mem[addr] ^= 0x55; }
int h_sim_progs(void) { return sim_progs; }
#endif

/* -- the entry points the tests use ---------------------------------------- */
int h_push(const char *s) { return databuf_push_lines(s, strlen(s)); }
int h_count(void) { return databuf_count(); }
int h_ring_count(void) { return databuf_ring_count(); }
int h_flush(int n) { return databuf_flush(n); }
uint32_t h_dropped(void) { return databuf_dropped(); }
void h_stream(int on) { databuf_stream(on); }
void h_ack_all(void) { for (uint32_t i = 1; i <= g_sent_id; i++) databuf_ack(i); }
int h_unacked(void) { return databuf_unacked(); }
int h_expire(void) { return databuf_expire(); }
void h_set_answer(int v) { g_answer = v; }
void h_set_send_cost(int ms) { g_send_cost_ms = ms; }
static bool h_stop(void) { return ++g_stop_calls > g_stop_after; }
/* stop_after < 0: no stop test; else it says stop on the call after that many */
void h_deliver(int timeout_ms, int budget_ms, int stop_after)
{
    g_stop_calls = 0;
    g_stop_after = stop_after;
    databuf_deliver(timeout_ms, budget_ms, stop_after < 0 ? NULL : h_stop);
}
int h_stop_calls(void) { return g_stop_calls; }
int64_t h_ms(void) { return g_ms; }
"""

CFLAGS = [
    "-std=gnu11", "-Wall", "-Wextra", "-Wno-unused-parameter", "-shared", "-fPIC", "-O1",
    "-DCONFIG_APP_DATABUF_SLOTS=32", "-DCONFIG_APP_DATABUF_REC_MAX=768",
    "-DCONFIG_APP_DATABUF_FLUSH_PER_CYCLE=2", "-DCONFIG_APP_DATABUF_UNACKED_SLOTS=6",
    "-DCONFIG_APP_ACK_TIMEOUT_S=30", "-DCONFIG_APP_LOG_LEVEL=3", "-DCONFIG_APP_DEBUG_LOG=1",
    "-DCONFIG_APP_DRIVE_STREAM=1", "-DCONFIG_APP_DATABUF_STREAM_GAP_S=3",
    "-DCONFIG_APP_DRIVE_STREAM_WINDOW_MS=0",
    "-DCONFIG_APP_FLASH_BACKLOG=1", "-DCONFIG_APP_FLASH_BACKLOG_SECTORS=32",
    "-DCONFIG_APP_FLASH_BACKLOG_GAP_S=1", "-DCONFIG_APP_FLASH_BACKLOG_MAX_CYCLES=100000",
    "-DCONFIG_APP_FLASH_BACKLOG_MAX_OPENS_PER_HOUR=2400",
]


def build(extra_flags, tag, flash=True):
    d = tempfile.mkdtemp()
    os.makedirs(os.path.join(d, "zephyr", "logging"))
    with open(os.path.join(d, "zephyr", "kernel.h"), "w") as f:
        f.write(KERNEL_H)
    with open(os.path.join(d, "zephyr", "logging", "log.h"), "w") as f:
        f.write(LOG_H)
    with open(os.path.join(d, "h.c"), "w") as f:
        f.write(HARNESS)
    so = os.path.join(d, f"libdb_{tag}.so")
    flags = CFLAGS if flash else [f for f in CFLAGS if "FLASH_BACKLOG" not in f]
    srcs = [os.path.join(SRC, "databuf.c")] + ([os.path.join(SRC, "flashlog.c")] if flash else [])
    subprocess.run(["cc", *flags, *extra_flags, "-o", so, "-I", d, "-I", SRC,
                    os.path.join(d, "h.c"), *srcs], check=True)
    return so


_copies = [0]


def load(so):
    # A fresh copy of the library for each test: databuf.c, the transport log and
    # the flash all keep their state in statics, and one load would share them.
    _copies[0] += 1
    fresh = os.path.join(os.path.dirname(so), f"copy{_copies[0]}_{os.path.basename(so)}")
    shutil.copyfile(so, fresh)
    L = C.CDLL(fresh)
    L.h_log_line.restype = C.c_char_p
    L.h_dg.restype = C.c_char_p
    L.h_dropped.restype = C.c_uint32
    L.h_set_ms.argtypes = [C.c_int64]
    L.h_ms.restype = C.c_int64
    L.h_set_answer.argtypes = [C.c_int]
    L.h_set_send_cost.argtypes = [C.c_int]
    L.h_deliver.argtypes = [C.c_int] * 3
    L.h_push.argtypes = [C.c_char_p]
    L.h_sim_erases.restype = C.c_uint32
    L.h_log_level.argtypes = [C.c_int]
    L.h_log_line.argtypes = [C.c_int]
    L.h_alert_msg.restype = C.c_char_p
    L.h_alert_msg.argtypes = [C.c_int]
    L.h_alert_prio.argtypes = [C.c_int]
    L.h_sim_set_id_ok.argtypes = [C.c_int]
    L.h_dg.argtypes = [C.c_int]
    L.h_dbg_set_unanswered.argtypes = [C.c_int]
    L.h_dbg_answered.restype = C.c_uint32
    L.h_dbg_answered.argtypes = [C.c_int]
    L.h_sim_set.argtypes = [C.c_int] * 4
    L.h_sim_erases.argtypes = [C.c_int]
    L.h_sim_corrupt.argtypes = [C.c_uint32]
    return L


@pytest.fixture(scope="module")
def so_flash():
    return build([], "flash")


@pytest.fixture(scope="module")
def so_noflash():
    # The same file with the feature off: no flash code is compiled or linked.
    return build([], "off", flash=False)


class DB:
    def __init__(self, so):
        self.L = load(so)
        self.L.h_sim_reset()
        self.n = 0                       # records made so far
        self.sent_idx = []

    # -- feeding it --------------------------------------------------------------
    def rec(self, size=300, up=None):
        i = self.n
        self.n += 1
        up = i + 1 if up is None else up        # up=0 is "unknown" to the age stamp
        head = b"R%06d," % i
        tail = b",up=%d" % up
        return head + b"x" * (size - len(head) - len(tail)) + tail

    def push(self, k=1, size=300, up=None):
        stored = 0
        for _ in range(k):
            stored += self.L.h_push(self.rec(size, up))
        return stored

    # -- draining it ---------------------------------------------------------------
    def drain(self, limit=100000):
        """Flush until nothing more comes out; every datagram is answered."""
        for _ in range(limit):
            got = self.L.h_flush(2)
            self.L.h_ack_all()
            if got == 0:
                break
        return self.delivered()

    def delivered(self):
        out = []
        for i in range(self.L.h_ndg()):
            for line in self.L.h_dg(i).split(b"\n"):
                m = re.match(rb"R(\d{6}),", line)
                assert m, line
                out.append(int(m.group(1)))
        return out

    def count(self):
        return self.L.h_count()

    def logs(self, level=None):
        out = []
        for i in range(self.L.h_log_n()):
            if level is None or self.L.h_log_level(i) == level:
                out.append(self.L.h_log_line(i).decode())
        return out

    def alerts(self):
        return [(self.L.h_alert_msg(i).decode(), self.L.h_alert_prio(i))
                for i in range(self.L.h_alert_n())]

    def check_flash_discipline(self):
        assert self.L.h_sim_violations() == 0


@pytest.fixture
def db(so_flash):
    d = DB(so_flash)
    d.L.h_set_registered(0)           # an outage: nothing goes out
    yield d
    d.check_flash_discipline()


# --------------------------------------------------------------------------- the tier works

def test_a_long_outage_is_kept_whole_in_the_flash_and_comes_back_in_order(db):
    db.push(300)                                    # the ring holds 32; the rest goes to the flash
    assert db.count() == 300
    assert db.L.h_dropped() == 0                    # nothing thinned away
    db.L.h_set_registered(1)
    assert db.drain() == list(range(300))
    assert db.count() == 0


def test_without_a_flash_the_same_outage_is_thinned_as_before(so_noflash):
    d = DB(so_noflash)
    d.L.h_set_registered(0)
    d.push(300)
    held = d.count()
    assert held <= 32                               # the ring halves itself; nothing else holds records
    assert d.L.h_dropped() == 300 - held
    d.L.h_set_registered(1)
    got = d.drain()
    assert len(got) == held and got == sorted(got)
    assert d.L.h_sim_sessions() == 0


def test_a_short_outage_never_touches_the_flash(db):
    db.push(30)                                     # fits the ring
    assert db.L.h_sim_sessions() == 0               # not so much as a wake-up
    db.L.h_set_registered(1)
    assert db.drain() == list(range(30))
    assert db.L.h_sim_sessions() == 0


def test_the_flash_is_opened_the_first_time_it_is_needed_and_says_so_once(db):
    db.push(200)
    info = [l for l in db.logs(3) if l.startswith("flash backlog")]
    assert len(info) == 1 and "32 sectors" in info[0]
    assert db.logs(2) == []                         # working as designed, quietly


def test_the_ring_fills_the_flash_in_batches_not_a_session_per_record(db):
    db.push(400)
    assert db.L.h_sim_sessions() < 400 // 4         # a batch of 8 at a time, plus the open


def test_records_are_whole_and_stamped_with_their_age_on_the_way_out(db):
    db.push(100)
    db.L.h_set_ms(500_000)                          # long after they were built
    db.L.h_set_registered(1)
    db.drain()
    ages = 0
    for i in range(db.L.h_ndg()):
        for line in db.L.h_dg(i).split(b"\n"):
            assert line.endswith(b",age=%d" % (500 - int(re.search(rb",up=(\d+)", line).group(1))))
            ages += 1
    assert ages == 100


def test_the_flash_is_read_before_the_ring_so_the_order_holds_across_a_drain_in_steps(db):
    db.push(120)
    db.L.h_set_registered(1)
    first = db.L.h_flush(2)
    assert first > 0
    db.L.h_ack_all()
    db.push(5)                                      # more arrives mid-drain
    assert db.drain() == list(range(125))


# --------------------------------------------------------------------------- the stream

def test_a_stream_keeps_a_record_a_second_when_there_is_a_flash_to_hold_them(db):
    db.L.h_stream(1)
    for sec in range(200):
        for _ in range(4):                          # four records in each second
            db.push(1, up=sec)
    assert db.count() == 200                        # one a second: the floor for a track
    db.L.h_set_registered(1)
    assert db.drain() == [4 * s for s in range(200)]


def test_a_stream_without_a_usable_flash_falls_back_to_the_rings_own_spacing(so_flash):
    d = DB(so_flash)
    d.L.h_sim_set(0, 0, -1, 0)                      # no such chip
    d.L.h_set_registered(0)
    d.L.h_stream(1)
    for sec in range(300):
        d.push(1, up=sec)
    assert any("unavailable" in l for l in d.logs(2))
    # the ring alone: it thinned itself, never held more than its slots
    assert d.count() <= 32
    assert d.L.h_dropped() > 150
    d.check_flash_discipline()


# --------------------------------------------------------------------------- when the flash is not there

def test_a_chip_that_is_not_the_one_expected_leaves_the_backlog_as_it_was(so_flash):
    d = DB(so_flash)
    d.L.h_sim_set(0, 0, -1, 0)
    d.L.h_set_registered(0)
    d.push(300)
    warn = d.logs(2)
    assert sum("unavailable" in l for l in warn) == 1       # said once
    assert d.L.h_dropped() > 200                            # thinned like the ring alone
    assert d.count() < 64
    d.L.h_set_registered(1)
    got = d.drain()
    assert got == sorted(got)
    d.check_flash_discipline()


def test_a_bus_that_is_not_free_is_retried_not_given_up_on(so_flash):
    d = DB(so_flash)
    d.L.h_sim_set(1, 1, -1, 0)                              # CAN rail down: begin() says busy
    d.L.h_set_registered(0)
    d.push(60)
    assert sum("cannot be reached" in l for l in d.logs(2)) == 1     # said once
    assert d.L.h_sim_sessions() == 0
    d.L.h_sim_set(1, 0, -1, 0)                              # rail up
    d.push(200)
    assert any(l.startswith("flash backlog:") and "sectors" in l for l in d.logs(3))
    assert d.L.h_sim_sessions() > 0
    d.check_flash_discipline()


def test_a_flash_that_fails_to_write_falls_back_to_thinning_and_keeps_what_it_has(so_flash):
    d = DB(so_flash)
    d.L.h_set_registered(0)
    d.push(120)                                             # some records are in the flash
    held = d.count()
    assert held == 120
    d.L.h_sim_set(1, 0, 0, 0)                               # nothing programs any more
    d.push(300)                                             # ...and the ring has to thin
    assert d.L.h_dropped() > 0
    assert any("write" in l or "stopped" in l for l in d.logs(2))
    # Having given up, it leaves the chip alone: no more sessions, programs or erases,
    # and a failing chip has cost a handful of erases, not one for every record.
    quiet = (d.L.h_sim_sessions(), d.L.h_sim_progs(), sum(d.L.h_sim_erases(i) for i in range(32)))
    d.push(500)
    assert (d.L.h_sim_sessions(), d.L.h_sim_progs(),
            sum(d.L.h_sim_erases(i) for i in range(32))) == quiet
    assert quiet[2] <= 4
    assert len(d.logs(2)) < 10                              # and not a warning for every record
    d.L.h_sim_set(1, 0, -1, 0)                              # still readable
    d.L.h_set_registered(1)
    got = d.drain()
    assert got[:len(range(120 - 32))] == list(range(120 - 32))   # the flash's records, in order
    assert got == sorted(got)
    d.check_flash_discipline()


def test_a_flash_that_cannot_be_read_just_now_does_not_hold_up_the_ring(so_flash):
    d = DB(so_flash)
    d.L.h_set_registered(0)
    d.push(120)
    d.L.h_sim_set(1, 1, -1, 0)                              # busy: the flash cannot be reached
    d.L.h_set_registered(1)
    from_ring = d.drain()
    assert 0 < len(from_ring) <= 32
    assert from_ring == list(range(120 - len(from_ring), 120))   # the newest, in order
    assert d.count() == 120 - len(from_ring)                # the flash's are still owed
    d.L.h_sim_set(1, 0, -1, 0)                              # reachable again
    everything = d.drain()
    assert everything[len(from_ring):] == list(range(120 - len(from_ring)))
    assert sorted(everything) == list(range(120))           # each exactly once
    assert d.count() == 0
    d.check_flash_discipline()


def test_a_record_that_cannot_be_read_back_is_skipped_counted_and_the_rest_sent(so_flash):
    d = DB(so_flash)
    d.L.h_set_registered(0)
    d.push(200)
    d.L.h_sim_corrupt(0 * 4096 + 16 + 5 + 50)               # a payload byte of the first record
    d.L.h_set_registered(1)
    got = d.drain()
    assert d.count() == 0
    assert 0 < 200 - len(got) < 20                           # the rest of that sector's records
    assert got == sorted(got)
    assert any("could not be read back" in l for l in d.logs(2))
    d.check_flash_discipline()


# --------------------------------------------------------------------------- bounds

def test_a_flash_that_fills_overwrites_the_oldest_sectors_and_says_so(so_flash):
    d = DB(so_flash)
    d.L.h_set_registered(0)
    d.push(1500)                                            # 32 sectors hold well under that
    assert any("oldest records overwritten" in l for l in d.logs(2))
    held = d.count()
    assert held < 1500
    assert d.L.h_dropped() == 1500 - held                   # every record is in the books
    d.L.h_set_registered(1)
    got = d.drain()
    assert got == list(range(1500 - held, 1500))             # the newest, whole and in order
    d.check_flash_discipline()


def test_the_wear_stays_level_through_many_outages(so_flash):
    d = DB(so_flash)
    for outage in range(40):
        d.L.h_set_registered(0)
        d.push(150)
        d.L.h_set_registered(1)
        d.drain()
    erases = [d.L.h_sim_erases(i) for i in range(32)]
    assert max(erases) - min(erases) <= 1, erases
    assert sum(erases) > 0
    d.check_flash_discipline()


def test_unanswered_records_that_come_back_are_not_lost_to_the_flash_path(so_flash):
    d = DB(so_flash)
    d.L.h_set_registered(0)
    d.push(100)
    d.L.h_set_registered(1)
    d.L.h_flush(2)                                          # sent, never answered
    d.L.h_set_ms(40_000)                                    # past the 30 s answer window
    assert d.L.h_expire() > 0                               # they go back into the backlog
    got = d.drain()
    assert set(range(100)) <= set(got)                      # all there (some twice is allowed)
    d.check_flash_discipline()


# --------------------------------------------------------------------------- the ring's share

def test_the_ring_count_is_the_rams_share_alone_so_a_backlog_cannot_hold_the_breaker_off(db):
    db.push(300)
    assert db.count() == 300                          # everything waiting
    assert db.L.h_ring_count() <= 32                  # of it, what a cut would park into
    db.L.h_set_registered(1)
    db.drain()
    assert db.count() == 0 and db.L.h_ring_count() == 0


def test_without_a_flash_the_two_counts_are_the_same(so_noflash):
    d = DB(so_noflash)
    d.L.h_set_registered(0)
    d.push(300)
    assert d.count() == d.L.h_ring_count()


def test_a_stream_holds_no_more_than_the_ring_while_the_flash_cannot_be_reached(so_flash):
    d = DB(so_flash)
    d.L.h_sim_set(1, 1, -1, 0)                          # the bus is not free
    d.L.h_set_registered(0)
    d.L.h_stream(1)
    for sec in range(1, 301):
        d.push(1, up=sec)
    assert d.count() <= 32                              # nothing but the ring held them
    assert d.L.h_dropped() > 150
    d.check_flash_discipline()


# --------------------------------------------------------------------------- the drain

@pytest.fixture
def link(so_flash):
    """A link that answers every datagram when polled, and a backlog built in an outage."""
    d = DB(so_flash)
    d.L.h_set_answer(1)
    d.L.h_set_ms(1_000)
    yield d
    d.check_flash_discipline()


def outage(d, n):
    d.L.h_set_registered(0)
    d.push(n)
    d.L.h_set_registered(1)


def test_a_drain_starts_no_round_after_its_budget_and_leaves_the_rest(link):
    outage(link, 300)
    link.L.h_set_send_cost(500)                       # two datagrams: a second a round
    t0 = link.L.h_ms()
    link.L.h_deliver(4000, 10_000, -1)
    spent = link.L.h_ms() - t0
    assert 9_000 <= spent <= 11_000                   # the budget, plus a round that had begun
    assert 0 < link.count() < 300                     # some went, the rest waits
    assert link.L.h_unacked() == 0


def test_the_rest_goes_on_the_next_drain_in_order_and_whole(link):
    outage(link, 300)                                 # fits the ring and the flash
    link.L.h_set_send_cost(500)
    drains = 0
    while link.count() > 0 and drains < 100:
        link.L.h_deliver(4000, 10_000, -1)
        drains += 1
    assert 1 < drains < 100                           # it took several, each capped
    assert link.delivered() == list(range(300))


def test_a_drain_with_nothing_waiting_returns_at_once(link):
    t0 = link.L.h_ms()
    link.L.h_deliver(4000, 60_000, -1)
    assert link.L.h_ms() == t0 and link.L.h_ndg() == 0


def test_a_drain_with_no_registration_sends_nothing_and_does_not_wait(link):
    link.L.h_set_registered(0)
    link.push(50)
    t0 = link.L.h_ms()
    link.L.h_deliver(4000, 60_000, -1)
    assert link.L.h_ms() == t0 and link.L.h_ndg() == 0 and link.count() == 50


def test_a_round_that_goes_unanswered_ends_the_drain_and_its_records_stay_queued(link):
    outage(link, 100)
    link.L.h_set_answer(0)                            # the server says nothing
    t0 = link.L.h_ms()
    link.L.h_deliver(4000, 60_000, -1)
    assert 4_000 <= link.L.h_ms() - t0 <= 4_500       # one wait, not a minute of them
    assert link.L.h_ndg() == 2                        # one round
    assert link.count() == 100                        # nothing lost, nothing held as sent


def test_a_budget_of_nothing_collects_the_replies_owed_and_sends_no_more(link):
    outage(link, 100)
    link.L.h_flush(2)                                 # out, not yet answered
    sent = link.L.h_ndg()
    link.L.h_deliver(4000, 0, -1)
    assert link.L.h_unacked() == 0 and link.L.h_ndg() == sent


def test_a_stop_test_that_fires_ends_the_drain_before_the_next_round(link):
    outage(link, 300)
    kicks = link.L.h_kicks()
    link.L.h_deliver(4000, 60_000, 3)                 # three rounds, then "stop"
    assert link.L.h_ndg() == 6                        # two datagrams a round
    assert link.L.h_stop_calls() == 4
    assert link.L.h_kicks() - kicks >= 3              # the watchdog fed at least once a round
    assert 0 < link.count() < 300


def test_a_stop_test_that_is_true_at_the_start_still_collects_the_replies_owed(link):
    outage(link, 100)
    link.L.h_flush(2)
    sent = link.L.h_ndg()
    link.L.h_deliver(4000, 60_000, 0)
    assert link.L.h_unacked() == 0 and link.L.h_ndg() == sent


# --------------------------------------------------------------------------- telling the operator, once

def only_alert(d):
    (msg, prio), = d.alerts()
    assert prio == 0
    assert msg.startswith("flash backlog:") and "RAM backlog only" in msg
    assert len(msg) < 120
    return msg


def test_a_chip_that_works_spills_a_long_outage_without_an_alert_or_a_warning(db):
    db.push(300)
    assert db.L.h_sim_sessions() > 0
    assert db.alerts() == [] and db.logs(2) == []


def test_a_chip_that_is_not_the_one_expected_raises_one_alert_and_not_a_second_on_a_later_outage(so_flash):
    d = DB(so_flash)
    d.L.h_sim_set(0, 0, -1, 0)
    d.L.h_set_registered(0)
    d.push(300)
    assert sum("unavailable" in l for l in d.logs(2)) == 1
    assert "chip not usable (" in only_alert(d)
    d.L.h_set_registered(1)
    d.drain()
    d.L.h_set_registered(0)
    d.push(300)
    assert len(d.alerts()) == 1
    assert sum("unavailable" in l for l in d.logs(2)) == 1
    d.check_flash_discipline()


def test_a_chip_that_goes_away_mid_outage_falls_back_to_thinning_with_one_warning_and_one_alert_and_comes_back_whole(so_flash):
    d = DB(so_flash)
    d.L.h_set_registered(0)
    d.push(100)                                         # the first spill worked: records are in the flash
    assert d.alerts() == [] and d.logs(2) == []
    in_flash = d.count() - d.L.h_ring_count()
    assert in_flash > 0
    d.L.h_sim_set_id_ok(0)                              # RDID now answers wrongly
    dropped0 = d.L.h_dropped()
    d.push(300)                                         # the ring thins itself
    assert d.L.h_dropped() > dropped0
    assert d.count() - d.L.h_ring_count() == in_flash   # the flash was not touched
    assert sum("does not answer" in l for l in d.logs(2)) == 1
    assert sum(l.startswith("flash backlog") for l in d.logs(2)) == 1   # the rest is the ring thinning
    assert "chip stopped answering" in only_alert(d)
    d.L.h_sim_set_id_ok(1)                              # back
    sessions = d.L.h_sim_sessions()
    d.push(200)
    assert d.L.h_sim_sessions() > sessions              # spills work again
    assert len(d.alerts()) == 1
    assert sum(l.startswith("flash backlog") for l in d.logs(2)) == 1
    d.L.h_set_registered(1)
    got = d.drain()
    assert got == sorted(got) and len(set(got)) == len(got)
    assert got[:in_flash] == list(range(in_flash))      # what was in the chip comes out first, in order
    d.check_flash_discipline()


def test_a_chip_that_stops_programming_raises_one_alert(so_flash):
    d = DB(so_flash)
    d.L.h_set_registered(0)
    d.push(120)
    d.L.h_sim_set(1, 0, 0, 0)                           # no program succeeds from here
    d.push(400)
    assert any("stopped" in l for l in d.logs(2))
    assert "writes stopped" in only_alert(d)
    d.push(400)
    assert len(d.alerts()) == 1
    d.check_flash_discipline()


def test_a_bus_that_is_not_free_raises_no_alert(so_flash):
    d = DB(so_flash)
    d.L.h_sim_set(1, 1, -1, 0)
    d.L.h_set_registered(0)
    d.push(300)
    assert sum("cannot be reached" in l for l in d.logs(2)) == 1
    assert d.alerts() == []
    d.check_flash_discipline()


def test_a_full_flash_overwriting_its_oldest_raises_no_alert(so_flash):
    d = DB(so_flash)
    d.L.h_set_registered(0)
    d.push(1500)
    assert any("oldest records overwritten" in l for l in d.logs(2))
    assert d.alerts() == []
    d.check_flash_discipline()


def test_records_that_cannot_be_read_back_raise_no_alert(so_flash):
    d = DB(so_flash)
    d.L.h_set_registered(0)
    d.push(200)
    d.L.h_sim_corrupt(16 + 5 + 50)
    d.L.h_set_registered(1)
    d.drain()
    assert any("could not be read back" in l for l in d.logs(2))
    assert d.alerts() == []
    d.check_flash_discipline()


# --------------------------------------------------------------------------- held log lines

def test_every_reply_reaches_the_log_lines_held_under_that_datagram_id(link):
    link.L.databuf_ack(7)
    link.L.databuf_ack(9)
    assert [link.L.h_dbg_answered(i) for i in range(link.L.h_dbg_answered_n())] == [7, 9]


def test_closing_the_socket_waits_for_a_held_log_batch_and_then_drops_the_hold(link):
    link.L.h_dbg_set_unanswered(1)
    t0 = link.L.h_ms()
    link.L.databuf_settle(1000)                       # no reply comes in
    assert link.L.h_ms() - t0 >= 1000                 # it waited the whole time for it
    assert link.L.h_dbg_settles() == 1


def test_closing_the_socket_with_no_log_batch_held_does_not_wait(link):
    t0 = link.L.h_ms()
    link.L.databuf_settle(1000)
    assert link.L.h_ms() == t0
    assert link.L.h_dbg_settles() == 1
