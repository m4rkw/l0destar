"""The home check, for one vehicle or several, each with its own home.

The configuration tests need no database.  The endpoint tests do, and are
skipped unless ``TRACKER_TEST_DB`` is set; see conftest.py.
"""

import pytest

from conftest import needs_db, record

from tracker import config, db, logs, notify, telemetry

CAR = '350000000000000'
VAN = '350000000000001'


# -- configuration -----------------------------------------------------------

def test_unset_means_nothing_to_check():
    for raw in (None, {}, []):
        assert config.home_checks(raw) == []


def test_a_single_mapping_is_one_check():
    assert config.home_checks(
        {'imei': 350000000000000, 'latitude': '51.5', 'longitude': -0.1}
    ) == [{'imei': CAR, 'latitude': 51.5, 'longitude': -0.1, 'radius_m': 300.0}]


def test_a_list_checks_every_vehicle():
    checks = config.home_checks([
        {'imei': CAR, 'latitude': 51.5, 'longitude': -0.1},
        {'imei': VAN, 'latitude': 52.0, 'longitude': -1.0, 'radius_m': 50},
    ])
    assert [(c['imei'], c['radius_m']) for c in checks] == [(CAR, 300.0), (VAN, 50.0)]


@pytest.mark.parametrize('raw', [
    'somewhere',
    [{'imei': CAR, 'latitude': 51.5}],
    [{'latitude': 51.5, 'longitude': -0.1}],
    ['not a mapping'],
])
def test_an_incomplete_entry_stops_startup(raw):
    with pytest.raises(RuntimeError):
        config.home_checks(raw)


# -- endpoint ----------------------------------------------------------------

def send(device, *lines):
    telemetry.process_lines(device, list(lines), '203.0.113.7', db.web, logs.udp)


@pytest.fixture
def alerts(monkeypatch):
    sent = []
    monkeypatch.setattr(notify, 'send',
                        lambda message, **kwargs: sent.append(message))
    return sent


@pytest.fixture
def both_homes(monkeypatch):
    monkeypatch.setattr(config, 'HOME_CHECKS', config.home_checks([
        {'imei': CAR, 'latitude': 51.5, 'longitude': -0.1},
        {'imei': VAN, 'latitude': 51.5, 'longitude': -0.1, 'radius_m': 100},
    ]))


@needs_db
def test_every_listed_vehicle_is_checked(client, bearer, device, second_device,
                                         both_homes, alerts):
    send(device, record(0, 51.5, -0.1, 0))              # at home
    send(second_device, record(0, 51.53, -0.1, 0))      # about 3.3 km away

    results = client.post('/api/1.0/home', headers=bearer).get_json()['devices']
    by_imei = {r['imei']: r for r in results}

    assert (by_imei[CAR]['at_home'], by_imei[CAR]['distance_m']) == (True, 0.0)
    assert (by_imei[VAN]['at_home'], by_imei[VAN]['name']) == (False, 'Van')
    assert 3300 < by_imei[VAN]['distance_m'] < 3400
    assert len(alerts) == 1
    assert alerts[0].startswith('Van: tracker may be stalled - vehicle is 33')


@needs_db
def test_one_vehicle_can_be_checked_alone(client, bearer, device, second_device,
                                          both_homes, alerts):
    send(device, record(0, 51.5, -0.1, 0))

    results = client.post('/api/1.0/home?imei=%s' % CAR,
                          headers=bearer).get_json()['devices']
    assert [r['imei'] for r in results] == [CAR]

    body = client.post('/api/1.0/home?imei=999999999999999', headers=bearer).get_json()
    assert body['status'] == 'error'


@needs_db
def test_garage_and_a_missing_position_raise_nothing(client, bearer, database, device,
                                                     second_device, both_homes, alerts):
    database.query('UPDATE `device` SET `garage` = 1 WHERE `imei` = %s', (CAR,))
    send(device, record(0, 52.5, -0.1, 0))              # away, but expected to be

    results = {r['imei']: r for r in
               client.post('/api/1.0/home', headers=bearer).get_json()['devices']}

    assert (results[CAR]['at_home'], results[CAR]['garage']) == (False, True)
    assert results[VAN]['error'] == 'no position recorded'
    assert alerts == []


@needs_db
def test_unconfigured(client, bearer, device, monkeypatch):
    monkeypatch.setattr(config, 'HOME_CHECKS', [])
    body = client.post('/api/1.0/home', headers=bearer).get_json()
    assert body['message'] == 'home_check not configured'
