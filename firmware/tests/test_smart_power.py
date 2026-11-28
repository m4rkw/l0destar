"""Host tests for src/smart_power.h: when a parked unit puts off a timed
report, every condition that sends it instead, and the longest gap allowed.

    uv run --python 3.12 --with pytest pytest -q firmware/tests/test_smart_power.py
"""
import ctypes as C
import os
import subprocess
import tempfile
from pathlib import Path

import pytest

SRC = os.environ.get("GNSS_SRC", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src"))

WRAPPER = """
#include "smart_power.h"
static struct smart_power s;
void reset(void) { smart_power_reset(&s); }
void start(void) { smart_power_report_start(&s); }
void sent(long long now, int rsrp, int mv) { smart_power_report_sent(&s, now, rsrp, mv); }
int ref_batt(void) { return s.ref_batt_mv; }
void moved(void) { smart_power_moved(&s); }
void measured(int rsrp) { smart_power_measured(&s, rsrp); }
int hangs(const struct smart_power_in *in) { return smart_power_hangs_on_signal(&s, in); }
int decide(const struct smart_power_in *in) { return smart_power_decide(&s, in); }
int in_size(void) { return sizeof(struct smart_power_in); }
"""

(DEFER, OFF, NOT_TIMED, PENDING, NO_REFERENCE, MOVED, BATTERY, BATTERY_DROP,
 SIGNAL, CAP) = range(10)
HOUR = 3600 * 1000
DAY_S = 86400


class In(C.Structure):
    _fields_ = [("enabled", C.c_bool), ("timed_only", C.c_bool),
                ("pending", C.c_uint32), ("imu_ok", C.c_bool),
                ("on_backup", C.c_bool), ("batt_mv", C.c_int),
                ("fresh_valid", C.c_bool), ("fresh_rsrp_dbm", C.c_int),
                ("now_ms", C.c_int64), ("next_interval_s", C.c_int),
                ("rsrp_thr_dbm", C.c_int), ("batt_thr_mv", C.c_int),
                ("batt_drop_mv", C.c_int), ("max_defer_s", C.c_int)]


@pytest.fixture(scope="module")
def lib():
    d = tempfile.mkdtemp()
    c = os.path.join(d, "s.c")
    so = os.path.join(d, "libs.so")
    with open(c, "w") as f:
        f.write(WRAPPER)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC", "-o", so, c,
                    "-I", SRC], check=True)
    lib = C.CDLL(so)
    lib.sent.argtypes = [C.c_longlong, C.c_int, C.c_int]
    lib.decide.argtypes = [C.POINTER(In)]
    lib.hangs.argtypes = [C.POINTER(In)]
    assert lib.in_size() == C.sizeof(In)
    return lib


@pytest.fixture
def s(lib):
    """A parked stay with one report sent at t=0 on a weak cell."""
    lib.reset()
    lib.start()
    lib.sent(0, -120, 12200)
    return lib


def ask(lib, fn="decide", **kw):
    """Everything in favour of putting it off, an hour after the last report."""
    v = dict(enabled=True, timed_only=True, pending=0, imu_ok=True,
             on_backup=False, batt_mv=12200, fresh_valid=False,
             fresh_rsrp_dbm=0, now_ms=HOUR, next_interval_s=3600,
             rsrp_thr_dbm=-116, batt_thr_mv=12400, batt_drop_mv=40,
             max_defer_s=DAY_S)
    v.update(kw)
    return getattr(lib, fn)(C.byref(In(**v)))


def test_all_conditions_met_puts_it_off(s):
    assert ask(s) == DEFER


def test_feature_off(s):
    assert ask(s, enabled=False) == OFF


def test_only_the_interval_report(s):
    # owed, backup cut, supply restored
    assert ask(s, timed_only=False) == NOT_TIMED
    assert ask(s, next_interval_s=0) == NOT_TIMED


@pytest.mark.parametrize("bit", range(5), ids=["alert", "records", "log", "fields", "work"])
def test_anything_waiting_sends(s, bit):
    assert ask(s, pending=1 << bit) == PENDING


def test_nothing_sent_in_this_stay_sends(lib):
    lib.reset()
    assert ask(lib) == NO_REFERENCE


def test_a_report_that_did_not_go_leaves_no_reference(s):
    s.start()                       # built, but the send failed or had no signal
    assert ask(s) == NO_REFERENCE


def test_movement_sends_until_the_next_report(s):
    s.moved()
    assert ask(s) == MOVED
    assert ask(s, now_ms=2 * HOUR) == MOVED
    s.start()
    s.sent(2 * HOUR, -120, 12200)
    assert ask(s, now_ms=3 * HOUR) == DEFER


def test_movement_during_the_send_counts(lib):
    lib.reset()
    lib.start()
    lib.moved()
    lib.sent(0, -120, 12200)
    assert ask(lib) == MOVED


def test_new_stay_forgets_the_last(s):
    s.reset()                       # an ignition change, or the accel was off
    assert ask(s) == NO_REFERENCE


def test_no_accelerometer_sends(s):
    assert ask(s, imu_ok=False) == MOVED


@pytest.mark.parametrize("mv,want", [(12000, DEFER), (12400, DEFER), (12401, BATTERY),
                                     (13200, BATTERY), (0, BATTERY), (-1000, BATTERY)])
def test_battery(s, mv, want):
    assert ask(s, batt_mv=mv, batt_drop_mv=0) == want    # the threshold alone


def test_backup_power_sends(s):
    assert ask(s, on_backup=True, batt_mv=9100) == BATTERY


# -- the battery drop since the last report (sent at 12200 mV) ----------------

@pytest.mark.parametrize("mv,want", [(12161, DEFER), (12160, BATTERY_DROP),
                                     (11900, BATTERY_DROP)])
def test_battery_drop(s, mv, want):
    assert ask(s, batt_mv=mv) == want


def test_battery_rise_never_sends(s):
    for mv in (12200, 12201, 12400):
        assert ask(s, batt_mv=mv, batt_drop_mv=1) == DEFER


def test_battery_drop_zero_is_off(s):
    assert ask(s, batt_mv=11000, batt_drop_mv=0) == DEFER


@pytest.mark.parametrize("ref", [0, -1000])
def test_battery_drop_needs_a_reference(s, ref):
    s.start()
    s.sent(0, -120, ref)
    assert ask(s, batt_mv=11000) == DEFER
    assert ask(s, batt_mv=11000, batt_drop_mv=1) == DEFER


def test_battery_drop_counts_from_the_last_report(s):
    assert ask(s, batt_mv=12160) == BATTERY_DROP
    s.start()
    s.sent(HOUR, -120, 12160)
    assert ask(s, batt_mv=12160, now_ms=2 * HOUR) == DEFER
    assert ask(s, batt_mv=12120, now_ms=2 * HOUR) == BATTERY_DROP


def test_a_measured_reading_keeps_the_battery_reference(s):
    s.measured(-121)
    assert s.ref_batt() == 12200
    assert ask(s, batt_mv=12160) == BATTERY_DROP


def test_battery_checks_come_in_order(s):
    s.start()
    s.sent(0, -100, 12600)
    assert ask(s, batt_mv=12500) == BATTERY             # above the threshold first
    assert ask(s, batt_mv=12300) == BATTERY_DROP        # then the drop, before signal


@pytest.mark.parametrize("rsrp,want", [(-102, SIGNAL), (-115, SIGNAL), (-116, DEFER),
                                       (-128, DEFER)])
def test_signal_of_the_last_report(s, rsrp, want):
    s.start()
    s.sent(0, rsrp, 12200)
    assert ask(s) == want


def test_fresh_reading_replaces_the_stored_one(s):
    assert ask(s, fresh_valid=True, fresh_rsrp_dbm=-100) == SIGNAL
    s.start()
    s.sent(0, -100, 12200)
    assert ask(s, fresh_valid=True, fresh_rsrp_dbm=-120) == DEFER
    assert ask(s, fresh_valid=False, fresh_rsrp_dbm=-120) == SIGNAL


def test_never_longer_than_the_cap(s):
    # put off only while the next wake still falls inside the cap
    assert ask(s, now_ms=23 * HOUR) == DEFER
    assert ask(s, now_ms=23 * HOUR + 1) == CAP
    assert ask(s, now_ms=24 * HOUR) == CAP
    assert ask(s, now_ms=90 * HOUR) == CAP


@pytest.mark.parametrize("interval", [120, 600, 3600, 7000, 43200])
def test_gap_never_exceeds_the_cap_over_a_run(s, interval):
    t, deferred = 0, 0
    while True:
        t += interval * 1000 + 1500          # wakes drift a little late
        if ask(s, now_ms=t, next_interval_s=interval) != DEFER:
            break
        deferred += 1
    assert deferred > 0 or 2 * interval >= DAY_S
    assert t <= DAY_S * 1000 + 1500 * (deferred + 1)
    assert t - 1500 * (deferred + 1) <= DAY_S * 1000


def test_cap_counts_from_the_last_report_sent(s):
    s.start()
    s.sent(100 * HOUR, -120, 12200)
    assert ask(s, now_ms=123 * HOUR) == DEFER
    assert ask(s, now_ms=124 * HOUR) == CAP


@pytest.mark.parametrize("cap", [0, 1800, 3599])
def test_cap_under_the_interval_never_defers(s, cap):
    assert ask(s, max_defer_s=cap) == CAP


def test_interval_longer_than_a_day_never_defers(s):
    assert ask(s, next_interval_s=DAY_S + 1, now_ms=1000) == CAP


def test_shorter_cap(s):
    assert ask(s, max_defer_s=3 * 3600, now_ms=2 * HOUR) == DEFER
    assert ask(s, max_defer_s=3 * 3600, now_ms=3 * HOUR) == CAP


# -- the fresh reading (APP_SMART_POWER_NCELL) --------------------------------

def hangs(s, **kw):
    return bool(ask(s, "hangs", **kw))


def test_measures_only_where_the_stored_reading_alone_defers(s):
    assert hangs(s)
    # waiting log lines may be held at a weak reading: the caller's question
    assert hangs(s, pending=1 << 2)
    for kw in (dict(enabled=False), dict(timed_only=False), dict(imu_ok=False),
               dict(batt_mv=13000), dict(on_backup=True), dict(now_ms=24 * HOUR),
               dict(pending=1 << 0), dict(pending=1 << 1), dict(pending=1 << 3),
               dict(pending=1 << 4), dict(pending=(1 << 2) | 1)):
        assert not hangs(s, **kw), kw
    s.moved()
    assert not hangs(s)
    s.start()
    assert not hangs(s)                         # nothing sent
    s.sent(0, -100, 12200)
    assert not hangs(s)                         # good signal: it sends anyway


def test_a_measured_reading_stands_for_later_wakes(s):
    s.measured(-100)
    assert ask(s) == SIGNAL
    s.measured(-121)
    assert ask(s, now_ms=2 * HOUR) == DEFER
    assert ask(s, now_ms=24 * HOUR) == CAP      # still from the report


def test_a_measured_reading_is_no_reference_without_a_report(s):
    s.moved()
    s.measured(-100)
    assert ask(s) == MOVED
    s.start()
    s.measured(-120)
    assert ask(s) == NO_REFERENCE


MAIN = (Path(SRC) / "main.c").read_text()
REPORT, PUT_OFF, INTERRUPTED = range(3)

HARNESS = r"""
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "smart_power.h"

#define IS_ENABLED(x) x
#define CONFIG_APP_SMART_POWER_NCELL NCELL
#define LOG_INF(...) ((void)0)
#define LOG_DBG(...) ((void)0)
static bool tun_smart_power(void) { return true; }
static int tun_smart_power_rsrp_dbm(void) { return -116; }
static int tun_smart_power_battery_mv(void) { return 12400; }
static int tun_smart_power_battery_drop_mv(void) { return 40; }
static int tun_smart_power_max_defer_s(void) { return 86400; }
static bool accel_available(void) { return true; }
static bool battery_on_backup(float v) { return false; }
static int64_t k_uptime_get(void) { return 10000000; }
static int telemetry_interval(void) { return 3600; }
static void data_report_deferred(void) {}
static bool registered = true;
static bool modem_is_registered(void) { return registered; }
static struct { int rsrp_dbm; } g_cell;
static int xmon;                /* 0: asleep, no reading */
static int modem_read_signal(void)
{
    if (xmon == 0) return -ENODATA;
    g_cell.rsrp_dbm = xmon;
    return 0;
}
static int measures, measure_err, measure_dbm;
static bool drain_woken(void) { return false; }
static int modem_measure_rsrp(int *dbm, int ms, bool (*stop)(void))
{
    assert(ms == 5000 && stop == drain_woken);
    measures++;
    if (measure_err) return measure_err;
    *dbm = measure_dbm;
    return 0;
}
/* waiting warnings: due unless the reading is weak enough to hold them */
static bool warnings, errors;
static size_t data_log_due(bool valid, int dbm)
{
    return errors || (warnings && !(valid && dbm <= -116));
}
static struct smart_power s_smart;
static uint32_t smart_pending(float v)
{
    return data_log_due(true, s_smart.rsrp_dbm) ? SMART_PENDING_LOG : 0;
}

%s

static void stay(int rsrp)
{
    smart_power_reset(&s_smart);
    smart_power_report_sent(&s_smart, k_uptime_get() - 3600000, rsrp, 12200);
    registered = true; warnings = errors = false;
    xmon = 0; measures = 0; measure_err = 0; measure_dbm = -120;
}

int main(void)
{
#if NCELL
    /* a good reading sends, and is what later wakes go by */
    stay(-120);
    measure_dbm = -100;
    assert(smart_defer(12.2f, true) == SMART_WAKE_REPORT);
    assert(measures == 1 && s_smart.rsrp_dbm == -100);
    /* at the threshold is still weak */
    stay(-125);
    measure_dbm = -116;
    assert(smart_defer(12.2f, true) == SMART_WAKE_DEFER);
    assert(measures == 1 && s_smart.rsrp_dbm == -116);
    stay(-125);
    measure_dbm = -115;
    assert(smart_defer(12.2f, true) == SMART_WAKE_REPORT);
    /* a weak one defers */
    stay(-120);
    measure_dbm = -122;
    assert(smart_defer(12.2f, true) == SMART_WAKE_DEFER);
    assert(measures == 1 && s_smart.rsrp_dbm == -122);
    /* failed: the stored reading stands */
    const int fails[] = { -ETIMEDOUT, -EINPROGRESS, -ENODATA, -EBUSY, -EFAULT };
    for (int i = 0; i < 5; i++) {
        stay(-120);
        measure_err = fails[i];
        assert(smart_defer(12.2f, true) == SMART_WAKE_DEFER);
        assert(measures == 1 && s_smart.rsrp_dbm == -120);
    }
    /* another wake: neither sent nor deferred */
    stay(-120);
    measure_err = -ECANCELED;
    assert(smart_defer(12.2f, true) == SMART_WAKE_INTERRUPTED);
    assert(s_smart.rsrp_dbm == -120);
    /* warnings a weak reading holds do not stop it; a good one releases them */
    stay(-120);
    warnings = true;
    assert(smart_defer(12.2f, true) == SMART_WAKE_DEFER && measures == 1);
    stay(-120);
    warnings = true;
    measure_dbm = -100;
    assert(smart_defer(12.2f, true) == SMART_WAKE_REPORT && measures == 1);
#else
    stay(-120);
    measure_dbm = -100;
    assert(smart_defer(12.2f, true) == SMART_WAKE_DEFER && measures == 0);
#endif

    /* no measurement on a wake that sends anyway */
    stay(-100);                                 /* good stored signal */
    assert(smart_defer(12.2f, true) == SMART_WAKE_REPORT);
    stay(-120);
    assert(smart_defer(13.0f, true) == SMART_WAKE_REPORT);    /* battery */
    assert(smart_defer(12.2f, false) == SMART_WAKE_REPORT);   /* not timed */
    smart_power_moved(&s_smart);
    assert(smart_defer(12.2f, true) == SMART_WAKE_REPORT);
    stay(-120);
    assert(smart_defer(12.16f, true) == SMART_WAKE_REPORT);   /* battery drop */
    stay(-120);
    errors = true;
    assert(smart_defer(12.2f, true) == SMART_WAKE_REPORT);
    stay(-120);
    s_smart.sent_ms -= 23 * 3600000LL;          /* the cap */
    assert(smart_defer(12.2f, true) == SMART_WAKE_REPORT);
    smart_power_reset(&s_smart);
    assert(smart_defer(12.2f, true) == SMART_WAKE_REPORT);
    assert(measures == 0);
    /* nor where the modem answers by itself, or has no registration */
    stay(-120);
    xmon = -100;
    assert(smart_defer(12.2f, true) == SMART_WAKE_REPORT && measures == 0);
    assert(s_smart.rsrp_dbm == -100);
    stay(-120);
    registered = false;
    assert(smart_defer(12.2f, true) == SMART_WAKE_DEFER && measures == 0);
    return 0;
}
"""


@pytest.mark.parametrize("ncell", [0, 1], ids=["off", "on"])
def test_the_parked_wake_measures_only_to_change_the_answer(tmp_path, ncell):
    body = MAIN[MAIN.index("#define SMART_NCELL_TIMEOUT_MS"):]
    body = body[:body.index("\n}\n", body.index("static enum smart_wake smart_defer(")) + 3]
    c = tmp_path / "t.c"
    c.write_text(HARNESS.replace("%s", body))
    exe = tmp_path / "t"
    subprocess.run(["cc", "-std=c11", f"-DNCELL={ncell}", "-o", str(exe), str(c),
                    "-I", SRC], check=True)
    subprocess.run([str(exe)], check=True)


def test_an_interrupted_wake_keeps_the_report_and_the_wake():
    body = MAIN[MAIN.index("static void do_sleep(void)\n{"):]
    a = body.index("if (smart == SMART_WAKE_INTERRUPTED) {")
    block = body[a:body.index("continue;", a)]
    assert "telemetry_remaining = 1;" in block
    assert "accel_irq_enable" not in block and "atomic_clear" not in block


def test_the_sent_report_keeps_the_wakes_battery_reading():
    body = MAIN[MAIN.index("static void do_sleep(void)\n{"):]
    a = body.index("smart_power_report_sent(")
    call = " ".join(body[a:body.index(";", a)].split())
    assert call.endswith("g_cell.rsrp_dbm, (int)(v * 1000.0f + 0.5f))")
