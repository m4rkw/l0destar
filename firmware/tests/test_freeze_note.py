"""Host tests for src/freeze_note.h: the note the hardware watchdog's early
warning leaves for the next boot, and how that boot words it.

The header is compiled with the host compiler behind a small wrapper and called
through ctypes.

    uv run --python 3.12 --with pytest pytest -q firmware/tests/test_freeze_note.py
"""
import ctypes as C
import os
import subprocess
import tempfile

import pytest

SRC = os.environ.get("GNSS_SRC", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src"))

WRAPPER = """
#include "freeze_note.h"
int note_size(void) { return (int)sizeof(struct freeze_note); }
int magic(void) { return (int)FREEZE_MAGIC; }
void seal(struct freeze_note *n) { freeze_note_seal(n); }
int valid(const struct freeze_note *n) { return freeze_note_valid(n); }
void shortf(const struct freeze_note *n, char *b, int size) { freeze_note_short(n, b, (size_t)size); }
int line(const struct freeze_note *n, int idx, char *b, int size) { return freeze_note_line(n, idx, b, (size_t)size); }
void irqs(const unsigned *w, char *b, int size) { freeze_note_irqs(w, b, (size_t)size); }
unsigned sumf(const struct freeze_note *n) { return freeze_note_sum(n); }
"""

LINES = 5


class Note(C.Structure):
    _fields_ = [
        ("magic", C.c_uint32), ("flags", C.c_uint32), ("exc_return", C.c_uint32),
        ("frame", C.c_uint32 * 8), ("sp", C.c_uint32), ("above", C.c_uint32 * 8),
        ("iabr", C.c_uint32 * 2), ("ispr", C.c_uint32 * 2), ("icsr", C.c_uint32),
        ("rtc", C.c_uint32 * 8), ("check", C.c_uint32),
    ]


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
    lib.note_size.restype = C.c_int
    lib.magic.restype = C.c_int
    lib.seal.argtypes = [C.POINTER(Note)]
    lib.valid.argtypes = [C.POINTER(Note)]
    lib.valid.restype = C.c_int
    lib.shortf.argtypes = [C.POINTER(Note), C.c_char_p, C.c_int]
    lib.line.argtypes = [C.POINTER(Note), C.c_int, C.c_char_p, C.c_int]
    lib.line.restype = C.c_int
    lib.irqs.argtypes = [C.POINTER(C.c_uint32 * 2), C.c_char_p, C.c_int]
    lib.sumf.argtypes = [C.POINTER(Note)]
    lib.sumf.restype = C.c_uint32
    return lib


def make(pc=0x48a79, lr=0x3e255, xpsr=0x61000000, **kw):
    n = Note()
    n.frame[6], n.frame[5], n.frame[7] = pc, lr, xpsr
    n.sp = 0x20021f38
    for k, v in kw.items():
        setattr(n, k, v)
    return n


def sealed(lib, **kw):
    n = make(**kw)
    lib.seal(C.byref(n))
    return n


def short(lib, n, size=64):
    b = C.create_string_buffer(size)
    lib.shortf(C.byref(n), b, size)
    return b.value.decode()


def line(lib, n, idx, size=200):
    b = C.create_string_buffer(size)
    ok = lib.line(C.byref(n), idx, b, size)
    return ok, b.value.decode()


# -- layout, sealing ---------------------------------------------------------------------

def test_the_layout_matches_the_c_struct(lib):
    assert C.sizeof(Note) == lib.note_size()


def test_a_sealed_note_is_valid(lib):
    assert lib.valid(C.byref(sealed(lib))) == 1
    assert sealed(lib).magic == lib.magic() & 0xffffffff


def test_a_note_that_was_never_sealed_is_not(lib):
    assert lib.valid(C.byref(make())) == 0


def test_any_single_flipped_bit_breaks_it(lib):
    base = bytes(sealed(lib))
    for byte in range(len(base)):
        for bit in (0, 7):
            b = bytearray(base)
            b[byte] ^= 1 << bit
            n = Note.from_buffer_copy(bytes(b))
            assert lib.valid(C.byref(n)) == 0, (byte, bit)


def test_ram_left_by_a_power_cut_does_not_pass(lib):
    for _ in range(2000):
        n = Note.from_buffer_copy(os.urandom(C.sizeof(Note)))
        assert lib.valid(C.byref(n)) == 0


def test_a_right_check_with_the_wrong_magic_is_not_valid(lib):
    n = sealed(lib)
    n.magic = 0x12345678
    assert lib.valid(C.byref(n)) == 0                 # the check no longer fits
    n.check = lib.sumf(C.byref(n))                    # now it does, over the wrong magic
    assert lib.valid(C.byref(n)) == 0                 # the magic itself is what is refused


# -- the token for rst= ---------------------------------------------------------------------

def test_thread_code_gives_pc_and_lr_with_the_thumb_bit_off(lib):
    assert short(lib, make(pc=0x48a79, lr=0x3e255)) == "fz:48a78/3e254"


def test_an_interrupt_handler_adds_its_irq_number(lib):
    # IPSR 58 = IRQ 42 (the modem's IPC)
    assert short(lib, make(xpsr=0x6100003a)) == "fz:48a78/3e254/i42"


def test_the_system_timers_irq(lib):
    assert short(lib, make(xpsr=0x61000025)) == "fz:48a78/3e254/i21"      # IPSR 37 = IRQ 21


def test_irq_zero_is_an_interrupt_not_a_system_exception(lib):
    assert short(lib, make(xpsr=0x61000010)) == "fz:48a78/3e254/i0"      # IPSR 16 = IRQ 0
    assert short(lib, make(xpsr=0x6100000f)).endswith("/e15")


def test_the_exception_number_is_nine_bits(lib):
    assert short(lib, make(xpsr=0x61000101)).endswith("/i241")      # IPSR 257, as the field allows


def test_a_system_exception_is_marked_e(lib):
    assert short(lib, make(xpsr=0x6100000f)) == "fz:48a78/3e254/e15"      # SysTick


def test_the_token_always_fits_rst(lib):
    n = make(pc=0xfffffff1, lr=0xfffffff1, xpsr=0x610001ff)
    assert len(short(lib, n, 64)) < 30


# -- the detail lines -------------------------------------------------------------------------

def test_there_are_five_lines_and_a_sixth_does_not_exist(lib):
    n = sealed(lib)
    assert [line(lib, n, i)[0] for i in range(LINES)] == [1] * LINES
    assert line(lib, n, LINES)[0] == 0 and line(lib, n, -1)[0] == 0


def test_line_0_says_where_the_code_was(lib):
    n = make(xpsr=0x61000000)
    _, text = line(lib, n, 0)
    assert "pc 0x00048a78" in text and "lr 0x0003e254" in text and "thread" in text
    n = make(xpsr=0x6100003a)
    assert "i42" in line(lib, n, 0)[1] and "thread" not in line(lib, n, 0)[1]


def test_line_0_says_when_the_task_watchdog_had_already_expired(lib):
    assert "task wdt had expired" not in line(lib, make(), 0)[1]
    assert "task wdt had expired" in line(lib, make(flags=1), 0)[1]


def test_line_1_lists_the_active_and_pending_interrupts(lib):
    n = make()
    n.iabr[0], n.iabr[1] = 1 << 24, 1 << 10          # IRQ 24 (the watchdog) and IRQ 42 (modem IPC)
    n.ispr[0] = 1 << 21                              # IRQ 21 (the system timer) waiting
    text = line(lib, n, 1)[1]
    assert "active irq 24,42" in text and "pending irq 21" in text


def test_line_1_says_none_with_a_dash(lib):
    text = line(lib, make(), 1)[1]
    assert "active irq - pending irq -" in text


def test_line_2_gives_the_timer_registers(lib):
    n = make()
    n.rtc[0], n.rtc[1], n.rtc[2], n.rtc[3], n.rtc[4] = 1000, 1100, 0, 7, 9
    n.rtc[5], n.rtc[6], n.rtc[7] = 1, 0, 1
    assert line(lib, n, 2)[1] == "rtc1 cnt 1000 cc 1100 0 7 9 ev 1 0 1"


def test_line_3_gives_the_registers(lib):
    n = make()
    for i in range(5):
        n.frame[i] = 0x10 + i
    assert line(lib, n, 3)[1] == "r0-r3,r12 10 11 12 13 14"


def test_line_4_gives_the_words_above_the_frame(lib):
    n = make()
    for i in range(8):
        n.above[i] = 0xa0 + i
    assert line(lib, n, 4)[1] == "stack above the frame a0 a1 a2 a3 a4 a5 a6 a7"


def test_no_line_is_long_even_with_every_word_at_its_widest(lib):
    n = Note.from_buffer_copy(b"\xff" * C.sizeof(Note))
    for i in range(LINES):
        _, text = line(lib, n, i, 400)
        assert 0 < len(text) < 120, (i, len(text), text)


# -- never past the buffer ---------------------------------------------------------------------

@pytest.mark.parametrize("size", [1, 2, 3, 5, 8, 13, 21, 34, 60])
def test_nothing_ever_writes_past_the_buffer(lib, size):
    n = Note.from_buffer_copy(b"\xff" * C.sizeof(Note))
    for what in range(1 + LINES):
        buf = C.create_string_buffer(b"\xAA" * (size + 16), size + 16)
        if what == 0:
            lib.shortf(C.byref(n), buf, size)
        else:
            lib.line(C.byref(n), what - 1, buf, size)
        raw = bytes(buf)
        assert raw[size:] == b"\xAA" * 16, (what, size)
        assert b"\x00" in raw[:size], (what, size)


def test_a_zero_length_buffer_is_left_alone(lib):
    n = make()
    buf = C.create_string_buffer(b"\xAA" * 8, 8)
    lib.shortf(C.byref(n), buf, 0)
    assert lib.line(C.byref(n), 0, buf, 0) == 0
    assert bytes(buf) == b"\xAA" * 8


def test_all_sixty_four_interrupts_active_still_fit(lib):
    w = (C.c_uint32 * 2)(0xffffffff, 0xffffffff)
    buf = C.create_string_buffer(b"\xAA" * 56, 56)
    lib.irqs(C.byref(w), buf, 40)
    raw = bytes(buf)
    assert raw[40:] == b"\xAA" * 16 and b"\x00" in raw[:40]
