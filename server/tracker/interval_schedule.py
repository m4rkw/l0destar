"""The engine-off reporting interval schedule.

A device's engine-off (parked) reporting interval is ``device.int``, sent to
the unit as the interval field of every reply; the unit sleeps for the last
interval it was told.  A schedule lets that interval depend on the time of
day and the day of the week ("1 h overnight, 15 min otherwise, something else
at weekends") with no firmware change: every reply carries the interval that
applies at that moment.

A schedule is a list of up to 8 rules, stored as JSON in
``device.interval_schedule``::

    [{"days": [0,1,2,3,4], "from": "22:00", "to": "06:00", "int": 3600},
     {"days": [5,6], "from": "00:00", "to": "00:00", "int": 7200}]

* ``days``: weekdays the rule applies to, Monday 0 to Sunday 6.
* ``from``/``to``: server local time (Europe/London), ``HH:MM``, ``to``
  exclusive.  ``from`` later than ``to`` wraps midnight, and the day it
  applies to is the day it *starts*.  ``from`` equal to ``to`` is the whole day.
* ``int``: seconds, 0 (no timed reports) or 10 to 86400.

The first matching rule wins; no match means the plain ``device.int``.

Boundaries: a unit told "1 h" at 05:30 would sleep through a 06:00 change to
15 min.  When the next period has a shorter interval and begins before the
current interval would end, the interval sent is the time to that boundary
(plus a few seconds, so the wake lands inside the new period).  A current
interval of 0 ("no timed reports") counts as unbounded, so the unit is woken at
the start of the next period that has timed reports.
"""

import datetime
import json
import math
import re
import zoneinfo

TIMEZONE = 'Europe/London'
MAX_RULES = 8
MAX_JSON = 600
MIN_INTERVAL = 10
MAX_INTERVAL = 86400
SLACK_S = 5          # past a boundary, so the wake lands after it
LOOKAHEAD_S = 86400

_TIME_RE = re.compile(r'([01]\d|2[0-3]):([0-5]\d)')
_KEYS = {'days', 'from', 'to', 'int'}

HELP = ('a list of up to %d rules {"days": [0-6, Monday is 0], "from": "HH:MM", '
        '"to": "HH:MM", "int": 0 or %d-%d seconds}' % (MAX_RULES, MIN_INTERVAL,
                                                       MAX_INTERVAL))


def local_now():
    """The current time in the schedule's zone (aware)."""
    return datetime.datetime.now(zoneinfo.ZoneInfo(TIMEZONE))


def _minutes(text):
    m = _TIME_RE.fullmatch(text) if isinstance(text, str) else None
    if not m:
        raise ValueError('time must be HH:MM (00:00 to 23:59), not %r' % (text,))
    return int(m.group(1)) * 60 + int(m.group(2))


def _whole(value):
    return isinstance(value, int) and not isinstance(value, bool)


def validate(schedule):
    """The normalised form of ``schedule`` (days sorted, only the four keys),
    or None for no schedule (None or an empty list).  Raises ValueError with a
    message fit to show the user when it is not valid."""
    if schedule is None or schedule == []:
        return None
    if not isinstance(schedule, list):
        raise ValueError('schedule must be null or %s' % HELP)
    if len(schedule) > MAX_RULES:
        raise ValueError('schedule has %d rules, the most is %d'
                         % (len(schedule), MAX_RULES))
    rules = []
    for n, rule in enumerate(schedule, 1):
        where = 'rule %d: ' % n
        if not isinstance(rule, dict):
            raise ValueError(where + 'must be an object with days, from, to, int')
        extra = sorted(set(rule) - _KEYS)
        missing = sorted(_KEYS - set(rule))
        if extra:
            raise ValueError(where + 'unknown key: ' + ', '.join(map(str, extra)))
        if missing:
            raise ValueError(where + 'missing key: ' + ', '.join(missing))
        days = rule['days']
        if (not isinstance(days, list) or not days
                or not all(_whole(d) and 0 <= d <= 6 for d in days)
                or len(set(days)) != len(days)):
            raise ValueError(where + 'days must be a non-empty list of unique '
                             'numbers 0-6 (Monday is 0, Sunday 6)')
        try:
            _minutes(rule['from'])
            _minutes(rule['to'])
        except ValueError as e:
            raise ValueError(where + str(e))
        seconds = rule['int']
        if not _whole(seconds) or not (
                seconds == 0 or MIN_INTERVAL <= seconds <= MAX_INTERVAL):
            raise ValueError(where + 'int must be 0 or a whole number of seconds '
                             'from %d to %d' % (MIN_INTERVAL, MAX_INTERVAL))
        rules.append({'days': sorted(days), 'from': rule['from'],
                      'to': rule['to'], 'int': seconds})
    if len(dumps(rules)) > MAX_JSON:
        raise ValueError('schedule is too long (%d characters at most)' % MAX_JSON)
    return rules


def dumps(rules):
    """The stored text for a validated schedule, or None for none."""
    return json.dumps(rules, separators=(',', ':')) if rules else None


def loads(text):
    """The rules in a stored column value; None if there are none or the text
    is not a valid schedule (a reply must never fail on bad stored data)."""
    if not text:
        return None
    try:
        return validate(json.loads(text))
    except ValueError:
        return None


def _match(rules, weekday, minute):
    """The first rule in force at (weekday, minute of day), or None."""
    for rule in rules:
        start, end = _minutes(rule['from']), _minutes(rule['to'])
        if start == end:
            hit = weekday in rule['days']
        elif start < end:
            hit = weekday in rule['days'] and start <= minute < end
        else:
            hit = ((weekday in rule['days'] and minute >= start)
                   or ((weekday - 1) % 7 in rule['days'] and minute < end))
        if hit:
            return rule
    return None


def _at(rules, base_int, moment):
    rule = _match(rules, moment.weekday(), moment.hour * 60 + moment.minute)
    return base_int if rule is None else rule['int']


def schedule_interval(schedule, base_int, now_local):
    """The interval to send at ``now_local`` (an aware datetime in the local
    zone): the interval of the first rule in force, else ``base_int``,
    shortened when needed so that the unit wakes at the start of a period with
    a shorter one.  An invalid or empty schedule gives ``base_int``."""
    try:
        rules = validate(schedule)
    except ValueError:
        rules = None
    base_int = base_int or 0
    if not rules:
        return base_int
    current = _at(rules, base_int, now_local)

    # The period boundaries are on whole minutes: step through them in
    # absolute time (so a DST change moves the local clock, not the step).
    zone = now_local.tzinfo
    start = now_local.replace(second=0, microsecond=0)
    horizon = min(current, LOOKAHEAD_S) if current else LOOKAHEAD_S
    for k in range(1, horizon // 60 + 3):
        if zone is None:
            moment = start + datetime.timedelta(minutes=k)
        else:
            moment = (start.astimezone(datetime.timezone.utc)
                      + datetime.timedelta(minutes=k)).astimezone(zone)
        wait = (moment - now_local).total_seconds()
        if wait >= horizon:
            break
        after = _at(rules, base_int, moment)
        if after != 0 and (current == 0 or after < current):
            wake = max(MIN_INTERVAL, math.ceil(wait) + SLACK_S)
            return min(wake, current) if current else wake
    return current


def interval_now(device, now_local=None):
    """The interval to send ``device`` (a row) now: its schedule applied to
    its plain interval."""
    return schedule_interval(loads(device.get('interval_schedule')),
                             device.get('int') or 0, now_local or local_now())
