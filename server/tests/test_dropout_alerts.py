"""Dropout and watchdog-reset alerts: raised from the records the server
stores.  No database: the alert function is called with the rows it is given,
and notify.send and device_events.record are stubbed."""

import datetime
import os

import pytest

from tracker import device_events, telemetry
from tracker.web import api

DEV = {'id': 1, 'imei': '350000000000001', 'name': 'Car'}


@pytest.fixture
def seen(monkeypatch):
    sent, events = [], []
    monkeypatch.setattr(telemetry.notify, 'send',
                        lambda message, **kw: sent.append((message, kw.get('priority'))))
    monkeypatch.setattr(device_events, 'record',
                        lambda database, device_id, kind, text, at=None, ref=None:
                        events.append((kind, text, ref)))
    telemetry._last_dropout_alert.clear()
    return sent, events


def row(second, ign=1):
    return {'ignition_state': ign,
            'gsm_timestamp': datetime.datetime(2026, 11, 21, 10, 0, 0)
            + datetime.timedelta(seconds=second)}


def entry(second, ign=1, **extra):
    return dict({'ignition_state': ign,
                 'gsm_timestamp': (datetime.datetime(2026, 11, 21, 10, 0, 0)
                                   + datetime.timedelta(seconds=second)
                                   ).strftime('%Y-%m-%d %H:%M:%S.%f')}, **extra)


def run(device, previous, new):
    telemetry._dropout_and_reset_alerts(None, device, previous, new,
                                        datetime.datetime(2026, 11, 21, 10, 5))


def test_dropout_raised_with_times(seen):
    sent, events = seen
    run(DEV, row(0), entry(45))
    assert sent == [('Car dropout: 45 s without data (10:00:00-10:00:45)', 0)]
    assert events[0][:2] == ('other', 'Dropout: 45 s without data')


def test_below_threshold_is_quiet(seen):
    run(DEV, row(0), entry(29))
    assert seen == ([], [])


def test_threshold_is_per_device(seen):
    run(dict(DEV, alert_dropout_s=60), row(0), entry(45))
    assert seen == ([], [])
    run(dict(DEV, alert_dropout_s=60), row(0), entry(60))
    assert len(seen[0]) == 1


def test_threshold_zero_turns_it_off_without_an_event(seen):
    run(dict(DEV, alert_dropout_s=0), row(0), entry(600))
    assert seen == ([], [])


@pytest.mark.parametrize('prev_ign, new_ign', [(0, 1), (1, 0), (0, 0)])
def test_not_a_drive_is_quiet(seen, prev_ign, new_ign):
    run(DEV, row(0, prev_ign), entry(600, new_ign))
    assert seen == ([], [])


def test_first_record_has_no_gap(seen):
    run(DEV, None, entry(600))
    assert seen == ([], [])


def test_priority_is_used(seen):
    run(dict(DEV, alert_dropout_priority=1), row(0), entry(90))
    assert seen[0][0][1] == 1


def test_rate_limited_but_every_dropout_is_an_event(seen):
    sent, events = seen
    run(DEV, row(0), entry(40))
    run(DEV, row(100), entry(160))
    assert len(sent) == 1
    assert [e[1] for e in events] == ['Dropout: 40 s without data',
                                      'Dropout: 60 s without data']
    assert events[0][2] != events[1][2]
    telemetry._last_dropout_alert[1] -= telemetry.DROPOUT_ALERT_INTERVAL_S
    run(DEV, row(200), entry(260))
    assert len(sent) == 2


def test_rate_limit_is_per_device(seen):
    run(DEV, row(0), entry(40))
    run(dict(DEV, id=2), row(0), entry(40))
    assert len(seen[0]) == 2


@pytest.mark.parametrize('rst, word', [
    ('wdt', 'watchdog'),
    ('wdt+isr:352', 'watchdog'),
    ('pin+sw+wdt', 'watchdog'),
    ('lockup', 'CPU lockup'),
])
def test_watchdog_reset_alert(seen, rst, word):
    sent, events = seen
    run(DEV, row(0, 0), entry(5, rst=rst))
    assert sent == [('Car reset: %s' % word, 0)]


@pytest.mark.parametrize('rst', ['sw', 'pin', 'pin+sw', 'pin+sw+stall:4aa02/3e254'])
def test_other_resets_are_quiet(seen, rst):
    run(DEV, row(0, 0), entry(5, rst=rst))
    assert seen == ([], [])


def test_reset_carries_gap_and_version_and_suppresses_the_dropout(seen):
    sent, events = seen
    run(DEV, row(0), entry(95, rst='wdt', fw='0.4.130'))
    assert sent == [('Car reset: watchdog, 95 s without data (fw 0.4.130)', 0)]
    assert events == []


def test_reset_priority_and_switch(seen):
    sent, _ = seen
    run(dict(DEV, alert_reset_priority=2), row(0), entry(5, rst='lockup'))
    assert sent == [('Car reset: CPU lockup, 5 s without data', 2)]
    run(dict(DEV, alert_reset=0), row(0), entry(5, rst='lockup'))
    assert len(sent) == 1
    run(dict(DEV, alert_reset=0), row(0), entry(500, rst='wdt'))
    assert len(sent) == 1          # still not a dropout alert either


# -- settings ----------------------------------------------------------------

@pytest.mark.parametrize('column, value, good', [
    ('alert_dropout_s', 0, 0), ('alert_dropout_s', 5, 5),
    ('alert_dropout_s', '60', 60), ('alert_dropout_s', 3600, 3600),
    ('alert_dropout_s', 'default', 30),
    ('alert_dropout_s', 4, None), ('alert_dropout_s', 3601, None),
    ('alert_dropout_s', -1, None), ('alert_dropout_s', 1.5, None),
    ('alert_dropout_priority', -2, -2), ('alert_dropout_priority', 2, 2),
    ('alert_dropout_priority', 3, None), ('alert_dropout_priority', '-3', None),
    ('alert_dropout_priority', 'default', 0),
    ('alert_reset', 0, 0), ('alert_reset', 1, 1), ('alert_reset', 2, None),
    ('alert_reset', True, None), ('alert_reset', 'default', 1),
    ('alert_reset_priority', -1, -1), ('alert_reset_priority', 9, None),
])
def test_setting_validation(column, value, good):
    assert api._setting(column, value) == good


def test_setting_error_messages():
    assert 'whole number from 0 to 1' in api._setting_error(
        'alert_reset', 'alert_reset').get_json()['message']
    assert api._setting_error('alert_dropout_s', 'alert_dropout_s').get_json()[
        'message'] == 'alert_dropout_s must be 0 or a whole number from 5 to 3600'


def test_settings_are_server_side_and_in_config():
    for name in ('alert_dropout_s', 'alert_dropout_priority', 'alert_reset',
                 'alert_reset_priority'):
        assert api.SERVER_SIDE_SETTINGS[name] == name
        assert api.CONFIG_FIELDS[name] == name
        assert api.ALERT_DEFAULTS[name] == {'alert_dropout_s': 30, 'alert_reset': 1}.get(name, 0)


@pytest.mark.parametrize('column, value, text', [
    ('alert_dropout_s', 60, 'Dropout alert threshold set to 60 s'),
    ('alert_dropout_s', 0, 'Dropout alerts disabled'),
    ('alert_dropout_priority', -1, 'Dropout alert priority set to -1'),
    ('alert_reset', 1, 'Watchdog reset alerts enabled'),
    ('alert_reset', 0, 'Watchdog reset alerts disabled'),
    ('alert_reset_priority', 2, 'Watchdog reset alert priority set to 2'),
])
def test_setting_text(column, value, text):
    assert device_events.setting_text(column, value) == text


def test_migration_and_schema_have_the_columns():
    here = os.path.dirname(os.path.dirname(__file__))
    migration = open(os.path.join(
        here, 'migrations', '2026-11-21_dropout_reset_alerts.sql')).read()
    schema = open(os.path.join(here, 'schema.sql')).read()
    for column in ('alert_dropout_s', 'alert_dropout_priority', 'alert_reset',
                   'alert_reset_priority'):
        assert 'ADD COLUMN IF NOT EXISTS `%s`' % column in migration
        assert '`%s' % column in schema
