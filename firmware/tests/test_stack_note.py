"""Host tests for src/stack_note.h: finding the thread whose stack ran out from
the INIT_STACKS pattern left in the stacks after a watchdog reset.

    uv run --python 3.12 --with pytest pytest -q firmware/tests/test_stack_note.py
"""
import ctypes as C
import os
import subprocess
import tempfile

import pytest

SRC = os.environ.get("GNSS_SRC", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src"))

WRAPPER = """
#include <stdlib.h>
#include "stack_note.h"
static uint8_t *g_mem[STACK_NOTE_MAX];
void set_mem(int i, uint8_t *p) { g_mem[i] = p; }
static const uint8_t *map(uint32_t start, uint32_t size) { (void)size; return start < STACK_NOTE_MAX ? g_mem[start] : NULL; }
int reg_size(void) { return (int)sizeof(struct stack_note_reg); }
unsigned magic(void) { return STACK_NOTE_MAGIC; }
int full(void) { return STACK_NOTE_FULL; }
void begin(struct stack_note_reg *r, unsigned stamp) { stack_note_begin(r, stamp); }
int add(struct stack_note_reg *r, unsigned t, unsigned start, unsigned size) { return stack_note_add(r, t, start, size); }
void seal(struct stack_note_reg *r) { stack_note_seal(r); }
int valid(const struct stack_note_reg *r, unsigned stamp) { return stack_note_valid(r, stamp); }
int unused(const uint8_t *b, int size) { return (int)stack_note_unused(b, (size_t)size); }
int find(const struct stack_note_reg *r) { return stack_note_find(r, map); }
"""

N = 16


class Ent(C.Structure):
    _fields_ = [("thread", C.c_uint32), ("start", C.c_uint32), ("size", C.c_uint32)]


class Reg(C.Structure):
    _fields_ = [("magic", C.c_uint32), ("stamp", C.c_uint32), ("n", C.c_uint32),
                ("e", Ent * N), ("check", C.c_uint32)]


@pytest.fixture(scope="module")
def lib():
    d = tempfile.mkdtemp()
    c, so = os.path.join(d, "w.c"), os.path.join(d, "libw.so")
    with open(c, "w") as f:
        f.write(WRAPPER)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC", "-o", so, c,
                    "-I", SRC], check=True)
    lib = C.CDLL(so)
    lib.magic.restype = C.c_uint32
    lib.set_mem.argtypes = [C.c_int, C.c_void_p]
    for n in ("begin", "seal", "valid", "find", "add"):
        getattr(lib, n).argtypes = [C.POINTER(Reg)] + ([C.c_uint] if n in ("begin", "valid") else [])
    lib.add.argtypes = [C.POINTER(Reg), C.c_uint, C.c_uint, C.c_uint]
    lib.unused.argtypes = [C.c_char_p, C.c_int]
    return lib


def table(lib, sizes, stamp=7):
    r = Reg()
    lib.begin(C.byref(r), stamp)
    for i, size in enumerate(sizes):
        assert lib.add(C.byref(r), 0x1000 + i, i, size) == 1
    lib.seal(C.byref(r))
    return r


def stacks(lib, sizes, left):
    """Stacks as found after the reset: `left[i]` bytes of 0xAA at the bottom, the rest used."""
    bufs = []
    for i, (size, n) in enumerate(zip(sizes, left)):
        b = C.create_string_buffer(b"\xaa" * n + b"\x33" * (size - n), size)
        lib.set_mem(i, C.addressof(b))
        bufs.append(b)
    return bufs


def test_layout_matches(lib):
    assert C.sizeof(Reg) == lib.reg_size()


def test_a_stack_with_no_pattern_left_is_the_one(lib):
    sizes = [8192, 2048, 2048, 4096]
    r = table(lib, sizes)
    keep = stacks(lib, sizes, [3000, 900, 0, 1500])
    assert lib.find(C.byref(r)) == 2
    del keep


def test_the_thinnest_margin_wins(lib):
    sizes = [2048, 2048]
    r = table(lib, sizes)
    keep = stacks(lib, sizes, [64, 8])
    assert lib.find(C.byref(r)) == 1
    del keep


def test_healthy_stacks_name_nothing(lib):
    sizes = [8192, 2048, 2048]
    r = table(lib, sizes)
    keep = stacks(lib, sizes, [2000, 500, lib.full()])   # at the limit is not under it
    assert lib.find(C.byref(r)) == -1
    del keep


def test_an_unreadable_stack_is_skipped(lib):
    sizes = [2048, 2048]
    r = table(lib, sizes)
    keep = stacks(lib, sizes, [0, 0])
    lib.set_mem(0, None)
    assert lib.find(C.byref(r)) == 1
    del keep


def test_the_table_is_invalid_until_sealed_and_for_another_image(lib):
    r = Reg()
    lib.begin(C.byref(r), 7)
    lib.add(C.byref(r), 1, 0, 2048)
    assert lib.valid(C.byref(r), 7) == 0
    lib.seal(C.byref(r))
    assert r.magic == lib.magic()
    assert lib.valid(C.byref(r), 7) == 1
    assert lib.valid(C.byref(r), 8) == 0


def test_any_flipped_bit_or_random_ram_is_invalid(lib):
    base = bytes(table(lib, [2048, 8192]))
    for byte in range(len(base)):
        b = bytearray(base)
        b[byte] ^= 1
        assert lib.valid(C.byref(Reg.from_buffer_copy(bytes(b))), 7) == 0, byte
    for _ in range(2000):
        assert lib.valid(C.byref(Reg.from_buffer_copy(os.urandom(len(base)))), 7) == 0


def test_the_table_stops_at_its_capacity(lib):
    r = Reg()
    lib.begin(C.byref(r), 1)
    for i in range(N):
        assert lib.add(C.byref(r), i, i, 256) == 1
    assert lib.add(C.byref(r), 99, 0, 256) == 0
    assert r.n == N
