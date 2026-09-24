"""End-to-end against a real database.

Skipped unless ``TRACKER_TEST_DB`` is set; see conftest.py.  These are
destructive — they truncate every table they touch.
"""

import re

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

def test_wake_figure_lands_on_the_record_it_names(device, database):
    # A wake cannot measure itself, so the figure arrives on a later record
    # and names the one it belongs to.
    send(device, record(0, 51.5, -0.1, 0, extras=',rid=100'))
    first = last_log(database)
    assert first['rec_id'] == 100
    assert first['wake_ms'] is None

    send(device, record(60, 51.5, -0.1, 0, extras=',rid=101,wt=100:73412'))
    second = last_log(database)

    updated = database.one('SELECT * FROM `log` WHERE `id` = %s', (first['id'],))
    assert updated['wake_ms'] == 73412
    # ...and never on the record carrying it, whose own wake is still running.
    assert second['wake_ms'] is None
    # The uptime column of the same-ish name is untouched.
    assert updated['waketime'] == first['waketime']


def test_wake_figure_skips_over_an_intervening_record(device, database):
    # The named record is not always the one before: anything the device sent
    # in between — a backlog flush, an ignition change — must not absorb it.
    send(device, record(0, 51.5, -0.1, 0, extras=',rid=100'))
    target = last_log(database)
    send(device, record(30, 51.5, -0.1, 1, extras=',rid=101'))
    between = last_log(database)
    send(device, record(60, 51.5, -0.1, 0, extras=',rid=102,wt=100:73412'))

    assert database.one('SELECT * FROM `log` WHERE `id` = %s',
                        (target['id'],))['wake_ms'] == 73412
    assert database.one('SELECT * FROM `log` WHERE `id` = %s',
                        (between['id'],))['wake_ms'] is None


def test_wake_figure_waits_for_a_record_that_has_not_arrived(device, database):
    # The case the tag exists for: the wake's own send failed, so its record
    # is in the device's backlog and is flushed BEHIND the record describing
    # it.  The figure is held until that record lands.
    send(device, record(60, 51.5, -0.1, 0, extras=',rid=101,wt=100:73412'))
    held = database.one('SELECT * FROM `device` WHERE `id` = %s', (device['id'],))
    assert held['wake_pending_rec_id'] == 100
    assert held['wake_pending_ms'] == 73412

    # ...and the backlogged record arrives.
    send(device, record(0, 51.5, -0.1, 0, extras=',rid=100'))
    late = last_log(database)
    assert late['rec_id'] == 100
    assert late['wake_ms'] == 73412

    cleared = database.one('SELECT * FROM `device` WHERE `id` = %s', (device['id'],))
    assert cleared['wake_pending_rec_id'] is None
    assert cleared['wake_pending_ms'] is None


def test_held_wake_figure_goes_only_to_its_own_record(device, database):
    # Another record arriving first must not absorb a held figure.
    send(device, record(60, 51.5, -0.1, 0, extras=',rid=101,wt=100:73412'))
    send(device, record(70, 51.5, -0.1, 0, extras=',rid=102'))
    assert last_log(database)['wake_ms'] is None
    still_held = database.one('SELECT * FROM `device` WHERE `id` = %s',
                              (device['id'],))
    assert still_held['wake_pending_rec_id'] == 100


def test_wake_figure_does_not_overwrite(device, database):
    # A duplicate datagram must not replace a figure already stored.
    send(device, record(0, 51.5, -0.1, 0, extras=',rid=100'))
    first = last_log(database)
    send(device, record(60, 51.5, -0.1, 0, extras=',rid=101,wt=100:73412'))
    send(device, record(61, 51.5, -0.1, 0, extras=',rid=102,wt=100:999'))

    updated = database.one('SELECT * FROM `log` WHERE `id` = %s', (first['id'],))
    assert updated['wake_ms'] == 73412


def test_wake_figure_implausible_is_dropped(device, database):
    # Two days awake is a corrupt field, and storing it would skew every
    # average taken over the column.
    send(device, record(0, 51.5, -0.1, 0, extras=',rid=100'))
    first = last_log(database)
    send(device, record(60, 51.5, -0.1, 0,
                        extras=',rid=101,wt=100:%d' % (telemetry.WAKE_MS_MAX + 1)))

    updated = database.one('SELECT * FROM `log` WHERE `id` = %s', (first['id'],))
    assert updated['wake_ms'] is None
    assert database.one('SELECT * FROM `device` WHERE `id` = %s',
                        (device['id'],))['wake_pending_rec_id'] is None


def test_wake_figure_naming_its_own_record_is_dropped(device, database):
    # A record cannot describe the wake that sent it.
    send(device, record(0, 51.5, -0.1, 0, extras=',rid=100,wt=100:73412'))
    assert last_log(database)['wake_ms'] is None
    assert database.one('SELECT * FROM `device` WHERE `id` = %s',
                        (device['id'],))['wake_pending_rec_id'] is None


def test_attach_split_lands_with_the_wake(device, database):
    send(device, record(0, 51.5, -0.1, 0, extras=',rid=100'))
    first = last_log(database)
    send(device, record(60, 51.5, -0.1, 0, extras=',rid=101,wt=100:44620:41180'))

    updated = database.one('SELECT * FROM `log` WHERE `id` = %s', (first['id'],))
    assert updated['wake_ms'] == 44620
    assert updated['attach_ms'] == 41180


def test_attach_split_survives_being_held(device, database):
    # Out of order, as a failed wake's record always is: both parts wait.
    send(device, record(60, 51.5, -0.1, 0, extras=',rid=100,wt=99:44620:41180'))
    held = database.one('SELECT * FROM `device` WHERE `id` = %s', (device['id'],))
    assert held['wake_pending_ms'] == 44620
    assert held['wake_pending_attach_ms'] == 41180

    send(device, record(0, 51.5, -0.1, 0, extras=',rid=99'))
    late = last_log(database)
    assert late['wake_ms'] == 44620
    assert late['attach_ms'] == 41180


def test_attach_longer_than_the_wake_is_dropped(device, database):
    # The attach is part of the wake, so it cannot exceed it; the total is
    # still worth keeping.
    send(device, record(0, 51.5, -0.1, 0, extras=',rid=100'))
    first = last_log(database)
    send(device, record(60, 51.5, -0.1, 0, extras=',rid=101,wt=100:4893:9999'))

    updated = database.one('SELECT * FROM `log` WHERE `id` = %s', (first['id'],))
    assert updated['wake_ms'] == 4893
    assert updated['attach_ms'] is None


def test_signal_quality_is_stored(device, database):
    send(device, record(0, 51.5, -0.1, 0,
                        extras=',mcc=234;mnc=10;lac=1a;cid=ff;rat=CATM1'
                               ';rsrp=-87;snr=7;band=20'))
    row = last_log(database)
    assert row['rsrp'] == -87
    assert row['snr'] == 7
    assert row['band'] == 20


def test_connection_evaluation_is_stored(device, database):
    send(device, record(0, 51.5, -0.1, 0,
                        extras=',rsrp=-111;snr=-5;band=20'
                               ';pathloss=128;rsrq=-145;ce=1;txrep=4'))
    row = last_log(database)
    assert row['pathloss'] == 128
    assert float(row['rsrq']) == -14.5
    assert row['ce_level'] == 1
    assert row['tx_rep'] == 4


def test_evaluation_fields_are_not_carried_forward(device, database):
    # They are measurements of a moment, and the modem often cannot run one.
    send(device, record(0, 51.5, -0.1, 0,
                        extras=',rsrp=-111;snr=-5;band=20'
                               ';pathloss=128;rsrq=-145;ce=1;txrep=4'))
    send(device, record(1, 51.5, -0.1, 0, extras=',rsrp=-109;snr=-3;band=20'))
    row = last_log(database)
    assert row['rsrp'] == -109          # the free reading still lands
    assert row['pathloss'] is None      # the evaluation did not run
    assert row['rsrq'] is None


def test_signal_quality_is_not_carried_forward(device, database):
    # Unlike the cell identity it rides with: signal moves continuously, so a
    # copied value would read as a measurement that was never taken.
    send(device, record(0, 51.5, -0.1, 0,
                        extras=',mcc=234;mnc=10;lac=1a;cid=ff;rat=CATM1'
                               ';rsrp=-87;snr=7;band=20'))
    send(device, record(1, 51.5, -0.1, 0))
    row = last_log(database)
    assert row['rsrp'] is None and row['snr'] is None and row['band'] is None
    assert row['rat'] == 'CATM1'      # the cell identity still carries


def test_record_without_an_id_still_stores(device, database):
    # rid= is new; a record built before it existed must not be rejected.
    send(device, record(0, 51.5, -0.1, 0))
    row = last_log(database)
    assert row['rec_id'] is None
    assert row['wake_ms'] is None


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


def test_hdop_arrives_in_tenths(device, database):
    # The firmware sends HDOP x 10: 12 on the wire is an HDOP of 1.2.
    send(device, record(0, 51.5, -0.1, 0, hdop=12))
    assert str(last_log(database)['hdop']) == '1.20'


def test_obd_fields_are_stored_scaled(device, database):
    send(device, record(0, 51.5, -0.1, 1, speed=40.0,
                        extras=',orpm=850;ospd=48;ocl=87;old=235;omaf=1234'))
    row = last_log(database)
    assert row['obd_rpm'] == 850
    assert str(row['obd_speed']) == '29.83'
    assert row['obd_coolant'] == 87
    assert str(row['obd_load']) == '23.5'
    assert str(row['obd_maf']) == '12.34'
    assert str(row['combined_speed']) == '29.83'   # the ECU's figure, not GNSS


def test_obd_fields_do_not_carry_forward(device, database):
    send(device, record(0, 51.5, -0.1, 1, extras=',orpm=850'))
    send(device, record(1, 51.5, -0.1, 1))
    row = last_log(database)
    assert row['obd_rpm'] is None
    assert str(row['combined_speed']) == str(row['speed'])


# -- fault codes -------------------------------------------------------------

def active_codes(database):
    return sorted(r['code'] for r in database.all(
        'SELECT `code` FROM `dtc` WHERE `active` = 1'))


def test_dtc_report_raises_and_clears(device, database):
    send(device, 'D,P0133,P0420')
    assert active_codes(database) == ['P0133', 'P0420']

    # The report is the complete set, so a missing code has cleared.
    send(device, 'D,P0420')
    assert active_codes(database) == ['P0420']
    cleared = database.one("SELECT * FROM `dtc` WHERE `code` = 'P0133'")
    assert cleared['active'] == 0 and cleared['cleared_at'] is not None

    # "D," alone means no stored codes.  Rows are kept as history.
    send(device, 'D,')
    assert active_codes(database) == []
    assert database.one('SELECT COUNT(*) AS n FROM `dtc`')['n'] == 2


def test_dtc_report_drops_junk_codes(device, database):
    send(device, 'D,P0133,bogus,X9999')
    assert active_codes(database) == ['P0133']


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
    assert (updated['int'], updated['movement_alarm']) == (900, 0)
    assert response.startswith('1,900,0')


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


def test_map_page_runs_no_inline_script(client, device, database, logged_in):
    # deploy/nginx.conf.example allows scripts only from files, so an inline
    # <script> would silently never run.
    page = client.get('/track').get_data(as_text=True)
    assert re.search(r'<script(?![^>]*\bsrc=)[^>]*>', page) is None
    assert 'data-maps="0"' in page      # no key configured in the tests


def test_map_page_survives_a_device_with_no_records(client, device, database, logged_in):
    # This is the first thing a new deployment sees.
    database.query("INSERT INTO `device` (`imei`, `name`) VALUES ('350000000000001', 'New')")
    assert client.get('/track?imei=350000000000001').status_code == 200
