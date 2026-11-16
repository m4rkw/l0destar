"""Host tests for src/obd_pids.h: the OBD PID catalogue and decode, the xop
selection, which fields a record carries, the support bitmaps / osp= text and
the os= scan request.

The header is compiled with the host compiler behind a small wrapper and called
through ctypes.

    uv run --python 3.12 --with pytest pytest -q firmware/tests/test_obd_pids.py
"""
import ctypes as C
import os
import subprocess
import tempfile

import pytest

SRC = os.environ.get("GNSS_SRC", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src"))
NA = -2147483648

WRAPPER = r"""
#include <stdlib.h>
#include "obd_pids.h"

int cat_n(void) { return (int)OBD_CAT_N; }
int cat_row(int i, int *pid, int *nbytes, int *scale, const char **name, const char **legacy)
{
	if (i < 0 || i >= (int)OBD_CAT_N) return 0;
	*pid = obd_catalogue[i].pid; *nbytes = obd_catalogue[i].nbytes;
	*scale = obd_catalogue[i].scale; *name = obd_catalogue[i].name;
	*legacy = obd_catalogue[i].legacy;
	return 1;
}
int decode(int pid, const uint8_t *b, int n, int32_t *out) { return obd_pid_decode((uint8_t)pid, b, n, out); }

int sel_parse(const char *s, uint8_t *out)
{
	struct obd_sel sel;
	int n = obd_sel_parse(s, &sel);
	memcpy(out, sel.pid, n);
	return n;
}
int sel_eff(const char *tun, const char *def, uint8_t *out)
{
	struct obd_sel sel;
	obd_sel_effective(tun, def, &sel);
	memcpy(out, sel.pid, sel.n);
	return sel.n;
}

/* A snapshot filled from "pid=value,pid=value" (values in the wire integers), the
 * rpm stats as given, then formatted for a selection. */
static struct obd_snapshot snap;
void snap_reset(void) { obd_snap_clear(&snap); snap.valid = true; }
void snap_set(int pid, int v) { obd_snap_set(&snap, (uint8_t)pid, v); }
void snap_status(int mil, int dtc) { snap.mil = mil; snap.dtc_count = dtc; }
void snap_rpm_stats(int mn, int mx, int avg) { snap.rpm_min = mn; snap.rpm_max = mx; snap.rpm_avg = avg; }
void snap_forget(int pid) { obd_snap_forget(&snap, (uint8_t)pid); }
void snap_valid(int v) { snap.valid = v; }
int  snap_nx(void) { return snap.nx; }
int fmt(const char *sel, char *buf, int max)
{
	struct obd_sel s;
	obd_sel_parse(sel, &s);
	buf[0] = 0;
	return obd_format(buf, max, &snap, &s);
}
int wanted(const char *sel, uint32_t amask, int pid)
{
	struct obd_sel s;
	obd_sel_parse(sel, &s);
	return obd_pid_wanted(&s, amask, (uint8_t)pid);
}

static uint8_t bm[OBD_BM_WORDS][4];
static int nw;
void bm_clear(void) { memset(bm, 0, sizeof bm); nw = 0; }
void bm_word(int w, uint32_t v) { bm[w][0] = v >> 24; bm[w][1] = v >> 16; bm[w][2] = v >> 8; bm[w][3] = v; if (w + 1 > nw) nw = w + 1; }
int bm_sup(int pid) { return obd_bm_supported(bm, nw, (uint8_t)pid); }
int bm_more(int w) { return obd_bm_more(bm[w]); }
int bm_hex(char *out, size_t size) { return (int)obd_bm_hex(bm, nw, out, size); }

int os_take(char *cmd, int cap) { (void)cap; return obd_os_take(cmd); }

static struct obd_scan sc;
void sc_init(void) { obd_scan_init(&sc); }
void sc_reply(int os, long long now) { obd_scan_reply(&sc, os, now); }
int  sc_due(long long now) { return obd_scan_due(&sc, now); }
int  sc_want(void) { return sc.want; }
void sc_failed(long long now) { obd_scan_failed(&sc, now); }
void sc_finished(long long now) { obd_scan_finished(&sc, now); }
"""


@pytest.fixture(scope="module")
def lib():
    d = tempfile.mkdtemp()
    c = os.path.join(d, "w.c")
    so = os.path.join(d, "libw.so")
    with open(c, "w") as f:
        f.write(WRAPPER)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC", "-o", so, c, "-I", SRC], check=True)
    L = C.CDLL(so)
    L.decode.argtypes = [C.c_int, C.c_char_p, C.c_int, C.POINTER(C.c_int32)]
    L.sel_parse.argtypes = [C.c_char_p, C.c_char_p]
    L.sel_eff.argtypes = [C.c_char_p, C.c_char_p, C.c_char_p]
    L.fmt.argtypes = [C.c_char_p, C.c_char_p, C.c_int]
    L.wanted.argtypes = [C.c_char_p, C.c_uint32, C.c_int]
    L.bm_hex.argtypes = [C.c_char_p, C.c_size_t]
    L.os_take.argtypes = [C.c_char_p, C.c_int]
    L.sc_reply.argtypes = [C.c_int, C.c_longlong]
    L.sc_due.argtypes = [C.c_longlong]
    L.sc_failed.argtypes = [C.c_longlong]
    L.sc_finished.argtypes = [C.c_longlong]
    return L


def dec(lib, pid, *data):
    out = C.c_int32()
    ok = lib.decode(pid, bytes(data), len(data), C.byref(out))
    return out.value if ok else None


# -- catalogue + decode --------------------------------------------------------

REQUIRED = [0x01, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11,
            *range(0x14, 0x1C), 0x1F, 0x21, 0x22, 0x23, 0x2C, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x33,
            0x3C, 0x3D, 0x3E, 0x3F, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4C,
            0x4D, 0x4E, 0x52, 0x5C, 0x5E]


def rows(lib):
    out = []
    pid, nb, sc, name, leg = C.c_int(), C.c_int(), C.c_int(), C.c_char_p(), C.c_char_p()
    for i in range(lib.cat_n()):
        lib.cat_row.argtypes = [C.c_int] + [C.c_void_p] * 5
        assert lib.cat_row(i, C.byref(pid), C.byref(nb), C.byref(sc), C.byref(name), C.byref(leg))
        out.append((pid.value, nb.value, sc.value, name.value.decode(), leg.value.decode() if leg.value else None))
    return out


def test_catalogue_covers_the_spec_and_is_unique(lib):
    pids = [r[0] for r in rows(lib)]
    assert sorted(set(pids)) == sorted(REQUIRED)
    assert len(pids) == len(set(pids))


def test_catalogue_legacy_keys(lib):
    leg = {r[0]: r[4] for r in rows(lib) if r[4]}
    assert leg == {0x01: "omil", 0x03: "ofs", 0x04: "old", 0x05: "ocl", 0x06: "ostft", 0x07: "oltft",
                   0x0C: "orpm", 0x0D: "ospd", 0x0E: "otim", 0x0F: "oit", 0x10: "omaf", 0x11: "oth"}


def test_legacy_scales_unchanged(lib):
    sc = {r[0]: r[2] for r in rows(lib)}
    assert (sc[0x04], sc[0x11], sc[0x10], sc[0x0E], sc[0x06], sc[0x07]) == (10, 10, 100, 10, 10, 10)
    assert (sc[0x05], sc[0x0C], sc[0x0D], sc[0x0F]) == (1, 1, 1, 1)


@pytest.mark.parametrize("pid,data,want", [
    (0x01, (0x83, 0x07, 0xE5, 0x00), 0x83),       # MIL + 3 codes: raw A
    (0x03, (0x02, 0x00), 2),
    (0x04, (255,), 1000), (0x04, (128,), 501),     # the original x10 scaling
    (0x05, (130,), 90), (0x05, (0,), -40),
    (0x06, (128,), 0), (0x06, (0,), -1000), (0x07, (192,), 500),
    (0x0A, (100,), 300),
    (0x0B, (95,), 95),
    (0x0C, (0x1A, 0xF8), 1726), (0x0C, (0, 0), 0),
    (0x0D, (87,), 87),
    (0x0E, (128,), 0), (0x0E, (140,), 60),
    (0x0F, (60,), 20),
    (0x10, (0x01, 0x2C), 300),
    (0x11, (255,), 1000),
    (0x14, (200, 0xFF), 1000),                    # 200 * 0.005 V = 1 V = 1000 mV
    (0x1F, (0x01, 0x00), 256),
    (0x21, (0x00, 0x10), 16),
    (0x22, (0x10, 0x00), 3236),                   # 4096 * 0.079 = 323.58 kPa, x10
    (0x23, (0x00, 0x64), 1000),
    (0x2C, (255,), 1000), (0x2D, (128,), 0), (0x2E, (51,), 200), (0x2F, (128,), 501),
    (0x30, (12,), 12), (0x31, (0x03, 0xE8), 1000), (0x33, (101,), 101),
    (0x3C, (0x0B, 0xB8), 2600),                   # 3000/10 - 40 = 260 C, x10
    (0x42, (0x38, 0x40), 14400),                  # 14.4 V in mV
    (0x43, (0x00, 0xFF), 1000),
    (0x44, (0x80, 0x00), 1000),                   # ratio 1.0, x1000
    (0x45, (51,), 200), (0x46, (65,), 25),
    (0x47, (255,), 1000), (0x49, (0,), 0), (0x4C, (128,), 501),
    (0x4D, (0x00, 0x3C), 60), (0x4E, (0x00, 0x05), 5),
    (0x52, (255,), 1000), (0x5C, (130,), 90),
    (0x5E, (0x01, 0x2C), 1500),                   # 300 * 0.05 = 15 L/h, x100
])
def test_decode(lib, pid, data, want):
    assert dec(lib, pid, *data) == want


def test_decode_short_unknown_and_garbage(lib):
    assert dec(lib, 0x0C, 0x1A) is None           # 2-byte PID, 1 byte
    assert dec(lib, 0x0C) is None
    assert dec(lib, 0x7F, 1, 2) is None           # not in the catalogue
    assert dec(lib, 0x00, 1, 2, 3, 4) is None
    assert dec(lib, 0x20, 1, 2, 3, 4) is None
    assert dec(lib, 0x05, 130, 99, 99) == 90      # extra bytes ignored


def test_decode_every_row_does_not_misbehave(lib):
    for pid, nb, *_ in rows(lib):
        for fill in (0, 0x7F, 0xFF):
            assert dec(lib, pid, *([fill] * nb)) is not None


# -- selection -----------------------------------------------------------------

def sel(lib, s):
    buf = C.create_string_buffer(32)
    n = lib.sel_parse(s.encode(), buf)
    return list(buf.raw[:n])


def test_selection_order_kept(lib):
    assert sel(lib, "0C05") == [0x0C, 0x05]
    assert sel(lib, "050C0B") == [0x05, 0x0C, 0x0B]


def test_selection_lowercase_and_duplicates(lib):
    assert sel(lib, "0c0B0C0b") == [0x0C, 0x0B]


@pytest.mark.parametrize("text,want", [
    ("", []), ("0", []), ("zz", []), ("0C0", [0x0C]), ("0CZZ05", [0x0C, 0x05]),
    ("00", []), ("20", []), ("FF", []),            # support words and unknown PIDs
    ("0C,05", [0x0C]),                             # a separator is not part of the grammar
    ("7F0C", [0x0C]),
])
def test_selection_invalid_tokens_ignored(lib, text, want):
    assert sel(lib, text) == want


def test_selection_max_twelve(lib):
    got = sel(lib, "".join(f"{p:02X}" for p in REQUIRED))
    assert got == REQUIRED[:12]


def test_selection_none_pointer_safe(lib):
    lib.sel_parse.argtypes = [C.c_char_p, C.c_char_p]
    assert lib.sel_parse(None, C.create_string_buffer(32)) == 0


def eff(lib, tun, default="0C05"):
    buf = C.create_string_buffer(32)
    n = lib.sel_eff(tun.encode(), default.encode(), buf)
    return list(buf.raw[:n])


def test_effective_selection_defaults(lib):
    assert eff(lib, "") == [0x0C, 0x05]
    assert eff(lib, "zzzz") == [0x0C, 0x05]
    assert eff(lib, "0B") == [0x0B]
    assert eff(lib, "", default="0D") == [0x0D]
    assert eff(lib, "", default="") == [0x0C, 0x05]        # a bad build default still works


# -- what a record carries -----------------------------------------------------

def fill(lib, vals, rpm_stats=(900, 2100, 1500), status=(0, 0)):
    lib.snap_reset()
    for pid, v in vals.items():
        lib.snap_set(pid, v)
    lib.snap_rpm_stats(*rpm_stats)
    lib.snap_status(*status)


def fmt(lib, selection, max=300):
    buf = C.create_string_buffer(max)
    n = lib.fmt(selection.encode(), buf, max)
    assert n == len(buf.value)
    return buf.value.decode()


ALL = {0x0C: 1500, 0x0D: 60, 0x05: 88, 0x0F: 30, 0x04: 400, 0x11: 250, 0x10: 1200, 0x0E: 120, 0x06: -20, 0x07: 15,
       0x03: 2, 0x0B: 95, 0x2F: 640}


def test_default_selection_sends_only_rpm_stats_and_coolant(lib):
    fill(lib, ALL, status=(1, 3))
    assert fmt(lib, "0C05") == ",orpm=1500;ormin=900;ormax=2100;oravg=1500;ocl=88"


def test_unselected_legacy_pids_are_not_sent(lib):
    fill(lib, ALL, status=(1, 3))
    s = fmt(lib, "0C05")
    for key in ("ospd", "oit", "old", "oth", "omaf", "otim", "ostft", "oltft", "ofs", "omil", "odtc", "o0B"):
        assert key not in s


def test_every_legacy_key_rides_with_its_pid(lib):
    fill(lib, ALL, status=(1, 3))
    assert fmt(lib, "0D") == ",ospd=60"
    assert fmt(lib, "01") == ",omil=1;odtc=3"
    assert fmt(lib, "03") == ",ofs=2"
    assert fmt(lib, "04") == ",old=400"
    assert fmt(lib, "0F") == ",oit=30"
    assert fmt(lib, "10") == ",omaf=1200"
    assert fmt(lib, "11") == ",oth=250"
    assert fmt(lib, "0E") == ",otim=120"
    assert fmt(lib, "0607") == ",ostft=-20;oltft=15"


def test_all_sixteen_legacy_keys_keep_their_order(lib):
    fill(lib, ALL, status=(0, 2))
    s = fmt(lib, "0C0D050F0410110E06070301")
    keys = [kv.split("=")[0] for kv in s[1:].split(";")]
    assert keys == ["orpm", "ormin", "ormax", "oravg", "ospd", "ocl", "oit", "old", "oth", "omaf", "otim",
                    "ostft", "oltft", "ofs", "omil", "odtc"]


def test_new_pids_ride_as_o_hex_in_selection_order(lib):
    fill(lib, ALL)
    assert fmt(lib, "0C050B2F") == ",orpm=1500;ormin=900;ormax=2100;oravg=1500;ocl=88;o0B=95;o2F=640"
    assert fmt(lib, "2F0B") == ",o2F=640;o0B=95"


def test_not_read_this_cycle_is_left_out(lib):
    fill(lib, {0x0C: 1500, 0x0B: 95})
    assert fmt(lib, "0C050B") == ",orpm=1500;ormin=900;ormax=2100;oravg=1500;o0B=95"
    lib.snap_forget(0x0B)
    assert "o0B" not in fmt(lib, "0C050B")
    fill(lib, {0x05: 80}, rpm_stats=(NA, NA, NA))
    assert fmt(lib, "0C05") == ",ocl=80"


def test_a_selected_pid_that_was_never_read_is_not_sent(lib):
    fill(lib, {0x0C: 1500})
    assert "o2F" not in fmt(lib, "0C2F")


def test_not_valid_sends_nothing(lib):
    fill(lib, ALL)
    lib.snap_valid(0)
    assert fmt(lib, "0C05") == ""


def test_format_stops_when_out_of_room(lib):
    fill(lib, ALL)
    s = fmt(lib, "0C050B2F", max=30)
    assert s == ",orpm=1500;ormin=900"      # whole items only


def test_snapshot_extras_do_not_overflow(lib):
    lib.snap_reset()
    for pid in range(0x20, 0x20 + 30):
        lib.snap_set(pid, pid)
    assert lib.snap_nx() == 12


def test_poll_wanted_set(lib):
    # always: rpm, speed, status word; then the selection; then the alert rules' PIDs
    assert [bool(lib.wanted(b"0C05", 0, p)) for p in (0x0C, 0x0D, 0x01, 0x05, 0x04, 0x11, 0x0B)] == \
        [True, True, True, True, False, False, False]
    assert lib.wanted(b"0C05", 1 << 0x11, 0x11)
    assert lib.wanted(b"0C050B", 0, 0x0B)
    assert not lib.wanted(b"0C05", 1 << 0x11, 0x04)
    assert not lib.wanted(b"0C05", 0xFFFFFFFF, 0x40)      # the mask only covers PIDs below 0x20


# -- support bitmaps and osp= --------------------------------------------------

def test_bitmap_supported_bits(lib):
    lib.bm_clear()
    lib.bm_word(0, 0xBE1FA812)                              # the J1979 sample word, chain bit cleared
    for pid in (0x01, 0x03, 0x04, 0x05, 0x06, 0x07, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x13, 0x15, 0x1C, 0x1F):
        assert lib.bm_sup(pid), hex(pid)
    for pid in (0x02, 0x08, 0x09, 0x0A, 0x0B, 0x12, 0x14, 0x16, 0x20, 0x21, 0x00, 0xE1):
        assert not lib.bm_sup(pid), hex(pid)
    assert not lib.bm_more(0)
    lib.bm_word(0, 0xBE1FA813)                              # ...and as the spec wrote it: PID 20 follows
    assert lib.bm_sup(0x20) and lib.bm_more(0)


def test_bitmap_chain_words(lib):
    lib.bm_clear()
    lib.bm_word(0, 0x00000001)          # PID 20 exists
    lib.bm_word(1, 0x80000011)          # PID 21 and 0x3C and 0x40
    assert lib.bm_more(0) and lib.bm_more(1)
    assert lib.bm_sup(0x21) and lib.bm_sup(0x40) and not lib.bm_sup(0x22)
    assert not lib.bm_sup(0x41)         # word 2 not read


def test_osp_example(lib):
    lib.bm_clear()
    lib.bm_word(0, 0xBE1FA813)
    buf = C.create_string_buffer(64)
    assert lib.bm_hex(buf, 64) == 8 and buf.value == b"BE1FA813"


def test_osp_trims_trailing_zero_words_and_keeps_one(lib):
    lib.bm_clear()
    lib.bm_word(0, 0xBE3FA813)
    lib.bm_word(1, 0x80000000)
    lib.bm_word(2, 0)
    lib.bm_word(3, 0)
    buf = C.create_string_buffer(64)
    assert lib.bm_hex(buf, 64) == 16 and buf.value == b"BE3FA81380000000"
    lib.bm_clear()
    lib.bm_word(0, 0)
    assert lib.bm_hex(buf, 64) == 8 and buf.value == b"00000000"
    lib.bm_clear()
    lib.bm_word(0, 1)
    lib.bm_word(1, 0)
    lib.bm_word(2, 0x00000100)          # an interior zero word stays
    assert lib.bm_hex(buf, 64) == 24 and buf.value == b"00000001" b"00000000" b"00000100"


def test_osp_full_chain_is_56_chars(lib):
    lib.bm_clear()
    for w in range(7):
        lib.bm_word(w, 0xFFFFFFFF)
    buf = C.create_string_buffer(57)
    assert lib.bm_hex(buf, 57) == 56 and buf.value == b"FFFFFFFF" * 7
    small = C.create_string_buffer(56)
    assert lib.bm_hex(small, 56) == 0 and small.value == b""
    assert lib.bm_hex(small, 0) == 0


def test_osp_no_words_is_empty(lib):
    lib.bm_clear()
    buf = C.create_string_buffer(64)
    assert lib.bm_hex(buf, 64) == 0 and buf.value == b""


# -- os= token and the once-per-request rule ------------------------------------

def take(lib, text):
    buf = C.create_string_buffer(text.encode(), 200)
    r = lib.os_take(buf, 200)
    return r, buf.value.decode()


@pytest.mark.parametrize("text,want,left", [
    ("os=1", 1, ""),
    ("os=0", 0, ""),
    ("gb=1,os=1", 1, "gb=1"),
    ("os=1,gx=off", 1, "gx=off"),
    ("a=1,os=0,b=2", 0, "a=1,b=2"),
    ("os=1,os=0", 1, ""),
    ("os=2", -1, "os=2"),
    ("os=", -1, "os="),
    ("cos=1", -1, "cos=1"),
    ("os=11", -1, "os=11"),
    ("", -1, ""),
    ("track=1,im=0", -1, "track=1,im=0"),
])
def test_os_token(lib, text, want, left):
    assert take(lib, text) == (want, left)


def test_scan_one_per_request(lib):
    lib.sc_init()
    lib.sc_reply(1, 1000)
    assert lib.sc_want() and lib.sc_due(1000)
    lib.sc_finished(2000)
    assert not lib.sc_want() and not lib.sc_due(2000)
    for t in range(3000, 600000, 1000):           # the server keeps saying 1: no second scan
        lib.sc_reply(1, t)
        assert not lib.sc_due(t)


def test_scan_new_request_after_os0(lib):
    lib.sc_init()
    lib.sc_reply(1, 0)
    lib.sc_finished(10)
    lib.sc_reply(1, 20)
    assert not lib.sc_want()
    lib.sc_reply(0, 30)
    lib.sc_reply(1, 40)
    assert lib.sc_want()


def test_scan_os0_alone_does_nothing_and_withdraws(lib):
    lib.sc_init()
    lib.sc_reply(0, 0)
    assert not lib.sc_want()
    lib.sc_reply(1, 5)
    assert lib.sc_want()
    lib.sc_reply(0, 10)                           # the server no longer wants it
    assert not lib.sc_want()


def test_scan_stays_wanted_until_it_succeeds(lib):
    lib.sc_init()
    lib.sc_reply(1, 0)
    for t in range(1000, 100000, 5000):           # engine not running: nothing finishes it
        lib.sc_reply(1, t)
        assert lib.sc_want()


def test_scan_failure_backs_off(lib):
    lib.sc_init()
    lib.sc_reply(1, 100)
    assert lib.sc_due(100)
    lib.sc_failed(100)
    assert lib.sc_want() and not lib.sc_due(100 + 29999) and lib.sc_due(100 + 30000)


def test_scan_lost_answer_is_retried_much_later(lib):
    lib.sc_init()
    lib.sc_reply(1, 0)
    lib.sc_finished(1000)
    lib.sc_reply(1, 1000 + 30 * 60 * 1000 - 1)
    assert not lib.sc_want()
    lib.sc_reply(1, 1000 + 30 * 60 * 1000)
    assert lib.sc_want()


def test_scan_survives_a_fresh_boot_mid_request(lib):
    lib.sc_init()                                  # RAM-only state: gone after a reboot
    lib.sc_reply(1, 50)
    assert lib.sc_want()
