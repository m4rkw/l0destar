"""Host tests for src/cmd_merge.h: merging a reply's commands into the ones
still waiting for the state machine to run them.

The header is compiled with the host compiler behind a small wrapper and called
through ctypes.

    uv run --python 3.12 --with pytest pytest -q firmware/tests/test_cmd_merge.py
"""
import ctypes as C
import os
import subprocess
import tempfile

import pytest

SRC = os.environ.get("GNSS_SRC", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src"))

CAP = 128                      # sizeof(pending_server_cmd)

WRAPPER = """
#include "cmd_merge.h"
static int drops;
static char dropped_text[512];
static void on_drop(const char *item, size_t len)
{
	drops++;
	size_t have = strlen(dropped_text);
	if (have + len + 2 < sizeof(dropped_text)) {
		if (have) dropped_text[have++] = '|';
		memcpy(dropped_text + have, item, len);
		dropped_text[have + len] = '\\0';
	}
}
int merge(char *pending, int cap, const char *cmd)
{
	drops = 0;
	dropped_text[0] = '\\0';
	cmd_merge_add(pending, (size_t)cap, cmd, on_drop);
	return drops;
}
const char *dropped(void) { return dropped_text; }
int has(const char *list, const char *item) { return cmd_merge_has(list, item, strlen(item)); }
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
    lib = C.CDLL(so)
    lib.merge.argtypes = [C.c_char_p, C.c_int, C.c_char_p]
    lib.merge.restype = C.c_int
    lib.dropped.restype = C.c_char_p
    lib.has.argtypes = [C.c_char_p, C.c_char_p]
    lib.has.restype = C.c_int
    return lib


class Pending:
    """The 128-byte pending_server_cmd, merged into through the C code."""

    def __init__(self, lib, text="", cap=CAP):
        self.lib = lib
        self.cap = cap
        self.buf = C.create_string_buffer(text.encode(), cap)

    def add(self, cmd):
        """Merge a reply's commands in; returns how many items were dropped."""
        return self.lib.merge(self.buf, self.cap, cmd.encode())

    @property
    def text(self):
        return self.buf.value.decode()

    @property
    def items(self):
        return [i for i in self.text.split(",") if i]

    @property
    def dropped(self):
        return self.lib.dropped().decode()


def old_merge(pending, cmd, cap=CAP):
    """The rules before cmd_merge.h (data.c, 0.4.125): only fota=, track=, gb=,
    im= and gx= were dropped from the waiting copy.  Returns (text, dropped)."""
    merged = cmd[:cap - 1]
    dropped = []
    for item in [i for i in pending.split(",") if i]:
        if item.startswith(("fota=", "track=", "gb=", "im=", "gx=")):
            continue
        if len(merged) + 1 + len(item) < cap:
            merged += "," + item
        else:
            dropped.append(item)
    return merged, dropped


REPLY = "gw=0-7,im=0,gx=off,le=1"          # what every reply carried on 15 Nov


def test_newest_reply_goes_first_and_the_waiting_one_follows(lib):
    p = Pending(lib, "selftest")
    assert p.add("gw=0-7,le=1") == 0
    assert p.items == ["gw=0-7", "le=1", "selftest"]


def test_empty_pending_takes_the_reply_as_it_is(lib):
    p = Pending(lib)
    assert p.add(REPLY) == 0
    assert p.text == REPLY


def test_a_waiting_copy_of_a_per_reply_field_is_dropped_not_kept_beside_it(lib):
    """The defect: gw= and le= were kept, so the buffer grew with every reply."""
    p = Pending(lib)
    for _ in range(50):
        assert p.add("gw=0-7,le=1") == 0
    assert p.items == ["gw=0-7", "le=1"]


def test_the_newest_value_wins_when_the_value_changed(lib):
    p = Pending(lib, "gw=0-7,le=1")
    p.add("gw=off,le=0")
    assert p.items == ["gw=off", "le=0"]


def test_a_burst_of_replies_keeps_a_real_command_and_drops_nothing(lib):
    """Nine replies handed over at once by a modem that held them, with a
    one-off command in the second: no cmd_run in between."""
    p = Pending(lib)
    dropped = 0
    for n in range(9):
        dropped += p.add(REPLY + (",reboot" if n == 1 else ""))
    assert dropped == 0
    assert "reboot" in p.items
    assert len(p.text) < 60


def test_the_old_rules_overflow_in_the_same_burst_and_can_lose_the_command():
    """Documents the defect the scenario above is about (the same replies)."""
    text, lost = "", []
    for n in range(40):
        text, d = old_merge(text, REPLY + (",reboot" if n == 0 else ""))
        lost += d
    assert lost, "the old rules never overflowed in this burst"
    # The oldest waiting item is last in the list, so it is the first to go.
    assert "reboot" in lost
    assert "reboot" not in text.split(",")


def test_a_field_the_newest_reply_lacks_survives_unless_it_is_state(lib):
    p = Pending(lib, "le=0,track=1,fota=0.4.130,gb=1,im=1,gx=off")
    p.add("gw=0-7")
    # le= is kept (the newest reply says nothing about it); the listed state fields go
    assert p.items == ["gw=0-7", "le=0"]


def test_keys_that_only_share_a_prefix_are_not_the_same_field(lib):
    p = Pending(lib, "le=1,gw=0-7")
    p.add("level=3,gwx=1")
    assert p.items == ["level=3", "gwx=1", "le=1", "gw=0-7"]


def test_a_bare_command_is_kept_once(lib):
    p = Pending(lib, "selftest,reboot")
    p.add("selftest,gw=0-7")
    assert p.items == ["selftest", "gw=0-7", "reboot"]


def test_a_bare_command_is_not_taken_for_a_field_that_starts_with_it(lib):
    p = Pending(lib, "sleep=60")
    p.add("sleep")
    assert p.items == ["sleep", "sleep=60"]


def test_duplicates_among_the_waiting_items_collapse(lib):
    p = Pending(lib, "reboot,reboot,gw=0-7,gw=0-7")
    p.add("le=1")
    assert p.items == ["le=1", "reboot", "gw=0-7"]


def test_distinct_one_off_commands_all_stay_in_order(lib):
    p = Pending(lib)
    p.add("netscan,gw=0-7")
    p.add("selftest,gw=0-7")
    p.add("backlog=flush,gw=0-7")
    assert p.items == ["backlog=flush", "gw=0-7", "selftest", "netscan"]


def test_overflow_of_genuinely_distinct_commands_is_still_reported(lib):
    p = Pending(lib, "a1=" + "x" * 40 + ",b2=" + "y" * 40)
    n = p.add("c3=" + "z" * 40)
    assert n == 1                           # the last one did not fit in 128 bytes
    assert p.dropped.startswith("b2=")
    assert len(p.text) < CAP


def test_a_reply_longer_than_the_buffer_is_cut_and_the_rest_dropped_with_a_report(lib):
    p = Pending(lib, "selftest")
    n = p.add("x=" + "q" * 200)
    assert n == 1 and p.dropped == "selftest"
    assert len(p.text) == CAP - 1


def test_the_buffer_is_always_terminated_and_never_overrun(lib):
    sentinel = b"\xAA" * 16
    buf = C.create_string_buffer(b"reboot", CAP + len(sentinel))
    C.memmove(C.addressof(buf) + CAP, sentinel, len(sentinel))
    for n in range(300):
        lib.merge(buf, CAP, ("f%d=%s,g=%d" % (n, "v" * (n % 37), n)).encode())
        assert len(buf.value) < CAP
    assert bytes(buf)[CAP:CAP + len(sentinel)] == sentinel


def test_has_matches_whole_keys_and_whole_bare_commands(lib):
    assert lib.has(b"gw=0-7,le=1", b"le=9")
    assert not lib.has(b"gw=0-7,le=1", b"l=9")
    assert lib.has(b"selftest", b"selftest")
    assert not lib.has(b"selftest=1", b"selftest")
    assert not lib.has(b"", b"gw=0-7")
