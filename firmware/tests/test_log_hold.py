"""Held log lines (CONFIG_APP_LOG_HOLD): the decisions in src/log_hold.h, and
that the send in src/data.c and intelligent power saving in src/main.c judge
the lines alike.  The store's side is in tests/test_dbglog.py.

    uv run --python 3.12 --with pytest pytest -q firmware/tests/test_log_hold.py
"""
import subprocess
from pathlib import Path

SRC = Path(__file__).parent.parent / "src"
DATA = (SRC / "data.c").read_text()
MAIN = (SRC / "main.c").read_text()


def _func(text, sig):
    start = text.index(sig + "\n{")
    return text[start:text.index("\n}\n", start) + 3]


def _run(tmp_path, code):
    c = tmp_path / "t.c"
    c.write_text(code)
    exe = tmp_path / "t"
    r = subprocess.run(["cc", "-Wall", "-Wno-unused-function", "-I", SRC, "-o", exe, c],
                       capture_output=True, text=True)
    assert r.returncode == 0 and r.stderr == "", r.stderr
    r = subprocess.run([exe], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr


def test_decisions(tmp_path):
    _run(tmp_path, r"""
#include <assert.h>
#include "log_hold.h"
int main(void)
{
    /* at or below the threshold is weak */
    assert(log_hold_weak(-116, true, -116, false, 0));
    assert(log_hold_weak(-116, true, -130, false, 0));
    assert(!log_hold_weak(-116, true, -115, false, 0));
    /* no reading: not held */
    assert(!log_hold_weak(-116, false, -130, false, -130));
    /* the parked reference stands in only without a reading of this moment */
    assert(log_hold_weak(-116, false, 0, true, -118));
    assert(!log_hold_weak(-116, false, 0, true, -100));
    assert(!log_hold_weak(-116, true, -100, true, -130));
    assert(log_hold_weak(-116, true, -120, true, -90));
    /* off */
    assert(!log_hold_weak(0, true, -140, true, -140));
    assert(!log_hold_weak(-89, true, -140, false, 0));
    assert(log_hold_weak(-90, true, -90, false, 0));
    /* the store: 75 % and the age */
    assert(log_hold_can_wait(3071, 4096, 0, 1000));
    assert(!log_hold_can_wait(3072, 4096, 0, 1000));
    assert(log_hold_can_wait(10, 4096, 999, 1000));
    assert(!log_hold_can_wait(10, 4096, 1000, 1000));
    assert(!log_hold_can_wait(0, 4096, 0, 0));
    return 0;
}
""")


HARNESS = r"""
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "log_hold.h"
#include "smart_power.h"

#define CONFIG_APP_LOG_HOLD_MAX_S 86400
#define SIGNAL_FRESH_MS 60000
#define LOG_INF(...) ((void)0)

static int64_t now_ms = 10000000;
static int64_t k_uptime_get(void) { return now_ms; }
static struct { bool signal_valid; int64_t signal_ms; int rsrp_dbm; } g_cell;

/* the store: bytes of errors and of everything, and whether it can still wait */
static size_t errors, total;
static bool full_or_old;
static size_t dbglog_pending(void) { return total; }
static size_t dbglog_due_weak(int64_t max_ms)
{
    assert(max_ms == 86400000);
    return full_or_old ? total : errors;
}

static int tun_log_hold_rsrp_dbm(void) { return -116; }
static bool tun_smart_power(void) { return true; }
static int tun_smart_power_rsrp_dbm(void) { return xsr; }
static int tun_smart_power_battery_mv(void) { return 12400; }
static int tun_smart_power_battery_drop_mv(void) { return 40; }
static int tun_smart_power_max_defer_s(void) { return 86400; }

static int alert_count, data_index;
static int alert_unanswered(void) { return 0; }
static bool data_alert_due(float v) { return false; }
static int databuf_count(void) { return 0; }
static int databuf_unacked(void) { return 0; }
static bool data_fields_waiting(void) { return false; }
static bool last_send_ok = true, power_reboot, s_move_needs_gps, s_fix_answer_due;
static int modem_unregistered_s(void) { return -1; }
static char pending_server_cmd[4];
static bool jobs_pending(void) { return false; }
static bool fota_check_requested(void) { return false; }
static bool fota_report_pending(void) { return false; }
static bool accel_ok = true;
static bool accel_available(void) { return accel_ok; }
static bool battery_on_backup(float v) { return false; }
static int telemetry_interval(void) { return 3600; }
static bool modem_is_registered(void) { return true; }
#define IS_ENABLED(x) x
#define CONFIG_APP_SMART_POWER_NCELL 0
#define LOG_DBG(...) ((void)0)
static bool drain_woken(void) { return false; }
static int modem_measure_rsrp(int *dbm, int ms, bool (*stop)(void)) { return -1; }
/* what AT%XMONITOR gives: 0 is a modem asleep with no reading */
static int xmon;
static int modem_read_signal(void)
{
    g_cell.signal_valid = false;
    if (xmon == 0) return -1;
    g_cell.signal_valid = true;
    g_cell.signal_ms = now_ms;
    g_cell.rsrp_dbm = xmon;
    return 0;
}
static int deferred;
static void data_report_deferred(void) { deferred++; }
static struct smart_power s_smart;

%s

/* A parked unit whose last report went an hour ago at `rsrp`. */
static void stay(int rsrp, size_t e, size_t t)
{
    smart_power_reset(&s_smart);
    smart_power_report_sent(&s_smart, now_ms - 3600000, rsrp, 12200);
    g_cell.signal_valid = false;
    errors = e; total = t; full_or_old = false; xmon = 0; accel_ok = true;
}

int main(void)
{
    /* held warnings do not force the weak report */
    stay(-118, 0, 200);
    assert(smart_pending(12.2f) == 0);
    assert(smart_defer(12.2f, true));
    /* an error does */
    stay(-118, 60, 200);
    assert(smart_pending(12.2f) == SMART_PENDING_LOG);
    assert(!smart_defer(12.2f, true));
    /* so does a store near full, or a line over the cap */
    stay(-118, 0, 200);
    full_or_old = true;
    assert(!smart_defer(12.2f, true));
    /* weak for power saving but not for the hold: the lines go, so it sends */
    xsr = -110;
    stay(-113, 0, 200);
    assert(!smart_defer(12.2f, true));
    stay(-113, 0, 0);
    assert(smart_defer(12.2f, true));
    xsr = -116;
    /* a reading on this wake overrides the reference, both ways */
    stay(-118, 0, 200);
    xmon = -100;
    assert(!smart_defer(12.2f, true));
    stay(-100, 0, 200);                     /* pending on the reference alone */
    xmon = -120;
    assert(smart_defer(12.2f, true));
    stay(-100, 60, 200);                    /* but an error still goes */
    xmon = -120;
    assert(!smart_defer(12.2f, true));
    /* moved, or no accelerometer: the reference says nothing */
    stay(-118, 0, 200);
    smart_power_moved(&s_smart);
    assert(smart_pending(12.2f) == SMART_PENDING_LOG);
    stay(-118, 0, 200);
    accel_ok = false;
    assert(smart_pending(12.2f) == SMART_PENDING_LOG);

    /* the send: the parked reference only while main.c gives it */
    g_cell.signal_valid = false;
    assert(!log_signal_weak(s_log_ref_valid, s_log_ref_dbm));
    data_log_signal_ref(true, -118);
    assert(log_signal_weak(s_log_ref_valid, s_log_ref_dbm));
    data_log_signal_ref(false, 0);
    assert(!log_signal_weak(s_log_ref_valid, s_log_ref_dbm));
    /* a stale reading is none */
    g_cell.signal_valid = true; g_cell.rsrp_dbm = -125;
    g_cell.signal_ms = now_ms - SIGNAL_FRESH_MS;
    assert(log_signal_weak(false, 0));
    g_cell.signal_ms--;
    assert(!log_signal_weak(false, 0));
    return 0;
}
"""


def test_power_saving_and_the_send_judge_the_lines_alike(tmp_path):
    block = DATA[DATA.index("/* -- held log lines"):DATA.index("/* -- send ---")]
    types = MAIN[MAIN.index("#define SMART_NCELL_TIMEOUT_MS"):
                 MAIN.index("/* What to do with this wake's report.")]
    funcs = "\n".join((
        _func(MAIN, "static bool parked_signal_known(void)"),
        _func(MAIN, "static uint32_t smart_pending(float v)"),
        types,
        _func(MAIN, "static enum smart_wake smart_defer(float v, bool timed_only)")))
    code = HARNESS.replace("%s", block + "\n" + funcs)
    code = code.replace("static int tun_log_hold", "static int xsr = -116;\nstatic int tun_log_hold")
    _run(tmp_path, code)


def test_the_parked_report_gives_and_takes_back_the_reference():
    body = MAIN[MAIN.index("static void do_sleep(void)\n{"):]
    give = body.index("data_log_signal_ref(parked_signal_known(), s_smart.rsrp_dbm);")
    forget = body.index("smart_power_report_start(&s_smart);")
    send = body.index("send_data();", forget)
    take = body.index("data_log_signal_ref(false, 0);")
    assert give < forget < send < take


def test_the_send_takes_by_the_signal():
    body = DATA[DATA.index("int send_data(void)\n{"):]
    assert "log_signal_weak(s_log_ref_valid, s_log_ref_dbm)" in body
    assert "dbglog_take_weak(" in body
