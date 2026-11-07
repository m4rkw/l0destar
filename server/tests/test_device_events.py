"""tracker/device_events.py: the words, the keys, and the three queries.

No database: a small fake handle answers each query by a fragment of its SQL
and keeps what it was asked.
"""

import datetime

import pytest

from tracker import device_events as ev


class FakeDB:
    """`rules` is [(SQL fragment, answer)]; the first fragment found in a query
    gives its answer, and an answer that is an exception is raised."""

    def __init__(self, *rules):
        self.rules = list(rules)
        self.calls = []

    def _answer(self, kind, sql, params):
        self.calls.append((kind, sql, params))
        for fragment, answer in self.rules:
            if fragment in sql:
                if isinstance(answer, Exception):
                    raise answer
                return answer
        return None

    def one(self, sql, params=None):
        return self._answer('one', sql, params)

    def all(self, sql, params=None):
        return self._answer('all', sql, params) or []

    def query(self, sql, params=None):
        return self._answer('query', sql, params)

    def writes(self):
        return [(sql, params) for kind, sql, params in self.calls if kind == 'query']


AT = datetime.datetime(2026, 11, 4, 13, 5, 43, 326000)


# -- keys and times -----------------------------------------------------------------

def test_ref_joins_a_prefix_and_its_parts():
    assert ev.ref('ign', 'x') == 'ign:x'
    assert ev.ref('fw', '0.4.104', 'x') == 'fw:0.4.104:x'
    assert ev.ref('bk', 1, 'x') == 'bk:1:x'
    assert ev.ref('lonely') == 'lonely'


def test_ref_writes_a_datetime_to_the_microsecond():
    assert ev.ref('ign', AT) == 'ign:2026-11-04 13:05:43.326000'
    assert ev.ref('ign', AT.replace(microsecond=0)) == 'ign:2026-11-04 13:05:43.000000'


def test_a_datetime_and_the_log_string_for_it_give_the_same_ref():
    assert ev.ref('ign', AT) == ev.ref('ign', '2026-11-04 13:05:43.326000')


def test_ref_keeps_a_string_as_given_and_fits_the_column():
    assert ev.ref('app', '2026-11-04 13:05:43,326') == 'app:2026-11-04 13:05:43,326'
    assert len(ev.ref('app', 'x' * 200)) == 96


def test_stamp_of_a_datetime():
    assert ev.stamp(AT) == '2026-11-04 13:05:43'


def test_stamp_of_a_string_cuts_it_to_the_second():
    assert ev.stamp('2026-11-04 13:05:43.326000') == '2026-11-04 13:05:43'
    assert ev.stamp('2026-11-04 13:05:43') == '2026-11-04 13:05:43'


# -- record -------------------------------------------------------------------------

def test_record_inserts_a_row_and_tolerates_a_key_already_stored():
    db = FakeDB()
    assert ev.record(db, 7, 'reboot', 'Rebooted (watchdog)', AT, 'rst:x') is True
    (sql, params), = db.writes()
    assert sql.startswith('INSERT INTO `device_event` (`device_id`, `at`, `type`, `text`, `ref`)')
    assert 'ON DUPLICATE KEY UPDATE `id` = `id`' in sql
    assert params == (7, '2026-11-04 13:05:43', 'reboot', 'Rebooted (watchdog)', 'rst:x')


def test_record_without_a_time_or_key_is_now_and_unkeyed():
    db = FakeDB()
    before = datetime.datetime.now().replace(microsecond=0)
    assert ev.record(db, 7, 'command', 'Command queued: status')
    after = datetime.datetime.now()
    _, params = db.writes()[0]
    assert before <= datetime.datetime.strptime(params[1], '%Y-%m-%d %H:%M:%S') <= after
    assert params[4] is None


def test_record_takes_a_time_as_text():
    db = FakeDB()
    ev.record(db, 7, 'command', 'x', '2026-11-04 13:05:43.326000')
    assert db.writes()[0][1][1] == '2026-11-04 13:05:43'


def test_an_unknown_kind_is_filed_as_other():
    db = FakeDB()
    assert ev.record(db, 7, 'party', 'x')
    assert db.writes()[0][1][2] == 'other'


@pytest.mark.parametrize('kind', ev.KINDS)
def test_every_kind_is_kept(kind):
    db = FakeDB()
    ev.record(db, 7, kind, 'x')
    assert db.writes()[0][1][2] == kind


def test_text_is_cut_to_the_column():
    db = FakeDB()
    ev.record(db, 7, 'other', 'x' * 300)
    assert db.writes()[0][1][3] == 'x' * 200


def test_a_failed_insert_is_swallowed_and_reported():
    db = FakeDB(('INSERT INTO', RuntimeError('gone away')))
    assert ev.record(db, 7, 'other', 'x') is False


# -- record_ignition ------------------------------------------------------------------

REF_SQL = '`ref` = %s'
BEFORE_SQL = '`at` <= %s'
AFTER_SQL = '`at` > %s'


def inserted(db):
    return [params for sql, params in db.writes() if sql.startswith('INSERT')]


def deleted(db):
    return [params for sql, params in db.writes() if sql.startswith('DELETE')]


def test_ignition_on_after_off_is_recorded():
    db = FakeDB((BEFORE_SQL, {'text': 'Ignition off'}))
    assert ev.record_ignition(db, 7, True, AT, 'ign:x') is True
    assert inserted(db) == [(7, '2026-11-04 13:05:43', 'ignition', 'Ignition on', 'ign:x')]
    assert deleted(db) == []


def test_the_first_ignition_event_is_recorded():
    db = FakeDB()
    assert ev.record_ignition(db, 7, False, AT) is True
    assert inserted(db) == [(7, '2026-11-04 13:05:43', 'ignition', 'Ignition off', None)]


def test_the_same_state_as_the_event_before_adds_nothing():
    db = FakeDB((BEFORE_SQL, {'text': 'Ignition on'}))
    assert ev.record_ignition(db, 7, True, AT, 'ign:x') is False
    assert db.writes() == []


def test_a_key_already_stored_adds_nothing():
    db = FakeDB((REF_SQL, {'id': 3}))
    assert ev.record_ignition(db, 7, True, AT, 'ign:x') is False
    assert db.writes() == []
    # ...and went no further than the key.
    assert len(db.calls) == 1


def test_without_a_key_the_key_is_not_looked_for():
    db = FakeDB()
    ev.record_ignition(db, 7, True, AT)
    assert not any(REF_SQL in sql for _, sql, _ in db.calls)


def test_a_later_event_that_says_the_same_is_no_longer_a_change_and_goes():
    # Off at 10:00, on at 12:00 (stored); the on at 11:00 arrives late.
    db = FakeDB((BEFORE_SQL, {'text': 'Ignition off'}),
                (AFTER_SQL, {'id': 9, 'text': 'Ignition on'}))
    assert ev.record_ignition(db, 7, True, AT, 'ign:x') is True
    assert len(inserted(db)) == 1
    assert deleted(db) == [(9,)]


def test_a_later_event_that_differs_stays():
    db = FakeDB((BEFORE_SQL, {'text': 'Ignition off'}),
                (AFTER_SQL, {'id': 9, 'text': 'Ignition off'}))
    assert ev.record_ignition(db, 7, True, AT) is True
    assert deleted(db) == []


def test_the_neighbours_are_judged_by_time_not_by_arrival():
    db = FakeDB()
    ev.record_ignition(db, 7, True, AT)
    by_time = [sql for kind, sql, _ in db.calls if 'FROM `device_event`' in sql]
    assert any('ORDER BY `at` DESC, `id` DESC LIMIT 1' in sql for sql in by_time)
    assert any('ORDER BY `at`, `id` LIMIT 1' in sql for sql in by_time)


def test_an_error_while_looking_is_swallowed():
    db = FakeDB((BEFORE_SQL, RuntimeError('gone away')))
    assert ev.record_ignition(db, 7, True, AT, 'ign:x') is False
    assert db.writes() == []


def test_a_failed_insert_leaves_the_later_event_alone():
    db = FakeDB((AFTER_SQL, {'id': 9, 'text': 'Ignition on'}),
                ('INSERT INTO', RuntimeError('gone away')))
    assert ev.record_ignition(db, 7, True, AT) is False
    assert deleted(db) == []


def test_a_failed_tidy_is_swallowed_and_the_event_still_counts():
    db = FakeDB((AFTER_SQL, {'id': 9, 'text': 'Ignition on'}),
                ('DELETE FROM', RuntimeError('gone away')))
    assert ev.record_ignition(db, 7, True, AT) is True


# -- parse_query -----------------------------------------------------------------------

@pytest.mark.parametrize('limit, before', [(None, None), ('', ''), (None, ''), ('', None)])
def test_query_defaults(limit, before):
    assert ev.parse_query(limit, before) == (200, None)


def test_limit_and_before_are_read():
    assert ev.parse_query('25', '300') == (25, 300)
    assert ev.parse_query('1', None) == (1, None)
    assert ev.parse_query(None, '1') == (200, 1)


def test_limit_is_capped():
    assert ev.parse_query('500', None)[0] == 500
    assert ev.parse_query('501', None)[0] == 500
    assert ev.parse_query('100000', None)[0] == 500


@pytest.mark.parametrize('limit', ['0', '-1', 'abc', '1.5', '1e3', ' ', '0x10'])
def test_a_bad_limit_is_refused(limit):
    with pytest.raises(ValueError) as refused:
        ev.parse_query(limit, None)
    assert str(refused.value) == 'limit must be a whole number from 1 to 500'


@pytest.mark.parametrize('before', ['0', '-5', 'abc', '2.5', ' ', '1e3'])
def test_a_bad_cursor_is_refused(before):
    with pytest.raises(ValueError) as refused:
        ev.parse_query('10', before)
    assert str(refused.value) == 'before must be an event id'


def test_the_limit_is_judged_before_the_cursor():
    with pytest.raises(ValueError) as refused:
        ev.parse_query('0', 'x')
    assert 'limit' in str(refused.value)


# -- page ------------------------------------------------------------------------------

def rows(*items):
    return [{'id': i, 'at': at, 'type': kind, 'text': text} for i, at, kind, text in items]


def test_page_without_a_cursor_reads_the_newest_first():
    db = FakeDB(('FROM `device_event` WHERE `device_id` = %s ORDER', rows(
        (5, AT, 'ignition', 'Ignition on'), (4, '2026-11-04 12:00:00', 'reboot', 'Rebooted'))))
    assert ev.page(db, 7, 50, None) == [
        {'id': 5, 'at': '2026-11-04 13:05:43', 'type': 'ignition', 'text': 'Ignition on'},
        {'id': 4, 'at': '2026-11-04 12:00:00', 'type': 'reboot', 'text': 'Rebooted'}]
    (kind, sql, params), = db.calls
    assert kind == 'all'
    assert 'ORDER BY `at` DESC, `id` DESC LIMIT %s' in sql
    assert '`id` <' not in sql and '`at` <' not in sql
    assert params == [7, 50]


def test_page_after_a_cursor_continues_from_that_event_in_time_order():
    when = datetime.datetime(2026, 11, 4, 12, 0, 0)
    db = FakeDB(('WHERE `id` = %s AND `device_id` = %s', {'at': when, 'id': 40}))
    ev.page(db, 7, 10, 40)
    kind, sql, params = db.calls[-1]
    assert '(`at` < %s OR (`at` = %s AND `id` < %s))' in sql
    assert params == [7, when, when, 40, 10]
    # The cursor is looked up for this device only.
    assert db.calls[0][2] == (40, 7)


def test_page_after_a_cursor_that_is_gone_counts_back_from_its_id():
    db = FakeDB()
    ev.page(db, 7, 10, 40)
    kind, sql, params = db.calls[-1]
    assert 'AND `id` < %s' in sql and '`at` <' not in sql
    assert params == [7, 40, 10]


def test_page_with_nothing_is_empty():
    assert ev.page(FakeDB(), 7) == []


def test_page_defaults_to_the_default_limit():
    db = FakeDB()
    ev.page(db, 7)
    assert db.calls[-1][2] == [7, ev.DEFAULT_LIMIT]


# -- the words -----------------------------------------------------------------------------

def test_ignition_text():
    assert ev.ignition_text(True) == 'Ignition on'
    assert ev.ignition_text(False) == 'Ignition off'


@pytest.mark.parametrize('rst, text', [
    (None, 'Rebooted'),
    ('', 'Rebooted'),
    ('pin', 'Rebooted (hardware reset)'),
    ('sw', 'Rebooted (software)'),
    ('lockup', 'Rebooted (CPU lockup)'),
    ('wdt', 'Rebooted (watchdog)'),
    # Flags are sticky: the most telling one wins.
    ('pin+sw+wdt', 'Rebooted (watchdog)'),
    ('pin+sw', 'Rebooted (software)'),
    ('pin+lockup', 'Rebooted (CPU lockup)'),
    ('sw+lockup', 'Rebooted (CPU lockup)'),
    ('wdt+stall:4aa02/3e254', 'Rebooted (watchdog, stalled)'),
    ('pin+wdt+stall:4aa02/3e254', 'Rebooted (watchdog, stalled)'),
    # A stall without the watchdog says nothing more than a reboot.
    ('stall:4aa02/3e254', 'Rebooted'),
    ('sw+stall:1', 'Rebooted (software)'),
    ('brownout', 'Rebooted'),
    ('+', 'Rebooted'),
])
def test_reset_text(rst, text):
    assert ev.reset_text(rst) == text


@pytest.mark.parametrize('version, parsed', [
    ('0.4.104', (0, 4, 104)),
    (' 0.4.9 ', (0, 4, 9)),
    ('12', (12,)),
    ('0.4', (0, 4)),
    ('', None), ('nightly', None), ('0.4.x', None), ('0..4', None), ('v0.4.1', None),
    (None, None), (104, None),
])
def test_version_tuple(version, parsed):
    assert ev.version_tuple(version) == parsed


def test_versions_compare_as_numbers():
    assert ev.version_tuple('0.4.9') < ev.version_tuple('0.4.104')


def test_firmware_text_downloading_and_staged():
    assert ev.firmware_text('downloading', '0.4.109') == 'Firmware 0.4.109 downloading'
    assert ev.firmware_text('staged', '0.4.109') == 'Firmware 0.4.109 staged'


def test_firmware_text_applied_and_restored():
    assert ev.firmware_text('applied', '0.4.109', '0.4.108') == 'Firmware 0.4.109 applied'
    assert ev.firmware_text('applied', '0.4.109') == 'Firmware 0.4.109 applied'
    # Numbers, not text: 0.4.99 is older than 0.4.104.
    assert ev.firmware_text('applied', '0.4.99', '0.4.104') == 'Firmware 0.4.99 restored'
    assert ev.firmware_text('applied', '0.4.104', '0.4.99') == 'Firmware 0.4.104 applied'
    # The same version again is not "restored".
    assert ev.firmware_text('applied', '0.4.104', '0.4.104') == 'Firmware 0.4.104 applied'
    # A version that is not one says nothing about order.
    assert ev.firmware_text('applied', '0.4.1', 'nightly') == 'Firmware 0.4.1 applied'
    assert ev.firmware_text('applied', 'nightly', '0.4.9') == 'Firmware nightly applied'


def test_firmware_text_failed():
    assert ev.firmware_text('failed', '0.4.13', '0.4.12') == \
        'Firmware 0.4.13 failed, still on 0.4.12'
    assert ev.firmware_text('failed', '0.4.13') == 'Firmware 0.4.13 failed'
    # Only an applied update is compared for "restored".
    assert ev.firmware_text('failed', '0.4.11', '0.4.12') == \
        'Firmware 0.4.11 failed, still on 0.4.12'


def test_command_text():
    assert ev.command_text('queued', 'int=600') == 'Command queued: int=600'
    assert ev.command_text('collected', 'netscan') == 'Command collected: netscan'
    for what in ('acknowledged', 'cancelled', 'replaced', 'expired'):
        assert ev.command_text(what, 'x') == 'Command %s: x' % what


def test_command_parts():
    assert ev.command_parts('status,gnssstat') == ['status', 'gnssstat']
    assert ev.command_parts(' int=600 , locate ,') == ['int=600', 'locate']
    assert ev.command_parts('gnssbk=0') == ['gnssbk=0']
    assert ev.command_parts('') == [] and ev.command_parts(None) == []


@pytest.mark.parametrize('column, value, text', [
    ('int', 600, 'Report interval set to 600 s'),
    ('int', 0, 'Report interval off'),
    ('backup_int', 1800, 'Backup report interval set to 1800 s'),
    ('backup_int', 0, 'Backup report interval follows the normal one'),
    ('movement_alarm', 1, 'Movement alarm on'),
    ('movement_alarm', 0, 'Movement alarm off'),
    ('alarm', 1, 'Ignition alarm on'),
    ('alarm', 0, 'Ignition alarm off'),
    ('garage', 1, 'Garage mode on'),
    ('garage', 0, 'Garage mode off'),
    ('overnight_alarm', 1, 'Overnight alarm on'),
    ('overnight_alarm', 0, 'Overnight alarm off'),
    ('overnight_alarm_hour_from', 23, 'Overnight alarm starts at 23:00'),
    ('overnight_alarm_hour_from', 0, 'Overnight alarm starts at 00:00'),
    ('overnight_alarm_hour_to', 6, 'Overnight alarm ends at 06:00'),
    ('gnss_backup', 1, 'GNSS backup supply set to on'),
    ('gnss_backup', 0, 'GNSS backup supply set to off'),
    ('wake_ms', 5, 'Setting changed: wake_ms=5'),
])
def test_setting_text(column, value, text):
    assert ev.setting_text(column, value) == text


def test_track_mode_text():
    assert ev.track_mode_text(True) == 'Track mode on'
    assert ev.track_mode_text(False) == 'Track mode off'
    assert ev.track_mode_text(False, 'ignition off') == 'Track mode off (ignition off)'
    assert ev.track_mode_text(False, 'idle 7300 s') == 'Track mode off (idle 7300 s)'


def test_backup_supply_text():
    assert ev.backup_supply_text(True) == 'GNSS backup supply on'
    assert ev.backup_supply_text(False) == 'GNSS backup supply off'


def test_result_text():
    assert ev.result_text('scan') == 'Network scan result received'
    assert ev.result_text('selftest') == 'Self test result received'
    assert ev.result_text('status') == 'Status result received'
    assert ev.result_text('gnssstat') == 'GNSS status result received'
    assert ev.result_text('backlog') == 'Backlog result received'
    assert ev.result_text('logdump') == 'Log dump result received'
    assert ev.result_text('thermal') == 'Thermal result received'


def test_fault_text():
    assert ev.fault_text('P0301', True) == 'Fault code P0301 raised'
    assert ev.fault_text('P0301', False) == 'Fault code P0301 cleared'


@pytest.mark.parametrize('message, event', [
    ('fota: 0.4.108 -> 0.4.109 available, downloading',
     ('firmware', 'Firmware 0.4.109 downloading')),
    # The staged report says it, so the alert adds nothing.
    ('fota: 0.4.108 -> 0.4.109, rebooting', None),
    ('google: 51.50,-0.12', ('result', 'Location received')),
    ('apple: 51.50,-0.12', ('result', 'Location received')),
    ('tomtom: 51.50,-0.12', ('result', 'Location received')),
    # The unit's bk= report says it.
    ('gnss backup supply OFF', None),
    ('gnss backup supply ON', None),
    ('GNSS backup supply ON', None),
    ('tilt 12deg - possible tow/jack', ('other', 'Alert: tilt 12deg - possible tow/jack')),
    ('selftest: 1 failed', ('other', 'Alert: selftest: 1 failed')),
    ('gps fix requested', ('other', 'Alert: gps fix requested')),
    # An update line that is not one of the two shapes is just an alert.
    ('fota: updated to 0.4.73', ('other', 'Alert: fota: updated to 0.4.73')),
    ('googled: nothing', ('other', 'Alert: googled: nothing')),
    ('  tilt 12deg  ', ('other', 'Alert: tilt 12deg')),
    ('', None), ('   ', None), (None, None),
])
def test_alert_event(message, event):
    assert ev.alert_event(message) == event
