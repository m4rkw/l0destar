"""Host tests for src/flashlog.c: the wear-levelled queue the flash backlog is built on.

The module is built as a shared library with the host compiler and run against a
simulated NOR flash that is stricter than the part it stands for.  It counts every
erase of every sector, and it records a violation for any program of a byte that
is not erased (the chip would allow clearing more bits; the log never does), any
program across a page, any erase not at a sector start, and any access out of
range.  Every test ends by checking there were none.

The wear tests are the point of the module: the flash is rated for 100,000 erases
per sector, and a log that put its bookkeeping in one place, or began again at
sector 0 after each reset, would be through that in days.

    uv run --python 3.12 --with pytest pytest -q firmware/tests/test_flashlog.py
"""
import binascii
import ctypes as C
import os
import re
import struct
import subprocess
import tempfile

import pytest

SRC = os.environ.get("FL_SRC", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src"))

SECTOR = 4096
PAGE = 256
HDR = 16
REC_HDR = 5

ERR_ARG, ERR_IO, ERR_RATE, ERR_WORN, ERR_DEAD, ERR_STALE, ERR_CRC = -1, -2, -3, -4, -5, -6, -7

WRAPPER = r"""
#include <stdlib.h>
#include <string.h>
#include "flashlog.h"

typedef int (*rd_t)(void *, uint32_t, void *, size_t);
typedef int (*pg_t)(void *, uint32_t, const void *, size_t);
typedef int (*er_t)(void *, uint32_t);

struct h {
    struct flashlog fl;
    struct flashlog_dev dev;
    struct flashlog_cur cur;
    uint16_t *cnt;
};

struct h *h_new(uint32_t sectors, rd_t r, pg_t p, er_t e)
{
    struct h *h = calloc(1, sizeof(*h));
    h->cnt = calloc(sectors, sizeof(uint16_t));
    h->dev.read = r; h->dev.prog = p; h->dev.erase = e; h->dev.sectors = sectors;
    return h;
}
void h_free(struct h *h) { free(h->cnt); free(h); }
int h_init(struct h *h, uint32_t max_cycles, uint32_t opens)
{
    struct flashlog_cfg cfg = { max_cycles, opens };
    return flashlog_init(&h->fl, &h->dev, h->cnt, &cfg);
}
int h_append(struct h *h, const void *rec, uint16_t len, uint32_t now)
{ return flashlog_append(&h->fl, rec, len, now); }
uint32_t h_pending(struct h *h) { return flashlog_pending(&h->fl); }
void h_cursor(struct h *h) { flashlog_cursor(&h->fl, &h->cur); }
int h_next(struct h *h, void *buf, uint16_t cap) { return flashlog_next(&h->fl, &h->cur, buf, cap); }
int h_commit(struct h *h) { return flashlog_commit(&h->fl, &h->cur); }
uint32_t h_cur_n(struct h *h) { return h->cur.n; }
uint32_t h_cur_lost(struct h *h) { return h->cur.lost; }
uint32_t h_cycles(struct h *h) { return flashlog_cycles(&h->fl); }
uint32_t h_next_seq(struct h *h) { return h->fl.next_seq; }
uint32_t h_head_idx(struct h *h) { return h->fl.head_idx; }
int h_dead(struct h *h) { return h->fl.dead; }
void h_stats(struct h *h, uint32_t *o)
{
    const struct flashlog_stats *s = &h->fl.st;
    o[0] = s->opens; o[1] = s->erases; o[2] = s->blank_skips; o[3] = s->skipped;
    o[4] = s->appended; o[5] = s->consumed; o[6] = s->dropped; o[7] = s->lost;
    o[8] = s->io_errors; o[9] = s->refused_rate; o[10] = s->refused_worn;
}
"""

STAT_NAMES = ["opens", "erases", "blank_skips", "skipped", "appended", "consumed",
              "dropped", "lost", "io_errors", "refused_rate", "refused_worn"]

RD = C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_uint32, C.c_void_p, C.c_size_t)
PG = C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_uint32, C.c_void_p, C.c_size_t)
ER = C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_uint32)


@pytest.fixture(scope="module")
def lib():
    d = tempfile.mkdtemp()
    wrapper = os.path.join(d, "w.c")
    so = os.path.join(d, "libfl.so")
    with open(wrapper, "w") as f:
        f.write(WRAPPER)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC", "-o", so,
                    wrapper, os.path.join(SRC, "flashlog.c"), "-I", SRC], check=True)
    L = C.CDLL(so)
    L.h_new.restype = C.c_void_p
    L.h_new.argtypes = [C.c_uint32, RD, PG, ER]
    L.h_free.argtypes = [C.c_void_p]
    L.h_init.argtypes = [C.c_void_p, C.c_uint32, C.c_uint32]
    L.h_append.argtypes = [C.c_void_p, C.c_char_p, C.c_uint16, C.c_uint32]
    L.h_pending.argtypes = [C.c_void_p]
    L.h_pending.restype = C.c_uint32
    L.h_cursor.argtypes = [C.c_void_p]
    L.h_next.argtypes = [C.c_void_p, C.c_void_p, C.c_uint16]
    L.h_commit.argtypes = [C.c_void_p]
    for name in ("h_cur_n", "h_cur_lost", "h_cycles", "h_next_seq", "h_head_idx"):
        getattr(L, name).argtypes = [C.c_void_p]
        getattr(L, name).restype = C.c_uint32
    L.h_dead.argtypes = [C.c_void_p]
    L.h_stats.argtypes = [C.c_void_p, C.POINTER(C.c_uint32)]
    return L


class Sim:
    """A NOR flash: erased bytes are 0xFF, programming can only clear bits, and
    everything the log must never do is recorded in .violations."""

    def __init__(self, sectors):
        self.n = sectors
        self.mem = bytearray(b"\xff" * (sectors * SECTOR))
        self.erases = [0] * sectors            # erases completed per sector
        self.erase_tries = [0] * sectors       # erase commands per sector, failed or not
        self.violations = []
        self.ops = 0                           # programs and erases issued
        self.fail_at = None                    # the op number power is lost at
        self.dead = False                      # power is off: every access fails
        self.fail_erase = set()                # sectors whose erase command fails
        self.fail_prog_after = None            # programs fail once this many have gone in
        self.progs = 0
        self.silent_sectors = set()            # programs here are accepted and do nothing
        self.transient = None                  # [addr, times]: reads covering addr come back wrong
        self.cb = (RD(self.read), PG(self.prog), ER(self.erase))

    # -- the three operations the log is given --------------------------------
    def read(self, _ctx, addr, buf, n):
        if self.dead:
            return -5
        if addr + n > len(self.mem):
            self.violations.append(f"read out of range {addr}+{n}")
            return -1
        data = bytearray(self.mem[addr:addr + n])
        t = self.transient
        if t and t[1] > 0 and addr <= t[0] < addr + n:
            data[t[0] - addr] ^= 0x01
            t[1] -= 1
        C.memmove(buf, bytes(data), n)
        return 0

    def prog(self, _ctx, addr, buf, n):
        if self.dead:
            return -5
        self.ops += 1
        data = C.string_at(buf, n)
        if n == 0 or n > PAGE or (addr % PAGE) + n > PAGE:
            self.violations.append(f"program crosses a page: {addr}+{n}")
        if addr + n > len(self.mem):
            self.violations.append(f"program out of range {addr}+{n}")
            return -1
        stale = [a for a in range(addr, addr + n) if self.mem[a] != 0xFF]
        if stale:
            self.violations.append(f"program over {len(stale)} byte(s) not erased at {stale[0]}")
        if self.fail_at is not None and self.ops == self.fail_at:
            keep = n // 2                      # power goes mid-program
            self.mem[addr:addr + keep] = data[:keep]
            if keep < n:
                self.mem[addr + keep] &= data[keep] | 0x0F    # a torn byte
            self.dead = True
            return -5
        if self.fail_prog_after is not None and self.progs >= self.fail_prog_after:
            return -1
        self.progs += 1
        if addr // SECTOR in self.silent_sectors:
            return 0
        for i in range(n):
            self.mem[addr + i] &= data[i]
        return 0

    def erase(self, _ctx, addr):
        if self.dead:
            return -5
        self.ops += 1
        if addr % SECTOR or addr >= len(self.mem):
            self.violations.append(f"erase not at a sector start: {addr}")
            return -1
        s = addr // SECTOR
        self.erase_tries[s] += 1
        if self.fail_at is not None and self.ops == self.fail_at:
            half = SECTOR // 2                 # power goes mid-erase
            self.mem[addr:addr + half] = b"\xff" * half
            self.dead = True
            return -5
        if s in self.fail_erase:
            return -1
        self.mem[addr:addr + SECTOR] = b"\xff" * SECTOR
        self.erases[s] += 1
        return 0

    # -- the test's side -------------------------------------------------------
    def reboot(self):
        self.dead = False
        self.fail_at = None

    def spread(self):
        return max(self.erases) - min(self.erases)

    def write_header(self, idx, seq, good=True):
        h = b"L0FL" + struct.pack("<IHHH", seq, 1, SECTOR // 256, 0)
        crc = binascii.crc_hqx(h, 0xFFFF)
        h += struct.pack("<H", crc if good else crc ^ 0x1234)
        self.mem[idx * SECTOR:idx * SECTOR + HDR] = h


class Log:
    def __init__(self, lib, sim, max_cycles=0, opens=0):
        self.lib, self.sim = lib, sim
        self.max_cycles, self.opens = max_cycles, opens
        self.h = lib.h_new(sim.n, *sim.cb)
        self.buf = C.create_string_buffer(2048)

    def init(self):
        return self.lib.h_init(self.h, self.max_cycles, self.opens)

    def append(self, rec, now=0):
        return self.lib.h_append(self.h, rec, len(rec), now)

    def pending(self):
        return self.lib.h_pending(self.h)

    def read(self, cap=2048):
        """(status, bytes): status is the length, 0 at the end, or an error."""
        n = self.lib.h_next(self.h, self.buf, cap)
        return n, (self.buf.raw[:n] if n > 0 else b"")

    def read_all(self, commit=True, limit=1_000_000):
        out = []
        self.lib.h_cursor(self.h)
        for _ in range(limit):
            n, rec = self.read()
            if n == 0:
                break
            if n > 0:
                out.append(rec)
            else:
                assert n == ERR_CRC, n
        if commit:
            assert self.lib.h_commit(self.h) == 0
        return out

    def stats(self):
        a = (C.c_uint32 * 11)()
        self.lib.h_stats(self.h, a)
        return dict(zip(STAT_NAMES, a))

    def __getattr__(self, name):       # cycles, next_seq, head_idx, dead
        fn = getattr(self.lib, "h_" + name)
        return lambda: fn(self.h)


def rec(i, size=300):
    """Record i, `size` bytes, recognisable and checkable."""
    body = (b"%08d," % i)
    return (body + bytes((i + k) % 251 for k in range(size - len(body))))[:size]


@pytest.fixture
def mk(lib):
    made = []

    def make(sectors=16, max_cycles=0, opens=0, init=True):
        sim = Sim(sectors)
        log = Log(lib, sim, max_cycles, opens)
        if init:
            assert log.init() == 0
        made.append((sim, log))
        return sim, log

    yield make
    for sim, log in made:
        lib.h_free(log.h)
        assert sim.violations == []


# --------------------------------------------------------------------------- format

def test_the_sector_header_and_record_are_laid_out_as_documented(mk):
    sim, log = mk()
    assert log.append(b"abc") == 0
    hdr = bytes(sim.mem[0:HDR])
    assert hdr[:4] == b"L0FL"
    assert struct.unpack("<IHHH", hdr[4:14]) == (1, 1, SECTOR // 256, 0)
    assert struct.unpack("<H", hdr[14:16])[0] == binascii.crc_hqx(hdr[:14], 0xFFFF)
    r = bytes(sim.mem[HDR:HDR + REC_HDR + 3])
    assert r[0] == 0xA7 and struct.unpack("<H", r[1:3])[0] == 3
    assert struct.unpack("<H", r[3:5])[0] == binascii.crc_hqx(r[:3] + b"abc", 0xFFFF)
    assert r[5:] == b"abc"


# --------------------------------------------------------------------------- queue behaviour

def test_records_come_back_in_order_and_intact(mk):
    sim, log = mk()
    sent = [rec(i, 40 + (i * 53) % 700) for i in range(120)]
    for r in sent:
        assert log.append(r) == 0
    assert log.pending() == 120
    assert log.read_all() == sent
    assert log.pending() == 0


def test_reading_consumes_nothing_until_it_is_committed(mk):
    sim, log = mk()
    sent = [rec(i) for i in range(10)]
    for r in sent:
        log.append(r)
    assert log.read_all(commit=False) == sent
    assert log.pending() == 10
    assert log.read_all(commit=False) == sent       # a send that failed: same records again
    assert log.read_all() == sent
    assert log.pending() == 0


def test_a_partial_commit_leaves_the_rest(mk):
    sim, log = mk()
    sent = [rec(i) for i in range(30)]
    for r in sent:
        log.append(r)
    log.lib.h_cursor(log.h)
    got = [log.read()[1] for _ in range(11)]
    assert got == sent[:11]
    assert log.lib.h_commit(log.h) == 0
    assert log.pending() == 19
    assert log.read_all() == sent[11:]


def test_new_records_after_a_drain_are_read_next(mk):
    sim, log = mk()
    for i in range(7):
        log.append(rec(i))
    assert len(log.read_all()) == 7
    for i in range(7, 20):
        log.append(rec(i))
    assert log.read_all() == [rec(i) for i in range(7, 20)]


def test_a_record_never_crosses_a_sector(mk):
    sim, log = mk()
    # 1000-byte records: four fit (16 + 4 * 1005 = 4036), the fifth opens the next sector
    for i in range(9):
        assert log.append(rec(i, 1000)) == 0
    assert log.stats()["opens"] == 3
    for s in range(3):
        base = s * SECTOR
        off = HDR
        for _ in range(4 if s < 2 else 1):
            assert sim.mem[base + off] == 0xA7
            off += REC_HDR + struct.unpack("<H", bytes(sim.mem[base + off + 1:base + off + 3]))[0]
        assert off <= SECTOR
    assert log.read_all() == [rec(i, 1000) for i in range(9)]


def test_a_record_longer_than_the_buffer_is_lost_to_that_reader_not_left_at_the_head(mk):
    sim, log = mk()
    for i in range(12):
        log.append(rec(i, 1000 if i == 5 else 300))        # one long record among short ones
    log.lib.h_cursor(log.h)
    got = []
    while True:
        n, r = log.read(cap=500)
        if n == 0:
            break
        if n > 0:
            got.append(r)
        else:
            assert n == ERR_CRC
    assert log.lib.h_commit(log.h) == 0
    st = log.stats()
    assert st["lost"] >= 1 and st["consumed"] + st["lost"] == 12
    assert log.pending() == 0                               # nothing is stuck behind it


def test_bad_arguments(mk, lib):
    sim, log = mk()
    assert log.append(b"") == ERR_ARG
    assert log.append(b"x" * 1025) == ERR_ARG
    assert log.append(b"x" * 1024) == 0
    sim2 = Sim(4)                       # too few sectors to be a ring
    small = Log(lib, sim2)
    assert small.init() == ERR_ARG
    lib.h_free(small.h)


# --------------------------------------------------------------------------- wear

def test_a_new_chip_is_filled_a_whole_lap_without_one_erase(mk):
    sim, log = mk(sectors=16)
    for i in range(16 * 4):
        assert log.append(rec(i, 1000)) == 0
        if i % 5 == 4:
            log.read_all()
    assert log.stats()["opens"] == 16
    assert sum(sim.erases) == 0
    assert log.stats()["blank_skips"] == 16


def test_every_sector_is_erased_once_per_lap(mk):
    sim, log = mk(sectors=16)
    laps = 6
    for i in range(16 * 4 * laps):
        assert log.append(rec(i, 1000)) == 0
        if i % 8 == 7:
            assert len(log.read_all()) == 8
    assert log.stats()["opens"] == 16 * laps
    # the first lap found blank sectors; every lap after it erased each sector once
    assert sim.erases == [laps - 1] * 16
    assert log.stats()["erases"] == sum(sim.erases)
    assert log.cycles() == laps


def test_the_erase_counts_never_differ_by_more_than_one(mk):
    sim, log = mk(sectors=16)
    for i in range(16 * 4 * 5):
        log.append(rec(i, 1000))
        if i % 8 == 7:
            log.read_all()
        assert sim.spread() <= 1


def test_nothing_is_written_to_consume_a_record(mk):
    sim, log = mk()
    for i in range(40):
        log.append(rec(i))
    ops = sim.ops
    log.read_all(commit=False)
    log.read_all()
    assert sim.ops == ops             # reading and committing programmed and erased nothing


def test_a_record_costs_programs_and_the_header_not_erases(mk):
    sim, log = mk(sectors=64)
    # an empty chip, so the whole run is the first lap: bytes written are all there is
    for i in range(200):
        log.append(rec(i, 300))
    assert sum(sim.erases) == 0
    assert log.stats()["appended"] == 200


def test_the_ring_carries_on_where_it_was_after_a_reset(mk):
    sim, log = mk(sectors=32)
    for boot in range(1500):
        assert log.init() == 0
        for i in range(1 + boot % 7):
            assert log.append(rec(boot * 10 + i, 200 + (boot * 37 + i * 11) % 500)) == 0
        if boot % 3 == 0:
            log.read_all()
        assert sim.spread() <= 1
    # A log that began at sector 0 each boot would have put every erase on the first
    # few sectors; this one has spread them over all of them.
    assert min(sim.erases) > 0
    assert sim.spread() <= 1
    assert log.stats()["erases"] <= sum(sim.erases)


def test_the_next_sector_after_a_reset_is_the_one_after_the_newest(mk):
    sim, log = mk(sectors=16)
    for i in range(10):
        log.append(rec(i, 1000))               # three sectors: 0, 1, 2
    log.read_all()                             # all consumed: still no reason to start over
    assert log.init() == 0
    assert (log.head_idx(), log.next_seq()) == (2, 4)
    log.append(rec(99))
    assert bytes(sim.mem[3 * SECTOR:3 * SECTOR + 4]) == b"L0FL"
    assert struct.unpack("<I", bytes(sim.mem[3 * SECTOR + 4:3 * SECTOR + 8]))[0] == 4


def test_what_an_earlier_boot_left_is_not_read(mk):
    sim, log = mk()
    for i in range(25):
        log.append(rec(i))
    assert log.init() == 0
    assert log.pending() == 0
    assert log.read_all() == []
    log.append(rec(1000))
    assert log.read_all() == [rec(1000)]


def test_writes_are_only_ever_to_erased_bytes_over_many_laps(mk):
    # The simulator records a violation otherwise; this drives it hard.
    sim, log = mk(sectors=16)
    for i in range(2000):
        log.append(rec(i, 30 + (i * 97) % 900))
        if i % 11 == 0:
            log.read_all()
    assert sim.violations == []


# --------------------------------------------------------------------------- a full ring

def test_a_full_ring_gives_up_its_oldest_sector_whole_and_in_order(mk):
    sim, log = mk(sectors=16)
    # 70 records are 18 sectors' worth, on a ring of 16: the two oldest sectors go
    sent = [rec(i, 1000) for i in range(16 * 4 + 6)]
    for r in sent:
        assert log.append(r) == 0
    got = log.read_all()
    st = log.stats()
    assert st["dropped"] == 8                       # two sectors of four
    assert got == sent[8:]
    assert len(got) + st["dropped"] == len(sent)
    assert log.pending() == 0


def test_only_what_was_unread_is_counted_when_the_ring_overwrites(mk):
    sim, log = mk(sectors=16)
    sent = [rec(i, 1000) for i in range(16 * 4)]
    for r in sent:
        log.append(r)
    log.lib.h_cursor(log.h)
    for _ in range(2):
        log.read()
    assert log.lib.h_commit(log.h) == 0             # two of sector 1's four are gone already
    log.append(rec(100, 1000))
    assert log.stats()["dropped"] == 2
    assert log.read_all() == sent[4:] + [rec(100, 1000)]


def test_a_cursor_overtaken_by_the_ring_is_stale(mk):
    sim, log = mk(sectors=16)
    for i in range(16 * 4):
        log.append(rec(i, 1000))
    log.lib.h_cursor(log.h)
    assert log.read()[0] > 0
    log.append(rec(500, 1000))                       # reuses the oldest sector
    assert log.read()[0] == ERR_STALE
    assert log.lib.h_commit(log.h) == ERR_STALE


# --------------------------------------------------------------------------- bad reads

def test_a_corrupt_record_costs_the_rest_of_its_sector_and_no_more(mk):
    sim, log = mk()
    for i in range(12):
        log.append(rec(i, 1000))                     # three sectors of four
    # sector 1, second record: one flipped byte in its payload
    sim.mem[1 * SECTOR + HDR + (REC_HDR + 1000) + REC_HDR + 20] ^= 0x40
    got = log.read_all()
    assert got == [rec(i, 1000) for i in (0, 1, 2, 3, 4, 8, 9, 10, 11)]
    st = log.stats()
    assert st["lost"] == 3 and st["consumed"] == 9
    assert log.pending() == 0


def test_a_read_that_goes_wrong_once_is_tried_again(mk):
    sim, log = mk()
    for i in range(8):
        log.append(rec(i, 400))
    sim.transient = [HDR + 2 * (REC_HDR + 400) + REC_HDR + 100, 1]   # record 2's payload, once
    assert log.read_all() == [rec(i, 400) for i in range(8)]
    assert log.stats()["lost"] == 0


# --------------------------------------------------------------------------- power loss

def scenario(log, sim):
    """Appends and reads that cross sectors, so power can go at every kind of step."""
    seq = 0
    for burst in range(3):
        for _ in range(14):
            if log.append(rec(seq, 700)) != 0:
                return
            seq += 1
        log.read_all()


def clean_ops(lib):
    sim = Sim(8)
    log = Log(lib, sim)
    log.init()
    scenario(log, sim)
    n = sim.ops
    lib.h_free(log.h)
    return n


def test_power_lost_at_any_step_leaves_a_log_that_starts_again_cleanly(lib):
    total = clean_ops(lib)
    assert total > 40
    for fail_at in range(1, total + 1):
        sim = Sim(8)
        log = Log(lib, sim)
        log.init()
        sim.fail_at = fail_at
        scenario(log, sim)
        assert sim.dead, fail_at
        sim.reboot()
        assert log.init() == 0, fail_at
        assert log.pending() == 0
        # the next boot works, whatever the interrupted one left behind
        fresh = [rec(1000 + i, 500) for i in range(30)]
        for r in fresh:
            assert log.append(r) == 0, fail_at
        assert log.read_all() == fresh, fail_at
        assert sim.violations == [], (fail_at, sim.violations)
        assert sim.spread() <= 2, (fail_at, sim.erases)      # one interrupted erase at most
        lib.h_free(log.h)


def test_a_torn_header_is_erased_and_the_sector_used(mk):
    sim, log = mk(sectors=16, init=False)
    for idx in range(0, 3):
        sim.write_header(idx, idx + 1)
    sim.write_header(3, 4, good=False)               # power went while this one was written
    assert log.init() == 0
    assert log.head_idx() == 2 and log.next_seq() == 4
    assert log.append(rec(1)) == 0
    assert sim.erases[3] == 1                         # not blank, so erased, once
    assert log.read_all() == [rec(1)]
    assert struct.unpack("<I", bytes(sim.mem[3 * SECTOR + 4:3 * SECTOR + 8]))[0] == 4


# --------------------------------------------------------------------------- failing flash

def dirty(sim, *sectors):
    for s in sectors:
        sim.mem[s * SECTOR + 100] = 0x00              # not blank: the log will have to erase it


def test_a_sector_that_will_not_erase_is_skipped_and_not_retried_in_a_loop(mk):
    sim, log = mk(sectors=16)
    for i in range(16 * 4):
        log.append(rec(i, 1000))
    log.read_all()
    sim.fail_erase = {1}
    sent = [rec(200 + i, 1000) for i in range(12)]
    for r in sent:
        assert log.append(r) == 0
    st = log.stats()
    assert st["skipped"] == 1
    assert sim.erase_tries[1] == 1                    # tried the once, in that lap
    assert log.read_all() == sent


def test_a_header_that_does_not_read_back_is_not_trusted(mk):
    sim, log = mk(sectors=16)
    sim.silent_sectors = {1}          # sector 1 takes the program command and keeps nothing
    sent = [rec(i, 1000) for i in range(12)]          # three sectors' worth
    for r in sent:
        assert log.append(r) == 0
    assert log.stats()["skipped"] == 1                # the log found out, and went on to the next
    assert log.read_all() == sent


def test_three_failures_in_a_row_stop_the_writing_and_with_it_the_erasing(mk):
    sim, log = mk(sectors=16)
    for i in range(16 * 4):
        log.append(rec(i, 1000))
    log.read_all()
    sim.fail_erase = {0, 1, 2, 3, 4}
    assert log.append(rec(1, 1000)) == ERR_DEAD
    assert log.dead()
    tries = list(sim.erase_tries)
    for i in range(50):
        assert log.append(rec(i, 1000)) == ERR_DEAD
    assert sim.erase_tries == tries                   # no erase storm
    assert sum(tries) <= 3


def test_a_chip_that_will_not_program_costs_three_erases_not_one_per_record(mk):
    sim, log = mk(sectors=16)
    for i in range(16 * 4):
        log.append(rec(i, 1000))
    log.read_all()                                    # lap one over: every open now erases
    sim.fail_prog_after = sim.progs                   # nothing more goes in
    results = [log.append(rec(i, 1000)) for i in range(40)]
    assert results[:3] == [ERR_IO, ERR_IO, ERR_IO] or ERR_DEAD in results
    assert log.dead()
    assert sum(sim.erases) <= 4
    # and what was there is still readable
    sim.fail_prog_after = None
    assert log.pending() == 0


def test_a_dead_log_still_gives_back_what_it_holds(mk):
    sim, log = mk(sectors=16)
    sent = [rec(i, 1000) for i in range(6)]
    for r in sent:
        log.append(r)
    sim.fail_prog_after = sim.progs
    for i in range(10):
        log.append(rec(i, 1000))
    assert log.dead()
    assert log.read_all() == sent


# --------------------------------------------------------------------------- guards

def test_a_runaway_cannot_open_more_sectors_than_the_hourly_limit(mk):
    sim, log = mk(sectors=16, opens=10)
    ok = 0
    for i in range(10_000):
        r = log.append(rec(i, 1000), now=100)
        if r == 0:
            ok += 1
        else:
            assert r == ERR_RATE
        if i % 4 == 3:
            log.read_all()
    assert log.stats()["opens"] == 10
    assert ok == 40 and log.stats()["refused_rate"] > 0
    assert log.append(rec(1, 1000), now=100 + 3599) == ERR_RATE
    assert log.append(rec(1, 1000), now=100 + 3600) == 0       # an hour on, allowed again
    assert log.stats()["opens"] == 11


KCONFIG = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "Kconfig")


def _kconfig_block(symbol):
    text = open(KCONFIG).read()
    for block in text.split("\nconfig ")[1:]:
        if block.startswith(symbol + "\n"):
            return block.split("\n\n")[0] + "\n" + "\n\n".join(block.split("\n\n")[1:2])
    raise AssertionError(symbol)


def _kconfig_default(symbol):
    return int(re.search(r"\n\s+default (\d+)", _kconfig_block(symbol)).group(1))


def test_the_rate_limit_is_what_stands_between_a_bug_and_the_rating():
    # What the Kconfig help and flashlog.h say about the default: a runaway that opens
    # sectors as fast as it is allowed to takes nearly ten years to get through the rating.
    per_hour = _kconfig_default("APP_FLASH_BACKLOG_MAX_OPENS_PER_HOUR")
    sectors = _kconfig_default("APP_FLASH_BACKLOG_SECTORS")
    cycles = _kconfig_default("APP_FLASH_BACKLOG_MAX_CYCLES")
    years = sectors * cycles / (per_hour * 24 * 365)
    assert 9 < years < 10, years
    assert "over nine years" in open(KCONFIG).read()


def test_the_cycle_limit_default_is_the_datasheet_minimum_and_no_more():
    assert _kconfig_default("APP_FLASH_BACKLOG_MAX_CYCLES") == 100_000


def test_a_spent_cycle_budget_opens_nothing_and_erases_nothing(mk):
    sim, log = mk(sectors=16, max_cycles=100, init=False)
    for idx in range(16):
        sim.write_header(idx, 99 * 16 + idx + 1)     # sequence 1585..1600: 100 laps behind it
    assert log.init() == 0
    assert log.cycles() == 100
    before = bytes(sim.mem)
    assert log.append(rec(1)) == ERR_WORN
    assert bytes(sim.mem) == before and sim.ops == 0
    assert log.stats()["refused_worn"] == 1

    sim2, log2 = mk(sectors=16, max_cycles=102, init=False)
    for idx in range(16):
        sim2.write_header(idx, 99 * 16 + idx + 1)
    assert log2.init() == 0
    assert log2.append(rec(1)) == 0


def test_the_cycle_count_comes_from_the_sequence_numbers(mk):
    sim, log = mk(sectors=16)
    for i in range(16 * 4 * 3 + 4):
        log.append(rec(i, 1000))
        if i % 4 == 3:
            log.read_all()
    assert log.cycles() == 3
    assert log.init() == 0
    assert log.cycles() == 3                          # survives a reset, with no storage of its own
