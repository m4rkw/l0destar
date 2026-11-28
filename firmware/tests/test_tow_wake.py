"""Host tests for src/tow_wake.h: the offsets and threshold of the IMU's
absolute wake, and when a parked unit is centred (comparator armed, no tilt
poll) or off centre (high-pass wake and the poll).

    uv run --python 3.12 --with pytest pytest -q firmware/tests/test_tow_wake.py
"""
import ctypes as C
import math
import os
import random
import subprocess
import tempfile

import pytest

SRC = os.environ.get("GNSS_SRC", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src"))

WRAPPER = """
#include "tow_wake.h"
static struct tow_wake s;
void reset(void) { s = (struct tow_wake){0}; }
void trip(void) { tow_wake_trip(&s); }
int ofs(int mg) { return tow_wake_ofs(mg); }
int ths(int x, int y, int z, int deg)
{ int r[3] = { x, y, z }; return tow_wake_ths(r, deg); }
int dev(int x, int y, int z, int rx, int ry, int rz)
{
	int n[3] = { x, y, z };
	int8_t o[3] = { tow_wake_ofs(rx), tow_wake_ofs(ry), tow_wake_ofs(rz) };
	return tow_wake_dev_ug(n, o);
}
int centred(int alerted, int x, int y, int z, int rx, int ry, int rz, int t)
{
	int n[3] = { x, y, z };
	int8_t o[3] = { tow_wake_ofs(rx), tow_wake_ofs(ry), tow_wake_ofs(rz) };
	return tow_wake_centred(&s, alerted, n, o, t);
}
int knock(int delta, int swing) { return tow_wake_knock(delta, swing); }
int ths_ug(void) { return TOW_WAKE_THS_UG; }
int ths_min(void) { return TOW_WAKE_THS_MIN; }
int ths_max(void) { return TOW_WAKE_THS_MAX; }
"""

FLAT = (0, 0, 1000)


@pytest.fixture(scope="module")
def lib():
    d = tempfile.mkdtemp()
    c = os.path.join(d, "t.c")
    so = os.path.join(d, "libt.so")
    with open(c, "w") as f:
        f.write(WRAPPER)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC", "-o", so, c,
                    "-I", SRC, "-lm"], check=True)
    return C.CDLL(so)


@pytest.fixture
def t(lib):
    lib.reset()
    return lib


def at(t, y, alerted=0, ref=FLAT, ths=6):
    """Re-arm with the unit y mg off the flat reference along Y."""
    return t.centred(alerted, ref[0], y, ref[2], *ref, ths)


def test_offsets_carry_the_reference_sign(t):
    assert t.ofs(0) == 0
    assert t.ofs(1000) == 64
    assert t.ofs(-1000) == -64
    assert t.ofs(7) == 0 and t.ofs(8) == 1 and t.ofs(-8) == -1
    assert t.ofs(5000) == 127 and t.ofs(-5000) == -127
    # at rest on the reference the comparator sees only the rounding
    assert t.dev(300, -500, 812, 300, -500, 812) <= 7813


def test_default_threshold(t):
    assert t.ths(0, 0, 1000, 6) in (6, 7)


def test_no_threshold_where_the_comparator_cannot_serve(t):
    assert t.ths(0, 0, 1000, 0) == 0          # tow watch off
    assert t.ths(0, 0, 1000, 3) == 0          # too small an angle: poll
    assert t.ths(0, 0, 0, 6) == 0             # powered-down IMU
    assert t.ths(0, 0, 4000, 6) == 0          # wrong full scale


def test_large_angle_is_capped(t):
    assert t.ths(0, 0, 1000, 30) == t.ths_max()


def rotate(v, axis, deg):
    """Rodrigues rotation of v about a unit axis."""
    a = math.radians(deg)
    dot = sum(p * q for p, q in zip(axis, v))
    cross = (axis[1] * v[2] - axis[2] * v[1], axis[2] * v[0] - axis[0] * v[2],
             axis[0] * v[1] - axis[1] * v[0])
    return [v[i] * math.cos(a) + cross[i] * math.sin(a) + axis[i] * dot * (1 - math.cos(a))
            for i in range(3)]


def unit(v):
    n = math.sqrt(sum(p * p for p in v))
    return [p / n for p in v]


def test_a_tow_angle_tilt_trips_in_any_direction_and_mounting(t):
    rng = random.Random(1)
    checked = 0
    for _ in range(4000):
        g = unit([rng.gauss(0, 1) for _ in range(3)])
        ref = [round(p * rng.uniform(970, 1030)) for p in g]
        deg = rng.randint(1, 30)
        ths = t.ths(*ref, deg)
        if ths == 0:
            continue
        assert t.ths_min() <= ths <= t.ths_max()
        # a rotation about an axis square to gravity tilts it by exactly deg
        r = [rng.gauss(0, 1) for _ in range(3)]
        u = unit(ref)
        axis = unit([r[i] - u[i] * sum(a * b for a, b in zip(r, u)) for i in range(3)])
        now = [round(p) for p in rotate(ref, axis, deg)]
        # within the 1 mg of the integer readings
        assert t.dev(*now, *ref) > ths * t.ths_ug() - 1000
        checked += 1
    assert checked > 2000
    # from 6 degrees up there is always a threshold
    for _ in range(500):
        ref = [round(p * 1000) for p in unit([rng.gauss(0, 1) for _ in range(3)])]
        assert t.ths(*ref, 6) > 0


def test_starts_centred_and_leaves_past_the_threshold(t):
    assert at(t, 0)
    assert at(t, 46)                 # 6 steps = 46.9 mg
    assert not at(t, 48)


def test_returns_only_within_half(t):
    assert not at(t, 60)
    assert not at(t, 40)             # inside the threshold, outside half
    assert not at(t, 24)
    assert at(t, 23)
    assert at(t, 40)                 # centred again: the full threshold


def test_latched_alert_holds_it_off_centre(t):
    assert not at(t, 0, alerted=1)
    assert not at(t, 30)             # latch cleared, still outside half
    assert at(t, 0)


def test_rearm_at_a_new_attitude_recentres(t):
    new = (0, 150, 989)
    assert not t.centred(1, *new, *FLAT, 6)      # alerted, off centre
    assert t.centred(0, *new, *new, 6)           # reference moved, latch cleared


def test_trip_holds_for_one_arm_even_when_back(t):
    assert at(t, 0)
    t.trip()
    assert not at(t, 0)              # this pass
    assert at(t, 0)                  # a poll later


def test_trip_then_half_threshold(t):
    t.trip()
    assert not at(t, 40)
    assert not at(t, 40)
    assert at(t, 10)


def test_knock(t):
    assert not t.knock(0, 0)
    assert not t.knock(155, 155)
    assert t.knock(156, 0)
    assert t.knock(0, 156)           # sideways: the magnitude barely moves
