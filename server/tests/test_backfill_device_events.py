"""tools/backfill_device_events.py: the events restored from the log table, the
app log and the current-state tables.

No database: the plan functions take rows and lines, and the run itself is
driven through a fake connection that keeps the unique key the table has.
"""

import datetime
import importlib.util
import os
import sys

import pytest

from tracker import device_events as ev
from tracker import telemetry

TOOLS = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                     'tools')


@pytest.fixture(scope='module')
def tool():
    """The tool loaded as a module; it imports its sibling, so tools/ is
    importable while it loads, as it is when the script runs."""
    sys.path.insert(0, TOOLS)
    try:
        spec = importlib.util.spec_from_file_location(
            'backfill_device_events', os.path.join(TOOLS, 'backfill_device_events.py'))
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
    finally:
        sys.path.remove(TOOLS)
    return module


def at(minute, second=0):
    return datetime.datetime(2026, 11, 4, 12, minute, second)


def gsm(minute):
    """What the log table holds for the unit's own clock, DATETIME(6)."""
    return datetime.datetime(2026, 11, 4, 11, minute, 7, 250000)


def row(n, minute, ignition=0, rst=None, fw=None):
    """A log row as the tool reads it: id, timestamp, gsm_timestamp,
    ignition_state, rst, fw."""
    return (n, at(minute), gsm(minute), ignition, rst, fw)


# -- the log table -----------------------------------------------------------------

def test_ignition_events_are_the_changes_between_rows(tool):
    events = list(tool.plan_log([row(1, 0, 0), row(2, 1, 0), row(3, 2, 1), row(4, 3, 1),
                                 row(5, 4, 0)]))
    assert events == [
        (at(2), 'ignition', 'Ignition on', 'ign:2026-11-04 11:02:07.250000'),
        (at(4), 'ignition', 'Ignition off', 'ign:2026-11-04 11:04:07.250000')]


def test_the_first_row_has_no_predecessor(tool):
    assert list(tool.plan_log([row(1, 0, 1)])) == []
    assert list(tool.plan_log([])) == []


def test_ignition_may_be_text_or_a_number(tool):
    events = list(tool.plan_log([row(1, 0, '0'), row(2, 1, '1'), row(3, 2, 1)]))
    assert [e[2] for e in events] == ['Ignition on']


def test_a_row_without_an_ignition_state_is_stepped_over(tool):
    events = list(tool.plan_log([row(1, 0, 0), row(2, 1, None), row(3, 2, 0), row(4, 3, 1)]))
    assert [e[2] for e in events] == ['Ignition on']


def test_a_reboot_is_each_row_with_a_reset_cause(tool):
    events = list(tool.plan_log([row(1, 0, rst='pin+sw'), row(2, 1), row(3, 2, rst=''),
                                 row(4, 3, rst='wdt+stall:4aa02/3e254')]))
    assert events == [
        (at(0), 'reboot', 'Rebooted (software)', 'rst:2026-11-04 11:00:07.250000'),
        (at(3), 'reboot', 'Rebooted (watchdog, stalled)', 'rst:2026-11-04 11:03:07.250000')]


def test_firmware_applied_is_a_new_version_after_a_known_one(tool):
    events = list(tool.plan_log([row(1, 0, fw='0.4.96'), row(2, 1, fw='0.4.96'),
                                 row(3, 2, fw='0.4.98'), row(4, 3, fw='0.4.98')]))
    assert events == [(at(2), 'firmware', 'Firmware 0.4.98 applied',
                       'fw:0.4.98:2026-11-04 11:02:07.250000')]


def test_the_first_version_has_no_predecessor_and_empty_ones_are_skipped(tool):
    events = list(tool.plan_log([row(1, 0, fw=None), row(2, 1, fw='0.4.96'),
                                 row(3, 2, fw=None), row(4, 3, fw=''),
                                 row(5, 4, fw='0.4.96')]))
    assert events == []


def test_going_back_to_an_older_version_is_restored(tool):
    events = list(tool.plan_log([row(1, 0, fw='0.4.104'), row(2, 1, fw='0.4.99'),
                                 row(3, 2, fw='0.4.104')]))
    assert [e[2] for e in events] == ['Firmware 0.4.99 restored', 'Firmware 0.4.104 applied']


def test_one_row_can_give_all_three(tool):
    events = list(tool.plan_log([row(1, 0, 0, fw='0.4.96'),
                                 row(2, 1, 1, rst='sw', fw='0.4.97')]))
    assert [e[1] for e in events] == ['ignition', 'reboot', 'firmware']
    assert {e[0] for e in events} == {at(1)}


def test_the_log_is_read_a_chunk_at_a_time_by_key(tool):
    rows = [row(n, n, n % 2) for n in range(1, 8)]

    class Cursor:
        def __init__(self):
            self.statements = []

        def execute(self, sql, params):
            self.statements.append((sql, params))
            device_id, *rest = params
            if len(rest) == 1:                      # the first chunk
                self.found = rows[:rest[0]]
            else:                                   # after the key (gsm, id)
                gsm_after, _, id_after, limit = rest
                after = [r for r in rows if (r[2], r[0]) > (gsm_after, id_after)]
                self.found = after[:limit]

        def fetchall(self):
            return self.found

    cursor = Cursor()
    assert list(tool.read_log(cursor, 5, chunk=3)) == rows
    first, second, third = cursor.statements
    assert 'IS NOT NULL' in first[0] and '`gsm_timestamp` >' in second[0]
    assert 'ORDER BY `gsm_timestamp`, `id`' in first[0]
    assert second[1] == (5, gsm(3), gsm(3), 3, 3)
    assert third[1] == (5, gsm(6), gsm(6), 6, 3)
    # A last chunk that is exactly full costs one more look, which finds nothing.
    rows[:] = rows[:6]
    cursor = Cursor()
    assert list(tool.read_log(cursor, 5, chunk=3)) == rows
    assert len(cursor.statements) == 3


class Recorder:
    """The handle the server's hooks write through; keeps what they insert."""

    def __init__(self):
        self.events = []

    def one(self, sql, params=None):
        return None

    def all(self, sql, params=None):
        return []

    def query(self, sql, params=None):
        if sql.startswith('INSERT INTO `device_event`'):
            device_id, when, kind, text, ref = params
            self.events.append((kind, text, ref))


def test_a_row_gives_the_same_events_the_server_gives_as_it_arrives(tool):
    """The key is what stops a row being recorded live and again here, so the
    live hooks and the plan must name an event identically."""
    live = Recorder()
    stamp_text = '2026-11-04 11:02:07.250000'
    previous = {'ignition_state': 0}
    entry = {'gsm_timestamp': stamp_text, 'ignition_state': 1, 'rst': 'pin+sw',
             'timestamp': '2026-11-04 12:02:00.000000', 'fw': '0.4.97'}
    telemetry._note_log_events(live, {'id': 1, 'imei': 'x'}, previous, entry, at(2))
    telemetry._note_versions(live, {'id': 1, 'imei': 'x', 'fw': '0.4.96'}, entry)

    planned = list(tool.plan_log([row(1, 1, 0, fw='0.4.96'),
                                  (2, at(2), gsm(2), 1, 'pin+sw', '0.4.97')]))
    assert sorted((kind, text, ref) for _, kind, text, ref in planned) == sorted(live.events)
    assert len(live.events) == 3


# -- the app log ---------------------------------------------------------------------

UNIT = '355025936386877'
OTHER = '355025936386844'
BEFORE = datetime.datetime(2026, 11, 5, 0, 0, 0)


def lines(*messages):
    return ['2026-11-04 13:05:43,326 - INFO - ' + m for m in messages]


def plan(tool, text, before=BEFORE, imeis=(UNIT,)):
    return tool.plan_app_log(text if isinstance(text, list) else [text], set(imeis), before)


def test_a_staged_line(tool):
    line = '2026-10-10 20:22:14,157 - INFO - %s[l0destar] staged 0.4.73' % UNIT
    assert plan(tool, line) == {UNIT: [(
        datetime.datetime(2026, 10, 10, 20, 22, 14, 157000), 'firmware',
        'Firmware 0.4.73 staged',
        'app:2026-10-10 20:22:14,157:Firmware 0.4.73 staged')]}


def test_a_delivery_is_one_event_per_command(tool):
    line = ('2026-11-04 19:30:59,675 - INFO - delivered 2 commands to %s[LK56AAF]: '
            'status,gnssstat' % UNIT)
    events = plan(tool, line)[UNIT]
    assert [(e[1], e[2]) for e in events] == [
        ('command', 'Command collected: status'), ('command', 'Command collected: gnssstat')]
    assert events[0][3] == 'app:2026-11-04 19:30:59,675:Command collected: status'
    assert events[1][3] == 'app:2026-11-04 19:30:59,675:Command collected: gnssstat'


def test_a_delivery_of_a_comma_joined_command(tool):
    line = '2026-11-04 13:05:43,326 - INFO - delivered 1 commands to %s[LK56AAF]: int=600,locate' % UNIT
    assert [e[2] for e in plan(tool, line)[UNIT]] == [
        'Command collected: int=600', 'Command collected: locate']


@pytest.mark.parametrize('message, kind, text', [
    ('fota: 0.4.72 -> 0.4.73 available, downloading', 'firmware', 'Firmware 0.4.73 downloading'),
    ('google: 51.50,-0.12', 'result', 'Location received'),
    ('tilt 14.2deg - possible tow/jack', 'other', 'Alert: tilt 14.2deg - possible tow/jack'),
    ('selftest: 1 failed', 'other', 'Alert: selftest: 1 failed'),
    ('SELFTEST:K 12V stuck', 'other', 'Alert: SELFTEST:K 12V stuck'),
    ('gps fix requested', 'other', 'Alert: gps fix requested'),
    ('impact 1.04g x=-22 y=23 z=1045 gyr=288.4dps dur=38ms spd=0.0',
     'other', 'Alert: impact 1.04g x=-22 y=23 z=1045 gyr=288.4dps dur=38ms spd=0.0'),
])
def test_an_alert_line(tool, message, kind, text):
    line = '2026-11-04 20:25:37,088 - INFO - alert from %s[LK56AAF] (pri=0): %s' % (UNIT, message)
    (event,) = plan(tool, line)[UNIT]
    assert event[1:3] == (kind, text)
    assert event[3] == ev.ref('app', '2026-11-04 20:25:37,088', text[:40])


def test_an_alert_the_staged_report_covers_adds_nothing(tool):
    line = '2026-10-10 20:22:14,164 - INFO - alert from %s[l0destar] (pri=0): fota: 0.4.72 -> 0.4.73, rebooting' % UNIT
    assert plan(tool, line) == {}


@pytest.mark.parametrize('word, on', [('ON', True), ('OFF', False), ('off', False)])
def test_the_backup_supply_alert_is_a_setting_event(tool, word, on):
    # live, the unit's bk= report records this; the log table does not hold it.
    line = '2026-11-04 14:08:42,532 - INFO - alert from %s[LK56AAF] (pri=0): gnss backup supply %s' % (UNIT, word)
    (event,) = plan(tool, line)[UNIT]
    assert event == (datetime.datetime(2026, 11, 4, 14, 8, 42, 532000), 'setting',
                     ev.backup_supply_text(on), 'bk:alert:2026-11-04 14:08:42,532')


def test_long_text_gives_a_key_from_its_first_forty_characters(tool):
    message = 'x' * 100
    line = '2026-11-04 14:08:42,532 - INFO - alert from %s (pri=1): %s' % (UNIT, message)
    (event,) = plan(tool, line)[UNIT]
    assert event[3] == 'app:2026-11-04 14:08:42,532:' + ('Alert: ' + message)[:40]
    assert len(event[3]) <= 96


def test_the_name_after_the_imei_is_not_needed(tool):
    for unit in (UNIT, UNIT + '[l0destar]', UNIT + '[LK56AAF]', UNIT + '[]'):
        line = '2026-11-04 13:05:43,326 - INFO - %s staged 0.4.74' % unit
        assert list(plan(tool, line)) == [UNIT], unit


def test_lines_about_other_things_are_passed_over(tool):
    assert plan(tool, [
        '2026-11-04 13:05:43,326 - INFO - 2 records from %s (203.0.113.7)' % UNIT,
        '2026-11-04 13:05:43,326 - ERROR - record error IMEI=%s: invalid gsm_timestamp format' % UNIT,
        '2026-11-04 13:05:43,326 - INFO - duplicate alert aid=5 from %s dropped' % UNIT,
        'Traceback (most recent call last):',
        '',
        '2026-11-04 13:05:43 - INFO - %s staged 0.4.74' % UNIT,      # no milliseconds
    ]) == {}


def test_only_the_units_asked_for_are_taken(tool):
    both = lines('%s[l0destar] staged 0.4.73' % UNIT, '%s[l0destar] staged 0.4.99' % OTHER)
    assert list(plan(tool, both)) == [UNIT]
    assert sorted(plan(tool, both, imeis=(UNIT, OTHER))) == sorted([UNIT, OTHER])
    assert plan(tool, both, imeis=()) == {}


def test_a_line_is_taken_only_if_older_than_before(tool):
    def staged(second, milli):
        return '2026-11-04 13:05:%02d,%03d - INFO - %s staged 0.4.74' % (second, milli, UNIT)

    before = datetime.datetime(2026, 11, 4, 13, 5, 43)
    assert plan(tool, staged(42, 999), before) != {}
    assert plan(tool, staged(43, 0), before) == {}          # not older than it
    assert plan(tool, staged(43, 326), before) == {}
    assert plan(tool, staged(44, 0), before) == {}


def test_events_come_back_per_unit_in_file_order(tool):
    text = lines('%s staged 0.4.73' % UNIT, '%s staged 0.4.99' % OTHER, '%s staged 0.4.74' % UNIT)
    found = plan(tool, text, imeis=(UNIT, OTHER))
    assert [e[2] for e in found[UNIT]] == ['Firmware 0.4.73 staged', 'Firmware 0.4.74 staged']
    assert [e[2] for e in found[OTHER]] == ['Firmware 0.4.99 staged']


# -- the current-state tables --------------------------------------------------------

def test_each_answer_and_the_scan_is_a_result(tool):
    reports = [('selftest', at(5)), ('status', at(6))]
    assert tool.plan_state(reports, at(7), BEFORE) == [
        (at(5), 'result', 'Self test result received', 'rep:selftest:2026-11-04 12:05:00.000000'),
        (at(6), 'result', 'Status result received', 'rep:status:2026-11-04 12:06:00.000000'),
        (at(7), 'result', 'Network scan result received', 'scan:2026-11-04 12:07:00.000000')]


def test_a_unit_that_never_answered_gives_nothing(tool):
    assert tool.plan_state([], None, BEFORE) == []
    assert tool.plan_state([('status', None)], None, BEFORE) == []


def test_answers_from_before_the_cut_off_only(tool):
    cut = at(6)
    assert tool.plan_state([('selftest', at(5)), ('status', at(6)), ('gnssstat', at(7))],
                           at(6), cut) == [
        (at(5), 'result', 'Self test result received', 'rep:selftest:2026-11-04 12:05:00.000000')]


def test_the_live_hook_and_the_plan_do_not_share_a_key_for_answers(tool):
    # The live hooks keep these unkeyed: the cut-off, not a key, is what stops
    # a result being counted twice.
    live = Recorder()
    ev.record(live, 1, 'result', ev.result_text('scan'))
    assert live.events == [('result', 'Network scan result received', None)]


# -- the summary ---------------------------------------------------------------------

def test_tally_counts_each_kind_in_the_order_the_app_lists_them(tool):
    events = [(at(n), kind, 'x', None) for n, kind in
              enumerate(['result', 'ignition', 'result', 'command', 'ignition', 'ignition'])]
    assert list(tool.tally(events).items()) == [('ignition', 3), ('command', 1), ('result', 2)]
    assert tool.tally([]) == {}


# -- running it ------------------------------------------------------------------------

class FakeCursor:
    """Answers the tool's queries from a few dicts, and keeps the unique key
    (device_id, ref) the real table has."""

    def __init__(self, world):
        self.world = world
        self.rowcount = 0
        self.statements = []

    def execute(self, sql, params=None):
        w = self.world
        self.statements.append((sql, params))
        self.found = []
        if sql.startswith('SHOW TABLES LIKE'):
            self.found = [(params[0],)] if params[0] in w['tables'] else []
        elif sql.startswith('SHOW COLUMNS FROM `device`'):
            self.found = [(c,) for c in w['device_columns']]
        elif sql.startswith('SELECT `id`, `imei`, `name` FROM `device`'):
            self.found = [(d, imei, name) for d, (imei, name) in sorted(w['devices'].items())]
        elif sql.startswith('SELECT 1 FROM `log`'):
            self.found = [(1,)] if w['log'].get(params[0]) else []
        elif sql.startswith('SELECT `id`, `net_scan_at`'):
            self.found = sorted(w['scans'].items())
        elif 'FROM `device_report`' in sql:
            self.found = w['reports']
        elif sql.startswith('SELECT `id`, `timestamp`, `gsm_timestamp`'):
            self.found = w['log'].get(params[0], [])
        else:
            raise AssertionError('unexpected query: ' + sql)

    def fetchall(self):
        return self.found

    def fetchone(self):
        return self.found[0] if self.found else None

    def executemany(self, sql, seq):
        assert sql.startswith('INSERT INTO `device_event`')
        assert 'ON DUPLICATE KEY UPDATE' in sql
        self.rowcount = 0
        for device_id, when, kind, text, ref in seq:
            self.world['batches'].append(len(seq))
            key = (device_id, ref)
            if ref is not None and key in self.world['keys']:
                continue
            self.world['keys'].add(key)
            self.world['inserted'].append((device_id, when, kind, text, ref))
            self.rowcount += 1


class FakeConnection:
    def __init__(self, **world):
        self.world = {'tables': {'device_event', 'device_report'},
                      'device_columns': ['id', 'imei', 'net_scan_at'],
                      'devices': {1: (UNIT, 'Car'), 2: (OTHER, 'Van')},
                      'log': {}, 'scans': {}, 'reports': [],
                      'keys': set(), 'inserted': [], 'batches': []}
        self.world.update(world)
        self.commits = 0
        self.closed = False

    def cursor(self):
        return FakeCursor(self.world)

    def commit(self):
        self.commits += 1

    def close(self):
        self.closed = True


LOG = [row(1, 0, 0), row(2, 1, 1, rst='sw'), row(3, 2, 1, fw='0.4.96'),
       row(4, 3, 0, fw='0.4.97')]


def app_log(tmp_path, *messages):
    path = tmp_path / 'app.log'
    path.write_text('\n'.join(lines(*messages)) + '\n')
    return str(path)


def run(tool, connection, apply=False, devices=None, app=None, before=BEFORE):
    return tool.backfill(connection, apply, devices, app or '/nonexistent/app.log', before)


def test_a_dry_run_shows_the_plan_and_writes_nothing(tool, tmp_path, capsys):
    connection = FakeConnection(log={1: LOG, 2: []},
                                scans={1: at(30)},
                                reports=[(1, 'status', 'ok', at(20))])
    path = app_log(tmp_path, '%s[LK56AAF] staged 0.4.97' % UNIT)
    assert run(tool, connection, app=path) == 0
    out = capsys.readouterr().out
    assert connection.world['inserted'] == [] and connection.commits == 0
    assert 'Car (%s): 7 events (firmware 2, ignition 2, reboot 1, result 2)' % UNIT in out
    assert 'Van' not in out                                  # no log rows, not asked for
    assert '2026-11-04 12:01:00  ignition Ignition on' in out
    assert '2026-11-04 12:03:00  firmware Firmware 0.4.97 applied' in out
    assert '2026-11-04 12:30:00  result   Network scan result received' in out
    assert '7 events found; nothing written; --apply adds them' in out


def test_a_dry_run_shows_the_first_and_last_three_of_each_kind(tool, capsys):
    rows = [row(n, n, n % 2) for n in range(1, 21)]                # 19 ignition changes
    assert run(tool, FakeConnection(log={1: rows})) == 0
    out = capsys.readouterr().out
    assert 'ignition 19' in out
    assert out.count(' ignition ') == 6 and '... 13 more' in out
    assert out.index('12:02:00') < out.index('... 13 more') < out.index('12:19:00')


def test_a_dry_run_works_before_the_table_exists(tool, capsys):
    connection = FakeConnection(log={1: LOG}, tables={'device_report'})
    assert run(tool, connection) == 0
    assert 'device_event does not exist yet' in capsys.readouterr().out


def test_apply_inserts_in_time_order_and_a_second_run_adds_nothing(tool, tmp_path, capsys):
    connection = FakeConnection(log={1: LOG}, scans={1: at(30)},
                                reports=[(1, 'status', 'ok', at(20))])
    path = app_log(tmp_path, 'delivered 1 commands to %s[LK56AAF]: gnssbk=0' % UNIT)
    assert run(tool, connection, True, app=path) == 0
    world = connection.world
    assert [r[3] for r in world['inserted']].count('Command collected: gnssbk=0') == 1
    assert len(world['inserted']) == 7
    # The app log line is from 13:05, the log rows and the answers earlier or later.
    times = [r[1] for r in world['inserted'] if r[0] == 1]
    assert times == sorted(times)
    assert all(r[0] == 1 for r in world['inserted'])
    assert connection.commits == 1
    assert '7 events found, 7 added' in capsys.readouterr().out

    assert run(tool, connection, True, app=path) == 0
    assert len(world['inserted']) == 7
    assert '7 events found, 0 added' in capsys.readouterr().out


def test_apply_inserts_in_batches(tool):
    start = datetime.datetime(2026, 11, 4, 0, 0, 0)
    rows = [(n, start + datetime.timedelta(seconds=n), start + datetime.timedelta(seconds=n),
             n % 2, None, None) for n in range(1, 1202)]            # 1200 changes
    connection = FakeConnection(log={1: rows})
    assert run(tool, connection, True) == 0
    assert len(connection.world['inserted']) == 1200
    sizes = []
    for size in connection.world['batches']:
        if not sizes or sizes[-1][0] != size:
            sizes.append([size, 0])
        sizes[-1][1] += 1
    assert sizes == [[500, 1000], [200, 200]]                      # 500, 500, 200 rows


def test_apply_without_the_table_says_what_to_run_first(tool, capsys):
    connection = FakeConnection(log={1: LOG}, tables=set())
    assert run(tool, connection, True) == 1
    assert 'run migrations/2026-11-06_device_event.sql first' in capsys.readouterr().out
    assert connection.world['inserted'] == []


def test_the_devices_asked_for_are_the_only_ones_filled(tool, capsys):
    connection = FakeConnection(log={1: LOG, 2: LOG})
    assert run(tool, connection, True, devices=[2]) == 0
    assert {r[0] for r in connection.world['inserted']} == {2}


def test_a_device_with_log_rows_but_no_name_is_still_shown(tool, capsys):
    connection = FakeConnection(devices={1: (UNIT, None)}, log={1: LOG})
    assert run(tool, connection) == 0
    assert '- (%s)' % UNIT in capsys.readouterr().out


def test_an_unknown_device_id_is_refused(tool, capsys):
    connection = FakeConnection(log={1: LOG})
    assert run(tool, connection, True, devices=[1, 9]) == 1
    assert 'no such device: 9' in capsys.readouterr().out
    assert connection.world['inserted'] == []


def test_every_device_with_log_rows_is_the_default(tool):
    connection = FakeConnection(log={1: LOG, 2: LOG})
    run(tool, connection, True)
    assert {r[0] for r in connection.world['inserted']} == {1, 2}


def test_a_missing_app_log_is_reported_and_skipped(tool, capsys):
    connection = FakeConnection(log={1: LOG})
    assert run(tool, connection, True, app='/nonexistent/app.log') == 0
    assert '/nonexistent/app.log does not exist' in capsys.readouterr().out
    assert len(connection.world['inserted']) == 4


def test_app_lines_after_before_are_not_added(tool, tmp_path):
    connection = FakeConnection(log={1: LOG})
    path = tmp_path / 'app.log'
    path.write_text('2026-11-04 13:00:00,000 - INFO - %s staged 0.4.97\n'
                    '2026-11-05 13:00:00,000 - INFO - %s staged 0.4.98\n' % (UNIT, UNIT))
    run(tool, connection, True, app=str(path))
    texts = [r[3] for r in connection.world['inserted']]
    assert 'Firmware 0.4.97 staged' in texts and 'Firmware 0.4.98 staged' not in texts


def test_keyed_events_do_not_depend_on_before(tool):
    early = datetime.datetime(2000, 1, 1)
    connection = FakeConnection(log={1: LOG})
    run(tool, connection, True, before=early)
    assert [r[2] for r in connection.world['inserted']] == [
        'ignition', 'reboot', 'ignition', 'firmware']


def test_an_event_the_server_recorded_live_is_not_added_again(tool):
    connection = FakeConnection(log={1: LOG})
    connection.world['keys'].add((1, 'ign:2026-11-04 11:01:07.250000'))
    assert run(tool, connection, True) == 0
    assert 'Ignition on' not in [r[3] for r in connection.world['inserted']]
    assert len(connection.world['inserted']) == 3


# -- the command line ------------------------------------------------------------------

@pytest.fixture
def connect(tool, monkeypatch):
    """The tool's database connection, replaced; returns the fake it hands out."""
    made = []

    def fake_connect(path):
        made.append(FakeConnection(log={1: LOG}))
        return {}, made[-1]

    monkeypatch.setattr(tool, 'connect', fake_connect)
    return made


def test_apply_wants_before(tool, connect, capsys):
    with pytest.raises(SystemExit) as stop:
        tool.main(['--apply'])
    assert stop.value.code == 2
    message = capsys.readouterr().err
    assert '--before is required with --apply' in message
    assert 'started recording events itself, so none are added twice' in message
    assert connect == []                                     # never connected


def test_before_must_be_a_time(tool, connect, capsys):
    with pytest.raises(SystemExit) as stop:
        tool.main(['--before', 'yesterday'])
    assert stop.value.code == 2
    assert "'yesterday' is not a time" in capsys.readouterr().err


def test_a_dry_run_needs_no_before(tool, connect, capsys):
    assert tool.main(['--app-log', '/nonexistent/app.log']) == 0
    assert 'nothing written' in capsys.readouterr().out
    assert connect[0].closed


def test_apply_with_before_runs_and_closes_the_connection(tool, connect, capsys):
    assert tool.main(['--apply', '--before', '2026-11-06 09:00:00',
                      '--app-log', '/nonexistent/app.log', '--device', '1']) == 0
    assert connect[0].closed and connect[0].commits == 1
    assert '4 events found, 4 added' in capsys.readouterr().out


def test_the_connection_is_closed_when_it_fails(tool, connect, monkeypatch):
    def broken(*args):
        raise RuntimeError('lost the database')

    monkeypatch.setattr(tool, 'backfill', broken)
    with pytest.raises(RuntimeError):
        tool.main(['--app-log', '/nonexistent/app.log'])
    assert connect[0].closed


def test_the_options_are_read(tool, monkeypatch):
    seen = {}

    def fake_backfill(connection, apply, devices, app_log, before):
        seen.update(apply=apply, devices=devices, app_log=app_log, before=before)
        return 0

    monkeypatch.setattr(tool, 'connect', lambda path: ({}, FakeConnection()))
    monkeypatch.setattr(tool, 'backfill', fake_backfill)
    assert tool.main(['--device', '3', '--device', '1', '--app-log', 'x.log',
                      '--before', '2026-11-06 09:00:00']) == 0
    assert seen == {'apply': False, 'devices': [3, 1], 'app_log': 'x.log',
                    'before': datetime.datetime(2026, 11, 6, 9, 0, 0)}

    tool.main([])
    assert seen['devices'] is None and seen['app_log'] == tool.DEFAULT_APP_LOG
    assert seen['before'] <= datetime.datetime.now()
    assert seen['before'] > datetime.datetime.now() - datetime.timedelta(minutes=1)
