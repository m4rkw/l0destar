"""Host tests for src/tunables_core.h: the tu= reply token that makes the
server-changeable settings (tunables) equal to the pairs listed.

The header is compiled with the host compiler behind a small wrapper and
called through ctypes.

    uv run --python 3.12 --with pytest pytest -q firmware/tests/test_tunables.py
"""
import ctypes as C
import os
import subprocess
import tempfile

import pytest

SRC = os.environ.get("GNSS_SRC", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src"))

KEYS = ["xtt", "xpi", "xii", "xap", "xab", "xis", "xbw", "xbp", "xds", "xdi", "xtp", "xdd", "xns", "xrp", "xlw"]
BUILT = dict(xtt=6, xpi=800, xii=700, xap=2, xab=300, xis=30, xbw=11900, xbp=11800, xds=0, xdi=0,
             xtp=1000, xdd=60, xns=300, xrp=30, xlw=0)
LIMITS = dict(xtt=(0, 30), xpi=(300, 2500), xii=(0, 1200), xap=(-2, 2), xab=(0, 86400), xis=(10, 600),
              xbw=(11000, 12500), xbp=(10500, 12200), xds=(0, 1), xdi=(0, 900), xtp=(200, 5000),
              xdd=(1, 600), xns=(60, 86400), xrp=(5, 600), xlw=(0, 1))

WRAPPER = r"""
#include <stdlib.h>
#include "tunables_core.h"
static struct tun_table t;
static int32_t built[TUN_N_INT];
void set_built(int k, int v) { built[k] = v; }
void apply(const char *s, size_t n) { tun_table_apply(&t, s, n); }
int  get(int k) { return tun_table_get(&t, (enum tun_key)k, built); }
int  is_set(int k) { return t.set[k]; }
int  oa_set(void) { return t.oa_set; }
const char *oa(void) { return t.oa; }
int  fmt_built(char *out, size_t cap, const char *oa) { return tun_format_built(out, cap, built, oa); }
/* the same, but the bytes after the list are a trap: a parser that reads past
 * the length would pick up xtt:11 and an xoa from them */
void apply_exact(const char *s, size_t n) {
	static const char trap[] = ";xtt:11;xoa:TRAP;xpi:999";
	char *p = malloc(n + sizeof trap);
	memcpy(p, s, n);
	memcpy(p + n, trap, sizeof trap);
	tun_table_apply(&t, p, n);
	free(p);
}
"""


@pytest.fixture(scope="module")
def lib():
    d = tempfile.mkdtemp()
    c = os.path.join(d, "w.c")
    so = os.path.join(d, "libw.so")
    with open(c, "w") as f:
        f.write(WRAPPER)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC", "-o", so, c, "-I", SRC], check=True)
    return C.CDLL(so)


class Tun:
    def __init__(self, lib):
        self.l = lib
        lib.apply.argtypes = [C.c_char_p, C.c_size_t]
        lib.apply_exact.argtypes = [C.c_char_p, C.c_size_t]
        lib.get.restype = C.c_int
        lib.oa.restype = C.c_char_p
        for i, k in enumerate(KEYS):
            lib.set_built(i, BUILT[k])
        self.apply("")

    def apply(self, s, exact=False):
        b = s.encode() if isinstance(s, str) else s
        (self.l.apply_exact if exact else self.l.apply)(b, len(b))

    def get(self, k):
        return self.l.get(KEYS.index(k))

    def is_set(self, k):
        return bool(self.l.is_set(KEYS.index(k)))

    @property
    def oa(self):
        return self.l.oa().decode() if self.l.oa_set() else None


@pytest.fixture
def t(lib):
    return Tun(lib)


def test_nothing_set_means_built_values(t):
    for k in KEYS:
        assert t.get(k) == BUILT[k]
        assert not t.is_set(k)
    assert t.oa is None


@pytest.mark.parametrize("lst", ["", "-"])
def test_empty_and_dash_clear_everything(t, lst):
    t.apply("xtt:9;xpi:1000")
    assert t.get("xtt") == 9
    t.apply(lst if lst != "-" else "")   # tunables.c turns "-" into an empty list
    assert t.get("xtt") == BUILT["xtt"]


def test_pairs_are_applied(t):
    t.apply("xtt:8;xpi:1000;xap:-1;xdi:5;xlw:1")
    assert (t.get("xtt"), t.get("xpi"), t.get("xap"), t.get("xdi"), t.get("xlw")) == (8, 1000, -1, 5, 1)
    assert t.get("xab") == BUILT["xab"]


def test_unlisted_keys_revert(t):
    t.apply("xtt:8;xpi:1000")
    t.apply("xpi:1100")
    assert t.get("xtt") == BUILT["xtt"] and not t.is_set("xtt")
    assert t.get("xpi") == 1100


def test_zero_is_a_value_not_unset(t):
    t.apply("xtt:0;xii:0;xdi:0")
    assert t.is_set("xtt") and t.get("xtt") == 0
    assert t.get("xii") == 0


@pytest.mark.parametrize("k", list(LIMITS))
def test_bounds_inclusive_and_outside_ignored(t, k):
    lo, hi = LIMITS[k]
    t.apply(f"{k}:{lo}")
    assert t.is_set(k) and t.get(k) == lo
    t.apply(f"{k}:{hi}")
    # 12200 poweroff is above the built warning level less 100 mV: clamped
    assert t.is_set(k) and t.get(k) == (11800 if k == "xbp" else hi)
    for bad in (lo - 1, hi + 1):
        t.apply(f"{k}:{bad}")
        assert not t.is_set(k) and t.get(k) == BUILT[k], (k, bad)


@pytest.mark.parametrize("v", ["", "x", "1x", "1.5", " 5", "+5", "--5", "-", "99999999999999999999"])
def test_garbage_values_ignored(t, v):
    t.apply(f"xtt:{v};xpi:900")
    assert not t.is_set("xtt")
    assert t.get("xpi") == 900


def test_floor_values_protect_the_unit(t):
    t.apply("xns:0;xis:0;xns:59;xis:9")
    assert t.get("xns") == BUILT["xns"] and t.get("xis") == BUILT["xis"]


def test_unknown_codes_and_malformed_segments_ignored(t):
    t.apply("zzz:5;xtt:7;;;x:1;xtt;:3;xttt:9;xt:4")
    assert t.get("xtt") == 7
    assert sum(t.is_set(k) for k in KEYS) == 1


def test_last_duplicate_wins_and_a_bad_later_one_unsets(t):
    t.apply("xtt:5;xtt:9")
    assert t.get("xtt") == 9
    t.apply("xtt:5;xtt:99")
    assert not t.is_set("xtt")


def test_xoa_pipes_become_commas(t):
    t.apply("xtt:5;xoa:coolant>=95|rpm>6500")
    assert t.oa == "coolant>=95,rpm>6500"
    assert t.get("xtt") == 5


def test_xoa_not_listed_reverts(t):
    t.apply("xoa:rpm>6500")
    t.apply("xtt:5")
    assert t.oa is None


def test_xoa_empty_or_overlong_or_unprintable_rejected(t):
    t.apply("xoa:")
    assert t.oa is None
    t.apply("xoa:" + "a" * 120)
    assert t.oa == "a" * 120
    t.apply("xoa:" + "a" * 121)
    assert t.oa is None
    t.apply("xoa:" + "a" * 5000)
    assert t.oa is None
    t.apply("xoa:rpm>1\x01")
    assert t.oa is None
    t.apply("xoa:rpm>1\xc3\xa9".encode("latin-1"))
    assert t.oa is None


def test_xoa_among_other_pairs_in_any_order(t):
    t.apply("xoa:rpm>6500;xtt:4;xpi:900")
    assert t.oa == "rpm>6500" and t.get("xtt") == 4 and t.get("xpi") == 900


def test_battery_poweroff_never_above_warning_minus_100(t):
    t.apply("xbw:11500;xbp:11400")
    assert (t.get("xbw"), t.get("xbp")) == (11500, 11400)
    t.apply("xbw:11500;xbp:11450")
    assert t.get("xbp") == 11400
    t.apply("xbw:11000")                  # built poweroff 11800 is clamped too
    assert t.get("xbp") == 10900
    t.apply("xbp:12200")                  # built warning 11900
    assert t.get("xbp") == 11800


def test_battery_built_pair_untouched_with_no_override(t):
    assert (t.get("xbw"), t.get("xbp")) == (11900, 11800)


@pytest.mark.parametrize("n", [0, 1, 4, 5, 6, 40])
def test_no_read_past_the_length(lib, n):
    tt = Tun(lib)
    full = "xtt:8;xpi:1000;xoa:rpm>6500|load>99;xdi:3"
    tt.apply(full[:n], exact=True)
    assert tt.get("xtt") in (BUILT["xtt"], 8)      # never the trap's 11
    assert tt.oa in (None, "rpm>6500,load>99") or full[:n].startswith("xtt:8;xpi:1000;xoa:")
    assert tt.oa != "TRAP" and tt.get("xpi") in (BUILT["xpi"], 1000)


def test_long_garbage_is_safe(lib):
    tt = Tun(lib)
    tt.apply(("xoa:" + "z" * 100 + ";") * 200, exact=True)
    assert tt.oa == "z" * 100
    tt.apply(b";" * 10000 + b"xtt:3", exact=True)
    assert tt.get("xtt") == 3


# -- tb=: the built values as a record field ---------------------------------

TB_ORDER = ["xtt", "xpi", "xii", "xap", "xab", "xoa", "xis", "xbw", "xbp", "xds", "xdi", "xtp", "xdd", "xns", "xrp", "xlw"]


def fmt(lib, oa, cap=272):
    lib.fmt_built.argtypes = [C.c_char_p, C.c_size_t, C.c_char_p]
    for i, k in enumerate(KEYS):
        lib.set_built(i, BUILT[k])
    buf = C.create_string_buffer(cap)
    n = lib.fmt_built(buf, cap, oa.encode())
    return n, buf.value.decode()


def test_tb_commas_become_pipes(lib):
    n, s = fmt(lib, "a>5,b<2")
    assert n == len(s)
    pairs = dict(p.split(":", 1) for p in s.split(";"))
    assert list(pairs) == TB_ORDER
    assert pairs["xoa"] == "a>5|b<2"
    assert pairs["xtt"] == "6" and pairs["xbw"] == "11900" and pairs["xlw"] == "0"


def test_tb_empty_xoa(lib):
    n, s = fmt(lib, "")
    assert ";xoa:;xis:30;" in s


def test_tb_negative(lib):
    lib.set_built(KEYS.index("xap"), -2)
    buf = C.create_string_buffer(272)
    assert lib.fmt_built(buf, 272, b"") > 0
    assert b"xap:-2;" in buf.value
    lib.set_built(KEYS.index("xap"), 2)


def test_tb_worst_case_fits(lib):
    oa = ",".join(["x"] * 61)[:120]
    assert len(oa) == 120
    for i, k in enumerate(KEYS):
        lib.set_built(i, LIMITS[k][1] if k != "xap" else -2)
    buf = C.create_string_buffer(272)
    lib.fmt_built.argtypes = [C.c_char_p, C.c_size_t, C.c_char_p]
    n = lib.fmt_built(buf, 272, oa.encode())
    assert 0 < n <= 250 and n == len(buf.value)
    assert buf.value.decode().count("|") == oa.count(",")


def test_tb_too_small_or_long(lib):
    n, s = fmt(lib, "x" * 121)
    assert (n, s) == (-1, "")
    n, s = fmt(lib, "x" * 120, cap=100)
    assert (n, s) == (-1, "")
    n, s = fmt(lib, "", cap=1)
    assert (n, s) == (-1, "")
