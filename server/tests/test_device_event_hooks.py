"""Where the device event log is written from: the telemetry path, the firmware
lifecycle and the API.

No database: a fake handle answers the queries each function makes and keeps
the event rows written.  Each hook is checked for the kind, text, time and key
it records, that it fires only when the thing it reports happened, and that an
event that cannot be written changes nothing about the function it sits in.
The same paths against a real database are in test_integration.py.
"""

import datetime
import time

import pytest

from conftest import record

from tracker import db, firmware, telemetry

IMEI = '350000000000000'
NOW = datetime.datetime.now()


class Quiet:
    """The log argument telemetry takes."""

    def info(self, *args):
        pass

    def warning(self, *args):
        pass

    def error(self, *args):
        pass

    def exception(self, *args):
        pass


class Handle:
    """A database handle for one device.

    `rules` maps a fragment of SQL to what .one() and .all() answer (a value,
    or a function of the parameters); every statement run through .query() is
    kept in `writes`, and the rows inserted into device_event in `events` as
    (kind, text, at, ref).  With `broken`, writing an event raises.
    """

    def __init__(self, rules=None, broken=False):
        self.rules = dict(rules or {})
        self.writes = []
        self.events = []
        self.broken = broken

    def _answer(self, sql, params, empty):
        for fragment, answer in self.rules.items():
            if fragment in sql:
                return answer(params) if callable(answer) else answer
        return empty

    def one(self, sql, params=None):
        return self._answer(sql, params, None)

    def all(self, sql, params=None):
        return self._answer(sql, params, [])

    def query(self, sql, params=None):
        self.writes.append((sql, params))
        if sql.startswith('INSERT INTO `device_event`'):
            if self.broken:
                raise RuntimeError('device_event is broken')
            _, at, kind, text, ref = params
            self.events.append((kind, text, at, ref))
        return 1

    def other_writes(self):
        return [w for w in self.writes if 'device_event' not in w[0]]


def texts(handle):
    return [e[1] for e in handle.events]


def at_str(when):
    return when.strftime('%Y-%m-%d %H:%M:%S')


# -- firmware: staged and failed ---------------------------------------------------

class FirmwareRow(Handle):
    """The device row the firmware lifecycle keeps."""

    def __init__(self, **row):
        super().__init__(broken=row.pop('broken', False))
        self.row = {'fw_staged': None, 'fw_staged_at': None, 'fw_blocked': None,
                    'fw_fail_count': 0}
        self.row.update(row)

    def one(self, sql, params=None):
        return dict(self.row)

    def query(self, sql, params=None):
        super().query(sql, params)
        if '`fw_staged` = %s' in sql:
            self.row.update(fw_staged=params[0], fw_staged_at=datetime.datetime.now())
        elif '`fw_blocked` = %s' in sql:
            self.row.update(fw_staged=None, fw_staged_at=None,
                            fw_blocked=params[0], fw_fail_count=params[1])
        return 1


DEV = {'id': 1, 'imei': IMEI, 'name': 'Car'}


@pytest.fixture
def alerts(monkeypatch):
    sent = []
    monkeypatch.setattr(firmware.notify, 'device_alert',
                        lambda name, msg, pri: sent.append((msg, pri)))
    return sent


def test_a_staged_report_is_an_event(alerts):
    handle = FirmwareRow()
    before = datetime.datetime.now().replace(microsecond=0)
    firmware.note_staged(DEV, '0.4.13', handle, Quiet())
    assert [(e[0], e[1], e[3]) for e in handle.events] == \
        [('firmware', 'Firmware 0.4.13 staged', None)]
    assert before <= datetime.datetime.strptime(handle.events[0][2], '%Y-%m-%d %H:%M:%S')


def test_the_same_staged_report_again_adds_nothing():
    handle = FirmwareRow()
    firmware.note_staged(DEV, '0.4.13', handle, Quiet())
    firmware.note_staged(DEV, '0.4.13', handle, Quiet())
    firmware.note_staged(DEV, ' 0.4.13 ', handle, Quiet())
    assert texts(handle) == ['Firmware 0.4.13 staged']


def test_a_different_staged_version_is_another_event():
    handle = FirmwareRow()
    firmware.note_staged(DEV, '0.4.13', handle, Quiet())
    firmware.note_staged(DEV, '0.4.14', handle, Quiet())
    assert texts(handle) == ['Firmware 0.4.13 staged', 'Firmware 0.4.14 staged']


def test_a_bad_staged_version_is_no_event():
    handle = FirmwareRow()
    firmware.note_staged(DEV, 'nightly', handle, Quiet())
    assert handle.events == [] and handle.writes == []


def test_the_event_follows_the_update_it_reports():
    handle = FirmwareRow()
    firmware.note_staged(DEV, '0.4.13', handle, Quiet())
    assert 'UPDATE `device`' in handle.writes[0][0]
    assert 'device_event' in handle.writes[1][0]


def test_a_staged_event_that_cannot_be_written_changes_nothing():
    handle = FirmwareRow(broken=True)
    firmware.note_staged(DEV, '0.4.13', handle, Quiet())
    assert handle.row['fw_staged'] == '0.4.13'


def test_a_staged_report_goes_through_when_the_stored_one_cannot_be_read():
    class Unreadable(FirmwareRow):
        def one(self, sql, params=None):
            raise RuntimeError('gone away')

    handle = Unreadable()
    firmware.note_staged(DEV, '0.4.13', handle, Quiet())
    assert handle.row['fw_staged'] == '0.4.13'
    assert handle.events == []


def test_a_failed_update_is_an_event_the_first_time_only(alerts):
    handle = FirmwareRow(fw_staged='0.4.13')
    firmware.note_failed(DEV, '0.4.13', '0.4.12', handle, Quiet())
    firmware.note_failed(DEV, '0.4.13', '0.4.12', handle, Quiet())
    assert [(e[0], e[1], e[3]) for e in handle.events] == \
        [('firmware', 'Firmware 0.4.13 failed, still on 0.4.12', None)]
    assert len(alerts) == 1


def test_a_bad_failed_version_is_no_event(alerts):
    handle = FirmwareRow()
    firmware.note_failed(DEV, 'nightly', '0.4.12', handle, Quiet())
    assert handle.events == []


def test_a_failed_event_that_cannot_be_written_changes_nothing(alerts):
    handle = FirmwareRow(fw_staged='0.4.13', broken=True)
    firmware.note_failed(DEV, '0.4.13', '0.4.12', handle, Quiet())
    assert handle.row['fw_blocked'] == '0.4.13' and handle.row['fw_fail_count'] == 1
    assert len(alerts) == 1


# -- a record: ignition and reboot -------------------------------------------------------

GSM = '2026-11-04 11:02:07.250000'
BUILT = datetime.datetime(2026, 11, 4, 12, 2, 0)


def test_an_ignition_change_is_an_event_at_the_records_build_time():
    handle = Handle()
    entry = {'gsm_timestamp': GSM, 'ignition_state': 1}
    telemetry._note_log_events(handle, DEV, {'ignition_state': 0}, entry, BUILT)
    assert handle.events == [('ignition', 'Ignition on', at_str(BUILT),
                              'ign:2026-11-04 11:02:07.250000')]


def test_ignition_off_is_an_event_too():
    handle = Handle()
    entry = {'gsm_timestamp': GSM, 'ignition_state': 0}
    telemetry._note_log_events(handle, DEV, {'ignition_state': '1'}, entry, BUILT)
    assert texts(handle) == ['Ignition off']


def test_no_change_and_no_predecessor_are_no_events():
    handle = Handle()
    telemetry._note_log_events(handle, DEV, {'ignition_state': 1},
                               {'gsm_timestamp': GSM, 'ignition_state': 1}, BUILT)
    telemetry._note_log_events(handle, DEV, None,
                               {'gsm_timestamp': GSM, 'ignition_state': 1}, BUILT)
    assert handle.events == []


def test_a_reset_cause_is_a_reboot_event_keyed_by_the_record():
    handle = Handle()
    entry = {'gsm_timestamp': GSM, 'ignition_state': 0, 'rst': 'pin+sw+wdt'}
    telemetry._note_log_events(handle, DEV, {'ignition_state': 0}, entry, BUILT)
    assert handle.events == [('reboot', 'Rebooted (watchdog)', at_str(BUILT),
                              'rst:2026-11-04 11:02:07.250000')]


def test_an_empty_reset_cause_is_no_reboot():
    handle = Handle()
    telemetry._note_log_events(handle, DEV, None,
                               {'gsm_timestamp': GSM, 'ignition_state': 0, 'rst': ''}, BUILT)
    assert handle.events == []


def test_an_ignition_and_a_reboot_in_one_record_are_both_kept():
    handle = Handle()
    entry = {'gsm_timestamp': GSM, 'ignition_state': 1, 'rst': 'sw'}
    telemetry._note_log_events(handle, DEV, {'ignition_state': 0}, entry, BUILT)
    assert texts(handle) == ['Ignition on', 'Rebooted (software)']


def test_a_record_already_recorded_adds_nothing_new():
    # What the stored key is for: the same record seen again.
    handle = Handle({'`ref` = %s': {'id': 1}})
    entry = {'gsm_timestamp': GSM, 'ignition_state': 1}
    telemetry._note_log_events(handle, DEV, {'ignition_state': 0}, entry, BUILT)
    assert handle.events == []


def test_a_failing_event_cannot_raise_into_the_record():
    handle = Handle(broken=True)
    entry = {'gsm_timestamp': GSM, 'ignition_state': 1, 'rst': 'sw'}
    telemetry._note_log_events(handle, DEV, {'ignition_state': 0}, entry, BUILT)
    # Nor can a row of the wrong shape.
    telemetry._note_log_events(Handle(), DEV, {'ignition_state': None}, entry, BUILT)
    telemetry._note_log_events(Handle(), DEV, {}, entry, BUILT)


def process(handle, device, line, previous_ignition=0):
    """One live record through process_record against a handle that knows the
    device's previous record."""
    prev = {'id': 5, 'timestamp': datetime.datetime.now() - datetime.timedelta(minutes=10),
            'ignition_state': previous_ignition}
    handle.rules.setdefault('ORDER BY `id` DESC LIMIT 50',
                            [{'id': 5, 'timestamp': prev['timestamp']}])
    handle.rules.setdefault('SELECT * FROM `log` WHERE `id`', prev)
    return telemetry.process_record(telemetry.parse_csv_line(line), device, '203.0.113.7',
                                    database=handle)


def test_process_record_records_the_ignition_change_after_the_insert():
    handle = Handle()
    process(handle, dict(DEV), record(0, 51.5, -0.1, 1), previous_ignition=0)
    kinds = [w[0].split(' (')[0] for w in handle.writes]
    assert handle.events[0][:2] == ('ignition', 'Ignition on')
    assert handle.events[0][3].startswith('ign:2026-08-12 00:00:00')
    assert kinds.index('INSERT INTO `log`') < kinds.index('INSERT INTO `device_event`')


def test_process_record_records_a_reboot():
    handle = Handle()
    process(handle, dict(DEV), record(0, 51.5, -0.1, 0, extras=',rst=pin+sw'))
    assert [e[:2] for e in handle.events] == [('reboot', 'Rebooted (software)')]


def test_process_record_with_nothing_to_report_records_nothing():
    handle = Handle()
    process(handle, dict(DEV), record(0, 51.5, -0.1, 0))
    assert handle.events == []


def test_process_record_survives_a_broken_event_log():
    handle = Handle(broken=True)
    process(handle, dict(DEV), record(0, 51.5, -0.1, 1, extras=',rst=sw'), previous_ignition=0)
    assert any(w[0].startswith('INSERT INTO `log`') for w in handle.writes)


# -- the version on the device row ----------------------------------------------------------

def version_entry(fw='0.4.97', board=None):
    return {'fw': fw, 'board': board, 'timestamp': '2026-11-04 12:02:00.123456',
            'gsm_timestamp': GSM}


def test_a_new_version_after_a_known_one_is_applied():
    handle = Handle()
    device = {'id': 1, 'imei': IMEI, 'fw': '0.4.96'}
    telemetry._note_versions(handle, device, version_entry())
    assert handle.events == [('firmware', 'Firmware 0.4.97 applied', '2026-11-04 12:02:00',
                              'fw:0.4.97:2026-11-04 11:02:07.250000')]
    assert device['fw'] == '0.4.97'
    assert 'UPDATE `device`' in handle.writes[0][0]


def test_an_older_version_is_restored():
    handle = Handle()
    telemetry._note_versions(handle, {'id': 1, 'fw': '0.4.104'}, version_entry('0.4.99'))
    assert texts(handle) == ['Firmware 0.4.99 restored']


def test_the_first_version_reported_is_no_event():
    handle = Handle()
    device = {'id': 1, 'fw': None}
    telemetry._note_versions(handle, device, version_entry())
    assert handle.events == [] and device['fw'] == '0.4.97'


def test_a_board_change_alone_is_no_firmware_event():
    handle = Handle()
    telemetry._note_versions(handle, {'id': 1, 'fw': '0.4.97', 'board': 'a'},
                             version_entry('0.4.97', 'b'))
    assert handle.events == []
    assert len(handle.other_writes()) == 1                        # the row was still written


def test_an_unchanged_version_writes_nothing():
    handle = Handle()
    telemetry._note_versions(handle, {'id': 1, 'fw': '0.4.97'}, version_entry())
    assert handle.writes == []


def test_a_version_event_that_cannot_be_written_changes_nothing():
    handle = Handle(broken=True)
    device = {'id': 1, 'fw': '0.4.96'}
    telemetry._note_versions(handle, device, version_entry())
    assert device['fw'] == '0.4.97'


# -- GNSS backup supply changes (bk=) ---------------------------------------------------------

def backup(handle, device, raw, built=BUILT):
    telemetry._store_backup_changes(handle, device, {'backup_changes': raw}, built)


def ago(seconds):
    return BUILT - datetime.timedelta(seconds=seconds)


def test_a_backup_change_is_an_event_at_the_time_it_happened():
    handle = Handle()
    backup(handle, {'id': 1, 'gnss_backup': 1, 'gnss_backup_at': ago(7200)}, '0:600')
    assert handle.events == [('setting', 'GNSS backup supply off', at_str(ago(600)),
                              'bk:0:' + at_str(ago(600)))]


def test_several_changes_come_oldest_first_and_only_real_ones_count():
    handle = Handle()
    # Reported newest first: off 60 s ago, on 300 s ago, on again 200 s ago.
    device = {'id': 1, 'gnss_backup': 0, 'gnss_backup_at': ago(7200)}
    backup(handle, device, '0:60|1:300|1:200')
    assert [(e[1], e[2]) for e in handle.events] == [
        ('GNSS backup supply on', at_str(ago(300))),
        ('GNSS backup supply off', at_str(ago(60)))]


def test_a_change_to_the_state_it_was_already_in_is_no_event():
    handle = Handle()
    backup(handle, {'id': 1, 'gnss_backup': 1, 'gnss_backup_at': ago(7200)}, '1:60')
    assert handle.events == []


def test_the_first_state_ever_known_is_an_event():
    handle = Handle()
    backup(handle, {'id': 1, 'gnss_backup': None, 'gnss_backup_at': None}, '1:60')
    assert texts(handle) == ['GNSS backup supply on']


def test_changes_no_later_than_the_one_held_are_left_alone():
    # The API set it after these happened (or they are the ones already held).
    handle = Handle()
    backup(handle, {'id': 1, 'gnss_backup': 1, 'gnss_backup_at': ago(100)}, '0:300|0:100|0:50')
    assert texts(handle) == ['GNSS backup supply off']
    assert handle.events[0][2] == at_str(ago(50))


def test_unusable_items_are_skipped_on_their_own():
    handle = Handle()
    backup(handle, {'id': 1, 'gnss_backup': 1}, '2:5|x:y|1:-3|0:60|0:%d' % (telemetry.BACKUP_AGE_MAX + 1))
    assert texts(handle) == ['GNSS backup supply off']


def test_the_device_row_is_updated_as_it_was_and_the_event_follows_the_update():
    handle = Handle()
    device = {'id': 1, 'gnss_backup': 1, 'gnss_backup_at': ago(7200)}
    backup(handle, device, '0:600')
    assert device['gnss_backup'] == 0
    assert 'UPDATE `device`' in handle.writes[0][0]
    assert 'device_event' in handle.writes[1][0]


def test_a_backup_event_that_cannot_be_written_changes_nothing():
    handle = Handle(broken=True)
    device = {'id': 1, 'gnss_backup': 1, 'gnss_backup_at': ago(7200)}
    backup(handle, device, '0:600')
    assert device['gnss_backup'] == 0


def test_a_record_with_no_backup_changes_records_nothing():
    handle = Handle()
    telemetry._store_backup_changes(handle, {'id': 1}, {}, BUILT)
    assert handle.writes == []


# -- the unit says what it ran (ak=) --------------------------------------------------------------

def test_each_acknowledged_command_is_an_event():
    handle = Handle()
    telemetry._store_applied(handle, {'id': 1}, {'acks': 'locatenow|status'},
                             {'fw': None})
    assert texts(handle) == ['Command acknowledged: locatenow', 'Command acknowledged: status']


def test_a_restart_that_kept_the_build_acknowledges_a_reboot():
    handle = Handle()
    telemetry._store_applied(handle, {'id': 1, 'fw': '0.4.97'}, {'acks': 'status'},
                             {'rst': 'sw', 'fw': '0.4.97'})
    assert texts(handle) == ['Command acknowledged: status', 'Command acknowledged: reboot']


def test_nothing_acknowledged_records_nothing():
    handle = Handle()
    telemetry._store_applied(handle, {'id': 1}, {'applied': '3600:1:0:0'}, {})
    assert handle.events == []


def test_an_acknowledgement_event_that_cannot_be_written_changes_nothing():
    handle = Handle(broken=True)
    telemetry._store_applied(handle, {'id': 1}, {'acks': 'status'}, {})
    assert any('ack_cmd' in w[0] for w in handle.writes)


# -- track mode -----------------------------------------------------------------------------------

def test_clearing_track_mode_is_an_event_with_the_reason():
    handle = Handle()
    device = {'id': 1, 'imei': IMEI, 'track_mode': 1}
    telemetry.clear_track_mode(device, handle, 'ignition off')
    assert [e[:2] for e in handle.events] == [('setting', 'Track mode off (ignition off)')]
    assert device['track_mode'] == 0
    assert 'UPDATE `device`' in handle.writes[0][0]


def test_clearing_a_switch_that_is_already_off_is_no_event():
    handle = Handle()
    telemetry.clear_track_mode({'id': 1, 'imei': IMEI, 'track_mode': 0}, handle, 'ignition off')
    telemetry.clear_track_mode({'id': 1, 'imei': IMEI}, handle, 'ignition off')
    assert handle.events == []


def test_an_idle_expiry_says_how_long():
    stale = Handle({'ORDER BY `id` DESC LIMIT 50': [{'id': 1, 'timestamp': datetime.datetime.now()
                                                     - datetime.timedelta(hours=2)}],
                    'SELECT * FROM `log`': {'id': 1, 'timestamp': datetime.datetime.now()
                                            - datetime.timedelta(hours=2)}})
    device = {'id': 1, 'imei': IMEI, 'track_mode': 1}
    telemetry.expire_track_mode(device, stale)
    (event,) = stale.events
    assert event[0] == 'setting' and event[1].startswith('Track mode off (idle 72')
    assert event[1].endswith(' s)')


def test_a_track_mode_event_that_cannot_be_written_changes_nothing():
    handle = Handle(broken=True)
    device = {'id': 1, 'imei': IMEI, 'track_mode': 1}
    telemetry.clear_track_mode(device, handle, 'ignition off')
    assert device['track_mode'] == 0


# -- fault codes ------------------------------------------------------------------------------------

def test_raised_and_cleared_fault_codes_are_events():
    handle = Handle({'FROM `dtc`': [{'id': 1, 'code': 'P0301'}, {'id': 2, 'code': 'P0420'}]})
    device = {'id': 1, 'imei': IMEI, 'name': 'Car'}
    assert telemetry.process_dtc_report(device, ['P0420', 'U0100', 'p0171'], handle, Quiet()) == (2, 1)
    assert texts(handle) == ['Fault code U0100 raised', 'Fault code P0171 raised',
                             'Fault code P0301 cleared']
    assert {e[0] for e in handle.events} == {'other'}
    assert all('device_event' not in w[0] for w in handle.writes[:3])


def test_a_report_that_changes_nothing_records_nothing():
    handle = Handle({'FROM `dtc`': [{'id': 1, 'code': 'P0301'}]})
    telemetry.process_dtc_report({'id': 1, 'imei': IMEI, 'name': 'Car'}, ['P0301'], handle, Quiet())
    assert handle.events == []


def test_an_empty_report_clears_everything():
    handle = Handle({'FROM `dtc`': [{'id': 1, 'code': 'P0301'}]})
    telemetry.process_dtc_report({'id': 1, 'imei': IMEI, 'name': 'Car'}, [''], handle, Quiet())
    assert texts(handle) == ['Fault code P0301 cleared']


def test_fault_events_that_cannot_be_written_change_nothing():
    handle = Handle({'FROM `dtc`': [{'id': 1, 'code': 'P0301'}]}, broken=True)
    result = telemetry.process_dtc_report({'id': 1, 'imei': IMEI, 'name': 'Car'},
                                          ['U0100'], handle, Quiet())
    assert result == (1, 1)


# -- alerts --------------------------------------------------------------------------------------------

@pytest.fixture
def relayed(monkeypatch):
    sent = []
    monkeypatch.setattr(telemetry.notify, 'device_alert',
                        lambda name, msg, pri: sent.append((msg, pri)))
    telemetry._alert_seen.clear()
    return sent


def alert(handle, line, device=None):
    telemetry._handle_alert(line, device or dict(DEV), handle, Quiet())


def test_a_relayed_alert_is_an_event(relayed):
    handle = Handle()
    alert(handle, 'A,2,tilt 12deg - possible tow/jack,aid=1')
    assert [e[:2] for e in handle.events] == [('other', 'Alert: tilt 12deg - possible tow/jack')]
    assert relayed == [('tilt 12deg - possible tow/jack', 2)]


def test_alert_lines_become_the_events_alert_event_gives(relayed):
    handle = Handle()
    alert(handle, 'A,0,fota: 0.4.108 -> 0.4.109 available, downloading')
    alert(handle, 'A,0,fota: 0.4.108 -> 0.4.109, rebooting')
    alert(handle, 'A,0,google: 51.50,-0.12')
    alert(handle, 'A,0,gnss backup supply OFF')
    assert [e[:2] for e in handle.events] == [('firmware', 'Firmware 0.4.109 downloading'),
                                              ('result', 'Location received')]
    assert len(relayed) == 4


def test_a_copy_of_an_alert_is_not_an_event_twice(relayed):
    handle = Handle()
    alert(handle, 'A,1,backup power: 9.03V,aid=8627')
    alert(handle, 'A,1,backup power: 9.03V,aid=8627')
    assert texts(handle) == ['Alert: backup power: 9.03V']


def test_a_suppressed_low_battery_alert_is_no_event(relayed):
    running = Handle({'ORDER BY `id` DESC LIMIT 50': [{'id': 1, 'timestamp': NOW}],
                      'SELECT * FROM `log`': {'id': 1, 'timestamp': NOW, 'ignition_state': 1}})
    alert(running, 'A,1,low battery: 11.2V')
    assert running.events == [] and relayed == []

    parked = Handle({'ORDER BY `id` DESC LIMIT 50': [{'id': 1, 'timestamp': NOW}],
                     'SELECT * FROM `log`': {'id': 1, 'timestamp': NOW, 'ignition_state': 0}})
    alert(parked, 'A,1,low battery: 11.2V')
    assert texts(parked) == ['Alert: low battery: 11.2V'] and len(relayed) == 1


def test_an_alert_event_that_cannot_be_written_does_not_stop_the_notification(relayed, monkeypatch):
    alert(Handle(broken=True), 'A,2,tilt 12deg - possible tow/jack')
    assert len(relayed) == 1

    # Nor can one that raises outright.
    def explode(*args, **kwargs):
        raise RuntimeError('boom')

    monkeypatch.setattr(telemetry.device_events, 'record', explode)
    alert(Handle(), 'A,2,tilt 13deg - possible tow/jack')
    assert len(relayed) == 2


def test_alerts_with_no_database_still_relay(relayed):
    # How the alert tests call it.
    alert(None, 'A,2,tilt 12deg - possible tow/jack')
    assert len(relayed) == 1


# -- results and delivered commands, through process_lines ------------------------------------------------

def lines_through(handle, *lines, device=None):
    device = device or {'id': 1, 'imei': IMEI, 'name': 'Car', 'int': 3600,
                        'movement_alarm': 1}
    handle.rules.setdefault('SELECT * FROM `device`', device)
    return telemetry.process_lines(device, list(lines), '203.0.113.7', handle, Quiet())


def test_a_network_scan_is_a_result_event():
    handle = Handle()
    lines_through(handle, 'F,scan,2,234-10:1:2:3:-90:10|234-15:1:2:3:-95:12')
    assert [e[:2] for e in handle.events] == [('result', 'Network scan result received')]
    assert any('net_scan' in w[0] for w in handle.writes)


def test_a_report_is_a_result_event_named_as_it_is_stored():
    handle = Handle()
    lines_through(handle, 'F,rep,selftest,all well', 'F,rep,status,ok', 'F,rep,backlog,0',
                  'F,rep,thermal,hot')
    assert texts(handle) == ['Self test result received', 'Status result received',
                             'Backlog result received', 'Thermal result received']
    assert {e[0] for e in handle.events} == {'result'}


def test_a_report_that_is_not_stored_is_no_event():
    handle = Handle()
    lines_through(handle, 'F,rep,self test,x', 'F,rep,onlyname')
    assert handle.events == []


def test_result_events_that_cannot_be_written_do_not_lose_the_reply():
    handle = Handle(broken=True)
    reply = lines_through(handle, 'F,scan,0,', 'F,rep,status,ok')
    assert reply.startswith('1,3600,1')
    assert any('device_report' in w[0] for w in handle.writes)


def test_delivered_commands_are_collected_events_one_each():
    handle = Handle({'FROM `command` WHERE `device_id` = %s ORDER': [
        {'id': 1, 'command': 'status,gnssstat'}, {'id': 2, 'command': 'int=600'}]})
    reply = telemetry.build_response({'id': 1, 'imei': IMEI, 'int': 60, 'movement_alarm': 1},
                                     handle, Quiet())
    assert reply.startswith('1,60,1,status,gnssstat,int=600,')
    assert [e[:2] for e in handle.events] == [
        ('command', 'Command collected: status'), ('command', 'Command collected: gnssstat'),
        ('command', 'Command collected: int=600')]
    deletes = [i for i, w in enumerate(handle.writes) if w[0].startswith('DELETE FROM `command` WHERE `id`')]
    first_event = min(i for i, w in enumerate(handle.writes) if 'device_event' in w[0])
    assert deletes and deletes[0] < first_event


def test_nothing_delivered_records_nothing():
    handle = Handle()
    telemetry.build_response({'id': 1, 'imei': IMEI, 'int': 60, 'movement_alarm': 1},
                             handle, Quiet())
    assert handle.events == []


def test_expired_commands_are_events_dated_when_they_expired():
    queued = datetime.datetime(2026, 11, 4, 3, 0, 0)
    handle = Handle({'AND `timestamp` < NOW(6)': [
        {'id': 1, 'command': 'locatenow', 'timestamp': queued},
        {'id': 2, 'command': 'int=600,locate', 'timestamp': queued + datetime.timedelta(minutes=5)}]})
    telemetry.build_response({'id': 1, 'imei': IMEI, 'int': 60, 'movement_alarm': 1},
                             handle, Quiet())
    assert [(e[0], e[1], e[2], e[3]) for e in handle.events] == [
        ('command', 'Command expired: locatenow', '2026-11-04 09:00:00', None),
        ('command', 'Command expired: int=600', '2026-11-04 09:05:00', None),
        ('command', 'Command expired: locate', '2026-11-04 09:05:00', None)]
    # They are read before the sweep that removes them, and noted after it.
    assert handle.writes[0][0].startswith('DELETE FROM `command`')
    assert 'device_event' in handle.writes[1][0]


def test_the_expiry_is_read_with_the_condition_the_sweep_uses():
    seen = []
    handle = Handle({'AND `timestamp` < NOW(6)': lambda params: seen.append(params) or []})
    telemetry.build_response({'id': 4, 'imei': IMEI, 'int': 60, 'movement_alarm': 1},
                             handle, Quiet())
    sweep = [w for w in handle.writes if w[0].startswith('DELETE FROM `command` WHERE `device_id`')]
    assert seen == [(4, telemetry.COMMAND_MAX_AGE_HOURS)]
    assert sweep[0][1] == (4, telemetry.COMMAND_MAX_AGE_HOURS)


def test_an_expiry_that_cannot_be_read_still_sweeps_and_replies():
    def broken(params):
        raise RuntimeError('gone away')

    handle = Handle({'AND `timestamp` < NOW(6)': broken})
    reply = telemetry.build_response({'id': 1, 'imei': IMEI, 'int': 60, 'movement_alarm': 1},
                                     handle, Quiet())
    assert reply.startswith('1,60,1,')
    assert handle.writes[0][0].startswith('DELETE FROM `command`')


def test_command_events_that_cannot_be_written_do_not_change_the_reply():
    handle = Handle({'FROM `command` WHERE `device_id` = %s ORDER': [{'id': 1, 'command': 'status'}]},
                    broken=True)
    reply = telemetry.build_response({'id': 1, 'imei': IMEI, 'int': 60, 'movement_alarm': 1},
                                     handle, Quiet())
    assert reply.startswith('1,60,1,status,')


# -- the API ---------------------------------------------------------------------------------------------------

class Web(Handle):
    """db.web for the API: one device, its command queue, its events."""

    def __init__(self, broken=False, **device):
        super().__init__(broken=broken)
        self.device = {'id': 1, 'imei': IMEI, 'name': 'Car', 'int': 3600, 'backup_int': 0,
                       'movement_alarm': 1, 'alarm': 0, 'garage': 0, 'overnight_alarm': 0,
                       'overnight_alarm_hour_from': 23, 'overnight_alarm_hour_to': 6,
                       'track_mode': 0, 'gnss_backup': None}
        self.device.update(device)
        self.queue = []
        self.rows = []                      # device_event rows served to page()
        self.pages = []                     # the page() queries: (sql, params)

    def one(self, sql, params=None):
        if 'FROM `api_token`' in sql:
            return {'id': 1}
        if 'FROM `user`' in sql:
            return {'id': 1, 'username': 'tester', 'user_id': 'tester-credential', 'locked': 0}
        if 'FROM `device` WHERE `imei`' in sql:
            return self.device if params[0] == IMEI else None
        if 'FROM `device` WHERE `id`' in sql:
            return self.device if int(params[0]) == 1 else None
        if 'FROM `device_event` WHERE `id`' in sql:
            return next(({'at': r['at'], 'id': r['id']} for r in self.rows if r['id'] == params[0]),
                         None)
        return super().one(sql, params)

    def all(self, sql, params=None):
        if 'FROM `command`' in sql:
            return list(self.queue)
        if 'FROM `device_event`' in sql:
            self.pages.append((sql, params))
            return list(self.rows)
        if 'FROM `device` ORDER' in sql:
            return [self.device]
        return super().all(sql, params)


@pytest.fixture
def web(monkeypatch):
    handle = Web()
    monkeypatch.setattr(db, 'web', handle)
    return handle


@pytest.fixture
def client(web):
    from tracker.web import create_app

    app = create_app()
    app.config['TESTING'] = True
    return app.test_client()


@pytest.fixture
def bearer():
    return {'Authorization': 'Bearer test-token'}


@pytest.fixture
def logged_in(client):
    with client.session_transaction() as session:
        session['username'] = 'tester'
        session['credential_id'] = 'tester-credential'
        session['login_at'] = int(time.time())
    return client


def post(client, path, headers, **body):
    return client.post('/api/1.0' + path, headers=headers, json=dict(imei=IMEI, **body))


# -- the endpoint

def test_the_event_list_wants_a_login(client, web):
    response = client.get('/api/1.0/devices/events?device_id=1')
    assert response.status_code == 302 and '/login' in response.headers['Location']


def test_the_event_list_is_newest_first_in_the_devices_envelope(logged_in, web):
    web.rows = [
        {'id': 9, 'at': datetime.datetime(2026, 11, 4, 13, 5, 43), 'type': 'ignition', 'text': 'Ignition on'},
        {'id': 4, 'at': datetime.datetime(2026, 11, 4, 12, 0, 0), 'type': 'reboot', 'text': 'Rebooted'}]
    response = logged_in.get('/api/1.0/devices/events?device_id=1')
    assert response.status_code == 200
    assert response.get_json() == {'status': 'ok', 'events': [
        {'id': 9, 'at': '2026-11-04 13:05:43', 'type': 'ignition', 'text': 'Ignition on'},
        {'id': 4, 'at': '2026-11-04 12:00:00', 'type': 'reboot', 'text': 'Rebooted'}]}


def test_the_event_list_of_a_device_with_none_is_an_empty_list(logged_in, web):
    assert logged_in.get('/api/1.0/devices/events?device_id=1').get_json() == \
        {'status': 'ok', 'events': []}


def test_the_event_list_passes_the_page_and_the_device_on(logged_in, web):
    web.rows = [{'id': 40, 'at': datetime.datetime(2026, 11, 4, 12, 0, 0), 'type': 'reboot',
                 'text': 'Rebooted'}]
    logged_in.get('/api/1.0/devices/events?device_id=1&limit=25&before=40')
    (sql, params), = web.pages
    assert params == [1, datetime.datetime(2026, 11, 4, 12, 0, 0),
                      datetime.datetime(2026, 11, 4, 12, 0, 0), 40, 25]


def test_the_event_list_defaults_to_the_only_device(logged_in, web):
    assert logged_in.get('/api/1.0/devices/events').get_json()['status'] == 'ok'


def test_the_device_can_be_named_by_imei(logged_in, web):
    assert logged_in.get('/api/1.0/devices/events?imei=' + IMEI).get_json()['status'] == 'ok'


def test_an_unknown_device_is_refused_as_the_other_endpoints_do(logged_in, web):
    response = logged_in.get('/api/1.0/devices/events?device_id=99')
    assert response.status_code == 400
    assert response.get_json() == {'status': 'error', 'message': 'device not found'}


@pytest.mark.parametrize('query, message', [
    ('limit=0', 'limit must be a whole number from 1 to 500'),
    ('limit=-3', 'limit must be a whole number from 1 to 500'),
    ('limit=lots', 'limit must be a whole number from 1 to 500'),
    ('before=0', 'before must be an event id'),
    ('before=x', 'before must be an event id'),
])
def test_a_bad_page_is_refused(logged_in, web, query, message):
    response = logged_in.get('/api/1.0/devices/events?device_id=1&' + query)
    assert response.status_code == 400
    assert response.get_json() == {'status': 'error', 'message': message}


def test_the_limit_is_capped_in_the_query(logged_in, web):
    logged_in.get('/api/1.0/devices/events?device_id=1&limit=9999')
    assert web.pages[0][1] == [1, 500]


def test_devices_and_events_are_different_routes(logged_in, web):
    assert 'devices' in logged_in.get('/api/1.0/devices').get_json()


# -- queue_command

def test_queued_commands_are_events_one_per_part(client, web, bearer):
    assert post(client, '/command', bearer, command='int=600,locate').get_json() == {'status': 'ok'}
    assert [e[:2] for e in web.events] == [('command', 'Command queued: int=600'),
                                           ('command', 'Command queued: locate')]
    sql = [w[0] for w in web.writes]
    assert sql.index(next(s for s in sql if s.startswith('INSERT INTO `command`'))) < \
        sql.index(next(s for s in sql if 'device_event' in s))


def test_a_refused_command_is_no_event(client, web, bearer):
    assert post(client, '/command', bearer, command='int=5').status_code == 400
    assert post(client, '/command', bearer, command='gnssbk=2').status_code == 400
    assert web.events == []


def test_a_command_that_replaces_a_queued_one_records_both(client, web, bearer):
    web.queue = [{'id': 3, 'command': 'locate'}, {'id': 4, 'command': 'status'}]
    post(client, '/command', bearer, command='locatenow')
    assert texts(web) == ['Command replaced: locate', 'Command queued: locatenow']
    sql = [w[0] for w in web.writes]
    assert sql[0].startswith('DELETE FROM `command`')


def test_server_side_settings_are_setting_events_when_they_change(client, web, bearer):
    web.device.update(alarm=0, garage=1, backup_int=0)
    post(client, '/command', bearer,
         command='alarm=1,garage=1,overnightalarm=0,overnight_alarm_hour_from=22,backup_interval=1800')
    assert texts(web) == ['Ignition alarm on', 'Overnight alarm starts at 22:00',
                          'Backup report interval set to 1800 s']
    assert {e[0] for e in web.events} == {'setting'}
    assert not any(w[0].startswith('INSERT INTO `command`') for w in web.writes)


def test_a_setting_set_to_what_it_is_is_no_event(client, web, bearer):
    post(client, '/command', bearer, command='alarm=0,garage=0')
    assert web.events == []
    assert any(w[0].startswith('UPDATE `device`') for w in web.writes)


def test_the_gnss_backup_command_is_a_setting_event_when_it_changes(client, web, bearer):
    web.device['gnss_backup'] = 1
    post(client, '/command', bearer, command='gnssbk=0')
    assert texts(web) == ['GNSS backup supply set to off']
    web.device['gnss_backup'] = 0                                # as the UPDATE left it
    post(client, '/command', bearer, command='gnssbk=1')
    assert texts(web) == ['GNSS backup supply set to off', 'GNSS backup supply set to on']
    assert not any(w[0].startswith('INSERT INTO `command`') for w in web.writes)


def test_the_gnss_backup_command_that_changes_nothing_is_no_event(client, web, bearer):
    web.device['gnss_backup'] = 1
    post(client, '/command', bearer, command='gnssbk=1')
    assert web.events == []


def test_a_mixed_command_records_each_thing_once(client, web, bearer):
    web.device['alarm'] = 0
    post(client, '/command', bearer, command='alarm=1,status')
    assert texts(web) == ['Ignition alarm on', 'Command queued: status']


def test_an_event_that_cannot_be_written_does_not_fail_the_command(client, web, bearer):
    web.broken = True
    response = post(client, '/command', bearer, command='alarm=1,status')
    assert response.get_json() == {'status': 'ok'}
    assert any(w[0].startswith('INSERT INTO `command`') for w in web.writes)
    assert any(w[0].startswith('UPDATE `device`') for w in web.writes)


# -- cancel_command

def test_cancelling_is_an_event_for_what_was_removed(client, web, bearer):
    web.queue = [{'id': 1, 'command': 'int=600,locate'}, {'id': 2, 'command': 'status'},
                 {'id': 3, 'command': 'locate'}]
    response = post(client, '/command/cancel', bearer, command='locate')
    assert response.get_json() == {'status': 'ok', 'success': True, 'cancelled': 2}
    assert texts(web) == ['Command cancelled: locate', 'Command cancelled: locate']
    assert {e[0] for e in web.events} == {'command'}


def test_cancelling_nothing_is_no_event(client, web, bearer):
    web.queue = [{'id': 2, 'command': 'status'}]
    post(client, '/command/cancel', bearer, command='locate')
    assert web.events == []


# -- update_config

def test_a_config_change_is_an_event_per_column_that_changes(client, web, bearer):
    web.device.update(int=3600, backup_int=0, movement_alarm=1, alarm=0, garage=0,
                      overnight_alarm=0, overnight_alarm_hour_from=23, overnight_alarm_hour_to=6)
    post(client, '/config', bearer, int=900, backup=0, ma=0, al=1, ga=0, oa=1, oaf=22, oat=6)
    assert texts(web) == ['Report interval set to 900 s', 'Movement alarm off',
                          'Ignition alarm on', 'Overnight alarm on',
                          'Overnight alarm starts at 22:00']
    assert {e[0] for e in web.events} == {'setting'}
    update = next(i for i, w in enumerate(web.writes) if w[0].startswith('UPDATE `device`'))
    assert update < next(i for i, w in enumerate(web.writes) if 'device_event' in w[0])


def test_a_config_that_changes_nothing_is_no_event(client, web, bearer):
    post(client, '/config', bearer, int=3600, al=0)
    assert web.events == []


def test_a_refused_config_is_no_event(client, web, bearer):
    post(client, '/config', bearer, int=5)
    post(client, '/config', bearer, inte=5)
    assert web.events == []


def test_an_event_that_cannot_be_written_does_not_fail_the_config(client, web, bearer):
    web.broken = True
    assert post(client, '/config', bearer, int=900).get_json() == {'status': 'ok'}


# -- trackmode

def test_switching_track_mode_is_an_event_when_it_changes(logged_in, web):
    assert logged_in.post('/api/1.0/trackmode', json={'imei': IMEI, 'on': 1}).status_code == 200
    assert texts(web) == ['Track mode on']
    web.device['track_mode'] = 1
    logged_in.post('/api/1.0/trackmode', json={'imei': IMEI, 'on': 1})
    assert texts(web) == ['Track mode on']                        # already on
    logged_in.post('/api/1.0/trackmode', json={'imei': IMEI, 'on': 0})
    assert texts(web) == ['Track mode on', 'Track mode off']
    assert {e[0] for e in web.events} == {'setting'}


def test_reading_track_mode_is_no_event(logged_in, web):
    logged_in.get('/api/1.0/trackmode?imei=' + IMEI)
    assert web.events == []


def test_a_track_mode_event_that_cannot_be_written_does_not_fail_the_switch(logged_in, web):
    web.broken = True
    response = logged_in.post('/api/1.0/trackmode', json={'imei': IMEI, 'on': 1})
    assert response.get_json() == {'status': 'ok', 'track_mode': 1}
