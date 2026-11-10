"""The runtime tunables: the tu= reply token, the /config and /command API for
them, the rule grammar and the event wording.

No database: the API half runs against the fake handle of
test_device_event_hooks.py.
"""

import pytest

from test_device_event_hooks import (  # noqa: F401  (fixtures)
    IMEI, Quiet, bearer, client, post, texts, web)

from tracker import device_events as ev, telemetry

EVERY = {
    'tow_tilt_deg': 8, 'parked_impact_mg': 900, 'impact_immediate_mg': 700,
    'accel_alert_priority': -1, 'accel_alert_backoff_s': 86400,
    'obd_alerts': 'coolant>=95,rpm>6500,stft<-12.5/2',
    'ignition_on_sleep_s': 30, 'battery_warning_mv': 11900,
    'battery_poweroff_mv': 11000, 'drive_stream': 1, 'drive_stream_interval_s': 900,
    'track_period_ms': 1000, 'databuf_drain_s': 60, 'network_search_s': 3600,
    'resp_poll_s': 20, 'led_accel_wake': 0,
}
CODES = {'tow_tilt_deg': 'xtt', 'parked_impact_mg': 'xpi', 'impact_immediate_mg': 'xii',
         'accel_alert_priority': 'xap', 'accel_alert_backoff_s': 'xab',
         'obd_alerts': 'xoa', 'ignition_on_sleep_s': 'xis', 'battery_warning_mv': 'xbw',
         'battery_poweroff_mv': 'xbp', 'drive_stream': 'xds', 'drive_stream_interval_s': 'xdi',
         'track_period_ms': 'xtp', 'databuf_drain_s': 'xdd', 'network_search_s': 'xns',
         'resp_poll_s': 'xrp', 'led_accel_wake': 'xlw'}


class Bare:
    def one(self, *a, **k):
        return None

    def all(self, *a, **k):
        return []

    def query(self, *a, **k):
        return 1


def reply(**tunables):
    row = {'id': 1, 'imei': 'x', 'int': 60, 'movement_alarm': 1}
    row.update(tunables)
    return telemetry.build_response(row, Bare(), Quiet())


# -- the token -------------------------------------------------------------------

def test_the_table_matches_the_contract():
    assert {c: code for code, c, _, _ in telemetry.TUNABLES} == CODES
    assert [code for code, *_ in telemetry.TUNABLES] == list(CODES.values())


def test_no_tunable_set_is_tu_dash_after_le():
    assert reply().endswith(',gw=off,le=1,tu=-')
    assert telemetry.tunables_token({'tow_tilt_deg': None, 'obd_alerts': ''}) == '-'


def test_some_set_are_listed_in_table_order():
    assert reply(battery_warning_mv=11900, tow_tilt_deg=8, accel_alert_priority=0).endswith(
        ',le=1,tu=xtt:8;xap:0;xbw:11900')
    # Zero is a value, not "unset".
    assert telemetry.tunables_token({'drive_stream': 0}) == 'xds:0'


def test_all_set_and_the_obd_commas_become_pipes():
    token = telemetry.tunables_token(EVERY)
    assert token == ('xtt:8;xpi:900;xii:700;xap:-1;xab:86400;'
                     'xoa:coolant>=95|rpm>6500|stft<-12.5/2;xis:30;xbw:11900;xbp:11000;'
                     'xds:1;xdi:900;xtp:1000;xdd:60;xns:3600;xrp:20;xlw:0')
    assert ',' not in token
    assert reply(**EVERY).endswith(',le=1,tu=' + token)


def test_the_reply_stays_small_with_every_tunable_at_its_largest():
    rules = ','.join(['rpm>6500'] * 13) + ',a>1'
    assert len(rules) == 120 and telemetry.valid_obd_alerts(rules)
    text = reply(**dict(EVERY, obd_alerts=rules))
    assert len(text.encode()) < 400 < 1400


# -- the rule grammar --------------------------------------------------------------

@pytest.mark.parametrize('text', [
    'coolant>=90', 'coolant>=90,rpm>6500,stft<-12.5/2', 'mil=1', 'ocl>100',
    'throttle<=5.5/0.5', 'a_b1>0'])
def test_valid_rules(text):
    assert telemetry.valid_obd_alerts(text)


@pytest.mark.parametrize('text', [
    '', 'coolant', 'coolant>=', '>=90', 'Coolant>=90', 'coolant==90', 'coolant>=90,',
    ',coolant>=90', 'coolant>=90,,rpm>1', 'coolant >= 90', 'coolant>=9;0',
    'coolant>=90|rpm>1', 'coolant>=90/', 'coolant>=.5', 'rpm>1é', 'rpm>1\n',
    ','.join(['rpm>6500'] * 13) + ',a>10', None, 5, ['rpm>1']])
def test_invalid_rules(text):
    assert not telemetry.valid_obd_alerts(text)


# -- the API -----------------------------------------------------------------------

def config(client, bearer):
    return client.get('/api/1.0/config?imei=%s' % IMEI, headers=bearer).get_json()


def updates(web):
    return [w for w in web.writes if w[0].startswith('UPDATE `device`')]


def test_config_shows_null_for_every_unset_tunable(client, web, bearer):
    got = config(client, bearer)
    assert {got[code] for code in CODES.values()} == {None}
    web.device.update(EVERY)
    got = config(client, bearer)
    assert {code: got[code] for code in CODES.values()} == {
        code: EVERY[column] for column, code in CODES.items()}


def test_config_post_sets_and_clears_with_null(client, web, bearer):
    response = post(client, '/config', bearer, xtt=8, xoa='coolant>=95,rpm>6500', xbw=11900)
    assert response.get_json()['status'] == 'ok'
    sql, params = updates(web)[-1]
    assert '`tow_tilt_deg` = %s' in sql and '`obd_alerts` = %s' in sql
    assert params == [8, 'coolant>=95,rpm>6500', 11900, 1]
    assert texts(web) == ['Tow tilt alarm set to 8 deg',
                          'OBD alert rules set to coolant>=95,rpm>6500',
                          'Battery warning set to 11900 mV']

    web.device.update(tow_tilt_deg=8, obd_alerts='rpm>1')
    web.events.clear()
    assert post(client, '/config', bearer, xtt=None, xoa=None).get_json()['status'] == 'ok'
    assert updates(web)[-1][1] == [None, None, 1]
    assert texts(web) == ['Tow tilt alarm back to the built value',
                          'OBD alert rules back to the built value']
    # An empty rule list means no override as well.
    assert post(client, '/config', bearer, xoa='').get_json()['status'] == 'ok'
    assert updates(web)[-1][1] == [None, 1]


@pytest.mark.parametrize('field, value', [
    ('xtt', 31), ('xtt', -1), ('xpi', 299), ('xpi', 2501), ('xii', 1201), ('xap', 3),
    ('xap', -3), ('xab', 86401), ('xis', 9), ('xis', 601), ('xbw', 10999), ('xbw', 12501),
    ('xbp', 10499), ('xbp', 12201), ('xds', 2), ('xdi', 901), ('xtp', 199), ('xtp', 5001),
    ('xdd', 0), ('xns', 59), ('xrp', 4), ('xlw', 2),
    ('xtt', 'abc'), ('xtt', 1.5), ('xtt', True), ('xtt', ''), ('xtt', 'default'),
    ('xoa', 7), ('xoa', 'coolant'), ('xoa', 'a>1;b>2'), ('xoa', 'a>1|b>2'),
    ('xoa', 'a>1,'), ('xoa', 'a>1,b>' + '9' * 120), ('xoa', 'rpm>1é'),
])
def test_config_post_refuses_what_is_out_of_range_or_the_wrong_type(client, web, bearer,
                                                                      field, value):
    response = post(client, '/config', bearer, **{field: value})
    assert response.status_code == 400
    assert field in response.get_json()['message']
    assert updates(web) == []


def test_the_range_edges_are_accepted(client, web, bearer):
    for field, _, low, high in [t for t in telemetry.TUNABLES if t[2] is not None]:
        for value in (low, high):
            assert post(client, '/config', bearer, **{field: value}).status_code == 200


def test_battery_poweroff_must_sit_below_the_warning(client, web, bearer):
    response = post(client, '/config', bearer, xbw=11500, xbp=11500)
    assert response.status_code == 400 and 'below' in response.get_json()['message']
    assert post(client, '/config', bearer, xbw=11500, xbp=11499).status_code == 200
    # One alone is checked against the other as stored.
    web.device.update(battery_warning_mv=11500)
    assert post(client, '/config', bearer, xbp=11600).status_code == 400
    assert post(client, '/config', bearer, xbp=11000).status_code == 200
    web.device.update(battery_poweroff_mv=11400)
    assert post(client, '/config', bearer, xbw=11300).status_code == 400
    # Clearing one is always fine.
    assert post(client, '/config', bearer, xbw=None).status_code == 200


def test_command_sets_a_tunable_and_never_queues_it(client, web, bearer):
    response = post(client, '/command', bearer,
                    command='tow_tilt_deg=8,obd_alerts=coolant>=95|rpm>6500,locate')
    assert response.get_json()['status'] == 'ok'
    assert updates(web)[-1][1] == [8, 'coolant>=95,rpm>6500', 1]
    queued = [w for w in web.writes if w[0].startswith('INSERT INTO `command`')]
    assert [w[1][1] for w in queued] == ['locate']


def test_command_clears_with_default_or_nothing(client, web, bearer):
    web.device.update(tow_tilt_deg=8, obd_alerts='rpm>1', drive_stream=1)
    assert post(client, '/command', bearer,
                command='tow_tilt_deg=default,obd_alerts=default,drive_stream=').status_code == 200
    assert updates(web)[-1][1] == [None, None, None, 1]
    assert texts(web) == ['Tow tilt alarm back to the built value',
                          'OBD alert rules back to the built value',
                          'Drive stream back to the built value']
    assert post(client, '/command', bearer, command='obd_alerts=').status_code == 200


@pytest.mark.parametrize('command', [
    'tow_tilt_deg=31', 'tow_tilt_deg=x', 'battery_warning_mv=1', 'obd_alerts=nonsense',
    'obd_alerts=a>1;b>2', 'accel_alert_priority=3'])
def test_command_refuses_bad_values(client, web, bearer, command):
    assert post(client, '/command', bearer, command=command).status_code == 400
    assert updates(web) == []


def test_command_checks_the_battery_pair(client, web, bearer):
    assert post(client, '/command', bearer,
                command='battery_warning_mv=11000,battery_poweroff_mv=11000').status_code == 400
    assert updates(web) == []


# -- the words ----------------------------------------------------------------------

@pytest.mark.parametrize('column, value, text', [
    ('tow_tilt_deg', 8, 'Tow tilt alarm set to 8 deg'),
    ('tow_tilt_deg', None, 'Tow tilt alarm back to the built value'),
    ('parked_impact_mg', 900, 'Parked impact threshold set to 900 mg'),
    ('impact_immediate_mg', 0, 'Immediate impact report threshold set to 0 mg'),
    ('accel_alert_priority', -1, 'Motion alert priority set to -1'),
    ('accel_alert_backoff_s', 600, 'Motion alert backoff set to 600 s'),
    ('obd_alerts', 'coolant>=95', 'OBD alert rules set to coolant>=95'),
    ('obd_alerts', None, 'OBD alert rules back to the built value'),
    ('ignition_on_sleep_s', 30, 'Ignition-on sleep interval set to 30 s'),
    ('battery_warning_mv', 11900, 'Battery warning set to 11900 mV'),
    ('battery_poweroff_mv', 11000, 'Battery power-off set to 11000 mV'),
    ('drive_stream', 1, 'Drive stream on'),
    ('drive_stream', 0, 'Drive stream off'),
    ('drive_stream', None, 'Drive stream back to the built value'),
    ('drive_stream_interval_s', 5, 'Drive stream limited to one record every 5 s'),
    ('drive_stream_interval_s', 0, 'Drive stream not limited'),
    ('drive_stream_interval_s', None, 'Drive stream limit back to the built value'),
    ('track_period_ms', 1000, 'Track period set to 1000 ms'),
    ('databuf_drain_s', 60, 'Data buffer drain set to 60 s'),
    ('network_search_s', 3600, 'Network search timeout set to 3600 s'),
    ('resp_poll_s', 20, 'Response poll set to 20 s'),
    ('led_accel_wake', 1, 'Wake-on-motion LED on'),
    ('led_accel_wake', None, 'Wake-on-motion LED back to the built value'),
])
def test_setting_text(column, value, text):
    assert ev.setting_text(column, value) == text


def test_every_tunable_has_wording():
    assert set(ev.TUNABLE_TEXT) == set(telemetry.TUNABLE_COLUMNS)


# -- tb=: the values the unit was built with ----------------------------------------

TB = ('xtt:8;xpi:900;xii:700;xap:-1;xab:86400;xoa:coolant>=95|rpm>6500;xis:30;'
      'xbw:11900;xbp:11000;xds:1;xdi:900;xtp:1000;xdd:60;xns:3600;xrp:20;xlw:0')


def test_tb_parses_every_code_and_turns_pipes_into_commas():
    built = telemetry.parse_tunables_built(TB)
    assert built['xoa'] == 'coolant>=95,rpm>6500'
    assert built['xap'] == -1 and built['xlw'] == 0
    assert set(built) == set(CODES.values())
    assert telemetry.tunables_built_text(built) == TB


def test_tb_empty_xoa_is_an_empty_string():
    assert telemetry.parse_tunables_built('xoa:;xtt:5') == {'xoa': '', 'xtt': 5}


def test_tb_ignores_unknown_codes_and_bad_values():
    got = telemetry.parse_tunables_built('zzz:1;xtt:abc;xpi:900;xii:;xab:1.5;nonsense;xtt:7;xoa:a b')
    assert got == {'xpi': 900, 'xtt': 7}
    assert telemetry.parse_tunables_built('garbage') == {}
    assert telemetry.tunables_built_text({}) is None


def test_tb_partial_is_kept_as_far_as_it_goes():
    assert telemetry.parse_tunables_built('xtt:8;xpi:9') == {'xtt': 8, 'xpi': 9}


def test_effective_value_is_override_then_built_then_null():
    device = {'tunables_built': 'xtt:8;xpi:900;xoa:a>1|b<2', 'tow_tilt_deg': 12,
              'obd_alerts': None}
    built = telemetry.tunables_built(device)
    assert built['xtt'] == 8 and built['xpi'] == 900 and built['xii'] is None
    assert built['xoa'] == 'a>1,b<2'
    now = telemetry.tunables_effective(device)
    assert now['xtt'] == 12          # override wins
    assert now['xpi'] == 900         # built used when no override
    assert now['xoa'] == 'a>1,b<2'
    assert now['xii'] is None        # neither
    assert set(now) == set(built) == set(CODES.values())


def test_a_zero_override_still_wins():
    device = {'tunables_built': 'xds:1', 'drive_stream': 0}
    assert telemetry.tunables_effective(device)['xds'] == 0


def test_config_has_the_built_and_effective_objects(client, web, bearer):
    got = config(client, bearer)
    assert set(got['tunables_built']) == set(got['tunables']) == set(CODES.values())
    assert set(got['tunables_built'].values()) == {None} == set(got['tunables'].values())
    web.device.update(tunables_built=TB, tow_tilt_deg=12, obd_alerts='rpm>1')
    got = config(client, bearer)
    assert got['tunables']['xtt'] == 12 and got['tunables_built']['xtt'] == 8
    assert got['tunables']['xoa'] == 'rpm>1'
    assert got['tunables_built']['xoa'] == 'coolant>=95,rpm>6500'
    assert got['tunables']['xpi'] == 900
    assert got['xtt'] == 12 and got['xpi'] is None   # raw fields unchanged


def test_devices_summary_has_the_same_objects():
    from tracker.web import devices
    row = {'id': 1, 'imei': 'x', 'tunables_built': 'xtt:8;xds:1', 'drive_stream': 0}
    got = devices.summary(row, Bare())
    assert got['tunables']['xtt'] == 8 and got['tunables']['xds'] == 0
    assert got['tunables_built']['xds'] == 1 and got['tunables_built']['xii'] is None
    assert got['tunables']['xii'] is None
    assert got['drive_stream'] == 0 and got['tow_tilt_deg'] is None
