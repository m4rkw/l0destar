"""Host tests for src/install_core.h: installation mode's ir= string, alert
summary, im= reply token and retained-RAM note.

The header is compiled with the host compiler behind a small wrapper and called
through ctypes.

    uv run --python 3.12 --with pytest pytest -q firmware/tests/test_install.py
"""
import ctypes as C
import os
import subprocess
import tempfile

import pytest

SRC = os.environ.get("GNSS_SRC", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src"))

WRAPPER = """
#include "install_core.h"
#include <stdlib.h>

/* items as "name:s,name:s" with s one of 1 0 -1 written as 1, 0, n */
static int parse(const char *spec, struct install_item *it, char names[][16])
{
    int n = 0;
    char *copy = strdup(spec), *save = NULL;
    for (char *t = strtok_r(copy, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
        char *c = strchr(t, ':');
        *c = 0;
        strcpy(names[n], t);
        it[n].name = names[n];
        it[n].state = c[1] == '1' ? INSTALL_OK : c[1] == '0' ? INSTALL_FAIL : INSTALL_NA;
        n++;
    }
    free(copy);
    return n;
}
int ir(const char *spec, char *buf, int size) {
    struct install_item it[64]; char names[64][16];
    int n = parse(spec, it, names);
    return (int)install_ir_build(it, n, buf, (size_t)size);
}
int summary(const char *spec, char *buf, int size) {
    struct install_item it[64]; char names[64][16];
    int n = parse(spec, it, names);
    return (int)install_summary(it, n, buf, (size_t)size);
}
int take_im(char *cmd) { return install_take_im(cmd); }
int name_valid(const char *n) { return install_name_valid(n); }

static struct install_crumb c;
void crumb_store(int mode) { install_crumb_store(&c, mode); }
int crumb_load(int def) { return install_crumb_load(&c, def); }
void crumb_poke(int field, unsigned v) {
    if (field == 0) c.magic = v; else if (field == 1) c.mode = v; else c.check = v;
}
void crumb_flip_bit(int bit) { ((unsigned char *)&c)[bit / 8] ^= 1u << (bit % 8); }
void crumb_wipe(void) { memset(&c, 0xA5, sizeof c); }
int crumb_size(void) { return (int)sizeof c; }
"""


@pytest.fixture(scope="module")
def lib():
    d = tempfile.mkdtemp()
    c = os.path.join(d, "w.c")
    so = os.path.join(d, "libw.so")
    with open(c, "w") as f:
        f.write(WRAPPER)
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC", "-o", so, c,
                    "-I", SRC], check=True)
    so = C.CDLL(so)
    for fn in (so.ir, so.summary):
        fn.argtypes = [C.c_char_p, C.c_char_p, C.c_int]
    so.take_im.argtypes = [C.c_char_p]
    so.name_valid.argtypes = [C.c_char_p]
    so.crumb_store.argtypes = [C.c_int]
    so.crumb_load.argtypes = [C.c_int]
    so.crumb_poke.argtypes = [C.c_int, C.c_uint]
    so.crumb_flip_bit.argtypes = [C.c_int]
    return so


def ir(lib, spec, size=512):
    buf = C.create_string_buffer(size)
    lib.ir(spec.encode(), buf, size)
    return buf.value.decode()


def summary(lib, spec, size=512):
    buf = C.create_string_buffer(size)
    lib.summary(spec.encode(), buf, size)
    return buf.value.decode()


def take(lib, cmd):
    buf = C.create_string_buffer(cmd.encode(), 256)
    v = lib.take_im(buf)
    return v, buf.value.decode()


# -- ir= ---------------------------------------------------------------------

def test_ir_states(lib):
    assert ir(lib, "v12:1,can3v3:0,k12v:n") == "v12:1;can3v3:0;k12v:-"


def test_ir_empty(lib):
    assert ir(lib, "") == ""


def test_ir_bad_names_are_left_out(lib):
    assert ir(lib, "v12:1,Bad:1,toolongname:1,a-b:1,ok2:0") == "v12:1;ok2:0"
    assert not lib.name_valid(b"")
    assert not lib.name_valid(b"UPPER")
    assert not lib.name_valid(b"nine12345")
    assert lib.name_valid(b"eight123")
    assert lib.name_valid(b"k12v")


def test_ir_max_20_entries(lib):
    spec = ",".join(f"p{i}:1" for i in range(30))
    out = ir(lib, spec)
    assert len(out.split(";")) == 20
    assert out.startswith("p0:1;p1:1")


def test_ir_max_160_chars(lib):
    # 8-char names: 11 chars an entry plus the separator, so 13 fit in 160
    spec = ",".join(f"abcdefg{i % 10}:1" for i in range(20))
    out = ir(lib, spec)
    assert len(out) <= 160
    assert len(out.split(";")) == 14 and len(out) == 153
    assert all(len(e) == 10 for e in out.split(";"))     # whole entries only


def test_ir_never_overflows_the_buffer(lib):
    out = ir(lib, "v12:1,ign:0,gps3v3:1,can3v3:1", size=12)
    assert out == "v12:1;ign:0"
    assert len(out) < 12


def test_ir_the_real_set_fits(lib):
    spec = ("v12:1,ign:1,gps3v3:1,can3v3:1,k3v3:1,k12v:1,can:1,imu:1,ina:1,"
            "modem:1,gnss:1,flash:1,backup:1")
    out = ir(lib, spec)
    assert len(out.split(";")) == 13 and len(out) <= 160


# -- summary -----------------------------------------------------------------

def test_summary_all_ok(lib):
    assert summary(lib, "v12:1,imu:1") == "Install check: 2 ok, 0 failed"


def test_summary_names_the_failures_and_skips_not_fitted(lib):
    assert (summary(lib, "v12:0,ign:1,k3v3:n,imu:0,modem:1")
            == "Install check: 2 ok, 2 failed: v12, imu")


def test_summary_is_cut_not_overflowed(lib):
    out = summary(lib, "v12:0,ign:0,gps3v3:0", size=40)
    assert len(out) < 40 and out.startswith("Install check: 0 ok, 3 failed")


def test_summary_fits_an_alert(lib):
    spec = ",".join(f"name{i:03d}:0" for i in range(13))
    assert len(summary(lib, spec, size=120)) < 120


# -- im= token ---------------------------------------------------------------

def test_im_alone(lib):
    assert take(lib, "im=1") == (1, "")
    assert take(lib, "im=0") == (0, "")


def test_im_among_the_usual_tokens(lib):
    assert take(lib, "track=0,gb=1,im=1") == (1, "track=0,gb=1")
    assert take(lib, "im=0,track=0,gb=1") == (0, "track=0,gb=1")
    assert take(lib, "track=0,im=1,gb=1") == (1, "track=0,gb=1")


def test_im_with_a_command(lib):
    assert take(lib, "track=0,im=1,instreport") == (1, "track=0,instreport")


def test_im_every_copy_is_removed_the_first_wins(lib):
    assert take(lib, "im=0,track=0,im=1") == (0, "track=0")
    assert take(lib, "im=1,im=0") == (1, "")


def test_im_absent_or_not_a_token(lib):
    assert take(lib, "track=0,gb=1") == (-1, "track=0,gb=1")
    assert take(lib, "") == (-1, "")
    assert take(lib, "tim=1") == (-1, "tim=1")
    assert take(lib, "xim=1,ab") == (-1, "xim=1,ab")
    assert take(lib, "im=") == (-1, "im=")


def test_im_bad_value_is_left_alone(lib):
    assert take(lib, "im=2") == (-1, "im=2")
    assert take(lib, "im=10,track=0") == (-1, "im=10,track=0")
    assert take(lib, "im=x") == (-1, "im=x")


# -- the retained-RAM note ---------------------------------------------------

def test_crumb_roundtrip(lib):
    lib.crumb_store(1)
    assert lib.crumb_load(0) == 1
    lib.crumb_store(0)
    assert lib.crumb_load(1) == 0


def test_crumb_wiped_ram_gives_the_default(lib):
    lib.crumb_wipe()
    assert lib.crumb_load(1) == 1
    assert lib.crumb_load(0) == 0


def test_crumb_bad_checksum_or_magic_gives_the_default(lib):
    lib.crumb_store(1)
    lib.crumb_poke(2, 0)
    assert lib.crumb_load(0) == 0
    lib.crumb_store(0)
    lib.crumb_poke(0, 0)
    assert lib.crumb_load(1) == 1


def test_crumb_mode_out_of_range_is_invalid_even_with_a_matching_sum(lib):
    lib.crumb_store(1)
    lib.crumb_poke(1, 2)
    assert lib.crumb_load(0) == 0


def test_crumb_every_single_bit_flip_is_caught(lib):
    for want in (0, 1):
        for bit in range(lib.crumb_size() * 8):
            lib.crumb_store(want)
            lib.crumb_flip_bit(bit)
            default = 1 - want
            assert lib.crumb_load(default) == default, (want, bit)
