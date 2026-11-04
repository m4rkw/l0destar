"""Host tests for src/backup_log.h: the GNSS backup supply changes waiting to be
reported as bk=<state>:<age_s>|... on the next record.

The header is compiled with the host compiler behind a small wrapper that
keeps one list, and called through ctypes.

    uv run --python 3.12 --with pytest pytest -q firmware/tests/test_backup_log.py
"""
import ctypes as C
import os
import subprocess
import tempfile

import pytest

SRC = os.environ.get("GNSS_SRC", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src"))

WRAPPER = """
#include "backup_log.h"
static struct bklog l;
void reset(void) { l.n = 0; }
void add(int state, long long now_ms) { bklog_add(&l, state, now_ms); }
int fmt(long long now_ms, char *buf, int size) { return bklog_format(&l, now_ms, buf, size); }
void drop(int n) { bklog_drop(&l, n); }
int count(void) { return l.n; }
"""


@pytest.fixture(scope="module")
def lib():
    d = tempfile.mkdtemp()
    c = os.path.join(d, "w.c")
    so = os.path.join(d, "libw.so")
    with open(c, "w") as f:
        f.write(WRAPPER)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC", "-o", so, c,
                    "-I", SRC], check=True)
    so = C.CDLL(so)
    so.add.argtypes = [C.c_int, C.c_longlong]
    so.fmt.argtypes = [C.c_longlong, C.c_char_p, C.c_int]
    so.drop.argtypes = [C.c_int]
    so.reset()
    return so


class Log:
    def __init__(self, so):
        self.so = so
        so.reset()

    def add(self, state, now_s):
        self.so.add(state, now_s * 1000)

    def text(self, now_s, size=73):
        buf = C.create_string_buffer(size)
        n = self.so.fmt(now_s * 1000, buf, size)
        return n, buf.value.decode()


@pytest.fixture
def log(lib):
    return Log(lib)


def test_empty_says_nothing(log):
    assert log.text(100) == (0, "")


def test_a_change_goes_out_as_its_age(log):
    log.add(1, 10)
    assert log.text(10) == (1, "1:0")
    assert log.text(70) == (1, "1:60")


def test_changes_are_oldest_first_and_joined(log):
    log.add(1, 0)
    log.add(0, 86400)          # the overnight cut-off
    log.add(1, 118000)         # back with the receiver's rail
    assert log.text(118005) == (3, "1:118005|0:31605|1:5")


def test_the_same_state_again_adds_nothing(log):
    log.add(1, 0)
    log.add(1, 50)
    assert log.so.count() == 1


def test_dropping_keeps_what_was_not_reported(log):
    log.add(1, 0)
    log.add(0, 10)
    log.add(1, 20)
    log.so.drop(2)
    assert log.text(30) == (1, "1:10")
    log.so.drop(1)
    assert log.text(30) == (0, "")


def test_a_full_list_loses_its_oldest(log):
    for i in range(10):
        log.add(i % 2, i * 10)
    assert log.so.count() == 6
    n, text = log.text(100)
    assert n == 6
    assert text.startswith("0:60|")      # change 4 (state 0) at t=40 is the oldest left


def test_a_buffer_too_small_stops_at_a_whole_entry(log):
    log.add(1, 0)
    log.add(0, 5)
    n, text = log.text(10, size=6)      # room for "1:10" and its NUL, not for "|0:5"
    assert (n, text) == (1, "1:10")
    log.so.drop(n)
    assert log.text(10) == (1, "0:5")


def test_a_clock_behind_the_stamp_reads_as_zero(log):
    log.add(1, 100)
    assert log.text(50) == (1, "1:0")
