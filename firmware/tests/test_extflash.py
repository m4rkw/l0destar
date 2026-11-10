"""Host tests for src/extflash.c, the MX25R6435F driver, against a model of the chip.

The real extflash.c and flashlog.c are compiled with the host compiler.  Their GPIO
calls land in a bit-level model of the part, written from the Macronix datasheet
(checked against Rev. 1.2): it clocks bits in on SCK's rising edge and out on its falling edge,
takes a command only at a byte boundary, honours write-enable, busy time, deep
power-down and the wake-up pulse, ANDs bits on program, wraps a page program inside
its page, and sets the program and erase fail flags.  Anything the part would
refuse, and anything the chip must never be sent, is recorded as a violation, and
every test ends by checking there were none:

  - a command other than the eight in extflash.c (so no status or security
    register write, no chip or block erase: nothing that can lock or change the part)
  - a command while busy, before tRDP after a wake, or tDPDD after a power-down
  - program or erase without write-enable; a program over bytes that are not erased,
    or one that wraps in its page
  - SCK or SDI driven while the CAN controller is on the bus with its rail down

It cannot say the driver works on a real board (timings, pin levels, the bus's
analogue behaviour); it says the command sequences are what the datasheet asks for.

    uv run --python 3.12 --with pytest pytest -q firmware/tests/test_extflash.py
"""
import ctypes as C
import errno
import os
import re
import shutil
import subprocess
import tempfile

import pytest

SRC = os.environ.get("EF_SRC", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src"))

SECTOR = 4096
EBUSY, ENODEV, EACCES, EIO, ETIMEDOUT, EINVAL = 16, 19, 13, 5, 60, 22      # macOS errno
EAGAIN = errno.EAGAIN                                                       # the host's, as the harness is built on it
ERR_IO, ERR_DEAD = -2, -5

STUB_UTIL_H = r"""
#ifndef STUB_UTIL_H
#define STUB_UTIL_H
#define Z_IS_ENABLED1(m) Z_IS_ENABLED2(_XXXX##m)
#define _XXXX1 _YYYY,
#define Z_IS_ENABLED2(a) Z_IS_ENABLED3(a 1, 0)
#define Z_IS_ENABLED3(ignore, val, ...) val
#define IS_ENABLED(m) Z_IS_ENABLED1(m)
#define ARG_UNUSED(x) (void)(x)
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define BUILD_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif
"""

STUB_KERNEL_H = r"""
#ifndef STUB_KERNEL_H
#define STUB_KERNEL_H
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <zephyr/sys/util.h>
int64_t k_uptime_get(void);
int32_t k_msleep(int32_t ms);
void k_busy_wait(uint32_t us);
#endif
"""

STUB_GPIO_H = r"""
#ifndef STUB_GPIO_H
#define STUB_GPIO_H
#include <stdint.h>
#include <zephyr/device.h>
#define GPIO_INPUT        (1u << 16)
#define GPIO_OUTPUT       (1u << 17)
#define GPIO_OUTPUT_HIGH  (GPIO_OUTPUT | (1u << 18))
#define GPIO_OUTPUT_LOW   (GPIO_OUTPUT | (1u << 19))
int gpio_pin_configure(const struct device *d, int pin, uint32_t flags);
int gpio_pin_set_raw(const struct device *d, int pin, int v);
int gpio_pin_get_raw(const struct device *d, int pin);
#endif
"""

STUB_DEVICE_H = "#ifndef STUB_DEVICE_H\n#define STUB_DEVICE_H\nstruct device;\n#endif\n"

STUB_LOG_H = r"""
#ifndef STUB_LOG_H
#define STUB_LOG_H
void h_log(int level, const char *fmt, ...);
#define LOG_MODULE_REGISTER(...)
#define LOG_ERR(...) h_log(1, __VA_ARGS__)
#define LOG_WRN(...) h_log(2, __VA_ARGS__)
#define LOG_INF(...) h_log(3, __VA_ARGS__)
#define LOG_DBG(...) h_log(4, __VA_ARGS__)
#endif
"""

HARNESS = r"""
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include "extflash.h"
#include "hw_domain.h"
#include "hw_common.h"

const struct device *hw_gpio0 = (const struct device *)0x1;

#define P_SCK CONFIG_APP_PIN_FLASH_SCK
#define P_SDI CONFIG_APP_PIN_FLASH_SDI
#define P_SDO CONFIG_APP_PIN_FLASH_SDO
#define P_CS  CONFIG_APP_PIN_FLASH_CS

#define SR_WIP 0x01
#define SR_WEL 0x02
#define SCUR_P_FAIL 0x20
#define SCUR_E_FAIL 0x40
#define MEM_BYTES (8u * 1024 * 1024)

/* ---- clock, log, violations ------------------------------------------------ */
static int64_t now_us;
int64_t k_uptime_get(void) { return now_us / 1000; }
int32_t k_msleep(int32_t ms) { now_us += (int64_t)ms * 1000; return 0; }
void k_busy_wait(uint32_t us) { now_us += us; }

static char logbuf[40][160];
static int  loglvl[40];
static int  nlog;
void h_log(int level, const char *fmt, ...)
{
    va_list ap;
    if (nlog >= 40) return;
    va_start(ap, fmt);
    vsnprintf(logbuf[nlog], sizeof(logbuf[0]), fmt, ap);
    va_end(ap);
    loglvl[nlog++] = level;
}
int m_log_n(void) { return nlog; }
int m_log_level(int i) { return loglvl[i]; }
const char *m_log(int i) { return logbuf[i]; }

static char viol[64][140];
static int  nviol;
static void violate(const char *fmt, ...)
{
    va_list ap;
    if (nviol < 64) {
        va_start(ap, fmt);
        vsnprintf(viol[nviol], sizeof(viol[0]), fmt, ap);
        va_end(ap);
    }
    nviol++;
}
int m_viol_n(void) { return nviol; }
const char *m_viol(int i) { return viol[i]; }

/* ---- the board: what hw_domain says, and what is really on the bus ----------- */
static int rail_on;                  /* PP3V3_CAN */
static int controller_on_bus;        /* an MCP2518FD is physically on SCK/SDI/SDO */
static hw_domain_notify_t notify_cb;

bool hw_domain_is_on(enum hw_domain d) { return d == hw_can_domain() && rail_on; }
void hw_domain_notify_set(enum hw_domain d, hw_domain_notify_t cb)
{
    if (d != hw_can_domain()) return;
    notify_cb = cb;
    if (cb && rail_on) cb(true);
}
void m_set_rail(int on) { rail_on = on; }
void m_rail_edge(int on) { rail_on = on; if (notify_cb) notify_cb(on != 0); }
int  m_notify_registered(void) { return notify_cb != NULL; }

/* ---- the chip ----------------------------------------------------------------- */
static uint8_t  mem[MEM_BYTES];
static uint32_t erases[2048];
static uint32_t cmd_counts[256];
static uint8_t  scur;
static int      wel, bp;             /* bp: status register bits 5..2, in place */
static int      dp;                  /* deep power-down */
static int64_t  dp_us, wake_us, busy_until;
static unsigned id[3] = { 0xC2, 0x28, 0x17 };
static int      absent;              /* 0 present, 1 reads all ones, 2 all zeros */
static int      prog_budget = -1, progs_ok;
static uint64_t fail_erase;          /* sectors < 64 whose erase fails */
static int64_t  dur_pp = 3200, dur_se = 58000;
static int      dp_ignored_busy;

/* ---- the bus ------------------------------------------------------------------- */
static int pin_sck, pin_sdi, pin_cs = 1, sdo = 1;
static int in_txn, clk_edges, nbytes, ignore, cmd_cur;
static int out_pending, out_active, out_mode, out_idx, out_bit;
static uint8_t  shreg, cur_out, bytes[300];
static uint32_t rd_addr;
static int      pin_events;

static int wip(void) { return now_us < busy_until; }
static void tick(void) { if (busy_until && now_us >= busy_until) { busy_until = 0; wel = 0; } }
static uint8_t sr_value(void) { return (uint8_t)(bp | (wel ? SR_WEL : 0) | (wip() ? SR_WIP : 0)); }

static void load_out_byte(void)
{
    if (absent) { cur_out = absent == 1 ? 0xFF : 0x00; return; }
    switch (out_mode) {
    case 1: cur_out = sr_value(); break;
    case 2: cur_out = scur; break;
    case 3: cur_out = (uint8_t)id[out_idx % 3]; break;
    case 4: cur_out = mem[(rd_addr + (uint32_t)out_idx) % MEM_BYTES]; break;
    default: cur_out = 0xFF; break;
    }
}

static void begin_cmd(uint8_t c)
{
    cmd_counts[c]++;
    if (dp) { violate("command %02x while in deep power-down", c); ignore = 1; return; }
    if (wake_us) {
        if (now_us - wake_us < 35) violate("command %02x only %lld us after the wake pulse, tRDP is 35", c, (long long)(now_us - wake_us));
        wake_us = 0;
    }
    tick();
    switch (c) {
    case 0x05: case 0x2B: out_mode = c == 0x05 ? 1 : 2; out_pending = 1; break;      /* allowed while busy */
    case 0x9F:
        if (wip()) { violate("RDID while busy"); ignore = 1; break; }
        out_mode = 3; out_pending = 1; break;
    case 0x03:
        if (wip()) { violate("READ while busy"); ignore = 1; break; }
        out_mode = 4; break;
    case 0x06:
        if (wip()) { violate("WREN while busy"); ignore = 1; }
        break;
    case 0x02: case 0x20:
        if (wip()) { violate("command %02x while busy", c); ignore = 1; }
        else if (!wel) { violate("command %02x without write enable", c); ignore = 1; }
        break;
    case 0xB9: break;
    default:
        violate("command %02x is not one this driver may send", c); ignore = 1; break;
    }
}

static uint32_t addr3(void) { return ((uint32_t)bytes[1] << 16) | ((uint32_t)bytes[2] << 8) | bytes[3]; }

static void on_byte(uint8_t b)
{
    if (nbytes < (int)sizeof(bytes)) bytes[nbytes] = b;
    if (nbytes == 0) begin_cmd(b);
    nbytes++;
    if (!ignore && cmd_cur == 0x03 && nbytes == 4) { rd_addr = addr3(); out_pending = 1; }
}

static void do_pp(void)
{
    int n = nbytes - 4;
    uint32_t a, page, off;
    if (n < 1) { violate("page program with no data"); return; }
    if (n > 256) violate("page program of %d bytes", n);
    if (bp) { scur |= SCUR_P_FAIL; wel = 0; return; }
    busy_until = now_us + dur_pp;
    if (prog_budget >= 0 && progs_ok >= prog_budget) { scur |= SCUR_P_FAIL; return; }
    a = addr3(); page = a & ~255u; off = a & 255u;
    if (off + (uint32_t)n > 256) violate("page program of %d bytes at offset %u wraps inside its page", n, off);
    for (int i = 0; i < n; i++) {
        uint32_t t = page + ((off + (uint32_t)i) & 255u);
        if (mem[t] != 0xFF) { violate("programmed over a byte that is not erased, at %u", t); break; }
    }
    for (int i = 0; i < n; i++) mem[page + ((off + (uint32_t)i) & 255u)] &= bytes[4 + i];
    scur &= (uint8_t)~SCUR_P_FAIL;
    progs_ok++;
}

static void do_se(void)
{
    uint32_t a, s;
    if (nbytes != 4) { violate("sector erase with %d bytes", nbytes); return; }
    a = addr3(); s = a >> 12;
    if (a & 4095u) violate("erase address %u is not at a sector start", a);
    if (bp) { scur |= SCUR_E_FAIL; wel = 0; return; }
    busy_until = now_us + dur_se;
    if (s < 64 && ((fail_erase >> s) & 1u)) { scur |= SCUR_E_FAIL; return; }
    memset(&mem[s << 12], 0xFF, 4096);
    erases[s]++;
    scur &= (uint8_t)~SCUR_E_FAIL;
}

static void cs_fall(void)
{
    tick();
    if (dp_us && now_us - dp_us < 30) violate("CS low %lld us after power-down, tDPDD is 30", (long long)(now_us - dp_us));
    if (controller_on_bus && !rail_on) violate("a transaction on a bus shared with the CAN controller, its rail down");
    in_txn = 1; clk_edges = 0; nbytes = 0; ignore = 0; cmd_cur = -1;
    out_pending = out_active = 0; out_mode = 0; shreg = 0;
}

static void cs_rise(void)
{
    if (!in_txn) return;
    in_txn = 0; out_active = out_pending = 0; sdo = 1;
    tick();
    if (clk_edges == 0) {                       /* a chip-select pulse with no clock */
        if (dp) { dp = 0; wake_us = now_us; dp_us = 0; }
        return;
    }
    if (ignore) return;
    if (clk_edges % 8) { violate("CS high after %d clocks, not on a byte boundary", clk_edges); return; }
    switch (bytes[0]) {
    case 0x06: wel = 1; break;
    case 0xB9:
        if (wip()) dp_ignored_busy++;
        else { dp = 1; dp_us = now_us; }
        break;
    case 0x02: do_pp(); break;
    case 0x20: do_se(); break;
    default: break;
    }
}

static void sck_rise(void)
{
    if (!in_txn) return;
    clk_edges++;
    if (out_active) return;                     /* the master is sampling SDO */
    shreg = (uint8_t)((shreg << 1) | (pin_sdi & 1));
    if (clk_edges % 8 == 0) {
        on_byte(shreg);
        cmd_cur = bytes[0];
    }
}

static void sck_fall(void)
{
    if (!in_txn) return;
    if (out_pending) {
        out_pending = 0; out_active = 1; out_idx = 0; out_bit = 7;
        load_out_byte();
        sdo = (cur_out >> 7) & 1;
    } else if (out_active) {
        if (--out_bit < 0) { out_bit = 7; out_idx++; load_out_byte(); }
        sdo = (cur_out >> out_bit) & 1;
    }
}

static void drive(int pin, int v)
{
    int old;
    if (pin == P_CS) {
        old = pin_cs; pin_cs = v;
        if (old && !v) cs_fall(); else if (!old && v) cs_rise();
    } else if (pin == P_SCK) {
        if (controller_on_bus && !rail_on) violate("SCK driven while the CAN rail is down");
        old = pin_sck; pin_sck = v;
        if (!old && v) sck_rise(); else if (old && !v) sck_fall();
    } else if (pin == P_SDI) {
        if (controller_on_bus && !rail_on) violate("SDI driven while the CAN rail is down");
        pin_sdi = v;
    }
}

int gpio_pin_configure(const struct device *d, int pin, uint32_t flags)
{
    (void)d;
    if (pin == P_SCK || pin == P_SDI || pin == P_SDO || pin == P_CS) pin_events++;
    if (flags & GPIO_OUTPUT) drive(pin, (flags & (1u << 18)) ? 1 : 0);
    return 0;
}
int gpio_pin_set_raw(const struct device *d, int pin, int v)
{
    (void)d;
    if (pin == P_SCK || pin == P_SDI || pin == P_SDO || pin == P_CS) pin_events++;
    drive(pin, v);
    return 0;
}
int gpio_pin_get_raw(const struct device *d, int pin)
{
    (void)d;
    if (pin == P_SDO) { if (absent) return absent == 1; return in_txn && out_active ? sdo : 1; }
    return 1;
}

/* ---- the test's side ----------------------------------------------------------- */
void m_reset(int rail, int controller)
{
    memset(mem, 0xFF, sizeof(mem));
    memset(erases, 0, sizeof(erases));
    memset(cmd_counts, 0, sizeof(cmd_counts));
    nviol = nlog = 0;
    now_us = 1000000; rail_on = rail; controller_on_bus = controller; notify_cb = NULL;
    scur = 0; wel = 0; bp = 0; dp = 0; dp_us = wake_us = busy_until = 0;
    id[0] = 0xC2; id[1] = 0x28; id[2] = 0x17; absent = 0; prog_budget = -1; progs_ok = 0;
    fail_erase = 0; dur_pp = 3200; dur_se = 58000; dp_ignored_busy = 0;
    pin_sck = pin_sdi = 0; pin_cs = 1; sdo = 1; in_txn = 0; pin_events = 0;
}
void m_fault(int kind, long long v)
{
    switch (kind) {
    case 1: absent = (int)v; break;
    case 2: id[2] = (unsigned)v; break;
    case 8: id[0] = (unsigned)v; break;
    case 9: id[1] = (unsigned)v; break;
    case 3: bp = (int)v; break;
    case 4: prog_budget = (int)v; break;
    case 5: fail_erase = (uint64_t)v; break;
    case 6: dur_pp = v; break;
    case 7: dur_se = v; break;
    }
}
uint32_t m_erases(int s) { return erases[s]; }
uint32_t m_cmd(int c) { return cmd_counts[c & 255]; }
int  m_dp(void) { return dp; }
int  m_pin_events(void) { return pin_events; }
int  m_dp_ignored_busy(void) { return dp_ignored_busy; }
int64_t m_now_us(void) { return now_us; }
void m_poke(uint32_t a, uint8_t v) { mem[a] = v; }
uint8_t m_peek(uint32_t a) { return mem[a]; }

void m_init(void) { extflash_init(); }
int  m_begin(void) { return extflash_begin(); }
void m_end(void) { extflash_end(); }
void m_park(void) { extflash_park(); }
int  m_probe(uint32_t *sectors) { return extflash_probe(sectors); }
int  m_check(void) { return extflash_check(); }
int  m_boot_check(uint8_t *id) { return extflash_boot_check(id); }

int m_dev_read(uint32_t a, void *b, size_t n) { const struct flashlog_dev *d = extflash_dev(8); return d->read(d->ctx, a, b, n); }
int m_dev_prog(uint32_t a, const void *b, size_t n) { const struct flashlog_dev *d = extflash_dev(8); return d->prog(d->ctx, a, b, n); }
int m_dev_erase(uint32_t a) { const struct flashlog_dev *d = extflash_dev(8); return d->erase(d->ctx, a); }

static struct flashlog fl;
static uint16_t cnt[2048];
static struct flashlog_cur cur;
int m_log_init(uint32_t sectors, uint32_t max_cycles, uint32_t opens)
{
    struct flashlog_cfg c = { max_cycles, opens };
    return flashlog_init(&fl, extflash_dev(sectors), cnt, &c);
}
int m_append(const void *r, uint16_t n, uint32_t now) { return flashlog_append(&fl, r, n, now); }
uint32_t m_pending(void) { return flashlog_pending(&fl); }
void m_cursor(void) { flashlog_cursor(&fl, &cur); }
int  m_next(void *b, uint16_t cap) { return flashlog_next(&fl, &cur, b, cap); }
int  m_commit(void) { return flashlog_commit(&fl, &cur); }
uint32_t m_log_erases(void) { return fl.st.erases; }
uint32_t m_log_skipped(void) { return fl.st.skipped; }
int m_log_dead(void) { return fl.dead; }
"""

FLASH_PINS = ["-DCONFIG_APP_PIN_FLASH_SCK=21", "-DCONFIG_APP_PIN_FLASH_SDI=20",
              "-DCONFIG_APP_PIN_FLASH_SDO=22", "-DCONFIG_APP_PIN_FLASH_CS=26"]
COMMON = ["-std=gnu11", "-Wall", "-Wextra", "-Wno-unused-parameter", "-shared", "-fPIC", "-O1",
          "-DCONFIG_APP_LOG_LEVEL=3", "-DCONFIG_APP_BOARD_SPLIT_OBD_DOMAIN=1"]
VARIANTS = {
    # the flash on bus pins of its own
    "own": ["-DCONFIG_APP_PIN_CAN_SCK=17", "-DCONFIG_APP_PIN_CAN_SDI=16", "-DCONFIG_APP_PIN_CAN_SDO=18"],
    # v4.1.2: the flash on the CAN controller's SCK, SDI and SDO
    "shared": ["-DCONFIG_APP_PIN_CAN_SCK=21", "-DCONFIG_APP_PIN_CAN_SDI=20", "-DCONFIG_APP_PIN_CAN_SDO=22"],
    # only one of the three in common is enough to count as shared
    "partial": ["-DCONFIG_APP_PIN_CAN_SCK=21", "-DCONFIG_APP_PIN_CAN_SDI=16", "-DCONFIG_APP_PIN_CAN_SDO=18"],
    # no CAN controller fitted: the documented override
    "nocan": ["-DCONFIG_APP_PIN_CAN_SCK=-1", "-DCONFIG_APP_PIN_CAN_SDI=-1", "-DCONFIG_APP_PIN_CAN_SDO=-1"],
}


def build(variant, sectors=8, out=None):
    d = tempfile.mkdtemp()
    for rel, text in (("zephyr/sys/util.h", STUB_UTIL_H), ("zephyr/kernel.h", STUB_KERNEL_H),
                      ("zephyr/drivers/gpio.h", STUB_GPIO_H), ("zephyr/device.h", STUB_DEVICE_H),
                      ("zephyr/logging/log.h", STUB_LOG_H), ("h.c", HARNESS)):
        path = os.path.join(d, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as f:
            f.write(text)
    so = os.path.join(d, f"libef_{variant}.so")
    r = subprocess.run(["cc", *COMMON, f"-DCONFIG_APP_FLASH_BACKLOG_SECTORS={sectors}",
                        *FLASH_PINS, *VARIANTS[variant], "-o", so, "-I", d, "-I", SRC,
                        os.path.join(d, "h.c"), os.path.join(SRC, "extflash.c"),
                        os.path.join(SRC, "flashlog.c")], capture_output=True, text=True)
    if out is not None:
        out.append(r)
    else:
        assert r.returncode == 0, r.stderr
        assert r.stderr == "", r.stderr                  # no warnings
    return so


_built = {}
_copies = [0]


def load(variant, sectors=8):
    if (variant, sectors) not in _built:
        _built[(variant, sectors)] = build(variant, sectors)
    so = _built[(variant, sectors)]
    _copies[0] += 1                      # a fresh copy per test: the model is all statics
    fresh = os.path.join(os.path.dirname(so), f"c{_copies[0]}.so")
    shutil.copyfile(so, fresh)
    L = C.CDLL(fresh)
    L.m_viol.restype = C.c_char_p
    L.m_log.restype = C.c_char_p
    L.m_now_us.restype = C.c_int64
    L.m_fault.argtypes = [C.c_int, C.c_longlong]
    L.m_reset.argtypes = [C.c_int, C.c_int]
    L.m_set_rail.argtypes = [C.c_int]
    L.m_rail_edge.argtypes = [C.c_int]
    L.m_viol.argtypes = [C.c_int]
    L.m_log.argtypes = [C.c_int]
    L.m_erases.restype = C.c_uint32
    L.m_erases.argtypes = [C.c_int]
    L.m_cmd.restype = C.c_uint32
    L.m_cmd.argtypes = [C.c_int]
    L.m_probe.argtypes = [C.POINTER(C.c_uint32)]
    L.m_check.argtypes = []
    L.m_boot_check.argtypes = [C.c_void_p]
    L.m_dev_read.argtypes = [C.c_uint32, C.c_void_p, C.c_size_t]
    L.m_dev_prog.argtypes = [C.c_uint32, C.c_char_p, C.c_size_t]
    L.m_dev_erase.argtypes = [C.c_uint32]
    L.m_log_init.argtypes = [C.c_uint32, C.c_uint32, C.c_uint32]
    L.m_append.argtypes = [C.c_char_p, C.c_uint16, C.c_uint32]
    L.m_pending.restype = C.c_uint32
    L.m_next.argtypes = [C.c_void_p, C.c_uint16]
    L.m_poke.argtypes = [C.c_uint32, C.c_uint8]
    L.m_peek.argtypes = [C.c_uint32]
    L.m_peek.restype = C.c_uint8
    L.m_log_erases.restype = C.c_uint32
    L.m_log_skipped.restype = C.c_uint32
    return L


class Chip:
    def __init__(self, variant="own", rail=True, controller=False, sectors=8):
        self.L = load(variant, sectors)
        self.L.m_reset(1 if rail else 0, 1 if controller else 0)
        self.buf = C.create_string_buffer(2048)

    def violations(self):
        return [self.L.m_viol(i).decode() for i in range(min(self.L.m_viol_n(), 64))]

    def logs(self, level=None):
        return [self.L.m_log(i).decode() for i in range(self.L.m_log_n())
                if level is None or self.L.m_log_level(i) == level]

    # a session with the log open, as databuf.c does it
    def open_log(self, sectors=8, max_cycles=0, opens=0):
        assert self.L.m_begin() == 0
        n = C.c_uint32()
        assert self.L.m_probe(C.byref(n)) == 0 and n.value == sectors
        assert self.L.m_log_init(sectors, max_cycles, opens) == 0

    def append(self, rec, now=0):
        return self.L.m_append(rec, len(rec), now)

    def read_all(self):
        out = []
        self.L.m_cursor()
        while True:
            n = self.L.m_next(self.buf, 2048)
            if n == 0:
                break
            if n > 0:
                out.append(self.buf.raw[:n])
        assert self.L.m_commit() == 0
        return out

    def erases(self, n=8):
        return [self.L.m_erases(i) for i in range(n)]


def rec(i, size=300):
    body = b"%08d," % i
    return (body + bytes((i + k) % 251 for k in range(size - len(body))))[:size]


@pytest.fixture
def chip_factory():
    made = []

    def make(*a, **k):
        c = Chip(*a, **k)
        made.append(c)
        return c

    yield make
    for c in made:
        assert c.violations() == [], c.violations()


# --------------------------------------------------------------------------- the chip is found and used

def test_the_chip_is_identified_over_the_bus(chip_factory):
    c = chip_factory()
    assert c.L.m_begin() == 0
    n = C.c_uint32()
    assert c.L.m_probe(C.byref(n)) == 0 and n.value == 8
    assert c.L.m_cmd(0x9F) == 1
    c.L.m_end()


def test_the_backlog_may_use_the_first_2016_sectors_and_not_the_32_kept_for_log_lines(chip_factory):
    c = chip_factory(sectors=2016)
    assert c.L.m_begin() == 0
    n = C.c_uint32()
    assert c.L.m_probe(C.byref(n)) == 0 and n.value == 2016
    c.L.m_end()


def test_a_build_that_asks_for_more_than_2016_sectors_does_not_compile():
    out = []
    build("own", sectors=2017, out=out)
    assert out[0].returncode != 0 and "kept for log lines" in out[0].stderr


def test_the_chip_check_passes_for_the_right_chip(chip_factory):
    c = chip_factory()
    assert c.L.m_begin() == 0
    assert c.L.m_check() == 0
    c.L.m_end()


@pytest.mark.parametrize("fault", [(2, 0x16), (8, 0xEF), (9, 0x40), (1, 1), (1, 2)],
                         ids=["density", "maker", "type", "all ones", "all zeros"])
def test_the_chip_check_says_no_device_for_a_chip_that_answers_wrongly_or_not_at_all(chip_factory, fault):
    c = chip_factory()
    c.L.m_fault(*fault)
    assert c.L.m_begin() == 0
    assert c.L.m_check() == -ENODEV
    c.L.m_end()
    assert c.logs() == []                          # the caller says what it found


def test_the_chip_check_sends_only_the_id_read_and_writes_nothing(chip_factory):
    c = chip_factory()
    assert c.L.m_begin() == 0
    for _ in range(3):
        assert c.L.m_check() == 0
    assert c.L.m_cmd(0x9F) == 3
    assert {cmd for cmd in range(256) if c.L.m_cmd(cmd)} == {0x9F}
    assert sum(c.erases(2048)) == 0 and c.L.m_pin_events() > 0
    c.L.m_end()


def test_the_chip_check_works_right_after_a_session_starts_from_deep_power_down(chip_factory):
    c = chip_factory()
    c.L.m_begin()
    c.L.m_end()
    assert c.L.m_dp() == 1
    for _ in range(5):
        assert c.L.m_begin() == 0
        assert c.L.m_check() == 0                  # a command before tRDP would be a violation
        c.L.m_end()


def test_a_record_written_through_the_driver_reads_back_through_it(chip_factory):
    c = chip_factory()
    c.open_log()
    sent = [rec(i, 40 + (i * 61) % 800) for i in range(60)]        # across pages and sectors
    for r in sent:
        assert c.append(r) == 0
    assert c.read_all() == sent
    c.L.m_end()


def test_a_whole_run_of_laps_uses_only_the_eight_commands_and_erases_each_sector_once_per_lap(chip_factory):
    c = chip_factory()
    c.open_log()
    laps = 5
    for i in range(8 * 4 * laps):                 # four 1000-byte records to a sector
        assert c.append(rec(i, 1000)) == 0
        if i % 4 == 3:
            c.read_all()
    # the first lap found the sectors blank; every lap after erased each once
    assert c.erases() == [laps - 1] * 8
    assert c.L.m_log_erases() == sum(c.erases())
    seen = {cmd for cmd in range(256) if c.L.m_cmd(cmd)}
    assert seen <= {0x03, 0x02, 0x20, 0x06, 0x05, 0x2B, 0x9F}      # DP comes with the session's end
    c.L.m_end()
    assert {cmd for cmd in range(256) if c.L.m_cmd(cmd)} <= {0x03, 0x02, 0x20, 0x06, 0x05, 0x2B, 0x9F, 0xB9}
    for forbidden in (0x01, 0x2F, 0xB1, 0x60, 0xC7, 0x52, 0xD8, 0x31, 0x15):
        assert c.L.m_cmd(forbidden) == 0


def test_the_wear_the_model_counts_is_level_through_many_sessions_and_resets(chip_factory):
    c = chip_factory()
    for boot in range(60):                         # each boot: a session, a few records, a reset
        c.open_log()
        for i in range(25):
            assert c.append(rec(boot * 100 + i, 900)) == 0
        c.read_all()
        c.L.m_end()
    e = c.erases()
    assert max(e) - min(e) <= 1 and sum(e) > 0


def test_every_program_is_in_a_page_to_erased_bytes_even_at_the_worst_datasheet_times(chip_factory):
    c = chip_factory()
    c.L.m_fault(6, 10_000)                        # page program 10 ms, the datasheet maximum
    c.L.m_fault(7, 240_000)                       # sector erase 240 ms, the maximum
    c.open_log()
    for i in range(8 * 4 * 3):
        assert c.append(rec(i, 1000)) == 0
        if i % 4 == 3:
            c.read_all()
    assert c.L.m_log_dead() == 0
    c.L.m_end()


# --------------------------------------------------------------------------- the chip is put to sleep

def test_a_session_ends_in_deep_power_down_and_the_next_one_wakes_it(chip_factory):
    c = chip_factory()
    c.L.m_begin()
    assert c.L.m_dp() == 0
    c.L.m_end()
    assert c.L.m_dp() == 1 and c.L.m_cmd(0xB9) == 1
    assert c.L.m_begin() == 0                      # the wake pulse, then tRDP
    n = C.c_uint32()
    assert c.L.m_probe(C.byref(n)) == 0            # a command only after the wait: no violation
    c.L.m_end()
    assert c.L.m_dp() == 1


def test_a_session_straight_after_the_last_waits_out_the_power_down_time(chip_factory):
    c = chip_factory()
    for _ in range(50):
        assert c.L.m_begin() == 0
        c.L.m_end()
    assert c.L.m_dp() == 1                         # the model flags a CS low inside tDPDD


def test_on_a_bus_of_its_own_the_chip_is_parked_by_init(chip_factory):
    c = chip_factory("own")
    c.L.m_init()
    assert c.L.m_dp() == 1
    assert c.L.m_notify_registered() == 0


def test_on_the_cans_bus_init_waits_for_the_rail_and_touches_nothing_until_then(chip_factory):
    c = chip_factory("shared", rail=False, controller=True)
    c.L.m_init()
    assert c.L.m_notify_registered() == 1
    assert c.L.m_pin_events() == 0 and c.L.m_dp() == 0           # not a pin moved
    c.L.m_rail_edge(1)                                           # CAN init raises the rail at boot
    assert c.L.m_dp() == 1                                       # parked in the window
    c.L.m_rail_edge(0)
    assert c.L.m_dp() == 1


def test_if_the_rail_is_already_up_when_init_runs_the_chip_is_parked_at_once(chip_factory):
    c = chip_factory("shared", rail=True, controller=True)
    c.L.m_init()
    assert c.L.m_dp() == 1


# --------------------------------------------------------------------------- the shared bus

@pytest.mark.parametrize("variant", ["shared", "partial"])
def test_a_session_on_the_cans_bus_with_its_rail_down_moves_no_pin_at_all(chip_factory, variant):
    c = chip_factory(variant, rail=False, controller=True)
    assert c.L.m_begin() == -EBUSY
    c.L.m_park()
    assert c.L.m_pin_events() == 0
    assert c.L.m_cmd(0xB9) == 0


def test_a_session_on_the_cans_bus_goes_ahead_while_its_rail_is_up(chip_factory):
    c = chip_factory("shared", rail=True, controller=True)
    c.open_log()
    for i in range(30):
        assert c.append(rec(i)) == 0
    assert len(c.read_all()) == 30
    c.L.m_end()


def test_the_model_does_catch_a_driver_that_drives_a_dead_rail(chip_factory):
    # The driver believes the bus is its own; the board has a controller on it.
    # This is what the guard prevents, and the model must say so.
    c = chip_factory("own", rail=False, controller=True)
    assert c.L.m_begin() == 0
    assert any("CAN rail" in v for v in c.violations())
    c.L.m_end()
    c.L.m_reset(0, 0)                                            # clear it: expected here


def test_with_no_controller_fitted_the_override_frees_the_bus(chip_factory):
    c = chip_factory("nocan", rail=False, controller=False)
    c.L.m_init()
    assert c.L.m_dp() == 1
    c.open_log()
    assert c.append(rec(1)) == 0
    c.L.m_end()


# --------------------------------------------------------------------------- the chip is asked who it is at boot

def boot_check(c):
    buf = C.create_string_buffer(3)
    err = c.L.m_boot_check(buf)
    return err, tuple(buf.raw[:3])


def commands_sent(c):
    return {cmd for cmd in range(256) if c.L.m_cmd(cmd)}


def test_before_init_nothing_has_been_asked(chip_factory):
    c = chip_factory()
    assert boot_check(c)[0] == -EAGAIN
    assert commands_sent(c) == set()


@pytest.mark.parametrize("variant", ["own", "nocan"])
def test_init_reads_the_id_once_and_parks_the_chip(chip_factory, variant):
    c = chip_factory(variant, rail=False, controller=False)
    c.L.m_init()
    assert boot_check(c) == (0, (0xC2, 0x28, 0x17))
    assert commands_sent(c) == {0x9F, 0xB9}               # the ID read and the deep power-down, nothing else
    assert c.L.m_cmd(0x9F) == 1 and c.L.m_dp() == 1
    assert c.logs() == []                                 # the caller says what it found


@pytest.mark.parametrize("fault, said", [
    ((1, 1), (0xFF, 0xFF, 0xFF)), ((1, 2), (0x00, 0x00, 0x00)),
    ((2, 0x16), (0xC2, 0x28, 0x16)), ((8, 0xEF), (0xEF, 0x28, 0x17)), ((9, 0x40), (0xC2, 0x40, 0x17))],
    ids=["no chip: all ones", "a line held low: all zeros", "wrong density", "another maker", "another type"])
def test_a_chip_that_answers_wrongly_or_not_at_all_is_reported_with_what_it_said(chip_factory, fault, said):
    c = chip_factory("own")
    c.L.m_fault(*fault)
    c.L.m_init()
    assert boot_check(c) == (-ENODEV, said)
    assert commands_sent(c) == {0x9F, 0xB9}               # still sent to sleep, and nothing written to it
    assert c.L.m_cmd(0xB9) == 1


def test_later_sessions_do_not_change_the_answer_the_boot_got(chip_factory):
    c = chip_factory("own")
    c.L.m_init()
    c.L.m_fault(1, 1)                                     # the chip goes away afterwards
    c.L.m_park()
    assert boot_check(c) == (0, (0xC2, 0x28, 0x17))       # what it said at boot
    assert c.L.m_cmd(0x9F) == 1                           # a park does not ask again
    assert c.L.m_begin() == 0
    assert c.L.m_check() == -ENODEV                       # the session check is what notices it
    c.L.m_end()


def test_a_chip_found_wrong_at_boot_is_not_quietly_forgiven_by_a_later_park(chip_factory):
    c = chip_factory("own")
    c.L.m_fault(1, 1)
    c.L.m_init()
    c.L.m_fault(1, 0)                                     # it answers next time
    c.L.m_park()
    assert boot_check(c) == (-ENODEV, (0xFF, 0xFF, 0xFF))  # the boot's answer is the boot's
    assert c.L.m_cmd(0x9F) == 1                           # the tier asks again itself, at its first spill


def test_on_the_cans_bus_the_id_is_read_when_the_rail_first_comes_up_and_not_again(chip_factory):
    c = chip_factory("shared", rail=False, controller=True)
    c.L.m_init()
    assert boot_check(c)[0] == -EAGAIN                    # the rail is down: nothing touched, nothing learnt
    assert c.L.m_pin_events() == 0 and commands_sent(c) == set()
    c.L.m_rail_edge(1)                                    # CAN init raises it at boot
    assert boot_check(c) == (0, (0xC2, 0x28, 0x17))
    assert c.L.m_cmd(0x9F) == 1 and c.L.m_cmd(0xB9) == 1
    c.L.m_rail_edge(0)
    c.L.m_rail_edge(1)                                    # a later CAN session: parked again, not asked again
    assert c.L.m_cmd(0x9F) == 1 and c.L.m_cmd(0xB9) == 2
    assert boot_check(c) == (0, (0xC2, 0x28, 0x17))


def test_if_the_rail_is_already_up_at_init_the_id_is_read_at_once(chip_factory):
    c = chip_factory("shared", rail=True, controller=True)
    c.L.m_init()
    assert boot_check(c) == (0, (0xC2, 0x28, 0x17))


def test_a_chip_that_is_wrong_on_the_cans_bus_is_reported_once_the_rail_has_been_up(chip_factory):
    c = chip_factory("shared", rail=False, controller=True)
    c.L.m_fault(1, 1)
    c.L.m_init()
    assert boot_check(c)[0] == -EAGAIN
    c.L.m_rail_edge(1)
    assert boot_check(c) == (-ENODEV, (0xFF, 0xFF, 0xFF))


# --------------------------------------------------------------------------- a chip that is not right

def test_another_chip_or_none_is_refused_before_anything_is_written(chip_factory):
    for fault in ((2, 0x16), (1, 1), (1, 2)):                     # wrong density, ones, zeros
        c = chip_factory()
        c.L.m_fault(*fault)
        assert c.L.m_begin() == 0
        n = C.c_uint32()
        assert c.L.m_probe(C.byref(n)) in (-ENODEV, -ETIMEDOUT)
        c.L.m_end()
        assert c.L.m_cmd(0x02) == 0 and c.L.m_cmd(0x20) == 0
        assert any("expected" in l for l in c.logs(1)) or fault[0] == 1


def test_block_protection_is_refused_up_front_not_discovered_one_failed_erase_at_a_time(chip_factory):
    c = chip_factory()
    c.L.m_fault(3, 0x04)                                          # BP0 set
    assert c.L.m_begin() == 0
    n = C.c_uint32()
    assert c.L.m_probe(C.byref(n)) == -EACCES
    c.L.m_end()
    assert c.L.m_cmd(0x20) == 0 and c.L.m_cmd(0x02) == 0


def test_a_protected_chip_that_is_written_anyway_fails_without_damage(chip_factory):
    c = chip_factory()
    assert c.L.m_begin() == 0
    c.L.m_fault(3, 0x04)
    assert c.L.m_dev_prog(0, b"abc", 3) == -EIO                   # the fail flag is read
    assert c.L.m_dev_erase(0) == -EIO
    assert c.L.m_peek(0) == 0xFF
    c.L.m_end()


# --------------------------------------------------------------------------- a chip that fails

def test_an_erase_that_never_finishes_costs_a_timeout_and_a_bounded_number_of_tries(chip_factory):
    c = chip_factory()
    c.open_log()
    for i in range(8 * 4):
        c.append(rec(i, 1000))
    c.read_all()                                                  # lap one done: opens now erase
    c.L.m_fault(7, 5_000_000)                                     # erases take 5 s: stuck
    t0 = c.L.m_now_us()
    results = [c.append(rec(100 + i, 1000)) for i in range(20)]
    assert c.L.m_log_dead() == 1 and ERR_DEAD in results
    assert c.L.m_cmd(0x20) <= 3                                   # never more than three erase commands
    assert (c.L.m_now_us() - t0) / 1e6 < 12                       # and a bounded time, not a loop
    c.violations()                                                # busy-time violations are expected here
    c.L.m_reset(1, 0)


def test_a_program_that_fails_is_noticed_and_does_not_become_an_erase_storm(chip_factory):
    c = chip_factory()
    c.open_log()
    for i in range(8 * 4):
        c.append(rec(i, 1000))
    c.read_all()
    c.L.m_fault(4, 0)                                             # no program succeeds again
    erases_before = c.L.m_log_erases()
    results = [c.append(rec(100 + i, 1000)) for i in range(30)]
    assert c.L.m_log_dead() == 1 and results.count(0) == 0
    assert c.L.m_log_erases() - erases_before <= 3
    c.L.m_end()


def test_a_sector_whose_erase_fails_is_skipped_and_the_rest_carry_on(chip_factory):
    c = chip_factory()
    c.open_log()
    for i in range(8 * 4):
        c.append(rec(i, 1000))
    c.read_all()
    c.L.m_fault(5, 1 << 3)                                        # sector 3 will not erase
    sent = [rec(200 + i, 1000) for i in range(24)]            # six sectors' worth: 0, 1, 2, then 3 fails
    for r in sent:
        assert c.append(r) == 0
    assert c.L.m_log_skipped() == 1
    assert c.read_all() == sent
    c.L.m_end()


# --------------------------------------------------------------------------- the device operations

def test_arguments_that_would_hurt_the_chip_are_refused_with_the_bus_untouched(chip_factory):
    c = chip_factory()
    assert c.L.m_begin() == 0
    before = c.L.m_pin_events()
    assert c.L.m_dev_prog(250, b"x" * 20, 20) == -EINVAL          # would wrap inside its page
    assert c.L.m_dev_prog(0, b"", 0) == -EINVAL
    assert c.L.m_dev_erase(100) == -EINVAL                        # not a sector start
    assert c.L.m_dev_erase(8 * 1024 * 1024) == -EINVAL            # off the end
    buf = C.create_string_buffer(8)
    assert c.L.m_dev_read(8 * 1024 * 1024 - 4, buf, 8) == -EINVAL
    assert c.L.m_pin_events() == before
    c.L.m_end()


def test_the_log_never_programs_a_page_across_its_end(chip_factory):
    c = chip_factory()
    c.open_log()
    sent = [rec(i, 5 + (i * 37) % 1000) for i in range(40)]      # sizes that straddle pages every way
    for r in sent:
        assert c.append(r) == 0
    assert c.read_all() == sent
    c.L.m_end()                                                   # a wrap would be a violation


# --------------------------------------------------------------------------- what the source may contain

def _source():
    return open(os.path.join(SRC, "extflash.c")).read()


def test_the_commands_in_the_source_are_exactly_the_eight_that_cannot_lock_or_change_the_part():
    src = _source()
    defined = dict(re.findall(r"#define\s+CMD_(\w+)\s+0x([0-9A-Fa-f]{2})\b", src))
    assert {k: int(v, 16) for k, v in defined.items()} == {
        "READ": 0x03, "PP": 0x02, "SE": 0x20, "WREN": 0x06,
        "RDSR": 0x05, "RDSCUR": 0x2B, "RDID": 0x9F, "DP": 0xB9}


def test_nothing_sends_a_byte_that_is_not_one_of_those_constants_or_an_address_or_data():
    src = _source()
    for m in re.finditer(r"spi_write_byte\(([^;]*)\);", src):
        arg = m.group(1).strip()
        assert not re.match(r"0x[0-9A-Fa-f]+$", arg), f"a raw opcode: {arg}"
    calls = [m.group(1).strip() for m in re.finditer(r"(?<!void )spi_write_cmd_addr\(([^,]+),", src)]
    assert calls and all(a.startswith("CMD_") for a in calls), calls
