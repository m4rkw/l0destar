"""Host tests for src/holdbreak.c: the rules for taking LTE away and giving it back.

The module is built as a shared library with the host compiler and driven
through ctypes, so the decision table runs without a modem or a board.

    uv run --python 3.12 --with pytest pytest -q firmware/tests
"""
import ctypes as C
import os
import subprocess
import tempfile

import pytest

SRC = os.environ.get("HB_SRC", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src"))
INT64_MAX = (1 << 63) - 1


class Cfg(C.Structure):
    _fields_ = [("probe_ms", C.c_int32), ("window_ms", C.c_int32), ("grace_ms", C.c_int32),
                ("cooldown_ms", C.c_int32), ("holdoff_ms", C.c_int32), ("min_kmh", C.c_int32),
                ("sticky", C.c_int32)]


class Now(C.Structure):
    _fields_ = [("now_ms", C.c_int64), ("speed_kmh", C.c_int32), ("rrc_up", C.c_bool),
                ("hold_ms", C.c_uint32), ("driving", C.c_bool), ("registered", C.c_bool),
                ("urgent", C.c_bool)]


class Watch(C.Structure):
    _fields_ = [("due_ms", C.c_int64), ("left", C.c_int32)]


class Hb(C.Structure):
    _fields_ = [("cut", C.c_bool), ("cut_ms", C.c_int64), ("off_until_ms", C.c_int64),
                ("reg_wait", C.c_bool), ("holdoff_until_ms", C.c_int64), ("sticky_left", C.c_int32),
                ("cycles", C.c_uint32), ("fails", C.c_uint32),
                ("off_ms_sum", C.c_uint32), ("reg_ms_sum", C.c_uint32), ("reg_ms_max", C.c_uint32)]


@pytest.fixture(scope="module")
def lib():
    out = os.path.join(tempfile.mkdtemp(), "libhb.so")
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC", "-o", out,
                    os.path.join(SRC, "holdbreak.c"), "-I", SRC], check=True)
    L = C.CDLL(out)
    L.hb_cut_wanted.argtypes = [C.POINTER(Hb), C.POINTER(Cfg), C.POINTER(Now), C.c_bool]
    L.hb_cut_wanted.restype = C.c_bool
    L.hb_cut.argtypes = [C.POINTER(Hb), C.POINTER(Cfg), C.c_int64, C.c_bool]
    L.hb_cut_failed.argtypes = [C.POINTER(Hb), C.POINTER(Cfg), C.c_int64, C.c_bool]
    L.hb_restore_due.argtypes = [C.POINTER(Hb), C.POINTER(Cfg), C.POINTER(Now), C.c_bool]
    L.hb_restore_due.restype = C.c_bool
    L.hb_restored.argtypes = [C.POINTER(Hb), C.c_int32, C.c_int32]
    L.hb_registered.argtypes = [C.POINTER(Hb), C.POINTER(Cfg), C.c_int64]
    L.hb_new_trip.argtypes = [C.POINTER(Hb)]
    L.hb_watch_due.argtypes = [C.POINTER(Watch), C.c_int64, C.c_int32, C.c_bool]
    L.hb_watch_due.restype = C.c_bool
    L.hb_crumb_format.argtypes = [C.c_char_p, C.c_size_t, C.c_char, C.c_uint32, C.c_uint32, C.c_uint32]
    L.hb_crumb_format.restype = C.c_size_t
    L.hb_rst_compose.argtypes = [C.c_char_p, C.c_size_t, C.c_char_p, C.c_char_p, C.c_char_p]
    L.hb_rst_compose.restype = C.c_size_t
    return L


@pytest.fixture
def cfg():
    return Cfg(probe_ms=5000, window_ms=20000, grace_ms=10000, cooldown_ms=600000, holdoff_ms=300000,
               min_kmh=10, sticky=4)


def now(**kw):
    d = dict(now_ms=100000, speed_kmh=60, rrc_up=True, hold_ms=6000, driving=True, registered=True,
             urgent=False)
    d.update(kw)
    return Now(**d)


def wanted(lib, hb, cfg, after_send=False, **kw):
    return lib.hb_cut_wanted(C.byref(hb), C.byref(cfg), C.byref(now(**kw)), after_send)


def due(lib, hb, cfg, send_due, **kw):
    return lib.hb_restore_due(C.byref(hb), C.byref(cfg), C.byref(now(**kw)), send_due)


def cut(lib, hb, cfg, at=0, after_send=False):
    lib.hb_cut(C.byref(hb), C.byref(cfg), at, after_send)


def restored(lib, hb, off, reg):
    lib.hb_restored(C.byref(hb), off, reg)


def registered(lib, hb, cfg, at):
    lib.hb_registered(C.byref(hb), C.byref(cfg), at)


# --- the probe ----------------------------------------------------------------
def test_probe_cuts_a_held_connection_on_the_move(lib, cfg):
    assert wanted(lib, Hb(), cfg)


@pytest.mark.parametrize("hold,expect", [(0, False), (2800, False), (4999, False), (5000, True),
                                         (347000, True)])
def test_probe_waits_for_the_hold_to_outlast_it(lib, cfg, hold, expect):
    assert wanted(lib, Hb(), cfg, hold_ms=hold) is expect


def test_probe_needs_a_connection_to_break(lib, cfg):
    assert not wanted(lib, Hb(), cfg, rrc_up=False, hold_ms=60000)


@pytest.mark.parametrize("kw", [dict(speed_kmh=9), dict(speed_kmh=0), dict(urgent=True), dict(driving=False),
                                dict(registered=False)])
def test_probe_does_not_cut_without_the_context(lib, cfg, kw):
    assert not wanted(lib, Hb(), cfg, **kw)


def test_no_second_cut_while_one_is_open(lib, cfg):
    hb = Hb()
    cut(lib, hb, cfg, 100000)
    assert not wanted(lib, hb, cfg)
    assert not wanted(lib, hb, cfg, after_send=True)


# --- sticky ---------------------------------------------------------------------
def test_after_send_needs_a_hold_seen_lately(lib, cfg):
    assert not wanted(lib, Hb(), cfg, after_send=True, hold_ms=100)


def test_after_send_needs_a_connection_but_not_an_old_one(lib, cfg):
    hb = Hb(sticky_left=3)
    assert not wanted(lib, hb, cfg, after_send=True, rrc_up=False, hold_ms=0)
    assert wanted(lib, hb, cfg, after_send=True, rrc_up=True, hold_ms=0)


def test_probe_cut_loads_the_sticky_count_and_each_sticky_cut_spends_one(lib, cfg):
    hb = Hb()
    cut(lib, hb, cfg, 0)
    assert hb.sticky_left == 4
    for left in (3, 2, 1, 0):
        restored(lib, hb, 20000, 3000)
        assert wanted(lib, hb, cfg, after_send=True, hold_ms=0)
        cut(lib, hb, cfg, 30000, after_send=True)
        assert hb.sticky_left == left
    restored(lib, hb, 20000, 3000)
    assert not wanted(lib, hb, cfg, after_send=True, hold_ms=0)           # back to waiting on the network
    assert not wanted(lib, hb, cfg, hold_ms=2800)                         # which lets go in 2.8 s: no cut
    assert wanted(lib, hb, cfg, hold_ms=5000)                             # and a hold is found again


def test_sticky_zero_probes_every_time(lib, cfg):
    cfg.sticky = 0
    hb = Hb()
    cut(lib, hb, cfg, 0)
    restored(lib, hb, 20000, 3000)
    assert not wanted(lib, hb, cfg, after_send=True, hold_ms=0)


# --- giving LTE back -------------------------------------------------------------
def test_nothing_to_restore_when_not_cut(lib, cfg):
    assert not due(lib, Hb(), cfg, True, urgent=True)


@pytest.mark.parametrize("t,send_due,expect", [
    (100000 + 19999, True, False), (100000 + 20000, True, True),
    (100000 + 20000, False, False), (100000 + 29999, False, False), (100000 + 30000, False, True)])
def test_window_ends_with_a_send_and_the_loop_check_allows_a_grace(lib, cfg, t, send_due, expect):
    hb = Hb()
    cut(lib, hb, cfg, 100000)
    assert due(lib, hb, cfg, send_due, now_ms=t) is expect


@pytest.mark.parametrize("kw", [dict(urgent=True), dict(driving=False), dict(speed_kmh=4), dict(speed_kmh=0)])
def test_anything_that_cannot_wait_ends_the_cut_at_once(lib, cfg, kw):
    hb = Hb()
    cut(lib, hb, cfg, 100000)
    assert due(lib, hb, cfg, False, now_ms=100001, **kw)


def test_slowing_to_a_crawl_is_not_a_stop(lib, cfg):
    hb = Hb()
    cut(lib, hb, cfg, 100000)
    assert not due(lib, hb, cfg, False, now_ms=100001, speed_kmh=5)       # half of 10: not yet
    assert due(lib, hb, cfg, False, now_ms=100001, speed_kmh=4)


def test_cut_and_restore_do_not_chatter_around_the_speed_limit(lib, cfg):
    hb = Hb()
    assert wanted(lib, hb, cfg, speed_kmh=10)
    cut(lib, hb, cfg, 0)
    for speed in (9, 8, 7, 6, 5):
        assert not due(lib, hb, cfg, False, now_ms=1000, speed_kmh=speed)


# --- failure ---------------------------------------------------------------------
def test_failed_restore_switches_the_feature_off_until_the_modem_registers(lib, cfg):
    hb = Hb()
    cut(lib, hb, cfg, 0)
    restored(lib, hb, 20000, -1)
    assert not hb.cut and hb.fails == 1 and hb.sticky_left == 0 and hb.cycles == 0
    assert hb.reg_wait and hb.off_until_ms == 0
    for t in (40000, 40000 + 600000, 40000 + 3600000, 10 ** 12):      # however long the network is away
        assert not wanted(lib, hb, cfg, now_ms=t)
    assert not wanted(lib, hb, cfg, after_send=True, hold_ms=0)


def test_registering_again_starts_the_holdoff_and_then_it_cuts_again(lib, cfg):
    hb = Hb()
    cut(lib, hb, cfg, 0)
    restored(lib, hb, 20000, -1)
    registered(lib, hb, cfg, 100000)
    assert not hb.reg_wait and hb.fails == 1
    assert not wanted(lib, hb, cfg, now_ms=100000)
    assert not wanted(lib, hb, cfg, now_ms=100000 + 299999)
    assert wanted(lib, hb, cfg, now_ms=100000 + 300000)


def test_registering_with_no_failed_restore_changes_nothing(lib, cfg):
    hb = Hb()
    registered(lib, hb, cfg, 100000)
    assert not hb.reg_wait and hb.holdoff_until_ms == 0 and wanted(lib, hb, cfg, now_ms=100001)
    cut(lib, hb, cfg, 0)
    restored(lib, hb, 20000, 3000)
    registered(lib, hb, cfg, 200000)
    assert hb.holdoff_until_ms == 0 and wanted(lib, hb, cfg, now_ms=200001)


def test_a_later_registration_does_not_extend_the_holdoff(lib, cfg):
    hb = Hb()
    cut(lib, hb, cfg, 0)
    restored(lib, hb, 20000, -1)
    registered(lib, hb, cfg, 100000)
    registered(lib, hb, cfg, 250000)                                  # lost it and got it back meanwhile
    assert hb.holdoff_until_ms == 100000 + 300000


def test_a_second_failure_waits_for_the_network_again(lib, cfg):
    hb = Hb()
    cut(lib, hb, cfg, 0)
    restored(lib, hb, 20000, -1)
    registered(lib, hb, cfg, 100000)
    cut(lib, hb, cfg, 500000)
    restored(lib, hb, 20000, -1)
    assert hb.reg_wait and hb.fails == 2
    assert not wanted(lib, hb, cfg, now_ms=10 ** 12)


def test_a_new_trip_lifts_the_wait_and_the_holdoff_and_keeps_the_count(lib, cfg):
    hb = Hb()
    cut(lib, hb, cfg, 0)
    restored(lib, hb, 20000, -1)
    lib.hb_new_trip(C.byref(hb))
    assert not hb.reg_wait and hb.fails == 1
    assert wanted(lib, hb, cfg, now_ms=50000)
    cut(lib, hb, cfg, 60000)
    restored(lib, hb, 20000, -1)
    registered(lib, hb, cfg, 100000)
    lib.hb_new_trip(C.byref(hb))
    assert hb.holdoff_until_ms == 0 and wanted(lib, hb, cfg, now_ms=100001)


def test_a_new_trip_does_not_lift_a_refusal(lib, cfg):
    hb = Hb()
    cut(lib, hb, cfg, 0)
    lib.hb_cut_failed(C.byref(hb), C.byref(cfg), 1000, True)
    lib.hb_new_trip(C.byref(hb))
    registered(lib, hb, cfg, 2000)
    assert not wanted(lib, hb, cfg, now_ms=10 ** 12)


def test_a_new_trip_after_a_good_one_changes_nothing(lib, cfg):
    hb = Hb()
    cut(lib, hb, cfg, 0)
    restored(lib, hb, 20000, 3000)
    lib.hb_new_trip(C.byref(hb))
    assert (hb.cycles, hb.fails, hb.reg_wait) == (1, 0, False)
    assert wanted(lib, hb, cfg)


def test_failed_cut_command_backs_off_and_a_refusal_is_for_good(lib, cfg):
    hb = Hb()
    cut(lib, hb, cfg, 0)
    lib.hb_cut_failed(C.byref(hb), C.byref(cfg), 1000, False)
    assert not hb.cut and not wanted(lib, hb, cfg, now_ms=1000 + 599999) and wanted(lib, hb, cfg, now_ms=601000)
    lib.hb_cut_failed(C.byref(hb), C.byref(cfg), 700000, True)
    assert hb.off_until_ms == INT64_MAX
    assert not wanted(lib, hb, cfg, now_ms=10 ** 12)


def test_statistics_accumulate(lib, cfg):
    hb = Hb()
    for off, reg in ((20000, 3000), (21000, 5200), (22000, 2800)):
        cut(lib, hb, cfg, 0)
        restored(lib, hb, off, reg)
    assert (hb.cycles, hb.fails) == (3, 0)
    assert hb.off_ms_sum == 63000 and hb.reg_ms_sum == 11000 and hb.reg_ms_max == 5200


# --- what the next boot says about the breaker (the crumb and the rst= budget) ----
def crumb(lib, phase, cut, phase_at, alive, size=32):
    buf = C.create_string_buffer(size)
    n = lib.hb_crumb_format(buf, size, phase, cut, phase_at, alive)
    return n, buf.value.decode()


def rst(lib, cause, crumb_=None, stall=None, size=64):
    buf = C.create_string_buffer(size)
    n = lib.hb_rst_compose(buf, size, cause, crumb_, stall)
    return n, buf.value.decode()


STALL4 = b"stall:4aa02/3e254/49722/48a78"


def test_crumb_says_the_phase_and_how_long_after_the_cut_and_after_entering_it(lib):
    # last alive 98 s after the cut and 3 s after entering phase Q
    assert crumb(lib, b"Q", 1000, 96000, 99000) == (8, "hb:Q98/3")


def test_crumb_clamps_and_never_reads_a_negative_time(lib):
    assert crumb(lib, b"F", 0, 0, 20_000_000)[1] == "hb:F9999/999"
    assert crumb(lib, b"R", 5000, 9000, 4000)[1] == "hb:R0/0"           # alive before its own start


def test_no_phase_no_crumb(lib):
    assert crumb(lib, b"\0", 1, 2, 3) == (0, "")


def test_crumb_fits_its_buffer_or_says_nothing(lib):
    # "hb:Q98/0" is eight characters and wants a ninth byte for the terminator
    assert crumb(lib, b"Q", 0, 98000, 98000, size=8) == (0, "")
    assert crumb(lib, b"Q", 0, 98000, 98000, size=9) == (8, "hb:Q98/0")
    assert crumb(lib, b"Q", 0, 98000, 98000, size=0) == (0, "")
    assert crumb(lib, b"Q", 0, 0, 98000, size=9) == (0, "")             # "hb:Q98/98" needs ten


def test_the_car_s_plain_watchdog_reset_gets_the_whole_story(lib):
    n, s = rst(lib, b"wdt", b"hb:Q98/0", STALL4)
    assert s == "wdt+hb:Q98/0+stall:4aa02/3e254/49722/48a78" and n == len(s) == 42


@pytest.mark.parametrize("cause", [b"wdt", b"pin+sw+wdt+dbg", b"pin+sw+wdt+dbg+lockup", b"x" * 60])
def test_rst_never_exceeds_what_the_server_takes(lib, cause):
    # varchar(48) in strict mode: one character more and the first record after a boot is refused
    n, s = rst(lib, cause, b"hb:Q9999/999", STALL4)
    assert n == len(s) <= 47


def test_a_long_cause_keeps_the_crumb_and_loses_stall_addresses_first(lib):
    n, s = rst(lib, b"pin+sw+wdt+dbg", b"hb:Q98/0", STALL4)
    assert s == "pin+sw+wdt+dbg+hb:Q98/0+stall:4aa02/3e254/49722" and n == 47


def test_half_an_address_is_not_kept(lib):
    # room for 16 characters of "stall:4aa02/3e254" (17): the first address only
    n, s = rst(lib, b"pin+sw+wdt+dbg+lockup", b"hb:Q98/0", b"stall:4aa02/3e254")
    assert s == "pin+sw+wdt+dbg+lockup+hb:Q98/0+stall:4aa02"


def test_a_crumb_that_does_not_fit_is_dropped_whole_not_cut(lib):
    n, s = rst(lib, b"x" * 40, b"hb:Q98/0", None)                       # 40 + 1 + 8 > 47
    assert s == "x" * 40 and n == 40


def test_absent_parts_are_skipped(lib):
    assert rst(lib, b"wdt", None, None) == (3, "wdt")
    assert rst(lib, b"", b"hb:F1/1", None)[1] == "hb:F1/1"
    assert rst(lib, None, None, b"stall:4aa02/3e254")[1] == "stall:4aa02/3e254"
    assert rst(lib, None, None, None) == (0, "")


def test_a_small_buffer_is_respected(lib):
    n, s = rst(lib, b"wdt", b"hb:Q98/0", b"stall:4aa02", size=10)
    assert s == "wdt" and n == 3
    assert rst(lib, b"wdt", None, None, size=0) == (0, "")
    assert rst(lib, b"wdt", None, None, size=2) == (1, "w")


# --- the modem's account of a search that has not registered (hb_watch_due) --------
INTERVAL = 30000


def watch_due(lib, w, now_ms, key_on):
    return lib.hb_watch_due(C.byref(w), now_ms, INTERVAL, key_on)


def test_a_report_is_due_at_its_time_and_not_before_and_the_next_is_an_interval_later(lib):
    w = Watch(due_ms=81000, left=20)
    assert not watch_due(lib, w, 80999, False)
    assert (w.due_ms, w.left) == (81000, 20)
    assert watch_due(lib, w, 81000, False)
    assert (w.due_ms, w.left) == (111000, 19)


def test_with_the_key_on_nothing_is_reported_and_nothing_is_used_up(lib):
    w = Watch(due_ms=81000, left=20)
    assert not watch_due(lib, w, 81000, True)
    assert not watch_due(lib, w, 200000, True)
    assert w.left == 20                                                 # a drive does not spend the reports


def test_the_reports_a_drive_missed_do_not_all_come_at_once_when_the_key_goes_off(lib):
    # 10 minutes of driving with the loop asking every second, then parked
    w = Watch(due_ms=81000, left=20)
    for t in range(81000, 681000, 1000):
        assert not watch_due(lib, w, t, True)
    assert w.left == 20
    # parked: the first is at most an interval away, then one an interval apart, never the lot together
    got = [t for t in range(681000, 681000 + 3 * INTERVAL, 1000) if watch_due(lib, w, t, False)]
    assert len(got) == 3 and got[1] - got[0] == INTERVAL and got[2] - got[1] == INTERVAL
    assert got[0] - 680000 <= INTERVAL
    assert w.left == 17


def test_with_the_key_off_all_of_them_come_every_interval_and_then_stop(lib):
    w = Watch(due_ms=INTERVAL, left=20)
    got = [t for t in range(0, 20 * INTERVAL + 5 * INTERVAL, 1000) if watch_due(lib, w, t, False)]
    assert got == [INTERVAL * (i + 1) for i in range(20)]
    assert w.left == 0
    assert not watch_due(lib, w, 10 ** 9, False)


def test_a_watch_with_none_left_never_reports_and_is_left_alone(lib):
    w = Watch(due_ms=0, left=0)
    assert not watch_due(lib, w, 10 ** 6, False)
    assert not watch_due(lib, w, 10 ** 6, True)
    assert (w.due_ms, w.left) == (0, 0)


def test_the_key_going_on_again_mid_search_holds_them_back_again(lib):
    w = Watch(due_ms=INTERVAL, left=3)
    assert watch_due(lib, w, INTERVAL, False)                           # parked: one
    assert not watch_due(lib, w, 2 * INTERVAL, True)                    # key on: held
    assert w.left == 2
    assert not watch_due(lib, w, 2 * INTERVAL + 1000, False)            # key off again: after an interval
    assert watch_due(lib, w, 3 * INTERVAL, False)
    assert w.left == 1
