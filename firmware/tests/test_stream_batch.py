"""Host tests for src/stream_batch.h: when the drive stream batches its records,
and the rule that keeps the live view at least once a second.

    uv run --python 3.12 --with pytest pytest -q firmware/tests/test_stream_batch.py
"""
import ctypes as C
import os
import subprocess
import tempfile

import pytest

SRC = os.environ.get("GNSS_SRC", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src"))

WRAPPER = """
#include <string.h>
#include "stream_batch.h"
int batch_max(int interval_s, int configured) { return stream_batch_max(interval_s, configured); }
int flush(int count, int max, long long since, long long period, long long gap)
{ return stream_batch_flush(count, max, since, period, gap); }
int due(long long now, long long last, long long every) { return stream_due(now, last, every); }
static struct stream_period pe;
void pe_reset(void) { memset(&pe, 0, sizeof(pe)); }
void pe_note(long long now, long long p) { stream_period_note(&pe, now, p); }
long long pe_est(void) { return stream_period_est(&pe); }
"""

GAP = 900            # CONFIG_APP_DRIVE_STREAM_BATCH_GAP_MS
MAX = 4              # CONFIG_APP_DRIVE_STREAM_BATCH


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
    lib.flush.argtypes = [C.c_int, C.c_int, C.c_longlong, C.c_longlong, C.c_longlong]
    lib.due.argtypes = [C.c_longlong, C.c_longlong, C.c_longlong]
    lib.pe_note.argtypes = [C.c_longlong, C.c_longlong]
    lib.pe_est.restype = C.c_longlong
    return lib


# -- how many records a datagram may take --------------------------------------------------------

def test_the_stream_with_no_limit_batches_to_the_configured_number(lib):
    assert lib.batch_max(0, 4) == 4
    assert lib.batch_max(0, 3) == 3
    assert lib.batch_max(0, 2) == 2


def test_a_stream_under_an_interval_sends_each_record_alone(lib):
    for interval in (1, 2, 5, 29, 30, 900):
        assert lib.batch_max(interval, 4) == 1, interval


def test_a_batch_of_one_or_less_is_no_batching(lib):
    for configured in (1, 0, -3):
        assert lib.batch_max(0, configured) == 1


# -- when to send what is held -------------------------------------------------------------------

def test_a_full_datagram_goes_whatever_the_time(lib):
    assert lib.flush(4, 4, 0, 1, GAP) == 1
    assert lib.flush(5, 4, 0, 1, GAP) == 1


def test_a_batch_with_room_and_time_waits_for_the_next_record(lib):
    assert lib.flush(1, 4, 200, 200, GAP) == 0
    assert lib.flush(2, 4, 400, 200, GAP) == 0
    assert lib.flush(3, 4, 600, 200, GAP) == 0          # 800 <= 900


def test_it_goes_when_the_next_record_would_come_too_late(lib):
    assert lib.flush(3, 4, 768, 256, GAP) == 1          # 1024 > 900
    assert lib.flush(1, 4, 600, 600, GAP) == 1          # a slow record is not held for the next


def test_a_record_that_arrives_exactly_on_the_limit_still_waits(lib):
    assert lib.flush(2, 4, 600, 300, GAP) == 0          # 900, not more
    assert lib.flush(2, 4, 601, 300, GAP) == 1


def test_an_unknown_period_counts_as_a_millisecond(lib):
    assert lib.flush(1, 4, 100, 0, GAP) == 0
    assert lib.flush(1, 4, 900, 0, GAP) == 1
    assert lib.flush(1, 4, 100, -5, GAP) == 0


def test_a_long_wait_since_the_last_send_goes_at_once(lib):
    # the first record of a stream, or after a stall
    assert lib.flush(1, 4, 60_000, 250, GAP) == 1


def _simulate(lib, period_ms, records=400, max_records=MAX, gap_ms=GAP):
    """Records finish every period_ms; returns the gaps between datagrams and
    how many records each carried."""
    now, last_send, held = 0, 0, 0
    gaps, sizes = [], []
    for _ in range(records):
        now += period_ms
        held += 1
        if lib.flush(held, max_records, now - last_send, period_ms, gap_ms):
            gaps.append(now - last_send)
            sizes.append(held)
            last_send, held = now, 0
    return gaps, sizes


@pytest.mark.parametrize('period', list(range(40, 1500, 13)) + [175, 250, 256, 300, 301, 450, 451, 900, 1000])
def test_the_live_view_never_moves_less_than_once_a_second_when_records_come_that_fast(lib, period):
    gaps, _ = _simulate(lib, period)
    if period <= 1000:
        assert max(gaps) <= 1000, (period, max(gaps))
    if period <= GAP:
        assert max(gaps) <= GAP, (period, max(gaps))
    else:
        assert set(gaps) == {period}                    # slower than the limit: every record alone


def test_the_batch_follows_the_record_rate(lib):
    # four from 4.4 records a second up, three to 3.3, two to 2.2, then each alone
    assert set(_simulate(lib, 175)[1]) == {4}           # 5.7 records a second (the last trip)
    assert set(_simulate(lib, 225)[1]) == {4}           # 4.4
    assert set(_simulate(lib, 226)[1]) == {3}
    assert set(_simulate(lib, 256)[1]) == {3}           # 3.9 (the month's average)
    assert set(_simulate(lib, 300)[1]) == {3}           # 3.3
    assert set(_simulate(lib, 301)[1]) == {2}
    assert set(_simulate(lib, 312)[1]) == {2}           # 3.2 (the 0.4.126 trips)
    assert set(_simulate(lib, 450)[1]) == {2}           # 2.2
    assert set(_simulate(lib, 451)[1]) == {1}
    assert set(_simulate(lib, 500)[1]) == {1}
    assert set(_simulate(lib, 667)[1]) == {1}
    assert set(_simulate(lib, 1000)[1]) == {1}          # 1 a second: the record goes as it is built


def test_no_datagram_carries_more_than_the_configured_number(lib):
    for period in (20, 50, 100, 175):
        for configured in (2, 3, 4):
            assert max(_simulate(lib, period, max_records=configured)[1]) <= configured


def test_a_tighter_gap_means_smaller_batches(lib):
    wide = max(_simulate(lib, 200, gap_ms=900)[1])
    tight = max(_simulate(lib, 200, gap_ms=500)[1])
    assert tight < wide


# -- guessing the next record's time ---------------------------------------------------------------

def test_a_slow_record_is_remembered_at_once(lib):
    lib.pe_reset()
    lib.pe_note(250, 216)
    lib.pe_note(900, 425)
    assert lib.pe_est() == 425


def test_nothing_is_known_at_first(lib):
    lib.pe_reset()
    assert lib.pe_est() == 0


def test_a_slow_record_is_forgotten_after_two_windows_not_before(lib):
    lib.pe_reset()
    lib.pe_note(100, 2400)                     # a stall
    now = 100
    for _ in range(30):
        now += 100
        lib.pe_note(now, 100)
        if now < 100 + 1000:
            assert lib.pe_est() == 2400, now   # still in the window it fell in, or the one after
    assert now == 3100 and lib.pe_est() == 100


def test_a_record_that_is_slow_once_a_second_is_always_remembered(lib):
    lib.pe_reset()
    now = 0
    for i in range(1, 400):
        period = 425 if i % 4 == 0 else 216
        now += period
        lib.pe_note(now, period)
        if now > 2500:
            assert lib.pe_est() == 425, (i, now)


def _simulate_with_slow_records(lib, base, slow_extra, every, records=600, gap_ms=GAP, max_records=MAX):
    """Every `every`-th record takes slow_extra longer (the periodic AT reads).
    Returns the gaps between datagrams and their sizes once the estimate has
    had time to learn."""
    lib.pe_reset()
    now, last_send, held, n = 0, 0, 0, 0
    gaps, sizes = [], []
    for _ in range(records):
        n += 1
        period = base + (slow_extra if n % every == 0 else 0)
        now += period
        lib.pe_note(now, period)
        held += 1
        if lib.flush(held, max_records, now - last_send, lib.pe_est(), gap_ms):
            gaps.append(now - last_send)
            sizes.append(held)
            last_send, held = now, 0
    return gaps[10:], sizes[10:]


@pytest.mark.parametrize('base', [100, 130, 150, 190, 216, 250, 300])
@pytest.mark.parametrize('slow', [100, 200, 300])
def test_a_record_that_is_sometimes_slow_does_not_stretch_the_gap_past_the_limit(lib, base, slow):
    gaps, _ = _simulate_with_slow_records(lib, base, slow, every=4)
    assert max(gaps) <= GAP, (base, slow, max(gaps))


@pytest.mark.parametrize('every', [2, 3, 5, 7, 10])
def test_whatever_the_rhythm_of_the_slow_record(lib, every):
    gaps, _ = _simulate_with_slow_records(lib, 150, 250, every=every)
    assert max(gaps) <= GAP


def test_the_bench_case_goes_two_or_three_at_a_time_and_under_the_limit(lib):
    # 216 ms records, every fourth 425: what the bench measured at 4.1 records a second
    gaps, sizes = _simulate_with_slow_records(lib, 216, 209, every=4)
    assert max(gaps) <= GAP
    assert set(sizes) <= {2, 3}


def test_a_stall_makes_the_next_second_or_two_unbatched_and_then_it_recovers(lib):
    lib.pe_reset()
    now, last_send, held = 0, 0, 0
    sizes = []
    for i in range(200):
        period = 2400 if i == 50 else 150
        now += period
        lib.pe_note(now, period)
        held += 1
        if lib.flush(held, MAX, now - last_send, lib.pe_est(), GAP):
            sizes.append((now, held))
            last_send, held = now, 0
    # the stall ends at 7500 + 2400 = 9900 ms
    after = [h for t, h in sizes if 9900 < t < 11000]         # the second after it: nothing waits
    later = [h for t, h in sizes if t > 13000]                # two windows on: four at a time again
    assert after and set(after) == {1} and set(later) == {4}


# -- pacing a reading --------------------------------------------------------------------------------

def test_a_reading_never_taken_is_due(lib):
    assert lib.due(1000, -(2 ** 62), 900) == 1


def test_a_reading_is_due_once_its_interval_has_passed(lib):
    assert lib.due(1899, 1000, 900) == 0
    assert lib.due(1900, 1000, 900) == 1
    assert lib.due(5000, 1000, 900) == 1


def test_an_interval_of_zero_is_every_time(lib):
    assert lib.due(1000, 1000, 0) == 1
    assert lib.due(1000, 1000, -1) == 1
    assert lib.due(1000, 5000, 0) == 1                  # whatever the clock did
    assert lib.due(1000, 5000, 900) == 0


def test_about_once_a_second_at_the_streams_usual_rates(lib):
    for period in (175, 256, 312):
        now, last, taken = 0, -(2 ** 62), []
        for _ in range(60):
            now += period
            if lib.due(now, last, 900):
                taken.append(now - last if last > 0 else 0)
                last = now
        steady = [t for t in taken[1:]]
        assert all(900 <= t < 900 + period for t in steady), (period, steady[:4])
