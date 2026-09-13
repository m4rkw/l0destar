"""Several vehicles on one server: the device list, landing on the right map,
and which requests may fall back to a default device.

Skipped unless ``TRACKER_TEST_DB`` is set; see conftest.py.
"""

import pytest

from conftest import needs_db, record

from tracker import config, db, logs, telemetry

pytestmark = needs_db

CAR = '350000000000000'
VAN = '350000000000001'


def send(device, *lines):
    return telemetry.process_lines(device, list(lines), '203.0.113.7',
                                   db.web, logs.udp)


def location(response):
    assert response.status_code == 302
    return response.headers['Location']


@pytest.fixture
def no_default(monkeypatch):
    """No default_device configured."""
    monkeypatch.setattr(config, 'DEFAULT_DEVICE_IMEI', '')


# -- the device list ---------------------------------------------------------

def test_device_list_shows_every_device(client, logged_in, device, second_device):
    send(device, record(0, 51.5, -0.1, 1, speed=48.0,
                        extras=',mcc=234;mnc=10;rat=CATM1,fw=0.4.12'))
    page = client.get('/devices').get_data(as_text=True)

    assert 'AB12CDE' in page and 'XY34ZZZ' in page
    assert 'href="/track?imei=%s"' % CAR in page
    assert 'href="/track?imei=%s"' % VAN in page
    assert '0.4.12' in page and 'Test Network' in page and 'CATM1' in page
    assert 'never' in page          # the van has not reported


def test_device_list_says_how_to_enrol_when_empty(client, logged_in, database):
    assert 'adddevice.py' in client.get('/devices').get_data(as_text=True)


def test_devices_api_describes_each_device(client, logged_in, device, second_device):
    send(device, record(0, 51.5, -0.1, 0,
                        extras=',mcc=234;mnc=10;rat=CATM1,fw=0.4.12'))
    listed = {d['imei']: d
              for d in client.get('/api/1.0/devices').get_json()['devices']}

    assert set(listed) == {CAR, VAN}
    assert set(listed[CAR]) == {
        'id', 'imei', 'name', 'registration', 'last_seen', 'latitude',
        'longitude', 'speed', 'battery_level', 'ignition_state', 'fw', 'rat',
        'operator', 'track_mode',
    }
    car, van = listed[CAR], listed[VAN]
    assert (car['name'], car['registration']) == ('Car', 'AB12CDE')
    assert (car['latitude'], car['longitude']) == (51.5, -0.1)
    assert (car['battery_level'], car['ignition_state']) == (12.4, 0)
    assert (car['fw'], car['rat'], car['operator']) == ('0.4.12', 'CATM1', 'Test Network')
    assert car['last_seen'] is not None
    assert (van['last_seen'], van['latitude'], van['speed']) == (None, None, None)


# -- landing -----------------------------------------------------------------

def test_landing_opens_the_default_device(client, logged_in, device, second_device):
    assert location(client.get('/')).endswith('/track?imei=%s' % CAR)


def test_landing_opens_the_only_device(client, logged_in, device, no_default):
    assert location(client.get('/')).endswith('/track?imei=%s' % CAR)


def test_a_default_that_is_not_enrolled_is_ignored(client, logged_in, device, monkeypatch):
    monkeypatch.setattr(config, 'DEFAULT_DEVICE_IMEI', '000000000000000')
    assert location(client.get('/')).endswith('/track?imei=%s' % CAR)


def test_several_devices_and_no_default_open_the_list(client, logged_in, device,
                                                      second_device, no_default):
    assert location(client.get('/')).endswith('/devices')
    assert location(client.get('/track')).endswith('/devices')


def test_map_page_for_an_unknown_device_opens_the_list(client, logged_in, device):
    assert location(client.get('/track?imei=999999999999999')).endswith('/devices')


def test_map_page_names_its_device_and_links_the_list(client, logged_in, device,
                                                      second_device):
    page = client.get('/track?imei=%s' % VAN).get_data(as_text=True)
    assert 'id="device_imei" value="%s"' % VAN in page
    assert 'href="/devices"' in page


def test_map_page_for_a_lone_device_has_no_list_link(client, logged_in, device):
    page = client.get('/track').get_data(as_text=True)
    assert 'id="device_imei" value="%s"' % CAR in page
    assert 'href="/devices"' not in page


# -- which device a request is about ----------------------------------------

def test_reads_fall_back_to_the_only_device(client, logged_in, device, no_default):
    send(device, record(0, 51.5, -0.1, 0))
    assert client.get('/api/1.0/carpos').get_json()['position']['latitude'] == 51.5


def test_reads_among_several_devices_must_name_one(client, logged_in, device,
                                                   second_device, no_default):
    send(device, record(0, 51.5, -0.1, 0))
    body = client.get('/api/1.0/carpos').get_json()
    assert body['message'] == 'imei or device_id required'

    for query, headers in (('?imei=%s' % CAR, {}),
                           ('?device_id=%d' % device['id'], {}),
                           ('', {'X-Imei': CAR})):
        body = client.get('/api/1.0/carpos' + query, headers=headers).get_json()
        assert body['position']['latitude'] == 51.5


def test_a_named_device_is_never_swapped_for_the_default(client, logged_in, device):
    send(device, record(0, 51.5, -0.1, 0))
    body = client.get('/api/1.0/carpos?imei=999999999999999').get_json()
    assert (body['status'], body['message']) == ('error', 'device not found')


def test_writes_must_name_their_device(client, logged_in, bearer, device):
    # One device and a default configured: a fallback would be right today
    # and wrong the day a second vehicle is enrolled.
    required = 'imei or device_id required'
    assert client.post('/api/1.0/config', json={'int': 60},
                       headers=bearer).get_json()['message'] == required
    assert client.post('/api/1.0/command', json={'command': 'locate'},
                       headers=bearer).get_json()['message'] == required
    assert client.post('/api/1.0/trackmode',
                       json={'on': 1}).get_json()['message'] == required
    unchanged = db.lookup_device(imei=CAR)
    assert (unchanged['int'], unchanged['track_mode']) == (3600, 0)
    assert db.web.one('SELECT COUNT(*) AS `n` FROM `command`')['n'] == 0

    assert client.post('/api/1.0/config', json={'imei': CAR, 'int': 60},
                       headers=bearer).get_json()['status'] == 'ok'
    assert client.post('/api/1.0/command', json={'command': 'locate'},
                       headers=dict(bearer, **{'X-Imei': CAR})).get_json()['status'] == 'ok'
    assert client.post('/api/1.0/trackmode?imei=%s' % CAR,
                       json={'on': 1}).get_json()['track_mode'] == 1
    changed = db.lookup_device(imei=CAR)
    assert (changed['int'], changed['track_mode']) == (60, 1)
    assert db.web.one('SELECT COUNT(*) AS `n` FROM `command`')['n'] == 1

    # Reading the switch back may still fall back.
    assert client.get('/api/1.0/trackmode').get_json()['track_mode'] == 1


def test_journey_points_stay_with_their_device(client, logged_in, device, second_device):
    send(device, record(0, 51.50, -0.1, 0))
    send(device, record(1, 51.50, -0.1, 1))
    send(device, record(2, 51.53, -0.1, 0))
    journey = db.web.one('SELECT `id` FROM `journey` WHERE `device_id` = %s',
                         (device['id'],))['id']
    url = '/api/1.0/journey/%d/points' % journey

    assert client.get(url + '?imei=%s' % CAR).get_json()['status'] == 'ok'
    assert client.get(url + '?imei=%s' % VAN).get_json()['message'] == 'journey not found'
    assert client.get(url).get_json()['status'] == 'ok'
