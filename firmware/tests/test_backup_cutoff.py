"""Host tests for src/backup_cutoff.h: when a sleep has to drop the GNSS backup
supply first because the cut-off hour falls inside it.

The header is compiled with the host compiler behind a one-function wrapper and
called through ctypes.

    uv run --python 3.12 --with pytest pytest -q firmware/tests/test_backup_cutoff.py
"""
import ctypes as C
import datetime
import os
import subprocess
import tempfile

import pytest

SRC = os.environ.get("GNSS_SRC", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src"))

WRAPPER = """
#include "backup_cutoff.h"
int due(long long now_s, long long wake_s, int hour) { return backup_cutoff_due(now_s, wake_s, hour); }
int win(long long now_s, int from, int to) { return backup_window_contains(now_s, from, to); }
"""


@pytest.fixture(scope="module")
def due():
    d = tempfile.mkdtemp()
    c = os.path.join(d, "w.c")
    so = os.path.join(d, "libw.so")
    with open(c, "w") as f:
        f.write(WRAPPER)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC", "-o", so, c,
                    "-I", SRC], check=True)
    lib = C.CDLL(so)
    fn = lib.due
    fn.argtypes = [C.c_longlong, C.c_longlong, C.c_int]
    fn.restype = C.c_int
    w = lib.win
    w.argtypes = [C.c_longlong, C.c_int, C.c_int]
    w.restype = C.c_int
    due_fn = lambda t, wake, hour: bool(fn(int(t.timestamp()), wake, hour))
    due_fn.window = lambda t, a, b: bool(w(int(t.timestamp()), a, b))
    return due_fn


def utc(h, m=0, s=0, day=1):
    return datetime.datetime(2026, 11, day, h, m, s, tzinfo=datetime.timezone.utc)


def test_a_sleep_that_ends_before_the_hour_keeps_the_supply(due):
    assert not due(utc(22, 0), 3600, 0)
    assert not due(utc(22, 59, 59), 3600 - 1 - 1, 0)


def test_a_sleep_that_reaches_the_hour_drops_it_first(due):
    assert due(utc(23, 0), 3600, 0)            # wakes at exactly midnight
    assert due(utc(23, 10), 3600, 0)           # wakes at 00:10
    assert due(utc(12, 0), 86400, 0)           # a day-long sleep always spans it


def test_the_hour_may_be_any_hour(due):
    assert due(utc(4, 30), 1800, 5)
    assert not due(utc(4, 30), 1799, 5)
    assert due(utc(4, 59, 59), 1, 5)
    assert due(utc(23, 30), 3 * 3600, 2)       # across midnight to 02:00


def test_the_instant_itself_is_due(due):
    assert due(utc(0, 0, 0), 0, 0)
    assert due(utc(5, 0, 0), 600, 5)


def test_past_the_hour_waits_for_the_next_day(due):
    # a drive after the cut-off turned the supply on: it stays until the next one
    assert not due(utc(0, 40), 3600, 0)
    assert not due(utc(6, 0), 3600, 5)
    assert due(utc(6, 0), 23 * 3600, 5)


def test_month_and_year_ends_are_ordinary_days(due):
    assert due(datetime.datetime(2026, 12, 31, 23, 30, tzinfo=datetime.timezone.utc), 3600, 0)
    assert not due(datetime.datetime(2026, 2, 28, 12, 0, tzinfo=datetime.timezone.utc), 3600, 0)


@pytest.mark.parametrize("hour", [-1, 24, 99])
def test_an_hour_outside_the_day_means_never(due, hour):
    assert not due(utc(23, 30), 7200, hour)


def test_a_clock_that_was_never_set_or_a_negative_sleep_does_nothing(due):
    assert not due(datetime.datetime(1969, 12, 31, 23, 0, tzinfo=datetime.timezone.utc), 7200, 0)
    assert not due(utc(23, 30), -1, 0)


def test_a_window_holds_from_its_start_hour_up_to_its_end_hour(due):
    assert not due.window(utc(6, 59, 59), 7, 12)
    assert due.window(utc(7, 0), 7, 12)
    assert due.window(utc(11, 59, 59), 7, 12)
    assert not due.window(utc(12, 0), 7, 12)


def test_a_window_may_wrap_midnight(due):
    assert due.window(utc(23, 0), 22, 6)
    assert due.window(utc(0, 0), 22, 6)
    assert due.window(utc(5, 59, 59), 22, 6)
    assert not due.window(utc(6, 0), 22, 6)
    assert not due.window(utc(21, 59, 59), 22, 6)


def test_midnight_to_seven_is_not_a_wrap(due):
    assert due.window(utc(0, 0), 0, 7)
    assert due.window(utc(6, 59, 59), 0, 7)
    assert not due.window(utc(7, 0), 0, 7)
    assert not due.window(utc(23, 59, 59), 0, 7)


def test_no_window_for_equal_or_out_of_range_hours(due):
    assert not due.window(utc(3), 3, 3)
    assert not due.window(utc(3), -1, -1)
    assert not due.window(utc(3), 0, 24)
    assert not due.window(utc(3), 24, 6)
