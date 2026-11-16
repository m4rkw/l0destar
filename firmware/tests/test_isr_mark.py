"""Host tests for src/isr_mark.h: the interrupt stack's high-water mark, kept
across a reset by filling the stack with a pattern and reading it back.

The header is compiled with the host compiler behind a small wrapper and called
through ctypes.

    uv run --python 3.12 --with pytest pytest -q firmware/tests/test_isr_mark.py
"""
import ctypes as C
import os
import subprocess
import tempfile

import pytest

SRC = os.environ.get("GNSS_SRC", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src"))

WRAPPER = """
#include "isr_mark.h"
int state_size(void) { return (int)sizeof(struct isr_mark_state); }
unsigned magic(void) { return ISR_MARK_MAGIC; }
int fill_byte(void) { return ISR_MARK_FILL; }
int keep(void) { return ISR_MARK_KEEP; }
int full(void) { return ISR_MARK_FULL; }
void seal(struct isr_mark_state *s, unsigned size) { isr_mark_seal(s, size); }
int valid(const struct isr_mark_state *s, unsigned size) { return isr_mark_valid(s, size); }
unsigned sumf(const struct isr_mark_state *s) { return isr_mark_sum(s); }
int unused(const uint8_t *b, int size) { return (int)isr_mark_unused(b, (size_t)size); }
int used(const uint8_t *b, int size) { return (int)isr_mark_used(b, (size_t)size); }
void fill(uint8_t *b, int size) { isr_mark_fill(b, (size_t)size); }
int loud(long used, long size) { return isr_mark_loud((size_t)used, (size_t)size); }
int report(long used, long size, int unplanned) { return isr_mark_report((size_t)used, (size_t)size, unplanned); }
void token(long used, char *b, int n) { isr_mark_token((size_t)used, b, (size_t)n); }
void line(long used, long size, char *b, int n) { isr_mark_line((size_t)used, (size_t)size, b, (size_t)n); }
"""

SIZE = 2048


class State(C.Structure):
    _fields_ = [("magic", C.c_uint32), ("size", C.c_uint32), ("check", C.c_uint32)]


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
    for name in ("state_size", "fill_byte", "keep", "full"):
        getattr(lib, name).restype = C.c_int
    lib.magic.restype = C.c_uint32
    lib.sumf.argtypes = [C.POINTER(State)]
    lib.sumf.restype = C.c_uint32
    lib.seal.argtypes = [C.POINTER(State), C.c_uint]
    lib.valid.argtypes = [C.POINTER(State), C.c_uint]
    lib.valid.restype = C.c_int
    for name in ("unused", "used"):
        getattr(lib, name).argtypes = [C.c_char_p, C.c_int]
        getattr(lib, name).restype = C.c_int
    lib.fill.argtypes = [C.c_char_p, C.c_int]
    lib.loud.argtypes = [C.c_long, C.c_long]
    lib.loud.restype = C.c_int
    lib.report.argtypes = [C.c_long, C.c_long, C.c_int]
    lib.report.restype = C.c_int
    lib.token.argtypes = [C.c_long, C.c_char_p, C.c_int]
    lib.line.argtypes = [C.c_long, C.c_long, C.c_char_p, C.c_int]
    return lib


def sealed(lib, size=SIZE):
    s = State()
    lib.seal(C.byref(s), size)
    return s


def stack(lib, depth=0, size=SIZE, top=0x11):
    """A stack as the next boot finds it: filled, then the top `depth` bytes
    overwritten by handlers; the unfilled top holds `top`."""
    buf = C.create_string_buffer(bytes([top]) * size, size)
    lib.fill(buf, size)
    if depth:
        raw = bytearray(buf.raw)
        raw[size - depth:] = bytes([0x33]) * depth
        buf = C.create_string_buffer(bytes(raw), size)
    return buf


def text(fn, *args, size=96):
    b = C.create_string_buffer(size)
    fn(*args, b, size)
    return b.value.decode()


# -- the record ------------------------------------------------------------------------------

def test_the_layout_matches_the_c_struct(lib):
    assert C.sizeof(State) == lib.state_size() == 12


def test_a_sealed_record_is_valid_for_its_size_only(lib):
    s = sealed(lib)
    assert s.magic == lib.magic()
    assert lib.valid(C.byref(s), SIZE) == 1
    assert lib.valid(C.byref(s), SIZE // 2) == 0
    assert lib.valid(C.byref(s), SIZE + 1) == 0


def test_a_record_that_was_never_sealed_is_not_valid(lib):
    assert lib.valid(C.byref(State()), SIZE) == 0
    assert lib.valid(C.byref(State.from_buffer_copy(b"\xff" * 12)), SIZE) == 0


def test_any_single_flipped_bit_breaks_it(lib):
    base = bytes(sealed(lib))
    for byte in range(len(base)):
        for bit in range(8):
            b = bytearray(base)
            b[byte] ^= 1 << bit
            assert lib.valid(C.byref(State.from_buffer_copy(bytes(b))), SIZE) == 0, (byte, bit)


def test_ram_left_by_a_power_cut_does_not_pass(lib):
    for _ in range(5000):
        s = State.from_buffer_copy(os.urandom(12))
        assert lib.valid(C.byref(s), SIZE) == 0


def test_a_right_check_over_the_wrong_magic_is_not_valid(lib):
    s = sealed(lib)
    s.magic = 0x12345678
    s.check = lib.sumf(C.byref(s))
    assert lib.valid(C.byref(s), SIZE) == 0


def test_a_right_check_over_another_size_is_not_valid(lib):
    s = sealed(lib)
    s.size = 1024
    s.check = lib.sumf(C.byref(s))
    assert lib.valid(C.byref(s), SIZE) == 0
    assert lib.valid(C.byref(s), 1024) == 1


def test_the_check_depends_on_both_words(lib):
    a = sealed(lib)
    b = sealed(lib, SIZE + 4)
    assert a.check != b.check
    c = State(magic=a.magic ^ 1, size=a.size, check=0)
    assert lib.sumf(C.byref(c)) != a.check


def test_the_pattern_is_not_the_kernels_own(lib):
    assert lib.fill_byte() == 0xA5          # CONFIG_INIT_STACKS fills with 0xAA


# -- reading the stack back -------------------------------------------------------------------

def test_a_stack_that_still_holds_the_pattern_is_unused(lib):
    buf = C.create_string_buffer(bytes([lib.fill_byte()]) * SIZE, SIZE)
    assert lib.unused(buf, SIZE) == SIZE
    assert lib.used(buf, SIZE) == 0


def test_nothing_in_a_stack_of_no_size(lib):
    assert lib.unused(b"", 0) == 0
    assert lib.used(b"", 0) == 0


def test_a_first_byte_that_is_not_the_pattern_is_all_of_it_used(lib):
    raw = bytearray([lib.fill_byte()]) * SIZE
    raw[0] = 0
    buf = C.create_string_buffer(bytes(raw), SIZE)
    assert lib.unused(buf, SIZE) == 0
    assert lib.used(buf, SIZE) == SIZE


def test_it_stops_at_the_first_byte_that_is_not_the_pattern(lib):
    raw = bytearray([lib.fill_byte()]) * SIZE
    raw[700] = 0                             # pattern again above it: not unused
    buf = C.create_string_buffer(bytes(raw), SIZE)
    assert lib.unused(buf, SIZE) == 700
    assert lib.used(buf, SIZE) == SIZE - 700


def test_only_the_last_byte_being_used_is_one_byte(lib):
    raw = bytearray([lib.fill_byte()]) * SIZE
    raw[-1] = 0
    buf = C.create_string_buffer(bytes(raw), SIZE)
    assert lib.unused(buf, SIZE) == SIZE - 1
    assert lib.used(buf, SIZE) == 1


# -- filling it --------------------------------------------------------------------------------

def test_fill_leaves_the_top_alone(lib):
    keep = lib.keep()
    buf = stack(lib)
    assert buf.raw[:SIZE - keep] == bytes([lib.fill_byte()]) * (SIZE - keep)
    assert buf.raw[SIZE - keep:] == b"\x11" * keep


def test_fill_of_a_stack_no_bigger_than_the_top_writes_nothing(lib):
    keep = lib.keep()
    for size in (1, keep - 1, keep):
        buf = C.create_string_buffer(b"\x11" * size, size)
        lib.fill(buf, size)
        assert buf.raw == b"\x11" * size, size


def test_fill_of_one_byte_more_than_the_top_writes_one_byte(lib):
    keep = lib.keep()
    size = keep + 1
    buf = C.create_string_buffer(b"\x11" * size, size)
    lib.fill(buf, size)
    assert buf.raw == bytes([lib.fill_byte()]) + b"\x11" * keep


def test_a_filled_stack_nothing_ran_on_reads_as_the_top_that_was_left(lib):
    # the unfilled top is not the pattern, so the floor is its size
    assert lib.used(stack(lib), SIZE) == lib.keep()


def test_the_peak_is_how_far_down_the_handlers_got(lib):
    for depth in (lib.keep() + 1, 304, 1024, 1500, 1920, SIZE - 1, SIZE):
        assert lib.used(stack(lib, depth), SIZE) == depth, depth


def test_a_shallower_use_than_the_top_left_reads_as_the_top(lib):
    for depth in (1, 64, lib.keep()):
        assert lib.used(stack(lib, depth), SIZE) == lib.keep(), depth


# -- what is said about it ---------------------------------------------------------------------

def test_half_the_stack_is_loud(lib):
    assert not lib.loud(1023, SIZE)
    assert lib.loud(1024, SIZE)
    assert lib.loud(SIZE, SIZE)
    assert not lib.loud(0, SIZE)


def test_after_a_reset_nothing_planned_the_peak_is_always_given(lib):
    for used in (0, 128, 304, 1023, 1024, SIZE):
        assert lib.report(used, SIZE, 1) == 1, used


def test_after_any_other_it_is_given_from_half_full_up(lib):
    assert lib.report(0, SIZE, 0) == 0
    assert lib.report(304, SIZE, 0) == 0
    assert lib.report(1023, SIZE, 0) == 0
    assert lib.report(1024, SIZE, 0) == 1
    assert lib.report(SIZE, SIZE, 0) == 1


def test_the_token_is_isr_and_the_bytes(lib):
    assert text(lib.token, 1948) == "isr:1948"
    assert text(lib.token, 0) == "isr:0"
    assert text(lib.token, SIZE) == "isr:2048"


def test_the_token_is_cut_to_fit_not_overrun(lib):
    assert text(lib.token, 1948, size=5) == "isr:"
    assert text(lib.token, 1948, size=1) == ""
    lib.token(1948, None, 0)                 # nothing to write into


def test_the_token_fits_the_buffer_it_is_given(lib):
    assert len(text(lib.token, 2 ** 31, size=16)) <= 15


def test_a_peak_line_says_how_much_of_how_much(lib):
    assert text(lib.line, 1400, SIZE) == "previous boot: interrupt stack peak 1400 of 2048 bytes"


def test_a_peak_within_a_frame_of_the_limit_is_marked_full(lib):
    full = lib.full()
    assert text(lib.line, SIZE - full, SIZE).endswith("bytes")             # 128 left: not full
    assert text(lib.line, SIZE - full + 1, SIZE).endswith("bytes (full)")  # 127 left
    assert text(lib.line, SIZE, SIZE).endswith("bytes (full)")


def test_the_line_fits_the_log_buffer(lib):
    for used, size in ((SIZE, SIZE), (65535, 65536), (2 ** 31, 2 ** 31)):
        assert len(text(lib.line, used, size, size=80)) < 79, (used, size)
