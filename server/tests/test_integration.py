"""End-to-end against a real database.

Skipped unless ``TRACKER_TEST_DB`` is set; see conftest.py.  These are
destructive — they truncate every table they touch.
"""

from conftest import needs_db, record

from tracker import db, logs, telemetry

pytestmark = needs_db


def send(device, *lines):
    return telemetry.process_lines(device, list(lines), '203.0.113.7',
                                   db.web, logs.udp)


def last_log(database):
    return database.one('SELECT * FROM `log` ORDER BY `id` DESC LIMIT 1')


def last_journey(database):
    return database.one('SELECT * FROM `journey` ORDER BY `id` DESC LIMIT 1')


# -- storage -----------------------------------------------------------------

def test_record_is_stored(device, database):
    response = send(device, record(
        0, 51.5, -0.1, 0,
        extras=',mcc=234;mnc=10;lac=1a;cid=ff;rat=CATM1,fw=0.4.12,vs=4.54;it=30.2'))
    assert response.startswith('1,3600,')

    row = last_log(database)
    assert str(row['latitude']) == '51.5000000'
    assert row['rat'] == 'CATM1'
    assert row['fw'] == '0.4.12'
    assert str(row['vsys']) == '4.54'
    assert str(row['imu_temp']) == '30.20'
    assert row['gsm_timestamp_offset'] == 1


def test_speed_is_converted_to_mph(device, database):
    send(device, record(0, 51.5, -0.1, 1, speed=48.0))
    assert str(last_log(database)['speed']) == '29.83'


def test_sticky_fields_carry_forward(device, database):
    # The device sends these only when they change; every row still has to be
    # self-describing or a query over history is full of holes.
    send(device, record(0, 51.5, -0.1, 0,
                        extras=',mcc=234;mnc=10;lac=1a;cid=ff;rat=CATM1,fw=0.4.12'))
    send(device, record(1, 51.5, -0.1, 0))

    row = last_log(database)
    assert row['mcc'] == '234'
    assert row['cid'] == 'ff'
    assert row['rat'] == 'CATM1'
    assert row['fw'] == '0.4.12'


def test_per_packet_fields_do_not_carry_forward(device, database):
    # A stale accelerometer reading copied onto a later row would be
    # indistinguishable from a real one.
    send(device, record(0, 51.5, -0.1, 0, extras=',vs=4.54;it=30.2;ax=100'))
    send(device, record(1, 51.5, -0.1, 0))

    row = last_log(database)
    assert row['vsys'] is None
    assert row['imu_temp'] is None
    assert row['accel_x'] is None


def test_malformed_record_does_not_lose_the_batch(device, database):
    # The device already spent the radio time; dropping good fixes because one
    # neighbour was malformed is the worse outcome.
    before = database.one('SELECT COUNT(*) c FROM `log`')['c']
    send(device, 'garbage', record(0, 51.5, -0.1, 0), 'also,bad')
    assert database.one('SELECT COUNT(*) c FROM `log`')['c'] == before + 1


# -- journeys ----------------------------------------------------------------

def test_journey_opens_on_ignition(device, database):
    send(device, record(0, 51.5, -0.1, 0))
    send(device, record(1, 51.5, -0.1, 1))

    row = last_log(database)
    assert row['powered_on'] == 1
    journey = last_journey(database)
    assert journey['end_time'] is None
    assert journey['start_log_id'] == row['id']


def test_journey_closes_with_distance(device, database):
    send(device, record(0, 51.50, -0.1, 0))
    send(device, record(1, 51.50, -0.1, 1))
    send(device, record(2, 51.51, -0.1, 1, speed=48.0))
    send(device, record(3, 51.52, -0.1, 1, speed=48.0))
    send(device, record(4, 51.53, -0.1, 0))

    journey = last_journey(database)
    assert journey['end_time'] is not None
    assert journey['end_log_id'] is not None
    # 51.50 to 51.53 is about 3.34 km, roughly 2.07 miles.
    assert 1.9 <= float(journey['miles']) <= 2.2


def test_restart_within_window_continues_the_journey(device, database):
    # A fuel stop or a stop-start engine cycle should not fragment history.
    send(device, record(0, 51.50, -0.1, 0))
    send(device, record(1, 51.50, -0.1, 1))
    send(device, record(2, 51.52, -0.1, 0))
    send(device, record(3, 51.52, -0.1, 1))

    assert database.one('SELECT COUNT(*) c FROM `journey`')['c'] == 1
    assert last_journey(database)['end_time'] is None


# -- settings and commands ---------------------------------------------------

def test_device_settings_are_mirrored(device, database):
    response = send(device, record(0, 51.5, -0.1, 0,
                                   extras=',ri=1;int=900;ao=1;ma=0'))
    updated = db.lookup_device(imei=device['imei'])
    assert (updated['int'], updated['always_on'], updated['movement_alarm']) == (900, 1, 0)
    assert response.startswith('1,900,1,0')


def test_commands_are_delivered_once(device, database):
    # At-most-once by design: re-queueing is safer than replaying a poweroff.
    for command in ('locate', 'reboot'):
        database.query(
            'INSERT INTO `command` (`device_id`, `timestamp`, `command`) '
            'VALUES (%s, NOW(6), %s)', (device['id'], command))

    response = send(device, record(0, 51.5, -0.1, 0))
    assert 'locate' in response and 'reboot' in response
    assert database.one('SELECT COUNT(*) c FROM `command`')['c'] == 0

    assert 'locate' not in send(device, record(1, 51.5, -0.1, 0))


def test_fota_indication(device, database, published_for_device):
    assert ',fota=0.4.12' in send(device, record(0, 51.5, -0.1, 0))


# -- API ---------------------------------------------------------------------

def test_endpoints_require_auth(client, device):
    assert client.get('/').status_code == 302
    assert client.get('/track').status_code == 302
    assert client.get('/api/1.0/carpos').status_code == 302
    assert client.post('/api/1.0/command', json={}).status_code == 401
    assert client.get('/api/1.0/config').status_code == 401
    assert client.get('/api/1.0/config',
                      headers={'Authorization': 'Bearer wrong'}).status_code == 401


def test_config_round_trip(client, device, database, bearer):
    response = client.get('/api/1.0/config?imei=%s' % device['imei'], headers=bearer)
    assert response.get_json()['int'] == 3600

    client.post('/api/1.0/config',
                json={'imei': device['imei'], 'int': 1800}, headers=bearer)
    assert db.lookup_device(imei=device['imei'])['int'] == 1800


def test_conflicting_commands_are_deduped(client, device, database, bearer):
    # Queuing locate twice should not make the device send two positions.
    client.post('/api/1.0/command',
                json={'imei': device['imei'], 'command': 'locate'}, headers=bearer)
    client.post('/api/1.0/command',
                json={'imei': device['imei'], 'command': 'locatenow'}, headers=bearer)
    assert database.one('SELECT COUNT(*) c FROM `command`')['c'] == 1


def test_server_side_settings_are_not_queued(client, device, database, bearer):
    # Alerting on ignition is the server's job; queuing it would spend radio
    # time to no purpose.
    client.post('/api/1.0/command',
                json={'imei': device['imei'], 'command': 'alarm=1'}, headers=bearer)
    assert db.lookup_device(imei=device['imei'])['alarm'] == 1
    assert database.one('SELECT COUNT(*) c FROM `command`')['c'] == 0


def test_unknown_device_rejected(client, bearer, device):
    response = client.post('/api/1.0/command',
                           json={'imei': '999999999999999', 'command': 'locate'},
                           headers=bearer)
    assert response.get_json()['status'] == 'error'


def test_browser_endpoints(client, device, database, logged_in):
    send(device, record(0, 51.50, -0.1, 0, extras=',mcc=234;mnc=10;fw=0.4.12'))
    send(device, record(1, 51.50, -0.1, 1))
    send(device, record(2, 51.53, -0.1, 0))

    position = client.get('/api/1.0/carpos').get_json()['position']
    assert position['latitude'] == 51.53
    assert position['operator'] == 'Test Network'

    journeys = client.get('/api/1.0/journeys').get_json()['journeys']
    assert len(journeys) == 1

    points = client.get('/api/1.0/journey/%d/points' % journeys[0]['id']).get_json()['points']
    assert len(points) >= 2
    assert points[0]['operator'] == 'Test Network'

    status = client.get('/api/1.0/status?imei=%s' % device['imei']).get_json()
    assert status['firmware'] == '0.4.12'


def test_map_page_renders(client, device, database, logged_in):
    send(device, record(0, 51.5, -0.1, 0, extras=',mcc=234;mnc=10'))
    response = client.get('/track')
    assert response.status_code == 200
    assert b'AB12CDE' in response.data


def test_map_page_survives_a_device_with_no_records(client, device, database, logged_in):
    # This is the first thing a new deployment sees.
    database.query("INSERT INTO `device` (`imei`, `name`) VALUES ('350000000000001', 'New')")
    assert client.get('/track?imei=350000000000001').status_code == 200
