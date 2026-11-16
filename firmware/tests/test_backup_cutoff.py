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
int drop(int mode, int gxf, int gxt, int gwf, int gwt, int fixed, int on_backup, long long now_s, long long wake_s)
{ return backup_drop_due(mode, gxf, gxt, gwf, gwt, fixed, on_backup, now_s, wake_s); }
/* returns mode*10000 + from*100 + to (+1000000 when taken), cmd rewritten in place */
int take(char *cmd)
{
	enum backup_gx_mode m = BACKUP_GX_KEEP; int f = -9, t = -9;
	int got = backup_gx_take(cmd, &m, &f, &t);
	return got ? 1000000 + m * 10000 + (f < 0 ? 0 : f) * 100 + (t < 0 ? 0 : t) : 0;
}
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
    d = lib.drop
    d.argtypes = [C.c_int] * 7 + [C.c_longlong] * 2
    d.restype = C.c_int
    due_fn.drop = lambda mode, t, wake, on_backup, gx=(-1, -1), gw=(-1, -1), fixed=-1: bool(
        d(mode, gx[0], gx[1], gw[0], gw[1], fixed, on_backup, int(t.timestamp()), wake))
    tk = lib.take
    tk.argtypes = [C.c_char_p]
    tk.restype = C.c_int

    def take(cmd):
        buf = C.create_string_buffer(cmd.encode(), 128)
        r = tk(buf)
        return r, buf.value.decode()

    due_fn.take = take
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


# -- gx=: the rule while on the backup module ---------------------------------
KEEP, DEF, WIN = 0, 1, 2
GW = (2, 5)          # the regular window
FIXED = 0            # the fixed cut-off hour


def regular(due, mode, t, wake, **kw):
    return due.drop(mode, t, wake, False, gw=GW, fixed=FIXED, **kw)


def backup(due, mode, t, wake, **kw):
    return due.drop(mode, t, wake, True, gw=GW, fixed=FIXED, **kw)


@pytest.mark.parametrize("mode,gx", [(KEEP, (-1, -1)), (DEF, (-1, -1)), (WIN, (10, 12))])
def test_regular_power_ignores_the_mode(due, mode, gx):
    assert regular(due, mode, utc(3), 60, gx=gx)           # in gw
    assert regular(due, mode, utc(1, 30), 3600, gx=gx)     # gw entered in the sleep
    assert regular(due, mode, utc(23, 30), 3600, gx=gx)    # fixed hour in the sleep
    assert not regular(due, mode, utc(7), 600, gx=gx)      # nothing near


def test_keep_never_drops_on_backup(due):
    assert not backup(due, KEEP, utc(3), 60)
    assert not backup(due, KEEP, utc(1, 30), 3600)
    assert not backup(due, KEEP, utc(23, 30), 3600)
    assert not backup(due, KEEP, utc(12), 86400)


def test_def_follows_the_regular_rules_on_backup(due):
    for t, wake in [(utc(3), 60), (utc(1, 30), 3600), (utc(23, 30), 3600), (utc(7), 600),
                    (utc(12), 86400), (utc(5), 60)]:
        assert backup(due, DEF, t, wake) == regular(due, DEF, t, wake)
    assert backup(due, DEF, utc(3), 60)
    assert backup(due, DEF, utc(23, 30), 3600)
    assert not backup(due, DEF, utc(7), 600)


def test_a_gx_window_replaces_gw_and_the_fixed_hour_on_backup(due):
    gx = (10, 12)
    assert backup(due, WIN, utc(11), 60, gx=gx)            # inside
    assert not backup(due, WIN, utc(12), 60, gx=gx)        # end is exclusive
    assert not backup(due, WIN, utc(3), 60, gx=gx)         # inside gw only
    assert not backup(due, WIN, utc(1, 30), 3600, gx=gx)   # gw entered, not gx
    assert not backup(due, WIN, utc(23, 30), 3600, gx=gx)  # fixed hour ignored
    assert not backup(due, WIN, utc(7), 600, gx=gx)


def test_entering_the_gx_window_during_a_sleep_drops_first(due):
    gx = (10, 12)
    assert backup(due, WIN, utc(9, 30), 1800, gx=gx)       # wakes at 10:00
    assert not backup(due, WIN, utc(9, 30), 1799, gx=gx)
    assert backup(due, WIN, utc(6), 86400, gx=gx)


def test_a_gx_window_may_wrap_midnight(due):
    gx = (22, 6)
    assert backup(due, WIN, utc(23), 60, gx=gx)
    assert backup(due, WIN, utc(0, 30), 60, gx=gx)
    assert backup(due, WIN, utc(5, 59), 60, gx=gx)
    assert not backup(due, WIN, utc(6), 60, gx=gx)
    assert not backup(due, WIN, utc(12), 60, gx=gx)
    assert backup(due, WIN, utc(21, 30), 1800, gx=gx)      # entering


def test_no_gx_window_in_window_mode_drops_nothing(due):
    assert not backup(due, WIN, utc(3), 3600)
    assert not backup(due, WIN, utc(23, 30), 3600)


def test_without_a_gw_window_or_fixed_hour_def_drops_nothing(due):
    assert not due.drop(DEF, utc(3), 3600, True)


def test_the_default_is_keep(due):
    # enum value 0, what hw_domain.c starts with
    assert KEEP == 0
    assert not backup(due, 0, utc(3), 60)
    assert not backup(due, 0, utc(23, 30), 3600)


def test_gx_token_forms(due):
    assert due.take("gx=def") == (1010000, "")
    assert due.take("gx=off") == (1000000, "")
    assert due.take("gx=22-6") == (1020000 + 2206, "")
    assert due.take("gx=0-23") == (1020000 + 23, "")


def test_gx_token_is_taken_out_wherever_it_sits(due):
    assert due.take("track=1,gb=1,gx=3-4,gw=2-5") == (1020000 + 304, "track=1,gb=1,gw=2-5")
    assert due.take("gx=def,gpsfix") == (1010000, "gpsfix")
    assert due.take("gpsfix,gx=off") == (1000000, "gpsfix")


def test_gx_token_invalid_is_dropped_and_ignored(due):
    for bad in ["gx=5-5", "gx=24-1", "gx=1-24", "gx=-1-3", "gx=", "gx=on", "gx=1-2x", "gx=1-2-3",
                "gx=abcdefghij"]:
        assert due.take(bad) == (0, ""), bad
    assert due.take("a,gx=9-9,b") == (0, "a,b")


def test_gx_token_only_at_a_token_start(due):
    assert due.take("xgx=def") == (0, "xgx=def")
    assert due.take("gx=def,gx=off") == (1010000, "")      # first valid wins
    assert due.take("gx=5-5,gx=off") == (1000000, "")
