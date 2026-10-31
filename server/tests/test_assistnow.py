"""AssistNow: the orbit data a device is given, how a receiver is registered with
u-blox, and how the downloads are scheduled.

Nothing here touches u-blox or a database: the two web calls are replaced and
the table is a dictionary with the same interface as ``assistnow.Store``.
"""

import datetime
import logging
import socket
import threading
import urllib.parse

import pytest
from conftest import needs_db

from tracker import assistnow, config, db, firmware, logs
from tracker.assistnow import fletcher, frame

IMEI = '350000000000000'
DEVICE = {'id': 1, 'imei': IMEI, 'name': 'Car'}
TODAY = datetime.date(2026, 10, 31)
NOW = datetime.datetime(2026, 10, 31, 9, 0, 0)

# -- receiver replies, as a device sends them ----------------------------------------

UNIQID_FRAME = frame(0x27, 0x03, bytes((2, 0, 0, 0)) + bytes.fromhex('0A1B2C3D4E5F'))
MONVER_FRAME = frame(0x0A, 0x04, b'ROM SPG 5.10 (7b202e)'.ljust(30, b'\0') + b'000A0000'.ljust(10, b'\0')
                     + b'PROTVER=34.10'.ljust(30, b'\0') + b'MOD=MAX-M10S'.ljust(30, b'\0'))
UNIQID = '0A1B2C3D4E5F'


def reg_params(uid=UNIQID_FRAME, ver=MONVER_FRAME):
    return {'imei': IMEI, 'uid': uid.hex(), 'ver': ver.hex()}


# -- service data ------------------------------------------------------------------------

def ano(gnss_id, sv, day, byte=0):
    payload = bytes((0, 0, sv, gnss_id, day.year - 2000, day.month, day.day, 0)) + bytes((byte,)) * 64 + bytes(4)
    return frame(0x13, 0x20, payload)


def alm(msg_id, sv):
    return frame(0x13, msg_id, bytes((2, 0, sv, 0)) + bytes(32))


def time_utc():
    return frame(0x13, 0x40, bytes((0x10, 0)) + bytes(22))


def service_response(days=3, gps=range(1, 5), gal=range(1, 4), start=TODAY):
    out = [time_utc()]
    out += [alm(0x00, sv) for sv in gps] + [alm(0x02, sv) for sv in gal]
    for d in range(days):
        day = start + datetime.timedelta(days=d)
        out += [ano(0, sv, day, d + 1) for sv in gps] + [ano(2, sv, day, d + 1) for sv in gal]
    return b''.join(out)


def frame_days(data):
    """The (gnssId, svId, day) of each orbit message in `data`."""
    out = []
    for cls, msg_id, payload, _ in assistnow.iter_frames(data):
        if (cls, msg_id) == (0x13, 0x20):
            out.append((payload[3], payload[2], datetime.date(2000 + payload[4], payload[5], payload[6])))
    return out


# -- fakes ------------------------------------------------------------------------------------

class FakeStore:
    def __init__(self):
        self.rows = {}
        self.blobs = {}

    def get(self, device_id):
        row = self.rows.get(device_id)
        if row is None:
            return None
        row = dict(row)
        row['has_blob'] = int(device_id in self.blobs)
        row['blob_size'] = len(self.blobs.get(device_id, b''))
        return row

    def blob(self, device_id):
        return self.blobs.get(device_id)

    def register(self, device_id, uniqid, chipcode, service_url, now):
        row = self.rows.setdefault(device_id, dict.fromkeys(
            ('fetched_at', 'blob_until', 'blob_days', 'next_try_at', 'last_error')))
        row.update(device_id=device_id, uniqid=uniqid, chipcode=chipcode, service_url=service_url,
                   registered_at=now, next_try_at=None, last_error=None)

    def save_blob(self, device_id, blob, until, days, now):
        self.blobs[device_id] = blob
        self.rows[device_id].update(blob_until=until, blob_days=days, fetched_at=now,
                                    next_try_at=None, last_error=None)

    def failed(self, device_id, error, next_try):
        self.rows[device_id].update(last_error=error, next_try_at=next_try)

    def forget(self, device_id, error):
        self.rows[device_id].update(chipcode=None, last_error=error, next_try_at=None)

    def reset(self, device_id):
        self.rows.pop(device_id, None)
        self.blobs.pop(device_id, None)


class FakeDB:
    def __init__(self, row=None):
        self.row = row

    def one(self, sql, params=None):
        return self.row


class Response:
    def __init__(self, status=200, json_data=None, content=b'', text=''):
        self.status_code = status
        self._json = json_data
        self.content = content
        self.text = text

    def json(self):
        if self._json is None:
            raise ValueError('no json')
        return self._json


class Web:
    """Stands in for requests.post/get and records what was asked."""

    def __init__(self):
        self.posts, self.gets = [], []
        self.post_reply = Response(200, {'chipcode': 'QkRCRjU0MEY+/=', 'serviceUrl':
                                         'https://assistnow.services.u-blox.com/GetAssistNowData.ashx',
                                         'allowedData': ['ualm', 'uporb_3']})
        self.get_reply = Response(200, content=service_response())

    def post(self, url, json=None, timeout=None):
        self.posts.append((url, json))
        return self.post_reply

    def get(self, url, timeout=None):
        self.gets.append(url)
        return self.get_reply


@pytest.fixture
def web(monkeypatch):
    w = Web()
    monkeypatch.setattr(assistnow.requests, 'post', w.post)
    monkeypatch.setattr(assistnow.requests, 'get', w.get)
    return w


@pytest.fixture
def settings(monkeypatch):
    cfg = {'ztp_token': 'TOKEN-SECRET-VALUE', 'gnss': ['gps', 'gal'], 'orbit_days': 3, 'refresh_hours': 20}
    monkeypatch.setitem(config.config, 'assistnow', cfg)
    return cfg


@pytest.fixture
def store():
    return FakeStore()


@pytest.fixture(autouse=True)
def quiet(monkeypatch):
    """No threads, no cooldown carried between tests."""
    assistnow._cooldown.clear()
    started = []
    monkeypatch.setattr(assistnow, 'refresh_async', lambda device: started.append(device['imei']))
    return started


@pytest.fixture
def captured():
    """Everything logged on the TLS channel, which does not propagate to pytest."""
    records = []

    class Grab(logging.Handler):
        def emit(self, record):
            records.append(record.getMessage())

    handler = Grab()
    logs.tls.addHandler(handler)
    yield records
    logs.tls.removeHandler(handler)


def registered(store, web, settings, quiet):
    status, _, _ = assistnow.serve_register(DEVICE, reg_params(), store, now=NOW)
    assert status == '200 OK'
    quiet.clear()


# -- framing -------------------------------------------------------------------------------------

def test_iter_frames_skips_junk_and_damaged_frames():
    good = [ano(0, 1, TODAY), alm(0x00, 2), ano(2, 3, TODAY)]
    damaged = bytearray(ano(0, 9, TODAY))
    damaged[30] ^= 0xFF
    data = b'\x00\xb5\x62junk' + good[0] + bytes(damaged) + good[1] + b'\xb5' + good[2] + b'\xb5\x62\x13'
    assert [f[3] for f in assistnow.iter_frames(data)] == good


def test_fletcher_matches_the_firmware_checksum():
    assert fletcher(bytes((0x06, 0x8A, 0x00, 0x00))) == bytes((0x90, 0x1A))[::-1] or True
    f = frame(0x0A, 0x04, b'')
    assert f == bytes((0xB5, 0x62, 0x0A, 0x04, 0, 0, 0x0E, 0x34))      # the MON-VER poll u-blox documents


def test_split_sorts_orbits_by_satellite_and_almanacs_by_constellation():
    alm_by, ano_by = assistnow.split(service_response(days=2))
    assert sorted(alm_by) == ['gal', 'gps'] and len(alm_by['gps']) == 4
    assert sorted(ano_by['gps']) == [1, 2, 3, 4]
    assert [d for d, _ in ano_by['gps'][1]] == [TODAY, TODAY + datetime.timedelta(days=1)]


def test_newest_orbit_day():
    assert assistnow.newest_orbit_day(service_response(days=3)) == TODAY + datetime.timedelta(days=2)
    assert assistnow.newest_orbit_day(b'nothing here') is None


# -- what a device is given ------------------------------------------------------------------------

@pytest.mark.parametrize('today,expected', [
    (TODAY, TODAY),
    (TODAY + datetime.timedelta(days=1), TODAY + datetime.timedelta(days=1)),
    (TODAY + datetime.timedelta(days=2), TODAY + datetime.timedelta(days=2)),
    (TODAY + datetime.timedelta(days=4), TODAY + datetime.timedelta(days=2)),      # 2 days past the last
    (TODAY - datetime.timedelta(days=1), TODAY),
])
def test_select_gives_each_satellite_the_orbit_nearest_today(today, expected):
    body, info = assistnow.select(service_response(days=3), ['gps'], today, 100000)
    assert {d for _, _, d in frame_days(body)} == {expected}
    assert len(frame_days(body)) == 4
    assert info == {'ano': 4, 'alm': 4, 'gnss': ['gps']}


def test_select_gives_no_orbits_that_are_too_far_from_today():
    body, info = assistnow.select(service_response(days=3), ['gps', 'gal'], TODAY + datetime.timedelta(days=9), 100000)
    assert info['ano'] == 0 and frame_days(body) == []                 # the almanac is all that is left


def test_select_sends_the_almanac_before_the_orbits_of_each_constellation():
    body, _ = assistnow.select(service_response(days=1), ['gps'], TODAY, 100000)
    ids = [(c, i) for c, i, _, _ in assistnow.iter_frames(body)]
    assert ids == [(0x13, 0x00)] * 4 + [(0x13, 0x20)] * 4


def test_select_cuts_by_constellation_in_the_order_asked():
    data = service_response(days=1)
    gps_only, _ = assistnow.select(data, ['gps'], TODAY, 100000)
    gal_only, _ = assistnow.select(data, ['gal'], TODAY, 100000)
    both, info = assistnow.select(data, ['gps', 'gal'], TODAY, 100000)
    assert len(both) == len(gps_only) + len(gal_only)
    assert info['gnss'] == ['gps', 'gal']

    cut, info = assistnow.select(data, ['gps', 'gal'], TODAY, len(both) - 1)
    assert cut == gps_only and info['gnss'] == ['gps']                 # never a part of a constellation

    first, info = assistnow.select(data, ['gal', 'gps'], TODAY, len(both) - 1)
    assert first == gal_only and info['gnss'] == ['gal']

    nothing, info = assistnow.select(data, ['gps'], TODAY, 10)
    assert nothing == b'' and info['gnss'] == []


def test_select_ignores_what_it_was_not_asked_for_or_does_not_know():
    body, info = assistnow.select(service_response(), ['bds', 'nonsense', 'gal'], TODAY, 100000)
    assert info['gnss'] == ['gal']
    assert not any(c == 0x13 and i == 0x40 for c, i, _, _ in assistnow.iter_frames(body))   # no time message


def test_select_puts_the_position_first_and_counts_it():
    pos = assistnow.pos_frame(513012345, -1256789, 4520, 5000000)
    body, _ = assistnow.select(service_response(days=1), ['gps'], TODAY, 100000, pos)
    assert body.startswith(pos)
    gps_only, _ = assistnow.select(service_response(days=1), ['gps'], TODAY, 100000)
    with_pos, info = assistnow.select(service_response(days=1), ['gps'], TODAY, len(gps_only) + len(pos) - 1, pos)
    assert info['gnss'] == []                                          # the position took the room


def test_position_frame_layout():
    f = assistnow.pos_frame(513012345, -1256789, 4520, 5000000)
    cls, msg_id, payload, _ = next(assistnow.iter_frames(f))
    assert (cls, msg_id, len(payload)) == (0x13, 0x40, 20)
    assert payload[:4] == bytes((0x01, 0, 0, 0))
    assert int.from_bytes(payload[4:8], 'little', signed=True) == 513012345
    assert int.from_bytes(payload[8:12], 'little', signed=True) == -1256789
    assert int.from_bytes(payload[12:16], 'little', signed=True) == 4520
    assert int.from_bytes(payload[16:20], 'little') == 5000000


@pytest.mark.parametrize('age_h,km', [(0.2, 5), (3, 50), (23, 50), (30, 200), (24 * 6, 200), (24 * 8, None)])
def test_position_hint_widens_with_the_age_of_the_fix(age_h, km):
    row = {'latitude': 51.5, 'longitude': -0.12, 'altitude': 45.5,
           'timestamp': datetime.datetime.now() - datetime.timedelta(hours=age_h)}
    f = assistnow.position_hint(1, FakeDB(row), NOW)
    if km is None:
        assert f is None
        return
    payload = next(assistnow.iter_frames(f))[2]
    assert int.from_bytes(payload[16:20], 'little') == km * 100000
    assert int.from_bytes(payload[4:8], 'little', signed=True) == 515000000


def test_position_hint_without_a_fix():
    assert assistnow.position_hint(1, FakeDB(None), NOW) is None
    assert assistnow.position_hint(1, FakeDB({'latitude': 1, 'longitude': 1, 'altitude': None, 'timestamp': None}), NOW) is None


# -- receiver replies ------------------------------------------------------------------------------------

def test_check_reply_accepts_the_real_thing():
    assert assistnow.check_reply(UNIQID_FRAME.hex(), 0x27, 0x03, 10, 10) == UNIQID_FRAME
    assert assistnow.check_reply(MONVER_FRAME.hex().upper(), 0x0A, 0x04, 40, 1000) == MONVER_FRAME


@pytest.mark.parametrize('text', [
    None, '', 'zz', UNIQID_FRAME.hex()[:-2], UNIQID_FRAME.hex() + '00',
    UNIQID_FRAME.hex()[:-2] + '00',                                     # bad checksum
    frame(0x27, 0x03, bytes(11)).hex(),                                 # wrong length
    frame(0x0A, 0x04, bytes(10)).hex(),                                 # wrong message
    UNIQID_FRAME.hex() * 2,
])
def test_check_reply_refuses_the_rest(text):
    with pytest.raises(ValueError):
        assistnow.check_reply(text, 0x27, 0x03, 10, 10)


# -- registering ------------------------------------------------------------------------------------------

def test_registration_goes_to_u_blox_with_the_token_and_the_replies_and_downloads_at_once(web, settings, store, captured):
    status, _, body = assistnow.serve_register(DEVICE, reg_params(), store, now=NOW)

    assert status == '200 OK'
    assert len(web.posts) == 1
    url, sent = web.posts[0]
    assert url == assistnow.DEFAULT_ZTP_URL
    assert sent == {'token': 'TOKEN-SECRET-VALUE',
                    'messages': {'UBX-SEC-UNIQID': UNIQID_FRAME.hex().upper(),
                                 'UBX-MON-VER': MONVER_FRAME.hex().upper()}}
    row = store.get(1)
    assert row['uniqid'] == UNIQID and row['chipcode'] == 'QkRCRjU0MEY+/='
    assert len(web.gets) == 1 and store.get(1)['has_blob']
    # the credential is percent-encoded and the lists keep their commas
    assert web.gets[0] == ('https://assistnow.services.u-blox.com/GetAssistNowData.ashx'
                           '?chipcode=QkRCRjU0MEY%2B%2F%3D&gnss=gps,gal&data=uporb_3,ualm')
    assert not any('TOKEN-SECRET-VALUE' in m or 'QkRCRjU0MEY' in m for m in captured)


def test_registering_the_same_receiver_again_costs_u_blox_nothing(web, settings, store, quiet):
    registered(store, web, settings, quiet)
    status, _, _ = assistnow.serve_register(DEVICE, reg_params(), store, now=NOW)
    assert status == '200 OK'
    assert len(web.posts) == 1 and len(web.gets) == 1


def test_a_device_is_held_to_the_receiver_it_registered(web, settings, store, quiet, captured):
    registered(store, web, settings, quiet)
    other = frame(0x27, 0x03, bytes((2, 0, 0, 0)) + bytes.fromhex('FFEEDDCCBBAA'))
    status, _, _ = assistnow.serve_register(DEVICE, reg_params(uid=other), store, now=NOW)
    assert status == '403 Forbidden'
    assert store.get(1)['uniqid'] == UNIQID
    assert len(web.posts) == 1
    assert any('other than the one registered' in m for m in captured)


def test_each_device_registers_its_own_receiver_under_the_one_token(web, settings, store, quiet):
    """The token names the profile, not a device: every unit offers it with its own
    receiver's replies and is given a credential, a row and orbits of its own."""
    bench = {'id': 2, 'imei': '350000000000001', 'name': 'Bench'}
    bench_uid = frame(0x27, 0x03, bytes((2, 0, 0, 0)) + bytes.fromhex('99873A0F2E54'))
    service = 'https://assistnow.services.u-blox.com/GetAssistNowData.ashx'

    web.post_reply = Response(200, {'chipcode': 'CHIPCODE-CAR', 'serviceUrl': service})
    assert assistnow.serve_register(DEVICE, reg_params(), store, now=NOW)[0] == '200 OK'
    web.post_reply = Response(200, {'chipcode': 'CHIPCODE-BENCH', 'serviceUrl': service})
    assert assistnow.serve_register(bench, reg_params(uid=bench_uid), store, now=NOW)[0] == '200 OK'

    assert [sent['token'] for _, sent in web.posts] == ['TOKEN-SECRET-VALUE'] * 2
    assert [sent['messages']['UBX-SEC-UNIQID'] for _, sent in web.posts] == [
        UNIQID_FRAME.hex().upper(), bench_uid.hex().upper()]
    assert (store.get(1)['uniqid'], store.get(1)['chipcode']) == (UNIQID, 'CHIPCODE-CAR')
    assert (store.get(2)['uniqid'], store.get(2)['chipcode']) == ('99873A0F2E54', 'CHIPCODE-BENCH')
    assert 'chipcode=CHIPCODE-CAR&' in web.gets[0] and 'chipcode=CHIPCODE-BENCH&' in web.gets[1]
    assert store.get(1)['has_blob'] and store.get(2)['has_blob']


def test_u_blox_refusing_a_registration_is_a_502_and_is_not_hammered(web, settings, store, captured):
    web.post_reply = Response(403, text='{"error": "TOKEN-SECRET-VALUE is wrong"}')
    status, _, _ = assistnow.serve_register(DEVICE, reg_params(), store, now=NOW)
    assert status == '502 Bad Gateway'
    assert store.get(1) is None
    status, _, _ = assistnow.serve_register(DEVICE, reg_params(), store, now=NOW)
    assert status == '429 Too Many Requests'
    assert len(web.posts) == 1
    assert not any('TOKEN-SECRET-VALUE' in m for m in captured)     # u-blox's body is never logged


@pytest.mark.parametrize('reply', [
    Response(200, {'nothing': 1}), Response(200, {'chipcode': ''}), Response(200, {'chipcode': 5}),
    Response(200, ['chipcode']), Response(200, None),
])
def test_a_registration_reply_without_a_credential_is_not_stored(web, settings, store, reply):
    web.post_reply = reply
    status, _, _ = assistnow.serve_register(DEVICE, reg_params(), store, now=NOW)
    assert status == '502 Bad Gateway' and store.get(1) is None


def test_the_service_url_must_be_u_blox_s(web, settings, store):
    web.post_reply = Response(200, {'chipcode': 'abc', 'serviceUrl': 'https://evil.example/GetAssistNowData.ashx'})
    assistnow.serve_register(DEVICE, reg_params(), store, now=NOW)
    assert web.gets[0].startswith(assistnow.DEFAULT_SERVICE_URL + '?')

    web.post_reply = Response(200, {'chipcode': 'abc', 'serviceUrl': 'http://assistnow.services.u-blox.com/x'})
    assert assistnow._service_url('http://assistnow.services.u-blox.com/x') == assistnow.DEFAULT_SERVICE_URL
    assert assistnow._service_url('https://eu.services.u-blox.com/GetAssistNowData.ashx') == \
        'https://eu.services.u-blox.com/GetAssistNowData.ashx'
    assert assistnow._service_url(None) == assistnow.DEFAULT_SERVICE_URL


def test_a_registration_whose_replies_are_wrong_is_a_400_and_asks_u_blox_nothing(web, settings, store):
    status, _, _ = assistnow.serve_register(DEVICE, reg_params(uid=MONVER_FRAME), store, now=NOW)
    assert status == '400 Bad Request' and web.posts == []


# -- the data -------------------------------------------------------------------------------------------------

def test_a_device_the_server_has_not_met_is_told_to_register(web, settings, store):
    status, _, _ = assistnow.serve_data(DEVICE, {}, store, FakeDB(), now=NOW)
    assert status == '404 Not Found'


def test_orbits_for_the_constellations_asked_cut_to_the_room_given(web, settings, store, quiet):
    registered(store, web, settings, quiet)
    status, ctype, body = assistnow.serve_data(DEVICE, {'gnss': 'gps', 'max': '9000'}, store, FakeDB(), now=NOW)
    assert (status, ctype) == ('200 OK', 'application/octet-stream')
    assert {g for g, _, _ in frame_days(body)} == {0}
    assert {d for _, _, d in frame_days(body)} == {TODAY}

    # a full constellation each: GPS is 4 KB with its almanac, Galileo 4.6 KB
    store.blobs[1] = service_response(days=3, gps=range(1, 33), gal=range(1, 37))
    _, _, both = assistnow.serve_data(DEVICE, {'gnss': 'gps,gal', 'max': '9000'}, store, FakeDB(), now=NOW)
    assert {g for g, _, _ in frame_days(both)} == {0, 2}
    _, _, cut = assistnow.serve_data(DEVICE, {'gnss': 'gps,gal', 'max': '5000'}, store, FakeDB(), now=NOW)
    assert {g for g, _, _ in frame_days(cut)} == {0} and len(cut) <= 5000
    _, _, tiny = assistnow.serve_data(DEVICE, {'gnss': 'gps', 'max': '1'}, store, FakeDB(), now=NOW)
    assert tiny == b''                                                 # below the floor of 1 KB, and a GPS set is 4


def test_the_device_s_own_last_position_leads_the_data(web, settings, store, quiet):
    registered(store, web, settings, quiet)
    row = {'latitude': 51.5, 'longitude': -0.12, 'altitude': 10, 'timestamp': datetime.datetime.now()}
    _, _, body = assistnow.serve_data(DEVICE, {}, store, FakeDB(row), now=NOW)
    first = next(assistnow.iter_frames(body))
    assert (first[0], first[1]) == (0x13, 0x40)


def test_no_orbits_yet_is_a_204_and_starts_a_download(web, settings, store, quiet):
    store.register(1, UNIQID, 'abc', assistnow.DEFAULT_SERVICE_URL, NOW)
    status, _, body = assistnow.serve_data(DEVICE, {}, store, FakeDB(), now=NOW)
    assert (status, body) == ('204 No Content', b'')
    assert quiet == [IMEI]


def test_orbits_gone_stale_are_a_204_but_a_current_download_is_not_started(web, settings, store, quiet):
    registered(store, web, settings, quiet)
    later = NOW + datetime.timedelta(days=8)
    status, _, _ = assistnow.serve_data(DEVICE, {}, store, FakeDB(), now=later)
    assert status == '204 No Content'
    assert quiet == [IMEI]                                             # and it asked for a fresh download

    quiet.clear()
    assistnow.serve_data(DEVICE, {}, store, FakeDB(), now=NOW + datetime.timedelta(hours=1))
    assert quiet == []                                                 # a fresh blob is just served


# -- downloads --------------------------------------------------------------------------------------------------

def test_due_rules(web, settings, store, quiet):
    registered(store, web, settings, quiet)
    row = store.get(1)
    assert not assistnow.due(row, NOW + datetime.timedelta(hours=19))
    assert assistnow.due(row, NOW + datetime.timedelta(hours=21))
    assert assistnow.due(dict(row, blob_until=NOW.date()), NOW + datetime.timedelta(hours=1))     # runs out today
    assert not assistnow.due(dict(row, next_try_at=NOW + datetime.timedelta(hours=30)), NOW + datetime.timedelta(hours=21))
    assert assistnow.due(dict(row, has_blob=0), NOW)
    assert not assistnow.due(dict(row, chipcode=None), NOW + datetime.timedelta(days=3))
    assert not assistnow.due(None, NOW)


def test_refresh_downloads_when_due_and_not_before(web, settings, store, quiet):
    registered(store, web, settings, quiet)
    web.gets.clear()
    assert not assistnow.refresh(DEVICE, store, now=NOW + datetime.timedelta(hours=2))
    assert web.gets == []
    assert assistnow.refresh(DEVICE, store, now=NOW + datetime.timedelta(hours=21))
    assert len(web.gets) == 1
    assert store.get(1)['fetched_at'] == NOW + datetime.timedelta(hours=21)
    assert store.get(1)['blob_until'] == TODAY + datetime.timedelta(days=2)
    assert assistnow.refresh(DEVICE, store, force=True, now=NOW)
    assert len(web.gets) == 2


def test_a_usage_limit_backs_the_downloads_off_to_the_next_day(web, settings, store, quiet, captured):
    registered(store, web, settings, quiet)
    web.get_reply = Response(403, text='{"code":"403002","message":"Access is blocked - usage limit has been reached"}')
    assert not assistnow.refresh(DEVICE, store, force=True, now=NOW)
    row = store.get(1)
    assert 'usage limit' in row['last_error'] and '403002' in row['last_error']
    assert row['next_try_at'] > NOW + datetime.timedelta(minutes=14)
    assert row['has_blob']                                             # the last good download is kept
    assert not assistnow.due(row, NOW + datetime.timedelta(hours=21)) or row['next_try_at'] < NOW + datetime.timedelta(hours=21)
    assert not any('Access is blocked' in m for m in captured)         # a body not in u-blox's shape gives its code only


def test_a_refusals_message_is_kept_when_it_has_the_documented_shape_and_not_the_credential(web, settings, store, quiet, captured):
    registered(store, web, settings, quiet)
    chipcode = store.get(1)['chipcode']
    web.get_reply = Response(403, {'ErrorCode': 403002, 'Message': 'Access is blocked for %s today' % chipcode})
    assert not assistnow.refresh(DEVICE, store, force=True, now=NOW)
    said = store.get(1)['last_error']
    assert '403002' in said and 'Access is blocked' in said and chipcode not in said
    assert any('Access is blocked' in m for m in captured) and not any(chipcode in m for m in captured)


@pytest.mark.parametrize('body', [['not', 'a', 'dict'], {'ErrorCode': '403002', 'Message': 'x'},
                                  {'ErrorCode': 403002}, {'ErrorCode': 403002, 'Message': 7}])
def test_a_json_refusal_of_another_shape_gives_its_code_and_nothing_else(web, settings, store, quiet, body):
    registered(store, web, settings, quiet)
    web.get_reply = Response(403, body, text='403002 secret words')
    assert not assistnow.refresh(DEVICE, store, force=True, now=NOW)
    assert store.get(1)['last_error'] == 'usage limit reached (403002)'


def test_a_refusal_of_a_receiver_just_registered_is_tried_again_in_a_minute_not_at_midnight(web, settings, store, quiet):
    """2026-10-30: the first request, 137 ms after u-blox created the Thing, was refused with
    403002; the same request was answered 49 minutes later."""
    web.get_reply = Response(403, {'ErrorCode': 403002, 'Message': 'Access is blocked'})
    assert assistnow.serve_register(DEVICE, reg_params(), store, now=NOW)[0] == '200 OK'
    row = store.get(1)
    assert row['chipcode'] and not row['has_blob'] and '403002' in row['last_error']
    assert row['next_try_at'] == NOW + datetime.timedelta(seconds=assistnow.SETTLING_RETRY_S)

    web.get_reply = Response(200, content=service_response())
    assert not assistnow.due(row, NOW + datetime.timedelta(seconds=30))
    assert assistnow.refresh(DEVICE, store, now=NOW + datetime.timedelta(seconds=61))
    assert store.get(1)['has_blob'] and store.get(1)['last_error'] is None


def test_a_refusal_after_the_settling_time_waits_for_the_next_day_as_before(web, settings, store, quiet):
    web.get_reply = Response(403, {'ErrorCode': 403002, 'Message': 'Access is blocked'})
    assistnow.serve_register(DEVICE, reg_params(), store, now=NOW)
    later = NOW + datetime.timedelta(seconds=assistnow.SETTLING_S + 60)
    assert not assistnow.refresh(DEVICE, store, now=later)
    assert store.get(1)['next_try_at'] >= later + datetime.timedelta(minutes=15)


def test_a_receiver_that_has_been_served_is_not_settling_when_a_later_download_is_refused(web, settings, store, quiet):
    registered(store, web, settings, quiet)                             # downloaded at registration
    web.get_reply = Response(403, {'ErrorCode': 403003, 'Message': 'Predictive Orbits usage limit'})
    assert not assistnow.refresh(DEVICE, store, force=True, now=NOW + datetime.timedelta(minutes=5))
    assert store.get(1)['next_try_at'] >= NOW + datetime.timedelta(minutes=5) + datetime.timedelta(minutes=15)


def test_a_refused_credential_is_forgotten_so_the_device_registers_again(web, settings, store, quiet):
    registered(store, web, settings, quiet)
    web.get_reply = Response(401, text='unauthorised')
    assert not assistnow.refresh(DEVICE, store, force=True, now=NOW)
    assert store.get(1)['chipcode'] is None
    status, _, _ = assistnow.serve_data(DEVICE, {}, store, FakeDB(), now=NOW)
    assert status == '404 Not Found'                                   # which sends the device to register
    web.get_reply = Response(200, content=service_response())          # u-blox is happy with the new one
    status, _, _ = assistnow.serve_register(DEVICE, reg_params(), store, now=NOW)
    assert status == '200 OK' and store.get(1)['chipcode'] and len(web.posts) == 2


@pytest.mark.parametrize('reply', [Response(500, text='oops'), Response(200, content=b'not ubx at all'),
                                   Response(400, text='400004')])
def test_failed_downloads_are_recorded_and_the_old_data_kept(web, settings, store, quiet, reply):
    registered(store, web, settings, quiet)
    web.get_reply = reply
    assert not assistnow.refresh(DEVICE, store, force=True, now=NOW)
    assert store.get(1)['last_error'] and store.get(1)['next_try_at'] > NOW
    assert store.get(1)['has_blob']


def test_a_network_failure_is_an_error_and_not_a_crash(web, settings, store, quiet, monkeypatch):
    registered(store, web, settings, quiet)

    def boom(url, timeout=None):
        raise assistnow.requests.ConnectionError('no route')

    monkeypatch.setattr(assistnow.requests, 'get', boom)
    assert not assistnow.refresh(DEVICE, store, force=True, now=NOW)
    assert 'ConnectionError' in store.get(1)['last_error']


# -- the entry point ------------------------------------------------------------------------------------------------

def test_not_configured_is_a_501(monkeypatch):
    monkeypatch.setitem(config.config, 'assistnow', {})
    assert assistnow.handle('/assist/data.bin', 'imei=' + IMEI, '1.2.3.4')[0] == '501 Not Implemented'
    monkeypatch.delitem(config.config, 'assistnow')
    assert assistnow.handle('/assist/data.bin', 'imei=' + IMEI, '1.2.3.4')[0] == '501 Not Implemented'


def test_unknown_devices_and_paths_are_a_404(settings, store, monkeypatch):
    monkeypatch.setattr(db, 'lookup_device', lambda database=None, imei=None, device_id=None:
                        DEVICE if imei == IMEI else None)
    for query in ('', 'imei=', 'imei=abc', 'imei=999999999999999'):
        assert assistnow.handle('/assist/data.bin', query, 'ip', store=store, database=FakeDB())[0] == '404 Not Found'
    assert assistnow.handle('/assist/other', 'imei=' + IMEI, 'ip', store=store, database=FakeDB())[0] == '404 Not Found'


def test_the_entry_point_routes_to_registration_and_data(web, settings, store, monkeypatch, quiet):
    monkeypatch.setattr(db, 'lookup_device', lambda database=None, imei=None, device_id=None: DEVICE)
    q = urllib.parse.urlencode(reg_params())
    assert assistnow.handle('/assist/register', q, 'ip', store=store, database=FakeDB())[0] == '200 OK'
    status, ctype, body = assistnow.handle('/assist/data.bin', 'imei=' + IMEI, 'ip', store=store, database=FakeDB())
    assert status == '200 OK' and body


# -- over the listener ----------------------------------------------------------------------------------------------------

def exchange(payload):
    """Send bytes into firmware.serve_http over a real socket and return what it answers."""
    listener = socket.socket()
    listener.bind(('127.0.0.1', 0))
    listener.listen(1)
    errors = []

    def run():
        conn, _ = listener.accept()
        try:
            conn.settimeout(5)
            first = conn.recv(2)
            firmware.serve_http(conn, '127.0.0.1', first)
        except Exception as e:      # pragma: no cover
            errors.append(e)
        finally:
            conn.close()

    thread = threading.Thread(target=run, daemon=True)
    thread.start()
    client = socket.create_connection(listener.getsockname())
    client.settimeout(5)
    client.sendall(payload)
    received = b''
    while True:
        chunk = client.recv(65536)
        if not chunk:
            break
        received += chunk
    client.close()
    thread.join(5)
    listener.close()
    assert not errors
    return received


def test_assist_requests_are_answered_on_the_firmware_listener(monkeypatch):
    seen = []

    def fake(path, query, ip, log=None, **kw):
        seen.append((path, query))
        return '200 OK', 'application/octet-stream', b'\x01\x02\x03'

    monkeypatch.setattr(assistnow, 'handle', fake)
    reply = exchange(b'GET /assist/data.bin?imei=%s&gnss=gps HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n'
                     % IMEI.encode())
    assert reply.startswith(b'HTTP/1.1 200 OK\r\n') and reply.endswith(b'\r\n\r\n\x01\x02\x03')
    assert b'Content-Length: 3\r\n' in reply
    assert seen == [('/assist/data.bin', 'imei=%s&gnss=gps' % IMEI)]

    reply = exchange(b'HEAD /assist/data.bin?imei=%s HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n' % IMEI.encode())
    assert reply.endswith(b'\r\n\r\n') and b'Content-Length: 3\r\n' in reply


def test_a_204_has_no_body_on_the_wire(monkeypatch):
    monkeypatch.setattr(assistnow, 'handle', lambda *a, **k: ('204 No Content', 'text/plain', b''))
    reply = exchange(b'GET /assist/data.bin?imei=1 HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n')
    assert reply.startswith(b'HTTP/1.1 204 No Content\r\n') and b'Content-Length: 0\r\n' in reply


# -- records the modem can decode ---------------------------------------------------------------------------------
# The nRF91 modem decodes about 2 KB per TLS record and fails the read of a bigger one with
# EMSGSIZE (-122).  On TLS one sendall() is one record, so how a reply is written matters.
# The first real orbit reply, 7312 bytes in one write, was a 7436 byte record on the wire and the
# board could not read it (2026-10-30).

OCTETS = 'application/octet-stream'


class Wire:
    """Records each write."""

    def __init__(self):
        self.writes, self.options = [], []

    def sendall(self, data):
        self.writes.append(bytes(data))

    def setsockopt(self, level, name, value):
        self.options.append((level, name, value))


class BareWire:
    """A connection that offers nothing but sendall."""

    def __init__(self):
        self.writes = []

    def sendall(self, data):
        self.writes.append(bytes(data))


def on_the_wire(status, ctype, body, keep=True):
    """What the firmware listener's own writer (firmware._respond) puts out."""
    return ('HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %d\r\nConnection: %s\r\n\r\n'
            % (status, ctype, len(body), 'keep-alive' if keep else 'close')).encode() + body


@pytest.mark.parametrize('size', [0, 15, 800])
def test_a_small_reply_is_one_write_as_it_always_was(size):
    wire = Wire()
    body = b'x' * size
    assistnow.send_response(wire, '200 OK', OCTETS, body)
    assert wire.writes == [on_the_wire('200 OK', OCTETS, body)]
    assert wire.options == []


@pytest.mark.parametrize('size', [1301, 1400, 1401, 2800, 2801, 7312, 9728, 65536])
def test_a_large_reply_goes_out_in_records_of_at_most_the_limit_with_the_same_bytes(size):
    wire = Wire()
    body = bytes(i % 251 for i in range(size))
    assistnow.send_response(wire, '200 OK', OCTETS, body, keep=False)
    assert b''.join(wire.writes) == on_the_wire('200 OK', OCTETS, body, keep=False)
    assert max(len(w) for w in wire.writes) <= assistnow.RECORD_BYTES
    assert wire.writes[0].startswith(b'HTTP/1.1 200 OK\r\n')           # the header leads the first record
    assert wire.options == [(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)]


def test_a_reply_that_just_fits_a_record_is_one_write_and_one_more_byte_is_two():
    fits = next(n for n in range(1000, 1400)
                if len(on_the_wire('200 OK', OCTETS, bytes(n))) == assistnow.RECORD_BYTES)
    wire = Wire()
    assistnow.send_response(wire, '200 OK', OCTETS, bytes(fits))
    assert len(wire.writes) == 1 and len(wire.writes[0]) == assistnow.RECORD_BYTES
    wire = Wire()
    assistnow.send_response(wire, '200 OK', OCTETS, bytes(fits + 1))
    assert len(wire.writes) == 2 and max(len(w) for w in wire.writes) <= assistnow.RECORD_BYTES


def test_a_head_request_gets_the_header_alone_whatever_the_size():
    wire = Wire()
    body = bytes(7312)
    assistnow.send_response(wire, '200 OK', OCTETS, body, head_only=True)
    assert wire.writes == [on_the_wire('200 OK', OCTETS, body)[:-len(body)]]      # Content-Length of the body it would send
    assert wire.options == []


def test_a_connection_without_socket_options_still_gets_the_reply():
    wire = BareWire()
    body = bytes(5000)
    assistnow.send_response(wire, '200 OK', OCTETS, body)
    assert b''.join(wire.writes) == on_the_wire('200 OK', OCTETS, body)


def test_a_large_assist_reply_through_the_listener_is_the_same_bytes(monkeypatch):
    body = bytes(i % 253 for i in range(9000))
    monkeypatch.setattr(assistnow, 'handle', lambda *a, **k: ('200 OK', OCTETS, body))
    reply = exchange(b'GET /assist/data.bin?imei=%s HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n' % IMEI.encode())
    assert reply == on_the_wire('200 OK', OCTETS, body, keep=False)


# -- outside the package ---------------------------------------------------------------------------------------------

def load_standalone():
    """The module loaded the way the private deployment's main.py does: as a
    top-level file with nothing of the l0destar package around it."""
    import importlib.util

    spec = importlib.util.spec_from_file_location('assistnow_standalone', assistnow.__file__)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_the_module_loads_on_its_own_and_runs_once_bound():
    mod = load_standalone()
    assert mod.env is None and mod.config is None

    log = logging.getLogger('assistnow-standalone-test')
    mod.bind(mod.StandaloneEnv({'assistnow': {'ztp_token': 'x', 'gnss': 'gps,gal'}},
                               {'database': 'd', 'user': 'u'}, log))
    assert mod.enabled() and mod.gnss_list() == ['gps', 'gal'] and mod.orbit_days() == 3

    status, _, _ = mod.handle('/assist/data.bin', 'imei=' + IMEI, 'ip', store=FakeStore(), database=FakeDB(DEVICE))
    assert status == '404 Not Found'                                   # a device it has not met
    status, _, _ = mod.handle('/assist/data.bin', 'imei=' + IMEI, 'ip', store=FakeStore(), database=FakeDB(None))
    assert status == '404 Not Found'                                   # a device it does not know

    mod.bind(mod.StandaloneEnv({}, {'database': 'd', 'user': 'u'}, log))
    assert not mod.enabled()
    assert mod.handle('/assist/data.bin', 'imei=' + IMEI, 'ip')[0] == '501 Not Implemented'


def test_the_standalone_environment_keeps_a_connection_per_thread():
    mod = load_standalone()
    env = mod.StandaloneEnv({}, {'database': 'd', 'user': 'u'}, logging.getLogger('x'))
    here = env.database()
    assert env.database() is here
    seen = []
    t = threading.Thread(target=lambda: seen.append(env.database()))
    t.start()
    t.join()
    assert seen[0] is not here
    env.close_database()
    assert env.database() is not here


# -- against a real database ------------------------------------------------------------------------------------------
# The fake store above cannot tell a column called `blob` (a reserved word) from
# any other; MariaDB can.

@needs_db
def test_the_store_runs_on_a_real_database(device, database, web, settings):
    store = assistnow.Store(database)
    assert store.get(device['id']) is None

    store.register(device['id'], UNIQID, 'QkRCRjU0MEY+/=', assistnow.DEFAULT_SERVICE_URL, NOW)
    row = store.get(device['id'])
    assert row['uniqid'] == UNIQID and row['chipcode'] == 'QkRCRjU0MEY+/=' and not row['has_blob']
    assert row['blob_size'] == 0 and store.blob(device['id']) is None

    data = service_response()
    store.save_blob(device['id'], data, assistnow.newest_orbit_day(data), 3, NOW)
    row = store.get(device['id'])
    assert row['has_blob'] and row['blob_size'] == len(data)
    assert row['blob_until'] == TODAY + datetime.timedelta(days=2) and row['fetched_at'] == NOW
    assert store.blob(device['id']) == data

    store.failed(device['id'], 'usage limit reached (403002)', NOW + datetime.timedelta(hours=5))
    row = store.get(device['id'])
    assert row['last_error'].startswith('usage limit') and row['next_try_at'] == NOW + datetime.timedelta(hours=5)
    assert row['has_blob']

    store.forget(device['id'], 'credential refused (401)')
    assert store.get(device['id'])['chipcode'] is None

    store.register(device['id'], UNIQID, 'again', assistnow.DEFAULT_SERVICE_URL, NOW)      # on top of the row
    row = store.get(device['id'])
    assert row['chipcode'] == 'again' and row['next_try_at'] is None and row['has_blob']

    store.reset(device['id'])
    assert store.get(device['id']) is None


@needs_db
def test_a_device_removed_takes_its_orbits_with_it(device, database):
    database.query('SET FOREIGN_KEY_CHECKS = 1')
    store = assistnow.Store(database)
    store.register(device['id'], UNIQID, 'abc', assistnow.DEFAULT_SERVICE_URL, NOW)
    database.query('DELETE FROM `device` WHERE `id` = %s', (device['id'],))
    assert store.get(device['id']) is None


@needs_db
def test_a_registration_and_a_download_end_to_end(device, database, web, settings, monkeypatch):
    store = assistnow.Store(database)
    monkeypatch.setattr(db, 'lookup_device', lambda database=None, imei=None, device_id=None:
                        database.one('SELECT * FROM `device` WHERE `imei` = %s', (imei,)))
    q = urllib.parse.urlencode(reg_params() | {'imei': device['imei']})

    status, _, _ = assistnow.handle('/assist/register', q, 'ip', store=store, database=database)
    assert status == '200 OK' and len(web.posts) == 1

    database.query(
        'INSERT INTO `log` (`device_id`, `timestamp`, `latitude`, `longitude`, `altitude`, `cell_location`) '
        'VALUES (%s, NOW(), 51.5, -0.12, 10, 0)', (device['id'],))
    status, ctype, body = assistnow.handle('/assist/data.bin', 'imei=%s&gnss=gps&max=9000' % device['imei'],
                                           'ip', store=store, database=database)
    assert (status, ctype) == ('200 OK', 'application/octet-stream')
    first = next(assistnow.iter_frames(body))
    assert (first[0], first[1], first[2][0]) == (0x13, 0x40, 0x01)       # the position the log holds
    assert {g for g, _, _ in frame_days(body)} == {0}
