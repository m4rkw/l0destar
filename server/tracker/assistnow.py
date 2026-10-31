"""u-blox AssistNow orbit assistance for devices with an external GNSS receiver.

A receiver that has sat unpowered for hours holds no usable ephemerides and
has to read them from the satellites, which takes 25 to 30 seconds with a
clear sky and minutes with a poor one.  AssistNow Predictive Orbits (free from
u-blox) tells it where the satellites will be for the coming days, so it need
only hear a few of them.

The device does not talk to u-blox; this does.  That is on purpose:

  * what a reboot or a firmware update would lose on the device is kept here;
  * u-blox limits requests per device per day (300 a month on an evaluation
    profile, 2 a day on a commercial one), and a unit that reboots on the
    bench or asks at every wake would spend that in an afternoon -- this asks
    once a day however often the device does;
  * the credential for u-blox's registration service (the ZTP token) lives in
    this server's config and never goes into a firmware image.

The sequence:

 1. A device that needs assistance asks ``GET /assist/data.bin?imei=...``.
    404 means this server has not met its receiver.
 2. It then sends ``GET /assist/register?imei=...&uid=...&ver=...`` with the
    receiver's replies to the chip id (UBX-SEC-UNIQID) and version
    (UBX-MON-VER) polls, as hex, byte for byte.  They are passed to u-blox's
    zero-touch provisioning service with the ZTP token, which answers with a
    credential (the "chipcode") for that one receiver.  The first download
    follows at once.
 3. From then on the orbits are downloaded at most once in ``refresh_hours``
    (in the background, never while a device waits) and each ``data.bin``
    request is answered from the last download: the almanac and, for each
    satellite, the one orbit message stamped nearest today, for the
    constellations the device named, cut to the size it said it had room for,
    a constellation at a time.  With a recent position of the device's own
    ahead of it (UBX-MGA-INI-POS_LLH) when there is one.  The device adds
    the time and hands the lot to the receiver.

Registration is first-come: a device that has registered a receiver is
refused a different one (403) until an operator runs ``tools/assistnow.py
reset``, so a stray request cannot replace a receiver's registration, and
cannot use up the few devices an evaluation profile allows.

The chipcode and the token are secrets of the same standing as a device's PSK.
Nothing here logs either, nor the URL that carries the chipcode, nor u-blox's
error bodies; of a refusal only its code is kept and, when it has the shape
u-blox documents, its short message (``_service_error``).
"""

import datetime
import re
import socket
import threading
import time
import urllib.parse

import requests

try:
    from . import config, db, logs
except ImportError:     # not inside the l0destar server package: see StandaloneEnv
    config = db = logs = None

DEFAULT_ZTP_URL = 'https://api.thingstream.io/ztp/assistnow/credentials'
DEFAULT_SERVICE_URL = 'https://assistnow.services.u-blox.com/GetAssistNowData.ashx'

# The most a TLS record of ours may hold; see send_response.
RECORD_BYTES = 1400

# -- UBX framing -------------------------------------------------------------

SYNC = b'\xb5\x62'
CLS_MGA = 0x13
ID_ANO = 0x20
ID_INI = 0x40
INI_POS_LLH = 0x01
ALM_TYPE = 0x02
MAX_PAYLOAD = 1024

# name -> (gnssId in an orbit message, id of that constellation's MGA messages)
CONSTELLATIONS = {
    'gps': (0, 0x00),
    'gal': (2, 0x02),
    'bds': (3, 0x03),
    'qzss': (5, 0x05),
    'glo': (6, 0x06),
}
_BY_GNSS_ID = {gid: name for name, (gid, _) in CONSTELLATIONS.items()}
_BY_MSG_ID = {mid: name for name, (_, mid) in CONSTELLATIONS.items()}

# An orbit message is stamped for 12:00 UTC of its day and is most accurate
# then.  One stamped further from today than this is not offered.
ANO_MAX_DISTANCE_DAYS = 3


def fletcher(data):
    a = b = 0
    for x in data:
        a = (a + x) & 0xFF
        b = (b + a) & 0xFF
    return bytes((a, b))


def frame(cls, msg_id, payload):
    body = bytes((cls, msg_id)) + len(payload).to_bytes(2, 'little') + payload
    return SYNC + body + fletcher(body)


def iter_frames(data):
    """Every well-formed UBX frame in ``data`` as (class, id, payload, raw).
    Bytes that are not part of one are skipped."""
    i, n = 0, len(data)
    while i + 8 <= n:
        j = data.find(SYNC, i)
        if j < 0 or j + 8 > n:
            return
        length = data[j + 4] | (data[j + 5] << 8)
        end = j + 8 + length
        if length <= MAX_PAYLOAD and end <= n and fletcher(data[j + 2:end - 2]) == data[end - 2:end]:
            yield data[j + 2], data[j + 3], data[j + 6:end - 2], data[j:end]
            i = end
        else:
            i = j + 1


def split(data):
    """A service response as ({constellation: [almanac frames]},
    {constellation: {satellite: [(day, orbit frame)]}})."""
    alm, ano = {}, {}
    for cls, msg_id, payload, raw in iter_frames(data):
        if cls != CLS_MGA:
            continue
        if msg_id == ID_ANO and len(payload) == 76:
            name = _BY_GNSS_ID.get(payload[3])
            try:
                day = datetime.date(2000 + payload[4], payload[5], payload[6])
            except ValueError:
                continue
            if name:
                ano.setdefault(name, {}).setdefault(payload[2], []).append((day, raw))
        elif msg_id in _BY_MSG_ID and payload and payload[0] == ALM_TYPE:
            alm.setdefault(_BY_MSG_ID[msg_id], []).append(raw)
    return alm, ano


def newest_orbit_day(data):
    """The last day any orbit message in ``data`` is stamped for, or None."""
    days = [d for sats in split(data)[1].values() for cands in sats.values() for d, _ in cands]
    return max(days) if days else None


def select(data, gnss, today, max_bytes, pos=None):
    """What to give a device: (bytes, {'ano': n, 'alm': n, 'gnss': [names]}).

    A constellation at a time, in the order the device named them, each as its
    almanac messages and then, for every satellite, the single orbit message
    stamped nearest ``today`` (and within ANO_MAX_DISTANCE_DAYS of it).  The
    first constellation that does not fit ends it: the device's buffer is
    sized for a few, not for a part of one.  ``pos``, a position frame, goes
    first and counts against the size.
    """
    alm, ano = split(data)
    out = [pos] if pos else []
    size = len(pos or b'')
    info = {'ano': 0, 'alm': 0, 'gnss': []}

    for name in gnss:
        if name not in CONSTELLATIONS:
            continue
        chunk = list(alm.get(name, []))
        n_alm = len(chunk)
        n_ano = 0
        for sv in sorted(ano.get(name, {})):
            best = min(ano[name][sv], key=lambda c: (abs((c[0] - today).days), c[0]))
            if abs((best[0] - today).days) <= ANO_MAX_DISTANCE_DAYS:
                chunk.append(best[1])
                n_ano += 1
        if not chunk:
            continue
        need = sum(len(f) for f in chunk)
        if size + need > max_bytes:
            break
        out.extend(chunk)
        size += need
        info['ano'] += n_ano
        info['alm'] += n_alm
        info['gnss'].append(name)
    return b''.join(out), info


def pos_frame(lat_e7, lon_e7, alt_cm, acc_cm):
    """UBX-MGA-INI-POS_LLH."""
    payload = (bytes((INI_POS_LLH, 0, 0, 0))
               + lat_e7.to_bytes(4, 'little', signed=True)
               + lon_e7.to_bytes(4, 'little', signed=True)
               + alt_cm.to_bytes(4, 'little', signed=True)
               + acc_cm.to_bytes(4, 'little'))
    return frame(CLS_MGA, ID_INI, payload)


def check_reply(hex_text, cls, msg_id, min_payload, max_payload):
    """A receiver reply sent as hex, checked to be what it says it is.
    Returns its bytes; ValueError says what is wrong."""
    text = (hex_text or '').strip()
    if not text or len(text) > 2 * (MAX_PAYLOAD + 8):
        raise ValueError('reply missing or too long')
    try:
        raw = bytes.fromhex(text)
    except ValueError:
        raise ValueError('reply is not hex')
    frames = list(iter_frames(raw))
    if len(frames) != 1 or frames[0][3] != raw:
        raise ValueError('reply is not a single UBX frame')
    f_cls, f_id, payload, _ = frames[0]
    if (f_cls, f_id) != (cls, msg_id) or not min_payload <= len(payload) <= max_payload:
        raise ValueError('reply is not the one asked for')
    return raw



# -- the server around it ---------------------------------------------------------

class PackageEnv:
    """What this needs of the server around it, from the l0destar server
    package: its configuration, a database handle per thread, its logs."""

    def settings(self):
        return config.get('assistnow') or {}

    def database(self):
        return db.tls

    def close_database(self):
        db.tls.close()

    def lookup_device(self, database, imei):
        return db.lookup_device(database=database, imei=imei)

    @property
    def log(self):
        return logs.tls

    @property
    def app_log(self):
        return logs.app


class _Connection:
    """A minimal MySQL handle (one/all/query), for StandaloneEnv."""

    def __init__(self, settings):
        self._settings = settings
        self._conn = None

    def _connect(self):
        import pymysql
        import pymysql.cursors

        if self._conn is not None:
            try:
                self._conn.ping(reconnect=False)
            except Exception:
                self.close()
        if self._conn is None:
            self._conn = pymysql.connect(
                host=self._settings.get('host', '127.0.0.1'),
                port=int(self._settings.get('port', 3306)),
                database=self._settings['database'],
                user=self._settings['user'],
                password=self._settings.get('password', ''),
                cursorclass=pymysql.cursors.DictCursor,
                autocommit=True,
            )
        return self._conn

    def close(self):
        if self._conn is not None:
            try:
                self._conn.close()
            except Exception:
                pass
            self._conn = None

    def one(self, sql, params=None):
        with self._connect().cursor() as cur:
            cur.execute(sql, params)
            return cur.fetchone()

    def all(self, sql, params=None):
        with self._connect().cursor() as cur:
            cur.execute(sql, params)
            return cur.fetchall()

    def query(self, sql, params=None):
        with self._connect().cursor() as cur:
            cur.execute(sql, params)
            return cur.lastrowid


class StandaloneEnv:
    """The same, for a server that is not that package (the private
    deployment's main.py): its parsed config.yaml, its database settings and
    the loggers it already has.  A connection per thread, since a pymysql
    connection is not shared between them.

        import assistnow
        assistnow.bind(assistnow.StandaloneEnv(_config, DB_CONFIG, tls_log))
    """

    def __init__(self, settings, db_config, log, app_log=None):
        self._settings = settings
        self._db_config = db_config
        self.log = log
        self.app_log = app_log or log
        self._local = threading.local()

    def settings(self):
        return self._settings.get('assistnow') or {}

    def database(self):
        handle = getattr(self._local, 'db', None)
        if handle is None:
            handle = self._local.db = _Connection(self._db_config)
        return handle

    def close_database(self):
        handle = getattr(self._local, 'db', None)
        if handle is not None:
            handle.close()
            self._local.db = None

    def lookup_device(self, database, imei):
        return database.one('SELECT * FROM `device` WHERE `imei` = %s', (imei,))


env = PackageEnv() if config is not None else None


def bind(new_env):
    """Use ``new_env`` for the server around this (see StandaloneEnv)."""
    global env
    env = new_env

# -- configuration ----------------------------------------------------------------

def _settings():
    return env.settings()


def enabled():
    return bool(_settings().get('ztp_token'))


def gnss_list():
    names = _settings().get('gnss') or ['gps', 'gal', 'bds']
    if isinstance(names, str):
        names = names.split(',')
    return [str(n).strip().lower() for n in names if str(n).strip().lower() in CONSTELLATIONS]


def orbit_days():
    days = int(_settings().get('orbit_days', 3))
    return days if days in (1, 3, 7, 14) else 3


def refresh_hours():
    return float(_settings().get('refresh_hours', 20))


def _service_url(value):
    """Where u-blox says to fetch from, if that is somewhere u-blox runs."""
    if isinstance(value, str):
        parts = urllib.parse.urlsplit(value)
        if parts.scheme == 'https' and (parts.hostname or '').endswith('.u-blox.com') and parts.path:
            return value
    return DEFAULT_SERVICE_URL


def _now():
    return datetime.datetime.now(datetime.timezone.utc).replace(tzinfo=None)


# -- u-blox ----------------------------------------------------------------------

class AssistError(Exception):
    """Something u-blox's side would not do.  ``retry`` is seconds before it
    is worth asking again; ``forget`` means the credential was refused and a
    registration has to be redone; ``limit`` means u-blox said a usage limit
    had been reached.  The text is safe to log."""

    def __init__(self, message, status=502, retry=900, forget=False, limit=False):
        super().__init__(message)
        self.status = status
        self.retry = retry
        self.forget = forget
        self.limit = limit


def _until_midnight():
    now = _now()
    tomorrow = datetime.datetime.combine(now.date() + datetime.timedelta(days=1), datetime.time(0, 1))
    return max(900, int((tomorrow - now).total_seconds()))


def ztp(uid_hex, ver_hex):
    """Register a receiver with u-blox: (chipcode, service url)."""
    body = {'token': _settings().get('ztp_token'),
            'messages': {'UBX-SEC-UNIQID': uid_hex, 'UBX-MON-VER': ver_hex}}
    try:
        r = requests.post(_settings().get('ztp_url') or DEFAULT_ZTP_URL, json=body, timeout=(5, 20))
    except requests.RequestException as e:
        raise AssistError('registration request failed (%s)' % type(e).__name__)
    if r.status_code != 200:
        raise AssistError('registration refused by u-blox: HTTP %d' % r.status_code,
                          retry=3600 if r.status_code < 500 else 900)
    try:
        reply = r.json()
        chipcode = reply['chipcode']
        url = reply.get('serviceUrl')
    except (ValueError, KeyError, TypeError, AttributeError):
        raise AssistError('registration reply not understood')
    if not isinstance(chipcode, str) or not 0 < len(chipcode) <= 200:
        raise AssistError('registration reply carried no usable credential')
    return chipcode, _service_url(url)


def download(chipcode, service_url, gnss, days):
    """The orbits and almanac as u-blox sends them: UBX-MGA messages."""
    # Built by hand: the commas of the lists stay commas, as the examples
    # have them, and only the credential is percent-encoded.
    url = '%s?chipcode=%s&gnss=%s&data=uporb_%d,ualm' % (
        service_url, urllib.parse.quote(chipcode, safe=''), ','.join(gnss), days)
    try:
        r = requests.get(url, timeout=(5, 60))
    except requests.RequestException as e:
        raise AssistError('download failed (%s)' % type(e).__name__)

    if r.status_code == 200:
        data = r.content
        if next(iter_frames(data), None) is None:
            raise AssistError('download held no messages', retry=3600)
        return data

    code, said = _service_error(r, chipcode)
    detail = '%s: %s' % (code, said) if said else code
    if r.status_code == 401:
        raise AssistError('credential refused (%s)' % detail, forget=True)
    if r.status_code == 403 and code in ('403002', '403003', '403004'):
        raise AssistError('usage limit reached (%s)' % detail, retry=_until_midnight(), limit=True)
    if r.status_code in (400, 403):
        raise AssistError('request not allowed (%s)' % detail, retry=6 * 3600)
    raise AssistError('u-blox answered %s' % detail)


def _service_error(r, chipcode):
    """(code, message) out of a refusal.

    u-blox answers an error with JSON, {"ErrorCode": 403002, "Message": "..."}.
    Its message is kept only from a body of exactly that shape, with the
    credential taken out of it and cut short, so that what a refusal says
    (which is what tells a spent allowance from a receiver not yet usable)
    reaches the log without the body of an arbitrary reply doing so.  Any
    other body gives the code alone: a six digit number found in its text, or
    the HTTP status.
    """
    try:
        body = r.json()
    except ValueError:
        body = None
    if (isinstance(body, dict) and isinstance(body.get('ErrorCode'), int)
            and isinstance(body.get('Message'), str)):
        said = body['Message']
        for secret in (chipcode, urllib.parse.quote(chipcode, safe='')):
            if secret:
                said = said.replace(secret, '')
        return str(body['ErrorCode']), re.sub(r'[^\x20-\x7e]', '', said).strip()[:100]
    match = re.search(r'\b(40\d{4}|500\d{3})\b', (r.text or '')[:300])
    return (match.group(1) if match else str(r.status_code)), ''


# -- storage --------------------------------------------------------------------

class Store:
    """The device_assist table.  The orbits themselves (``blob``) are not read
    with the row: a status check should not drag a hundred kilobytes."""

    COLUMNS = ('device_id, uniqid, chipcode, service_url, registered_at, fetched_at, '
               'blob_until, blob_days, next_try_at, last_error, '
               '(`blob` IS NOT NULL) AS has_blob, COALESCE(LENGTH(`blob`), 0) AS blob_size')

    def __init__(self, database=None):
        self.db = database or env.database()

    def get(self, device_id):
        return self.db.one('SELECT %s FROM `device_assist` WHERE `device_id` = %%s' % self.COLUMNS,
                           (device_id,))

    def blob(self, device_id):
        row = self.db.one('SELECT `blob` FROM `device_assist` WHERE `device_id` = %s', (device_id,))
        return (row or {}).get('blob')

    def register(self, device_id, uniqid, chipcode, service_url, now):
        self.db.query(
            'INSERT INTO `device_assist` (`device_id`, `uniqid`, `chipcode`, `service_url`, '
            '`registered_at`) VALUES (%s, %s, %s, %s, %s) '
            'ON DUPLICATE KEY UPDATE `uniqid` = VALUES(`uniqid`), `chipcode` = VALUES(`chipcode`), '
            '`service_url` = VALUES(`service_url`), `registered_at` = VALUES(`registered_at`), '
            '`next_try_at` = NULL, `last_error` = NULL',
            (device_id, uniqid, chipcode, service_url, now))

    def save_blob(self, device_id, blob, until, days, now):
        self.db.query(
            'UPDATE `device_assist` SET `blob` = %s, `blob_until` = %s, `blob_days` = %s, '
            '`fetched_at` = %s, `next_try_at` = NULL, `last_error` = NULL WHERE `device_id` = %s',
            (blob, until, days, now, device_id))

    def failed(self, device_id, error, next_try):
        self.db.query(
            'UPDATE `device_assist` SET `last_error` = %s, `next_try_at` = %s WHERE `device_id` = %s',
            (error[:250], next_try, device_id))

    def forget(self, device_id, error):
        self.db.query(
            'UPDATE `device_assist` SET `chipcode` = NULL, `last_error` = %s, `next_try_at` = NULL '
            'WHERE `device_id` = %s', (error[:250], device_id))

    def reset(self, device_id):
        self.db.query('DELETE FROM `device_assist` WHERE `device_id` = %s', (device_id,))


# -- refreshing -------------------------------------------------------------------

_locks = {}
_locks_guard = threading.Lock()
_inflight = set()
_cooldown = {}       # imei -> monotonic time before which a registration is not attempted again
COOLDOWN_S = 60
SETTLING_S = 1800    # a receiver registered this recently, and not yet served, may still be settling
SETTLING_RETRY_S = 60


def _device_lock(device_id):
    with _locks_guard:
        return _locks.setdefault(device_id, threading.RLock())


def due(row, now):
    """Is a download worth making: there is a credential, no back-off is
    running, and there is nothing yet, or what there is is old or runs out
    today."""
    if not row or not row['chipcode']:
        return False
    if row['next_try_at'] and now < row['next_try_at']:
        return False
    if not row['has_blob'] or row['fetched_at'] is None:
        return True
    if (now - row['fetched_at']).total_seconds() > refresh_hours() * 3600:
        return True
    return row['blob_until'] is None or row['blob_until'] <= now.date()


def settling(row, now):
    """Is this a receiver registered a moment ago that has not been served yet.

    u-blox refused the first request of the first receiver this server
    registered with a usage limit it had not touched, 137 ms after creating its
    Thing (2026-10-30), and answered the identical request 49 minutes later.
    Waiting for midnight on the strength of that turned a blip into a night
    without orbits, so a refusal in this state is tried again soon."""
    registered = row.get('registered_at')
    return (row['fetched_at'] is None and registered is not None
            and (now - registered).total_seconds() < SETTLING_S)


def refresh(device, store=None, force=False, now=None):
    """Download the orbits for one device if they are due (or ``force``).
    Failures are recorded on the row and not raised.  True if it downloaded."""
    store = store or Store()
    now = now or _now()
    log = env.log

    with _device_lock(device['id']):
        row = store.get(device['id'])
        if not row or not row['chipcode']:
            return False
        if not force and not due(row, now):
            return False
        days = orbit_days()
        try:
            data = download(row['chipcode'], row['service_url'] or DEFAULT_SERVICE_URL, gnss_list(), days)
        except AssistError as e:
            if e.forget:
                store.forget(device['id'], str(e))
            else:
                retry = e.retry
                if e.limit and settling(row, now):
                    retry = min(retry, SETTLING_RETRY_S)
                store.failed(device['id'], str(e), now + datetime.timedelta(seconds=retry))
            log.warning('assistnow: %s: %s', device['imei'], e)
            return False
        store.save_blob(device['id'], data, newest_orbit_day(data), days, now)
        log.info('assistnow: %s orbits downloaded, %d bytes, to %s',
                 device['imei'], len(data), newest_orbit_day(data))
        return True


def refresh_async(device):
    """``refresh`` in the background, one at a time per device, on a database
    connection of its own."""
    with _locks_guard:
        if device['id'] in _inflight:
            return
        _inflight.add(device['id'])

    def run():
        try:
            refresh(device)
        except Exception:
            env.app_log.exception('assistnow refresh failed for %s', device['imei'])
        finally:
            with _locks_guard:
                _inflight.discard(device['id'])
            env.close_database()

    threading.Thread(target=run, name='assistnow', daemon=True).start()


# -- what the device is told ------------------------------------------------------------

def position_hint(device_id, database, now):
    """A UBX-MGA-INI-POS_LLH frame for the device's last GNSS fix, or None.

    The receiver is told a position wrong by more than the accuracy given
    does harm, so the accuracy grows with the fix's age, generously: a parked
    vehicle has not moved, one that was carried or towed could be anywhere,
    and past a week nothing is said.
    """
    row = database.one(
        'SELECT `latitude`, `longitude`, `altitude`, `timestamp` FROM `log` '
        'WHERE `device_id` = %s AND `latitude` IS NOT NULL AND `longitude` IS NOT NULL '
        'AND `cell_location` = 0 ORDER BY `id` DESC LIMIT 1', (device_id,))
    if not row or row.get('timestamp') is None:
        return None
    age_h = abs((datetime.datetime.now() - row['timestamp']).total_seconds()) / 3600
    if age_h < 1:
        km = 5
    elif age_h < 24:
        km = 50
    elif age_h < 24 * 7:
        km = 200
    else:
        return None
    lat, lon = float(row['latitude']), float(row['longitude'])
    if not (-90 <= lat <= 90 and -180 <= lon <= 180):
        return None
    alt = row.get('altitude')
    return pos_frame(round(lat * 1e7), round(lon * 1e7),
                     round(float(alt) * 100) if alt is not None else 0, km * 100000)


def _text(body):
    return 'text/plain', (body + '\n').encode()


def _empty():
    return '204 No Content', 'text/plain', b''


def serve_data(device, params, store, database, now=None):
    """(status, content type, body) for GET /assist/data.bin."""
    now = now or _now()
    row = store.get(device['id'])
    if not row or not row['chipcode']:
        return ('404 Not Found',) + _text('not registered')

    names = [n for n in (params.get('gnss') or '').lower().split(',') if n in CONSTELLATIONS]
    try:
        room = max(1024, min(int(params.get('max') or 8192), 65536))
    except ValueError:
        room = 8192

    if due(row, now):
        refresh_async(device)
    blob = store.blob(device['id']) if row['has_blob'] else None
    if not blob:
        return _empty()

    body, info = select(blob, names or gnss_list(), now.date(), room, position_hint(device['id'], database, now))
    if not info['ano']:
        return _empty()
    env.log.info('assistnow: %s given %d bytes: %d orbits, %d almanacs (%s)',
                  device['imei'], len(body), info['ano'], info['alm'], ','.join(info['gnss']))
    return '200 OK', 'application/octet-stream', body


def serve_register(device, params, store, now=None):
    """(status, content type, body) for GET /assist/register."""
    now = now or _now()
    log = env.log
    try:
        uid = check_reply(params.get('uid'), 0x27, 0x03, 10, 10)
        ver = check_reply(params.get('ver'), 0x0A, 0x04, 40, MAX_PAYLOAD)
    except ValueError as e:
        return ('400 Bad Request',) + _text(str(e))
    uniqid = uid[10:16].hex().upper()           # the chip id: payload bytes 4 to 9

    # One registration at a time per device: a device that asks twice in a
    # row (its first answer was slow) must not register its receiver twice.
    with _device_lock(device['id']):
        row = store.get(device['id'])
        if row and row['chipcode']:
            if row['uniqid'] == uniqid:
                return ('200 OK',) + _text('registered')
            log.warning('assistnow: %s offered a receiver other than the one registered: refused',
                        device['imei'])
            return ('403 Forbidden',) + _text('another receiver is registered')

        # After a failure only: a device told to try again does, and a stray
        # request must not be able to keep u-blox busy with this one's.
        with _locks_guard:
            if time.monotonic() < _cooldown.get(device['imei'], 0):
                return ('429 Too Many Requests',) + _text('try again later')

        try:
            chipcode, service_url = ztp(uid.hex().upper(), ver.hex().upper())
        except AssistError as e:
            with _locks_guard:
                _cooldown[device['imei']] = time.monotonic() + COOLDOWN_S
            log.warning('assistnow: %s: %s', device['imei'], e)
            return ('502 Bad Gateway',) + _text('registration failed')

        store.register(device['id'], uniqid, chipcode, service_url, now)
        log.info('assistnow: %s registered, receiver %s', device['imei'], uniqid)
        # The first download now, not at the device's next ask.
        refresh(device, store, force=True, now=now)
    return ('200 OK',) + _text('registered')


def handle(path, query, ip, log=None, store=None, database=None):
    """Answer one GET under /assist/: (status line, content type, body)."""
    log = log or env.log
    if not enabled():
        return ('501 Not Implemented',) + _text('assistnow is not configured')

    params = {k: v[0] for k, v in urllib.parse.parse_qs(query).items() if v}
    imei = params.get('imei', '')
    database = database or env.database()
    device = env.lookup_device(database, imei) if imei.isdigit() else None
    if device is None:
        log.info('assistnow: %s from %s -> 404 (no usable imei)', path, ip)
        return ('404 Not Found',) + _text('not found')

    store = store or Store(database)
    if path == '/assist/data.bin':
        return serve_data(device, params, store, database)
    if path == '/assist/register':
        return serve_register(device, params, store)
    return ('404 Not Found',) + _text('not found')


def send_response(conn, status, ctype, body, keep=True, head_only=False):
    """Write one HTTP response in TLS records the nRF91 modem can decode.

    The modem decodes a record of about 2 KB at most
    (NRF_SOCKET_TLS_MAX_MESSAGE_SIZE) and fails the read of a larger one with
    EMSGSIZE (-122); that is why a firmware image comes down in 2 KB ranges.
    One ``sendall`` of the 7 KB orbit reply is one 7 KB record, which the board
    could not read on its first real reply (2026-10-30).  So a reply that does
    not fit a record goes out in slices of RECORD_BYTES, each a record of its
    own, the first carrying the header.  TCP_NODELAY keeps Nagle from holding
    each slice back for the ACK of the one before.

    A small reply (the 204s and 404s, the registration answer) is one write,
    as it always was, and the bytes on the wire are those of the firmware
    listener's own writer either way.
    """
    header = ('HTTP/1.1 %s\r\n'
              'Content-Type: %s\r\n'
              'Content-Length: %d\r\n'
              'Connection: %s\r\n'
              '\r\n' % (status, ctype, len(body), 'keep-alive' if keep else 'close')).encode('ascii')

    if head_only:
        conn.sendall(header)
        return
    if len(header) + len(body) <= RECORD_BYTES:
        conn.sendall(header + body)
        return

    try:
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    except (OSError, AttributeError):
        pass                    # slower, not wrong
    first = RECORD_BYTES - len(header)
    conn.sendall(header + body[:first])
    for start in range(first, len(body), RECORD_BYTES):
        conn.sendall(body[start:start + RECORD_BYTES])
