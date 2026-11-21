"""Host tests for src/weak_hold.h: when a very weak serving cell holds a send
back, and the cap and cooldown that keep it from holding for good.

    uv run --python 3.12 --with pytest pytest -q firmware/tests/test_weak_hold.py
"""
import ctypes as C
import os
import subprocess
import tempfile

import pytest

SRC = os.environ.get("GNSS_SRC", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src"))

WRAPPER = """
#include "weak_hold.h"
static struct weak_hold h;
void reset(void) { h = (struct weak_hold){0}; }
int weak(int valid, long long age, int rsrp, int snr, int rt, int st)
{ return weak_hold_is_weak(valid, age, rsrp, snr, rt, st); }
int decide(long long now, int w, long long max_ms) { return weak_hold_decide(&h, now, w, max_ms); }
int enabled(int rt) { return weak_hold_enabled(rt); }
int fresh_ms(void) { return WEAK_HOLD_FRESH_MS; }
int cool_ms(void) { return WEAK_HOLD_COOLDOWN_MS; }
"""

RT, ST = -115, -6
MAX = 60000


@pytest.fixture(scope="module")
def lib():
    d = tempfile.mkdtemp()
    c = os.path.join(d, "w.c")
    so = os.path.join(d, "libw.so")
    with open(c, "w") as f:
        f.write(WRAPPER)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC", "-o", so, c,
                    "-I", SRC], check=True)
    lib = C.CDLL(so)
    lib.weak.argtypes = [C.c_int, C.c_longlong, C.c_int, C.c_int, C.c_int, C.c_int]
    lib.decide.argtypes = [C.c_longlong, C.c_int, C.c_longlong]
    return lib


@pytest.fixture
def h(lib):
    lib.reset()
    return lib


def test_weak_when_either_is_at_or_below(h):
    assert h.weak(1, 0, -115, 3, RT, ST)          # rsrp at its threshold
    assert h.weak(1, 0, -119, -8, RT, ST)         # the car's case: both
    assert h.weak(1, 0, -100, -6, RT, ST)         # snr alone, at its threshold
    assert h.weak(1, 0, -115, 10, RT, ST)         # rsrp alone
    assert not h.weak(1, 0, -114, -5, RT, ST)     # just above both


def test_off_with_zero_or_above_minus_100(h):
    assert not h.weak(1, 0, -130, -15, 0, ST)
    assert not h.weak(1, 0, -130, -15, -99, ST)
    assert h.weak(1, 0, -130, -15, -100, ST)
    assert not h.enabled(0) and not h.enabled(-99) and h.enabled(-100) and h.enabled(-140)


def test_stale_or_invalid_reading_is_not_weak(h):
    assert not h.weak(0, 0, -130, -15, RT, ST)
    assert not h.weak(1, h.fresh_ms() + 1, -130, -15, RT, ST)
    assert h.weak(1, h.fresh_ms(), -130, -15, RT, ST)
    assert not h.weak(1, -1, -130, -15, RT, ST)


def test_snr_zero_is_no_reading(h):
    # 0 is also "the network gave none": a threshold of +5 must not hold on it
    assert not h.weak(1, 0, -100, 0, RT, 5)
    assert h.weak(1, 0, -100, 1, RT, 5)


def test_holds_until_the_cap_then_one_goes(h):
    assert h.decide(1000, 1, MAX)                 # first held send starts the spell
    assert h.decide(30000, 1, MAX)
    assert h.decide(1000 + MAX - 1, 1, MAX)
    assert not h.decide(1000 + MAX, 1, MAX)       # the cap: this one goes out


def test_cooldown_then_rearm(h):
    t = 1000
    assert h.decide(t, 1, MAX)
    t += MAX
    assert not h.decide(t, 1, MAX)                # attempt
    assert not h.decide(t + h.cool_ms() - 1, 1, MAX)   # cooling down
    assert h.decide(t + h.cool_ms(), 1, MAX)      # re-armed, a new spell
    assert h.decide(t + h.cool_ms() + MAX - 1, 1, MAX)
    assert not h.decide(t + h.cool_ms() + MAX, 1, MAX)  # and capped again


def test_recovery_ends_the_spell(h):
    assert h.decide(1000, 1, MAX)
    assert not h.decide(2000, 0, MAX)             # signal back: sends go
    assert h.decide(3000, 1, MAX)                 # weak again: a fresh spell...
    assert h.decide(3000 + MAX - 1, 1, MAX)       # ...with its own full cap


def test_never_holds_when_not_weak(h):
    for t in range(0, 200000, 1000):
        assert not h.decide(t, 0, MAX)


def test_cap_is_bounded_under_continuous_weak(h):
    """Over ten minutes of constant weak signal, sends are not starved: one at
    least every cap + cooldown, and holds are only ever a cap long."""
    sent, last, worst = [], 0, 0
    for t in range(0, 600000, 1000):
        if not h.decide(t, 1, MAX):
            sent.append(t)
            worst = max(worst, t - last)
            last = t
    assert len(sent) >= 8
    assert worst <= MAX + 1000
