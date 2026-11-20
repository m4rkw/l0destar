"""Host tests for src/imu_gate.h: whether a streamed record carries ax..gz.

    uv run --python 3.12 --with pytest pytest -q firmware/tests/test_imu_gate.py
"""
import ctypes as C
import os
import subprocess
import tempfile

import pytest

SRC = os.environ.get("GNSS_SRC", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src"))

WRAPPER = r"""
#include "imu_gate.h"
static struct imu_gate g = IMU_GATE_INIT;
void reset(void) { struct imu_gate n = IMU_GATE_INIT; g = n; }
void impact(void) { imu_gate_impact(&g); }
int due(int lean, long long now, int accel_on, int gap, int capture)
{
	return imu_gate_due(&g, lean, now, accel_on, gap, capture);
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
    l = C.CDLL(so)
    l.due.argtypes = [C.c_int, C.c_longlong, C.c_int, C.c_int, C.c_int]
    return l


@pytest.fixture
def g(lib):
    lib.reset()
    return lib


def test_not_streaming_always(g):
    for xia in (0, 1):
        assert g.due(0, 1000, xia, 900, 1)


def test_gap_zero_every_record(g):
    assert all(g.due(1, t, 1, 0, 1) for t in (0, 10, 20, 21))


def test_gap_n_limits_readings(g):
    assert g.due(1, 5000, 1, 900, 1)
    assert not g.due(1, 5100, 1, 900, 1)
    assert not g.due(1, 5899, 1, 900, 1)
    assert g.due(1, 5900, 1, 900, 1)
    assert not g.due(1, 5901, 1, 900, 1)


def test_accel_off_never(g):
    assert not any(g.due(1, t, 0, 0, 1) for t in range(0, 10000, 250))


def test_impact_with_accel_off_next_record_once(g):
    assert not g.due(1, 0, 0, 0, 1)
    g.impact()
    assert g.due(1, 100, 0, 0, 1)
    assert not g.due(1, 200, 0, 0, 1)
    g.impact()
    assert g.due(1, 300, 0, 0, 1)


def test_impact_capture_off(g):
    g.impact()
    assert not g.due(1, 100, 0, 0, 0)
    assert not g.due(1, 200, 0, 0, 1)      # the impact was consumed, not kept


def test_impact_no_effect_with_accel_on(g):
    assert g.due(1, 0, 1, 900, 1)
    g.impact()
    assert not g.due(1, 100, 1, 900, 1)     # still paced by xim
    assert not g.due(1, 200, 0, 900, 1)     # and not left over for xia = 0 later


def test_stale_impact_outside_stream_is_dropped(g):
    g.impact()
    assert g.due(0, 100, 0, 0, 1)
    assert not g.due(1, 200, 0, 0, 1)
