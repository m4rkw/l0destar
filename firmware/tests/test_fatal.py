"""Host tests for src/fatal.c: the note a crash leaves for the next boot.

The real fatal.c is #included into a harness translation unit built with the
host compiler against stub Zephyr headers.  The harness "crashes" a chosen
thread (sys_reboot() longjmps back instead of rebooting) and then reads the note
the way the next boot does.  What is checked: the rst= fragment is exactly what
it always was (the server column is only 48 characters wide, so it must not
grow), the ERR line names the thread and its stack, a stack overflow says its
registers are not valid, a thread pointer that is not plainly in RAM is never
read, and a note from an older layout or from random power-on RAM reports nothing.

    uv run --python 3.12 --with pytest pytest -q firmware/tests/test_fatal.py
"""
import ctypes as C
import os
import shutil
import subprocess
import tempfile

import pytest

SRC = os.environ.get("FATAL_SRC", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src"))

KERNEL_H = r"""
#ifndef STUB_KERNEL_H
#define STUB_KERNEL_H
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define Z_IS_ENABLED1(m) Z_IS_ENABLED2(_XXXX##m)
#define _XXXX1 _YYYY,
#define Z_IS_ENABLED2(a) Z_IS_ENABLED3(a 1, 0)
#define Z_IS_ENABLED3(ignore, val, ...) val
#define IS_ENABLED(m) Z_IS_ENABLED1(m)
#define ARG_UNUSED(x) (void)(x)
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define BUILD_ASSERT(c, msg) _Static_assert(c, msg)
#define CODE_UNREACHABLE __builtin_unreachable()

struct k_thread { struct { uintptr_t start; size_t size; } stack_info; };
typedef struct k_thread *k_tid_t;
struct k_work_q { struct k_thread thread; };
/* k_sys_work_q is a macro in the harness, so that it lives in the test's RAM. */
k_tid_t k_current_get(void);
static inline k_tid_t k_work_queue_thread_get(struct k_work_q *q) { return &q->thread; }
void printk(const char *fmt, ...);
#endif
"""

FATAL_H = r"""
#ifndef STUB_FATAL_H
#define STUB_FATAL_H
#include <stdint.h>
struct arch_esf { struct { uint32_t r0, r1, r2, r3, r12, lr, pc, xpsr; } basic; };
enum k_fatal_error_reason {
    K_ERR_CPU_EXCEPTION, K_ERR_SPURIOUS_IRQ, K_ERR_STACK_CHK_FAIL, K_ERR_KERNEL_OOPS, K_ERR_KERNEL_PANIC,
};
void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf);
#endif
"""

REBOOT_H = r"""
#ifndef STUB_REBOOT_H
#define STUB_REBOOT_H
#define SYS_REBOOT_COLD 1
void sys_reboot(int type);
#endif
"""

LOG_H = r"""
#ifndef STUB_LOG_H
#define STUB_LOG_H
#define LOG_MODULE_REGISTER(...) typedef int h_log_module_unused
void h_log_err(const char *fmt, ...);
#define LOG_ERR(...) h_log_err(__VA_ARGS__)
#endif
"""

LOG_CTRL_H = r"""
#ifndef STUB_LOG_CTRL_H
#define STUB_LOG_CTRL_H
#define LOG_PANIC() ((void)0)
#endif
"""

HARNESS = r"""
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>

/* The test's RAM: everything a thread object may legitimately be.  fatal.c reads
 * CONFIG_SRAM_BASE_ADDRESS / CONFIG_SRAM_SIZE (KB) to decide what it may touch. */
static struct {
    struct k_work_q sysq;
    struct k_thread other;
    uint8_t pad[4096];
} h_mem;
#define CONFIG_SRAM_BASE_ADDRESS ((uintptr_t)&h_mem)
#define CONFIG_SRAM_SIZE (sizeof(h_mem) / 1024)
#define k_sys_work_q (h_mem.sysq)

#include "fatal.c"

static struct k_thread g_outside;           /* a valid object, but not in "RAM" */
static k_tid_t g_current;
static jmp_buf g_jb;
static char g_err[512], g_printk[512];
static int g_rebooted, g_returned;

k_tid_t k_current_get(void) { return g_current; }

void printk(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_printk, sizeof(g_printk), fmt, ap);
    va_end(ap);
}

void h_log_err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_err, sizeof(g_err), fmt, ap);
    va_end(ap);
}

void sys_reboot(int type) { g_rebooted = type; longjmp(g_jb, 1); }

/* which: 0 the system workqueue, 1 another thread in RAM, 2 a thread outside RAM,
 * 3 a misaligned pointer inside RAM, 4 no thread at all. */
static k_tid_t pick(int which)
{
    switch (which) {
    case 0: return &h_mem.sysq.thread;
    case 1: return &h_mem.other;
    case 2: return &g_outside;
    case 3: return (k_tid_t)((uint8_t *)h_mem.pad + 1);
    default: return NULL;
    }
}

uint32_t h_thread_addr(int which) { return (uint32_t)(uintptr_t)pick(which); }

void h_crash(int which, unsigned reason, unsigned pc, unsigned lr, unsigned stack, int use_esf)
{
    struct arch_esf esf;

    memset(&esf, 0, sizeof(esf));
    esf.basic.pc = pc;
    esf.basic.lr = lr;
    g_current = pick(which);
    h_mem.sysq.thread.stack_info.size = stack;
    h_mem.other.stack_info.size = stack;
    g_outside.stack_info.size = 777;        /* what a read of the wrong object would show */
    g_rebooted = 0;
    g_returned = 0;
    if (setjmp(g_jb) == 0) {
        k_sys_fatal_error_handler(reason, use_esf ? &esf : NULL);
        g_returned = 1;
    }
}

const char *h_last_crash(void) { return fatal_last_crash(); }
const char *h_err(void) { return g_err; }
const char *h_printk(void) { return g_printk; }
int h_rebooted(void) { return g_rebooted; }
int h_returned(void) { return g_returned; }
uint32_t h_magic(void) { return s_crash.magic; }
void h_set_magic(uint32_t m) { s_crash.magic = m; }
void h_poison(unsigned seed)
{
    uint8_t *b = (uint8_t *)&s_crash;
    unsigned x = seed * 2654435761u + 1;

    for (size_t i = 0; i < sizeof(s_crash); i++) {
        x = x * 1664525u + 1013904223u;
        b[i] = (uint8_t)(x >> 24);
    }
    if (s_crash.magic == FATAL_MAGIC) {
        s_crash.magic ^= 1;
    }
}
int h_size(void) { return (int)sizeof(s_crash); }
uint32_t h_magic_now(void) { return FATAL_MAGIC; }
"""

CFLAGS = ["-std=gnu11", "-Wall", "-Wextra", "-Wno-unused-parameter", "-O1", "-D__noinit=",
          "-DCONFIG_THREAD_STACK_INFO=1", "-DCONFIG_APP_LOG_LEVEL=3"]

OLD_MAGIC = 0x10DE57A2          # the layout before the thread and stack were added
K_ERR_CPU_EXCEPTION, K_ERR_STACK_CHK_FAIL = 0, 2


def write_stubs(d):
    for rel, text in (("zephyr/kernel.h", KERNEL_H), ("zephyr/fatal.h", FATAL_H),
                      ("zephyr/sys/reboot.h", REBOOT_H), ("zephyr/logging/log.h", LOG_H),
                      ("zephyr/logging/log_ctrl.h", LOG_CTRL_H)):
        path = os.path.join(d, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as f:
            f.write(text)
    with open(os.path.join(d, "h.c"), "w") as f:
        f.write(HARNESS)


@pytest.fixture(scope="module")
def so():
    d = tempfile.mkdtemp()
    write_stubs(d)
    out = os.path.join(d, "libfatal.so")
    cmd = ["cc", *CFLAGS, "-shared", "-fPIC", "-o", out, "-I", d, "-I", SRC, os.path.join(d, "h.c")]
    r = subprocess.run(cmd, capture_output=True, text=True)
    assert r.returncode == 0 and r.stderr == "", r.stderr          # clean under -Wall -Wextra
    return out


_copies = [0]


def load(path):
    # A fresh copy per test: fatal_last_crash() keeps function-level statics.
    _copies[0] += 1
    fresh = os.path.join(os.path.dirname(path), f"copy{_copies[0]}.so")
    shutil.copyfile(path, fresh)
    L = C.CDLL(fresh)
    L.h_crash.argtypes = [C.c_int, C.c_uint, C.c_uint, C.c_uint, C.c_uint, C.c_int]
    L.h_thread_addr.argtypes = [C.c_int]
    L.h_thread_addr.restype = C.c_uint32
    for f in ("h_last_crash", "h_err", "h_printk"):
        getattr(L, f).restype = C.c_char_p
    L.h_magic.restype = C.c_uint32
    L.h_set_magic.argtypes = [C.c_uint32]
    L.h_poison.argtypes = [C.c_uint]
    L.h_magic_now.restype = C.c_uint32
    return L


def text(b):
    return b.decode() if b is not None else None


def test_stack_overflow_in_the_system_workqueue(so):
    L = load(so)
    L.h_crash(0, K_ERR_STACK_CHK_FAIL, 0x3E99C735, 0x00057CE7, 2048, 1)
    assert L.h_rebooted() == 1 and L.h_returned() == 0     # a cold reboot, and the handler never returns
    thread = "0x%08x" % L.h_thread_addr(0)
    # at the time of the crash, on the console
    assert text(L.h_printk()) == ("*** FATAL: reason 2 pc 0x3e99c735 lr 0x00057ce7 thread %s stack 2048"
                                  " — rebooting ***\n" % thread)
    # on the next boot: the rst= fragment is what it always was, the ERR line says which thread
    assert text(L.h_last_crash()) == "fatal:2@0x3e99c735"
    assert text(L.h_err()) == ("previous boot ended in fatal error 2 at 0x3e99c735 (lr 0x00057ce7), "
                               "thread %s (sysworkq), stack 2048 B (overflow: pc/lr invalid)" % thread)


def test_another_thread_is_told_apart_by_its_stack(so):
    L = load(so)
    L.h_crash(1, K_ERR_STACK_CHK_FAIL, 0x1234, 0x5678, 8192, 1)
    assert text(L.h_last_crash()) == "fatal:2@0x1234"
    err = text(L.h_err())
    assert err == ("previous boot ended in fatal error 2 at 0x00001234 (lr 0x00005678), "
                   "thread 0x%08x, stack 8192 B (overflow: pc/lr invalid)" % L.h_thread_addr(1))
    assert "sysworkq" not in err


def test_a_fault_that_is_not_an_overflow_has_no_overflow_note(so):
    L = load(so)
    L.h_crash(0, K_ERR_CPU_EXCEPTION, 0x2A1C8, 0x2A1CD, 2048, 1)
    assert text(L.h_last_crash()) == "fatal:0@0x2a1c8"
    err = text(L.h_err())
    assert "overflow" not in err and "(sysworkq), stack 2048 B" in err


def test_the_rst_fragment_cannot_outgrow_its_column(so):
    L = load(so)
    L.h_crash(0, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 1)
    frag = text(L.h_last_crash())
    assert frag == "fatal:4294967295@0xffffffff"
    assert len(frag) <= 31                       # s_str[32]; the whole rst= value is only 48 on the server


@pytest.mark.parametrize("which, why", [(2, "outside RAM"), (3, "misaligned"), (4, "no thread")])
def test_a_thread_pointer_that_is_not_plainly_ram_is_not_read(so, which, why):
    L = load(so)
    L.h_crash(which, K_ERR_STACK_CHK_FAIL, 0x1, 0x2, 2048, 1)
    assert text(L.h_last_crash()) == "fatal:2@0x1"
    # the object outside RAM was set to 777 bytes: seeing that would mean it was read
    assert ", stack 0 B" in text(L.h_err()), why
    assert L.h_rebooted() == 1


def test_a_missing_frame_still_reports(so):
    L = load(so)
    L.h_crash(0, 4, 0, 0, 2048, 0)               # K_ERR_KERNEL_PANIC, and no exception frame at all
    assert text(L.h_last_crash()) == "fatal:4@0x0"
    assert "(sysworkq), stack 2048 B" in text(L.h_err())


def test_the_note_is_read_once_and_cleared(so):
    L = load(so)
    L.h_crash(0, K_ERR_STACK_CHK_FAIL, 0x10, 0x20, 2048, 1)
    assert L.h_magic() == L.h_magic_now()
    assert text(L.h_last_crash()) == "fatal:2@0x10"
    assert L.h_magic() == 0                       # the boot after this one reports nothing


def test_the_layout_changed_so_the_magic_did(so):
    L = load(so)
    assert L.h_magic_now() != OLD_MAGIC


def test_a_note_from_the_old_layout_reports_nothing(so):
    L = load(so)
    L.h_set_magic(OLD_MAGIC)
    assert L.h_last_crash() is None
    assert text(L.h_err()) == ""


@pytest.mark.parametrize("seed", range(40))
def test_random_power_on_ram_reports_nothing(so, seed):
    L = load(so)
    L.h_poison(seed)
    assert L.h_last_crash() is None
    assert text(L.h_err()) == ""
