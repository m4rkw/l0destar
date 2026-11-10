"""The engine-off reporting interval schedule (tracker/interval_schedule.py).

No database needed.  Times are real Europe/London datetimes, so the BST and
GMT cases and the clock changes are the zone's own.
"""

import datetime
import zoneinfo

import pytest

from tracker import interval_schedule as sch

LONDON = zoneinfo.ZoneInfo('Europe/London')


def at(year, month, day, hour=0, minute=0, second=0):
    return datetime.datetime(year, month, day, hour, minute, second, tzinfo=LONDON)


# 2026-11-09 is a Monday, 2026-11-14 a Saturday.
MON = (2026, 11, 9)
FRI = (2026, 11, 13)
SAT = (2026, 11, 14)
SUN = (2026, 11, 15)

WEEKDAYS = [0, 1, 2, 3, 4]
WEEKEND = [5, 6]


def rule(days, start, end, seconds):
    return {'days': days, 'from': start, 'to': end, 'int': seconds}


def test_the_dates_are_the_days_the_tests_say():
    assert at(*MON).weekday() == 0
    assert at(*SAT).weekday() == 5
    assert at(*SUN).weekday() == 6


# -- which interval ------------------------------------------------------------

def test_no_schedule_is_the_plain_interval():
    for schedule in (None, []):
        assert sch.schedule_interval(schedule, 900, at(*MON, 12)) == 900
    assert sch.schedule_interval(None, None, at(*MON, 12)) == 0


def test_weekday_and_weekend():
    schedule = [rule(WEEKEND, '00:00', '00:00', 7200)]
    assert sch.schedule_interval(schedule, 900, at(*SAT, 12)) == 7200
    assert sch.schedule_interval(schedule, 900, at(*SUN, 20, 0)) == 7200
    # Sunday 23:59: Monday's 900 s begins in a minute, so the long sleep is cut.
    assert sch.schedule_interval(schedule, 900, at(*SUN, 23, 59)) == 60 + sch.SLACK_S
    assert sch.schedule_interval(schedule, 900, at(*MON, 12)) == 900
    assert sch.schedule_interval(schedule, 900, at(*FRI, 23, 59)) == 900


def test_a_daytime_rule_is_from_inclusive_to_exclusive():
    schedule = [rule(WEEKDAYS, '09:00', '17:00', 300)]
    assert sch.schedule_interval(schedule, 3600, at(*MON, 8, 59, 59)) == 10   # cut to the start
    assert sch.schedule_interval(schedule, 3600, at(*MON, 9, 0)) == 300
    assert sch.schedule_interval(schedule, 3600, at(*MON, 16, 59)) == 300
    assert sch.schedule_interval(schedule, 3600, at(*MON, 17, 0)) == 3600
    assert sch.schedule_interval(schedule, 3600, at(*SAT, 12)) == 3600


def test_an_overnight_rule_wraps_midnight_and_belongs_to_the_day_it_starts():
    schedule = [rule([4], '22:00', '06:00', 3600)]      # Friday night
    base = 900
    assert sch.schedule_interval(schedule, base, at(*FRI, 21, 0)) == base
    assert sch.schedule_interval(schedule, base, at(*FRI, 23, 0)) == 3600
    # Into Saturday morning: still Friday's night.
    assert sch.schedule_interval(schedule, base, at(*SAT, 1, 0)) == 3600
    assert sch.schedule_interval(schedule, base, at(*SAT, 6, 30)) == base
    # Saturday night is not Friday's.
    assert sch.schedule_interval(schedule, base, at(*SAT, 23, 0)) == base
    # Nor is the early hours of Friday, which belong to Thursday night.
    assert sch.schedule_interval(schedule, base, at(*FRI, 1, 0)) == base


def test_a_sunday_night_rule_wraps_into_monday():
    schedule = [rule([6], '22:00', '06:00', 3600)]
    assert sch.schedule_interval(schedule, 900, at(*MON, 3, 0)) == 3600
    assert sch.schedule_interval(schedule, 900, at(*SUN, 3, 0)) == 900


def test_from_equal_to_is_the_whole_day():
    schedule = [rule([2], '08:00', '08:00', 1800)]
    wednesday = (2026, 11, 11)
    for hour, minute in ((0, 0), (7, 59), (8, 0), (12, 0)):
        assert sch.schedule_interval(schedule, 900, at(*wednesday, hour, minute)) == 1800
    assert sch.schedule_interval(schedule, 900, at(2026, 11, 12, 12)) == 900


def test_the_first_matching_rule_wins():
    schedule = [rule(WEEKDAYS, '12:00', '13:00', 120),
                rule(WEEKDAYS, '00:00', '00:00', 3000)]
    assert sch.schedule_interval(schedule, 900, at(*MON, 12, 30)) == 120
    assert sch.schedule_interval(schedule, 900, at(*MON, 14, 0)) == 3000
    assert sch.schedule_interval(list(reversed(schedule)), 900, at(*MON, 12, 30)) == 3000


def test_no_matching_rule_is_the_plain_interval():
    schedule = [rule([0], '10:00', '11:00', 120)]
    assert sch.schedule_interval(schedule, 777, at(*MON, 12)) == 777


def test_an_interval_of_zero_means_no_timed_reports():
    schedule = [rule(WEEKDAYS, '00:00', '00:00', 0)]
    assert sch.schedule_interval(schedule, 900, at(*MON, 12)) == 0
    assert sch.schedule_interval(schedule, 900, at(*SAT, 12)) == 900


# -- boundaries ------------------------------------------------------------------

OVERNIGHT = [rule(list(range(7)), '22:00', '06:00', 3600)]   # base 900 otherwise


def test_a_long_sleep_is_cut_at_the_start_of_a_shorter_period():
    # 05:30 in the hour-long overnight period: the next period (06:00, 15 min)
    # is shorter and arrives before the hour is up -> wake at the boundary.
    assert sch.schedule_interval(OVERNIGHT, 900, at(*MON, 5, 30)) == 30 * 60 + sch.SLACK_S
    # With seconds: 05:59:30 -> 30 s to go.
    assert sch.schedule_interval(OVERNIGHT, 900, at(*MON, 5, 59, 30)) == 30 + sch.SLACK_S


def test_the_cut_is_never_below_ten_seconds_or_above_the_interval():
    assert sch.schedule_interval(OVERNIGHT, 900, at(*MON, 5, 59, 59)) == 10
    # 05:00:00 is exactly an hour away: not shorter than the hour, no cut.
    assert sch.schedule_interval(OVERNIGHT, 900, at(*MON, 5, 0)) == 3600
    assert sch.schedule_interval(OVERNIGHT, 900, at(*MON, 4, 59, 58)) <= 3600


def test_no_cut_when_the_next_period_is_longer():
    # 21:50 in the 15-minute daytime: the hour-long night is no reason to wake.
    assert sch.schedule_interval(OVERNIGHT, 900, at(*MON, 21, 50)) == 900


def test_no_cut_when_the_boundary_is_beyond_the_interval():
    assert sch.schedule_interval(OVERNIGHT, 900, at(*MON, 2, 0)) == 3600
    assert sch.schedule_interval(OVERNIGHT, 900, at(*MON, 4, 0)) == 3600


def test_a_zero_interval_period_is_woken_for_the_next_timed_one():
    schedule = [rule(list(range(7)), '22:00', '07:00', 0)]
    # Silent from 22:00; at 06:00 the unit is not due to wake by itself, so
    # it is woken for 07:00, when 900 s reports resume.
    assert sch.schedule_interval(schedule, 900, at(*MON, 6, 0)) == 3600 + sch.SLACK_S
    # Boundary into silence: nothing to cut for.
    assert sch.schedule_interval(schedule, 900, at(*MON, 21, 59, 0)) == 900


def test_a_zero_interval_period_with_nothing_timed_after_it_stays_zero():
    schedule = [rule(list(range(7)), '00:00', '00:00', 0)]
    assert sch.schedule_interval(schedule, 0, at(*MON, 12)) == 0


def test_a_weekend_boundary_cuts_friday_night():
    schedule = [rule([4], '23:00', '00:00', 3600), rule(WEEKEND, '00:00', '00:00', 300)]
    # Friday 23:30 is in the hour-long rule, Saturday 00:00 starts 300 s.
    assert sch.schedule_interval(schedule, 900, at(*FRI, 23, 30)) == 1800 + sch.SLACK_S


def test_a_boundary_across_midnight_between_days():
    schedule = [rule([0], '00:00', '00:00', 7200), rule([1], '00:00', '00:00', 600)]
    # Monday 23:50 (7200 s): Tuesday's 600 s starts in 10 minutes.
    assert sch.schedule_interval(schedule, 900, at(*MON, 23, 50)) == 600 + sch.SLACK_S


def test_a_shorter_period_after_a_longer_one_is_still_caught():
    schedule = [rule(list(range(7)), '10:00', '11:00', 7200),
                rule(list(range(7)), '11:00', '12:00', 300)]
    # At 10:10 on 7200 s: 11:00 (300 s) is 50 minutes away.
    assert sch.schedule_interval(schedule, 3600, at(*MON, 10, 10)) == 50 * 60 + sch.SLACK_S
    # At 09:30 on 3600 s: 10:00 (7200, longer) is no reason to wake, and 11:00
    # is beyond the hour.
    assert sch.schedule_interval(schedule, 3600, at(*MON, 9, 30)) == 3600


# -- clock changes -----------------------------------------------------------------

def test_the_clocks_going_forward():
    # 2026-03-29: 01:00 GMT -> 02:00 BST, so local 01:xx does not exist.
    schedule = [rule(list(range(7)), '00:00', '03:00', 3600)]   # night, base 900
    before = at(2026, 3, 29, 0, 30)
    assert before.utcoffset() == datetime.timedelta(0)
    # 30 minutes to the change; local 03:00 is 2 h of real time away, beyond
    # the hour, so no cut and no surprise.
    assert sch.schedule_interval(schedule, 900, before) == 3600
    # 02:30 BST: the period ends 30 real minutes later.
    late = at(2026, 3, 29, 2, 30)
    assert late.utcoffset() == datetime.timedelta(hours=1)
    assert sch.schedule_interval(schedule, 900, late) == 1800 + sch.SLACK_S


def test_the_clocks_going_back():
    # 2026-10-25: 02:00 BST -> 01:00 GMT, so local 01:xx happens twice.
    schedule = [rule(list(range(7)), '00:00', '03:00', 3600)]
    first = at(2026, 10, 25, 1, 30)
    assert first.utcoffset() == datetime.timedelta(hours=1)
    second = first.replace(fold=1)
    assert second.utcoffset() == datetime.timedelta(0)
    assert sch.schedule_interval(schedule, 900, first) == 3600
    assert sch.schedule_interval(schedule, 900, second) == 3600
    # 02:30 GMT: 30 real minutes to 03:00.
    assert sch.schedule_interval(schedule, 900, at(2026, 10, 25, 2, 30)) == 1800 + sch.SLACK_S
    # The boundary is measured in real time across the repeated hour: 01:00
    # BST is 02:00 BST-equivalent to the end of the BST hour; the period (to
    # 03:00 GMT) is 2.5 h away, no cut.
    assert sch.schedule_interval(schedule, 900, at(2026, 10, 25, 0, 10)) == 3600


def test_a_naive_time_is_taken_as_local_wall_time():
    schedule = [rule(WEEKDAYS, '09:00', '17:00', 300)]
    assert sch.schedule_interval(schedule, 3600, datetime.datetime(2026, 11, 9, 10)) == 300


# -- validation --------------------------------------------------------------------

GOOD = {'days': [1, 0], 'from': '22:00', 'to': '06:00', 'int': 3600}


def test_validate_normalises():
    assert sch.validate([GOOD]) == [{'days': [0, 1], 'from': '22:00',
                                     'to': '06:00', 'int': 3600}]
    assert sch.validate(None) is None
    assert sch.validate([]) is None
    assert sch.dumps(None) is None
    assert sch.loads(sch.dumps(sch.validate([GOOD]))) == sch.validate([GOOD])


@pytest.mark.parametrize('bad, fragment', [
    ('x', 'schedule must be'),
    ({'days': [0]}, 'schedule must be'),
    ([1], 'rule 1'),
    ([{**GOOD, 'extra': 1}], 'unknown key'),
    ([{'days': [0], 'from': '00:00', 'to': '01:00'}], 'missing key: int'),
    ([{**GOOD, 'days': []}], 'days'),
    ([{**GOOD, 'days': [7]}], 'days'),
    ([{**GOOD, 'days': [-1]}], 'days'),
    ([{**GOOD, 'days': [1, 1]}], 'days'),
    ([{**GOOD, 'days': [True]}], 'days'),
    ([{**GOOD, 'days': '01'}], 'days'),
    ([{**GOOD, 'from': '24:00'}], 'HH:MM'),
    ([{**GOOD, 'from': '9:00'}], 'HH:MM'),
    ([{**GOOD, 'to': '12:60'}], 'HH:MM'),
    ([{**GOOD, 'to': 600}], 'HH:MM'),
    ([{**GOOD, 'int': 5}], 'int must be'),
    ([{**GOOD, 'int': 9}], 'int must be'),
    ([{**GOOD, 'int': 86401}], 'int must be'),
    ([{**GOOD, 'int': -1}], 'int must be'),
    ([{**GOOD, 'int': '600'}], 'int must be'),
    ([{**GOOD, 'int': 60.5}], 'int must be'),
    ([{**GOOD, 'int': True}], 'int must be'),
    ([GOOD] * 9, 'most is 8'),
])
def test_validate_refuses(bad, fragment):
    with pytest.raises(ValueError) as caught:
        sch.validate(bad)
    assert fragment in str(caught.value)


def test_validate_accepts_the_limits():
    for seconds in (0, 10, 86400):
        assert sch.validate([{**GOOD, 'int': seconds}])
    assert len(sch.validate([GOOD] * 8)) == 8


def test_validate_refuses_an_overlong_schedule():
    wide = {'days': [0, 1, 2, 3, 4, 5, 6], 'from': '00:00', 'to': '00:00', 'int': 86400}
    assert len(sch.dumps([wide] * 8)) <= sch.MAX_JSON      # 8 widest rules fit
    with pytest.raises(ValueError):
        # Sixty-odd characters a rule is the most that can be: shrink the cap.
        old, sch.MAX_JSON = sch.MAX_JSON, 100
        try:
            sch.validate([wide] * 8)
        finally:
            sch.MAX_JSON = old


def test_invalid_stored_or_given_data_falls_back_to_the_plain_interval():
    now = at(*MON, 12)
    assert sch.schedule_interval('nonsense', 900, now) == 900
    assert sch.schedule_interval([{'days': [9]}], 900, now) == 900
    assert sch.loads('not json') is None
    assert sch.loads('[{"days":[9]}]') is None
    assert sch.loads('') is None and sch.loads(None) is None


def test_interval_now_reads_a_device_row():
    row = {'int': 900, 'interval_schedule': sch.dumps(sch.validate(OVERNIGHT))}
    assert sch.interval_now(row, at(*MON, 3)) == 3600
    assert sch.interval_now(row, at(*MON, 12)) == 900
    assert sch.interval_now({'int': 900}, at(*MON, 3)) == 900
    assert sch.interval_now({'int': None, 'interval_schedule': None}, at(*MON, 3)) == 0
