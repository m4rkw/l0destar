"""Host tests for src/modem_call_mark.h: the marker that says which modem
library call the firmware was in when a watchdog reset came.

The header is compiled with the host compiler (with the mark switched on, so the
call macros are the real ones) behind a small wrapper and called through ctypes.

    uv run --python 3.12 --with pytest pytest -q firmware/tests/test_modem_call_mark.py
"""
import ctypes as C
import os
import subprocess
import tempfile

import pytest

SRC = os.environ.get("GNSS_SRC", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src"))

WRAPPER = r"""
#define CONFIG_APP_MODEM_CALL_MARK 1
#include "modem_call_mark.h"
struct mcm_state g_mcm;
void mcm_note(unsigned cls, uint8_t what, const char *cmd) { mcm_note_into(&g_mcm, cls, what, cmd); }
void mcm_done(unsigned cls) { mcm_leave(&g_mcm, cls); }
int state_size(void) { return (int)sizeof(struct mcm_state); }
void clear(void) { mcm_clear(&g_mcm); }
void poke_magic(unsigned m) { g_mcm.magic = m; }
void enter(int cls, int what, const char *cmd) { mcm_enter(&g_mcm, cls, what, cmd ? mcm_at_code(cmd) : 0); }
void leave(int cls) { mcm_leave(&g_mcm, cls); }
void token(char *b, int n) { mcm_token(&g_mcm, b, n); }
int newest(void) { return mcm_newest(&g_mcm); }
void set_seq(int v) { g_mcm.seq = (uint16_t)v; }
unsigned code(const char *c) { return mcm_at_code(c); }
void atname(unsigned c, char *b, int n) { mcm_at_name(c, b, n); }
const char *evname(unsigned t) { return mcm_event_name(t); }

/* The macros around a call that looks at the mark from inside. */
static char g_inside[32];
static int fake(int x) { mcm_token(&g_mcm, g_inside, sizeof(g_inside)); return x * 2; }
static void fakev(void) { mcm_token(&g_mcm, g_inside, sizeof(g_inside)); }
int run_call(void) { g_inside[0] = 0; return MCM_SOCK(MCM_SOCK_SEND, fake(21)); }
int run_at(void) { g_inside[0] = 0; return MCM_CALL(MCM_C_AT, MCM_AT_ANY, "AT+CFUN=4", fake(1)); }
void run_callv(void) { g_inside[0] = 0; MCM_LTEV(MCM_LTE_NORMAL, fakev()); }
const char *inside(void) { return g_inside; }
"""


@pytest.fixture(scope="module")
def lib():
    d = tempfile.mkdtemp()
    c = os.path.join(d, "w.c")
    so = os.path.join(d, "libmcm.so")
    open(c, "w").write(WRAPPER)
    r = subprocess.run(["cc", "-Wall", "-Werror", "-O2", "-shared", "-fPIC", "-I", SRC, "-o", so, c],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    L = C.CDLL(so)
    L.inside.restype = C.c_char_p
    L.evname.restype = C.c_char_p
    L.code.restype = C.c_uint
    L.code.argtypes = [C.c_char_p]
    L.enter.argtypes = [C.c_int, C.c_int, C.c_char_p]
    L.atname.argtypes = [C.c_uint, C.c_char_p, C.c_int]
    return L


def tok(L):
    b = C.create_string_buffer(32)
    L.token(b, 32)
    return b.value.decode()


# the class and what numbers, as the header defines them
SOCK, SOCK2, AT, LTE, INIT, HND, GNSS = range(7)
SEND, RECV = 3, 4


def test_the_state_is_small(lib):
    assert lib.state_size() <= 64


def test_a_cleared_state_reports_nothing(lib):
    lib.clear()
    assert tok(lib) == "" and lib.newest() == -1


def test_a_call_still_entered_is_the_token(lib):
    lib.clear()
    lib.enter(SOCK, SEND, None)
    assert tok(lib) == "mc:send"


def test_a_call_that_returned_is_not_reported(lib):
    lib.clear()
    lib.enter(SOCK, SEND, None)
    lib.leave(SOCK)
    assert tok(lib) == ""


def test_power_on_noise_is_not_a_mark(lib):
    lib.clear()
    lib.enter(SOCK, SEND, None)
    lib.poke_magic(0x12345678)
    assert tok(lib) == "" and lib.newest() == -1


def test_the_newest_of_several_entered_calls_wins(lib):
    lib.clear()
    lib.enter(SOCK, RECV, None)
    lib.enter(AT, 1, b"AT+CFUN=1")
    assert tok(lib) == "mc:at-cfun"
    lib.leave(AT)
    assert tok(lib) == "mc:recv"            # the older one is still stuck in


def test_newest_survives_the_sequence_wrapping(lib):
    lib.clear()
    lib.set_seq(0xfffe)
    lib.enter(SOCK, RECV, None)             # 0xffff
    lib.enter(LTE, 3, None)                 # wraps to 0
    assert tok(lib) == "mc:lte-normal"


@pytest.mark.parametrize("cmd,name", [
    ("AT+CFUN=4", "cfun"), ("AT%XMONITOR", "xmon"), ("AT%%XBANDLOCK=2,\"\",\"8\"", "xban"),
    ("AT+COPS?", "cops"), ("AT%%RAI=2", "rai"), ("AT+CGDCONT=0,\"IP\",\"%s\"", "cgdc"),
    ("AT%XVBAT", "xvba"), ("AT+CPSMS=0", "cpsm"), ("at+cgsn", "cgsn"),
])
def test_at_commands_are_named_by_their_first_letters(lib, cmd, name):
    lib.clear()
    lib.enter(AT, 1, cmd.encode())
    assert tok(lib) == "mc:at-" + name


def test_an_at_command_with_no_letters_still_gives_a_valid_token(lib):
    lib.clear()
    lib.enter(AT, 1, b"%%")
    assert tok(lib) == "mc:at-x"


def test_the_digits_of_a_command_survive(lib):
    b = C.create_string_buffer(8)
    lib.atname(lib.code(b"AT+C5G"), b, 8)
    assert b.value == b"c5g"


def test_handlers_are_named_by_event_type(lib):
    lib.clear()
    lib.enter(HND, 0 + 1, None)             # LTE_LC_EVT_NW_REG_STATUS
    assert tok(lib) == "mc:hnd-reg"
    lib.enter(HND, 4 + 1, None)             # cell update
    assert tok(lib) == "mc:hnd-cell"
    lib.enter(HND, 40 + 1, None)            # a type this table does not know
    assert tok(lib) == "mc:hnd-40"
    assert lib.evname(1) == b"psm" and lib.evname(12) == b"rai" and lib.evname(13) is None


@pytest.mark.parametrize("cls,what,name", [
    (SOCK, 1, "sock"), (SOCK, 2, "conn"), (SOCK2, 3, "send"), (SOCK, 5, "poll"), (SOCK, 6, "close"),
    (SOCK, 7, "opt"), (SOCK, 8, "dns"), (SOCK, 9, "sendto"), (INIT, 1, "lib-init"),
    (INIT, 2, "lib-shut"), (LTE, 4, "lte-offline"), (LTE, 12, "lte-info"), (GNSS, 2, "gnss-start"),
])
def test_names(lib, cls, what, name):
    lib.clear()
    lib.enter(cls, what, None)
    assert tok(lib) == "mc:" + name


def test_a_garbage_what_gives_a_valid_token(lib):
    lib.clear()
    for cls in (SOCK, LTE, GNSS):
        lib.enter(cls, 200, None)
        assert tok(lib).startswith("mc:") and tok(lib).isascii()


def test_every_token_is_rst_safe(lib):
    import re
    for cls, whats in ((SOCK, range(1, 10)), (SOCK2, range(1, 10)), (LTE, range(1, 13)),
                       (INIT, (1, 2)), (GNSS, range(1, 6)), (HND, range(1, 20))):
        for w in whats:
            lib.clear()
            lib.enter(cls, w, None)
            assert re.fullmatch(r"mc:[a-z0-9-]+", tok(lib)), (cls, w, tok(lib))
    lib.clear()
    lib.enter(AT, 1, b"AT%XTEMP?")
    assert re.fullmatch(r"mc:[a-z0-9-]+", tok(lib))


def test_the_macro_marks_the_call_while_it_runs_and_clears_it_after(lib):
    lib.clear()
    assert lib.run_call() == 42                       # the value comes back unchanged
    assert lib.inside() == b"mc:send"
    assert tok(lib) == ""
    assert lib.run_at() == 2 and lib.inside() == b"mc:at-cfun" and tok(lib) == ""
    lib.run_callv()
    assert lib.inside() == b"mc:lte-normal" and tok(lib) == ""
