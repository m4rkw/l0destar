"""Host tests for src/send_policy.h: when the drive stream gives up on a send
the modem will not take, and which timeout the socket carries.

The header is compiled with the host compiler behind a small wrapper and called
through ctypes.

    uv run --python 3.12 --with pytest pytest -q firmware/tests/test_send_policy.py
"""
import ctypes as C
import errno
import os
import subprocess
import tempfile

import pytest

SRC = os.environ.get("GNSS_SRC", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src"))

WRAPPER = """
#include "send_policy.h"
int tmo(int short_send, int short_ms, int normal_ms) { return send_policy_timeout_ms(short_send, short_ms, normal_ms); }
int hold(int err, int short_send, int short_ms, int *held, int held_max)
{ return send_policy_hold(err, short_send, short_ms, held, held_max); }
"""

NORMAL = 10000
SHORT = 2000
MAX = 3


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
    lib.tmo.argtypes = [C.c_int] * 3
    lib.tmo.restype = C.c_int
    lib.hold.argtypes = [C.c_int, C.c_int, C.c_int, C.POINTER(C.c_int), C.c_int]
    lib.hold.restype = C.c_int
    return lib


class Policy:
    """The caller's side: one `held` counter, a stream of failed sends."""

    def __init__(self, lib, short_send=True, short_ms=SHORT, held_max=MAX):
        self.lib, self.short_send, self.short_ms, self.held_max = lib, short_send, short_ms, held_max
        self.held = C.c_int(0)

    def fail(self, err=errno.EAGAIN):
        return bool(self.lib.hold(err, int(self.short_send), self.short_ms, C.byref(self.held), self.held_max))

    def sent(self):                     # transport_send() clears it when a send goes out
        self.held.value = 0


# -- the timeout the socket carries ---------------------------------------------------

def test_the_stream_gets_its_own_short_timeout(lib):
    assert lib.tmo(1, SHORT, NORMAL) == SHORT


def test_everything_else_keeps_the_long_one(lib):
    assert lib.tmo(0, SHORT, NORMAL) == NORMAL


def test_a_stream_timeout_of_zero_means_the_long_one(lib):
    assert lib.tmo(1, 0, NORMAL) == NORMAL


# -- giving a send up -------------------------------------------------------------------

def test_a_timed_out_stream_send_is_given_up(lib):
    assert Policy(lib).fail() is True


def test_the_third_in_a_row_replaces_the_socket(lib):
    p = Policy(lib)
    assert [p.fail(), p.fail(), p.fail()] == [True, True, False]


def test_after_the_socket_is_replaced_the_count_starts_again(lib):
    p = Policy(lib)
    assert [p.fail() for _ in range(6)] == [True, True, False, True, True, False]


def test_a_send_that_went_out_clears_the_count(lib):
    p = Policy(lib)
    p.fail()
    p.fail()
    p.sent()
    assert [p.fail(), p.fail(), p.fail()] == [True, True, False]


def test_only_a_timeout_is_given_up(lib):
    for err in (errno.ENOTCONN, errno.ENETDOWN, errno.EBADF, errno.ECONNRESET, errno.EIO, errno.ENOMEM):
        assert Policy(lib).fail(err) is False, errno.errorcode[err]


def test_another_error_resets_the_count(lib):
    p = Policy(lib)
    p.fail()
    p.fail()
    assert p.fail(errno.ENOTCONN) is False
    assert [p.fail(), p.fail(), p.fail()] == [True, True, False]


def test_outside_the_stream_nothing_is_given_up(lib):
    p = Policy(lib, short_send=False)
    assert all(p.fail() is False for _ in range(5))


def test_with_the_short_timeout_off_nothing_is_given_up(lib):
    p = Policy(lib, short_ms=0)
    assert all(p.fail() is False for _ in range(5))


def test_a_limit_of_one_never_gives_up(lib):
    p = Policy(lib, held_max=1)
    assert all(p.fail() is False for _ in range(5))


def test_a_limit_of_two_gives_up_once_and_then_replaces(lib):
    p = Policy(lib, held_max=2)
    assert [p.fail(), p.fail(), p.fail(), p.fail()] == [True, False, True, False]
