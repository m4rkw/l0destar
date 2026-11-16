"""OBD PID selection and the supported-PID scan: the catalogue, osp= and the
o<pid>= values, the xop tunable, the os= reply token and the API.

No database: the API half runs against the fake handle of
test_device_event_hooks.py.
"""

import json

import pytest

from test_device_event_hooks import (  # noqa: F401  (fixtures)
    Handle, IMEI, Quiet, bearer, client, logged_in, post, texts, web)

from tracker import obd, telemetry
from tracker.web import devices

from test_parsing import FULL

# One word; its last bit (PID 20) is set, so a real scan would have more words.
OSP = 'BE1FA813'


# -- the catalogue -------------------------------------------------------------------

def test_catalogue_covers_the_spec_and_is_well_formed():
    pids = {e['pid'] for e in obd.CATALOGUE}
    assert len(pids) == len(obd.CATALOGUE)
    wanted = {0x01, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C,
              0x0D, 0x0E, 0x0F, 0x10, 0x11, *range(0x14, 0x1C), 0x1F, 0x21, 0x22,
              0x23, 0x2C, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x33, 0x3C, 0x3D, 0x3E,
              0x3F, 0x42, 0x43, 0x44, 0x45, 0x46, *range(0x47, 0x4D), 0x4D, 0x4E,
              0x52, 0x5C, 0x5E}
    assert pids == {'%02X' % p for p in wanted}
    for e in obd.CATALOGUE:
        assert e['size'] in (1, 2, 4) and e['scale'] >= 1 and e['name']
        assert e['pid'] == e['pid'].upper() and len(e['pid']) == 2


def test_legacy_keys_are_the_sixteen_in_obd_fields():
    legacy = [k for e in obd.CATALOGUE for k in e['legacy_keys']]
    assert sorted(legacy) == sorted(telemetry.OBD_FIELDS)
    # The scales the catalogue states agree with the columns' own.
    for e in obd.CATALOGUE:
        for key in e['legacy_keys']:
            _, scale, _ = telemetry.OBD_FIELDS[key]
            if key in ('omil', 'odtc', 'ofs'):
                continue
            assert scale == e['scale'], key


def test_the_api_shape_of_an_entry():
    got = {e['pid']: e for e in obd.catalogue()}
    assert got['0C'] == {'pid': '0C', 'name': 'Engine speed', 'unit': 'rpm',
                         'scale': 1, 'size': 2, 'legacy_key': 'orpm'}
    assert got['0B']['legacy_key'] is None and got['0B']['unit'] == 'kPa'


# -- the selection ---------------------------------------------------------------------

@pytest.mark.parametrize('value,want', [
    ('0C,05,0B', ['0C', '05', '0B']), ('0c05', ['0C', '05']), ('0C|05', ['0C', '05']),
    (' 0C , 05 ', ['0C', '05']), (['0b', '0C'], ['0B', '0C']),
    ('0C,05,0C', ['0C', '05']), ('', []), ([], [])])
def test_parse_selection(value, want):
    assert obd.parse_selection(value) == want


@pytest.mark.parametrize('value', [
    '0C,5', '0G', 'ZZ', '0C,FF', '00', '20', ['0C', 5], ['0C05'], 5, {'0C': 1},
    ','.join('%02X' % p for p in range(0x04, 0x11)) ])          # 13 PIDs
def test_parse_selection_refuses(value):
    with pytest.raises(ValueError):
        obd.parse_selection(value)


def test_twelve_is_the_limit_and_the_text_has_no_separators():
    twelve = ['04', '05', '06', '07', '08', '09', '0A', '0B', '0C', '0D', '0E', '0F']
    assert obd.parse_selection(twelve) == twelve
    assert obd.selection_text(twelve) == '0405060708090A0B0C0D0E0F'
    assert len(obd.selection_text(twelve)) == 24


def test_effective_selection_is_override_then_built_then_default():
    assert obd.effective_selection({'obd_pids': '0C050B'}) == (['0C', '05', '0B'], False)
    assert obd.effective_selection({}, '0C0D') == (['0C', '0D'], True)
    assert obd.effective_selection({}) == (['0C', '05'], True)
    assert obd.effective_selection({'obd_pids': ''}) == (['0C', '05'], True)


# -- the supported bitmaps -------------------------------------------------------------

def test_supported_pids_reads_the_bits_msb_first():
    # BE1FA813: 1011 1110 0001 1111 1010 1000 0001 0011
    assert obd.supported_pids(OSP) == [
        '01', '03', '04', '05', '06', '07', '0C', '0D', '0E', '0F', '10', '11',
        '13', '15', '1C', '1F']


def test_supported_chain_bits_are_not_pids():
    # PID 20 (last bit of word 0) says word 2 follows; it is not selectable.
    got = obd.supported_pids('00000001' + '80000000')
    assert got == ['21']
    assert obd.supported_pids('FFFFFFFF')[-1] == '1F'


@pytest.mark.parametrize('text', ['', 'BE1FA81', 'BE1FA813A', 'be1fa813', 'BE1FA81G',
                                  'FF' * 29, None, 5])
def test_supported_refuses_bad_text(text):
    assert obd.supported_pids(text) is None


def test_the_longest_scan_is_seven_words():
    assert obd.supported_pids('00000000' * 7) == []
    assert obd.supported_pids('00000000' * 8) is None


# -- parsing the record ----------------------------------------------------------------

def parse(extra):
    return telemetry.parse_csv_line(FULL + extra)


def test_osp_is_taken_whole():
    assert parse(',osp=' + OSP)['obd_supported'] == OSP


def test_generic_pid_values_are_collected_and_legacy_keys_are_not():
    d = parse(',orpm=850;o0B=95;o2F=640;ocl=88')
    assert d['obd_extra'] == {'0B': '95', '2F': '640'}
    assert d['obd_rpm'] == '850' and d['obd_coolant'] == '88'


def test_unscaled_by_the_catalogue():
    entry = telemetry._build_entry(
        parse(',o0B=95;o2F=640;o14=450;o5E=1234;o0C=9;oFF=7'),
        {'id': 1}, '1.2.3.4', None)
    # 0C is a legacy PID (its own columns), FF is not in the catalogue.
    assert json.loads(entry['obd_extra']) == {
        '0B': 95.0, '14': 0.45, '2F': 64.0, '5E': 12.34}


def test_a_record_without_generic_values_has_no_obd_extra():
    entry = telemetry._build_entry(parse(',orpm=850'), {'id': 1}, '1.2.3.4', None)
    assert entry.get('obd_extra') is None


def test_a_bad_generic_value_is_dropped():
    entry = telemetry._build_entry(parse(',o0B=x;o0A=3'), {'id': 1}, '1.2.3.4', None)
    assert json.loads(entry['obd_extra']) == {'0A': 3.0}


def test_obd_extra_is_a_log_column():
    assert 'obd_extra' in telemetry.LOG_COLUMNS


# -- filing a scan ----------------------------------------------------------------------

def test_a_scan_is_stored_clears_the_request_and_is_an_event():
    handle = Handle()
    device = {'id': 1, 'imei': IMEI, 'obd_scan': 1}
    telemetry._note_obd_supported(handle, device, OSP)
    sql, params = handle.other_writes()[0]
    assert '`obd_supported` = %s' in sql and '`obd_scan` = 0' in sql
    assert params == (OSP, 1)
    assert device['obd_scan'] == 0 and device['obd_supported'] == OSP
    n = len(obd.supported_pids(OSP))
    assert texts(handle) == ['OBD supported PIDs scanned: %d PIDs' % n]


def test_a_malformed_scan_changes_nothing():
    handle = Handle()
    device = {'id': 1, 'imei': IMEI, 'obd_scan': 1}
    for bad in ('', 'XYZ', 'BE1F', 'FF' * 30):
        telemetry._note_obd_supported(handle, device, bad)
    assert handle.writes == [] and device['obd_scan'] == 1


def test_lowercase_scan_is_stored_uppercase():
    handle = Handle()
    telemetry._note_obd_supported(handle, {'id': 1}, ' be1fa813 ')
    assert handle.other_writes()[0][1][0] == OSP


def test_the_first_obd_record_requests_a_scan_once():
    handle = Handle()
    device = {'id': 1}
    telemetry._request_first_scan(handle, device)
    assert device['obd_scan'] == 1 and len(handle.writes) == 1
    telemetry._request_first_scan(handle, device)
    assert len(handle.writes) == 1


def test_no_request_when_a_scan_is_known():
    handle = Handle()
    telemetry._request_first_scan(handle, {'id': 1, 'obd_supported': OSP})
    assert handle.writes == []


def test_only_obd_records_count():
    assert not telemetry._has_obd_values({'speed': '1'})
    assert telemetry._has_obd_values({'obd_rpm': 0})
    assert telemetry._has_obd_values({'obd_extra': '{"0B": 1.0}'})


# -- the reply -------------------------------------------------------------------------

class Bare:
    def one(self, *a, **k):
        return None

    def all(self, *a, **k):
        return []

    def query(self, *a, **k):
        return 1


def reply(**cols):
    row = {'id': 1, 'imei': 'x', 'int': 60, 'movement_alarm': 1}
    row.update(cols)
    return telemetry.build_response(row, Bare(), Quiet())


def test_os_is_the_last_token_and_follows_obd_scan():
    assert reply().endswith(',im=0,gx=off,os=0')
    assert reply(obd_scan=0).endswith(',gx=off,os=0')
    assert reply(obd_scan=1).endswith(',im=0,gx=off,os=1')


def test_xop_rides_in_tu_without_separators():
    assert reply(obd_pids='0C050B').endswith(',tu=xop:0C050B,im=0,gx=off,os=0')
    assert telemetry.tunables_token({'obd_pids': None}) == '-'
    assert telemetry.tunables_token({'obd_pids': ''}) == '-'


def test_tb_xop():
    built = telemetry.parse_tunables_built('xtt:8;xop:0C05')
    assert built == {'xtt': 8, 'xop': '0C05'}
    assert telemetry.tunables_built_text(built) == 'xtt:8;xop:0C05'
    for bad in ('xop:0C0', 'xop:0c05', 'xop:ZZ', 'xop:' + '0C' * 13):
        assert telemetry.parse_tunables_built(bad) == {}
    assert telemetry.parse_tunables_built('xop:') == {'xop': ''}


# -- the API -----------------------------------------------------------------------------

def updates(web):
    return [w for w in web.writes if w[0].startswith('UPDATE `device`')]


def test_devices_summary_fields():
    row = {'id': 1, 'imei': 'x'}
    got = devices.summary(row, Bare())
    assert got['obd_pids'] == ['0C', '05'] and got['obd_pids_default'] is True
    assert got['obd_supported'] is None and got['obd_supported_at'] is None
    assert got['obd_scan'] is False
    assert got['tunables']['xop'] is None

    import datetime
    row.update(obd_pids='0C050B', obd_supported=OSP, obd_scan=1,
               obd_supported_at=datetime.datetime(2026, 11, 16, 9, 30, 0))
    got = devices.summary(row, Bare())
    assert got['obd_pids'] == ['0C', '05', '0B'] and got['obd_pids_default'] is False
    assert got['obd_supported'] == obd.supported_pids(OSP)
    assert got['obd_supported_at'] == '2026-11-16 09:30:00'
    assert got['obd_scan'] is True
    assert got['tunables']['xop'] == '0C050B'


def test_summary_uses_the_built_selection_when_there_is_no_override():
    got = devices.summary({'id': 1, 'imei': 'x', 'tunables_built': 'xop:0C0D'}, Bare())
    assert got['obd_pids'] == ['0C', '0D'] and got['obd_pids_default'] is True
    assert got['tunables']['xop'] == '0C0D'


def test_pid_catalogue_endpoint(client, web, bearer, logged_in):
    got = client.get('/api/1.0/obd/pids', headers=bearer).get_json()
    assert got['status'] == 'ok' and got['pids'] == obd.catalogue()
    assert got['max_selected'] == 12 and got['default'] == ['0C', '05']
    assert logged_in.get('/api/1.0/obd/pids').status_code == 200


def test_pid_catalogue_endpoint_wants_credentials(client, web):
    assert client.get('/api/1.0/obd/pids').status_code == 401


def test_config_shows_the_selection(client, web, bearer):
    got = client.get('/api/1.0/config?imei=%s' % IMEI, headers=bearer).get_json()
    assert got['xop'] is None and got['obd_pids'] == ['0C', '05']
    assert got['obd_pids_default'] is True and got['obd_supported'] is None
    assert got['obd_scan'] is False
    web.device.update(obd_pids='0B0C', obd_scan=1, obd_supported=OSP)
    got = client.get('/api/1.0/config?imei=%s' % IMEI, headers=bearer).get_json()
    assert got['xop'] == '0B0C' and got['obd_pids'] == ['0B', '0C']
    assert got['obd_scan'] is True and got['obd_pids_default'] is False


def test_post_obd_pids_list_or_string(client, web, bearer):
    web.device.update(obd_supported=OSP)
    for value in ('0C,05,0D', ['0C', '05', '0D'], '0C050D'):
        web.writes.clear()
        web.events.clear()
        assert post(client, '/config', bearer, obd_pids=value).get_json()['status'] == 'ok'
        sql, params = updates(web)[-1]
        assert '`obd_pids` = %s' in sql and params == ['0C050D', 1]
        assert texts(web) == ['OBD PIDs captured set to 0C,05,0D']


def test_post_xop_is_the_same_setting(client, web, bearer):
    web.device.update(obd_supported=OSP)
    assert post(client, '/config', bearer, xop='0C0D').get_json()['status'] == 'ok'
    assert updates(web)[-1][1] == ['0C0D', 1]
    assert post(client, '/config', bearer, xop='0C', obd_pids='0D').status_code == 400


def test_post_default_clears(client, web, bearer):
    web.device.update(obd_pids='0C0D')
    for value in ('default', None, '', []):
        web.writes.clear()
        web.events.clear()
        assert post(client, '/config', bearer, obd_pids=value).get_json()['status'] == 'ok'
        assert updates(web)[-1][1] == [None, 1]
        assert texts(web) == ['OBD PIDs captured back to the built value']


@pytest.mark.parametrize('value', ['0G', '0C,FF', '20', '0C,5', ['0C', 12], 7,
                                   ','.join('%02X' % p for p in range(4, 17))])
def test_post_refuses_bad_selections(client, web, bearer, value):
    response = post(client, '/config', bearer, obd_pids=value)
    assert response.status_code == 400
    assert response.get_json()['message'].startswith('obd_pids:')
    assert updates(web) == []


def test_post_refuses_an_unsupported_pid_once_scanned(client, web, bearer):
    web.device.update(obd_supported=OSP)
    response = post(client, '/config', bearer, obd_pids='0C,0B,2F')
    assert response.status_code == 400
    message = response.get_json()['message']
    assert '2F' in message and 'not supported' in message and '0C' not in message.split(':', 1)[1]
    assert updates(web) == []
    assert post(client, '/config', bearer, obd_pids='0C,05,0F').get_json()['status'] == 'ok'


def test_only_the_defaults_are_selectable_before_a_scan(client, web, bearer):
    assert post(client, '/config', bearer, obd_pids='0C,05').get_json()['status'] == 'ok'
    r = post(client, '/config', bearer, obd_pids='2F,5E')
    assert r.status_code == 400 and 'not selectable until' in r.get_json()['message']


def test_selectable_is_the_defaults_then_the_supported_ones():
    assert obd.selectable(None) == ['0C', '05']
    assert obd.selectable(['0C', '0B', 'FF']) == ['0C', '0B']


def test_post_obd_scan_requests_a_rescan(client, web, bearer):
    assert post(client, '/config', bearer, obd_scan=1).get_json()['status'] == 'ok'
    sql, params = updates(web)[-1]
    assert '`obd_scan` = %s' in sql and params == [1, 1]
    assert texts(web) == ['OBD supported-PID rescan requested']
    web.device.update(obd_scan=1)
    web.events.clear()
    assert post(client, '/config', bearer, obd_scan=0).get_json()['status'] == 'ok'
    assert texts(web) == ['OBD supported-PID rescan cancelled']
    for bad in (2, -1, 'yes', 1.5, True):
        assert post(client, '/config', bearer, obd_scan=bad).status_code == 400


def test_command_sets_the_selection_with_pipes(client, web, bearer):
    web.device.update(obd_supported=OSP)
    r = client.post('/api/1.0/command', headers=bearer,
                    json={'imei': IMEI, 'command': 'obd_pids=0C|05|0D'})
    assert r.get_json()['status'] == 'ok'
    assert updates(web)[-1][1] == ['0C050D', 1] and web.queue == []
    r = client.post('/api/1.0/command', headers=bearer,
                    json={'imei': IMEI, 'command': 'obd_pids=default'})
    assert updates(web)[-1][1] == [None, 1]
    r = client.post('/api/1.0/command', headers=bearer,
                    json={'imei': IMEI, 'command': 'obd_pids=0C|ZZ'})
    assert r.status_code == 400


def test_command_requests_a_scan(client, web, bearer):
    r = client.post('/api/1.0/command', headers=bearer,
                    json={'imei': IMEI, 'command': 'obd_scan=1'})
    assert r.get_json()['status'] == 'ok' and updates(web)[-1][1] == [1, 1]


# -- the live record ----------------------------------------------------------------------

def test_position_carries_obd_extra():
    row = {'obd_extra': '{"0B": 95.0, "2F": 64.0}'}
    assert devices.position(row)['obd_extra'] == {'0B': 95.0, '2F': 64.0}
    assert devices.position({})['obd_extra'] is None
    assert devices.position({'obd_extra': 'not json'})['obd_extra'] is None
    assert devices.position({'obd_extra': '[1]'})['obd_extra'] is None
    assert devices.position({'obd_extra': '{"0B": "x"}'})['obd_extra'] is None


def test_every_tunable_has_event_wording_including_the_selection():
    from tracker import device_events as ev
    assert ev.tunable_text('obd_pids', '0C05') == 'OBD PIDs captured set to 0C,05'
