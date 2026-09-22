"""JSON API.

Two audiences with different authentication:

* Browser endpoints (``/api/1.0/carpos``, ``journeys``, ``journey/<id>/points``)
  are session-authenticated and only ever read.
* Automation endpoints (``track``, ``home``, ``config``, ``command``) take a
  bearer token from the ``api_token`` table.  Tokens are opaque, minted by
  ``tools/gentoken.py``, and carry no scopes — anything holding one can queue a
  command, so treat one as equivalent to console access.

Which device a request is about is settled in ``devices.py``: a read that
names none falls back to ``default_device`` or the only enrolled device, and
anything that changes state has to name its device.
"""

import math
import re

from flask import Blueprint, redirect, request

from .. import config, db, notify, telemetry
from . import audit, devices, error, ok, unauthorised
from .auth import login_required

bp = Blueprint('api', __name__, url_prefix='/api/1.0')

# Commands that supersede one another.  Queuing `locate` when a `locate` is
# already pending should not make the device send two positions.
COMMAND_CONFLICTS = {
    'locate':    {'locate', 'locatenow'},
    'locatenow': {'locate', 'locatenow'},
    'tomtom':    {'tomtom', 'tomtomnow'},
    'tomtomnow': {'tomtom', 'tomtomnow'},
    'poweroff':  {'poweroff'},
}

# Settings the server acts on itself.  These never reach the device — alerting
# on ignition is the server's job, so putting them in the device's queue would
# spend radio time to no purpose.
SERVER_SIDE_SETTINGS = {
    'alarm': 'alarm',
    'garage': 'garage',
    'overnightalarm': 'overnight_alarm',
    'overnight_alarm_hour_from': 'overnight_alarm_hour_from',
    'overnight_alarm_hour_to': 'overnight_alarm_hour_to',
}

CONFIG_FIELDS = {
    'int': 'int',
    'ma': 'movement_alarm',
    'al': 'alarm',
    'ga': 'garage',
    'oa': 'overnight_alarm',
    'oaf': 'overnight_alarm_hour_from',
    'oat': 'overnight_alarm_hour_to',
}

# The values each setting's column can hold.
SETTING_RANGES = {
    'int': (0, 4294967295),
    'movement_alarm': (0, 1),
    'alarm': (0, 1),
    'garage': (0, 1),
    'overnight_alarm': (0, 1),
    'overnight_alarm_hour_from': (0, 23),
    'overnight_alarm_hour_to': (0, 23),
}


def _setting(column, value):
    """``value`` as an int if it is a whole number in the column's range, else
    None.  A boolean or a fraction is refused rather than turned into 1 or
    truncated, and an out-of-range number is refused before the database
    rejects it."""
    if isinstance(value, bool):
        return None
    if isinstance(value, str) and re.fullmatch(r'\s*[+-]?\d+\s*', value):
        value = int(value)
    if not isinstance(value, int):
        return None
    low, high = SETTING_RANGES[column]
    return value if low <= value <= high else None


def _setting_error(key, column):
    low, high = SETTING_RANGES[column]
    return error('%s must be a whole number from %d to %d' % (key, low, high))


def _flag(name):
    """A query-string switch: 1, true, yes or on turn it on; anything else,
    0 and false included, leaves it off."""
    return request.args.get(name, '').strip().lower() in ('1', 'true', 'yes', 'on')


def bearer_ok():
    header = request.headers.get('Authorization', '')
    if not header.startswith('Bearer '):
        return False
    token = header[7:].strip()
    if not token:
        return False
    return db.web.one('SELECT `id` FROM `api_token` WHERE `token` = %s',
                      (token,)) is not None


def _device(allow_default=True):
    """The device a request is about, and the error to answer without one.

    Which error depends on whether the request tried: a device it named that
    is not enrolled is not found, and one that named none where no fallback
    applies has to name one.
    """
    device = devices.from_request(allow_default=allow_default)
    if device:
        return device, None
    if devices.named():
        return None, error('device not found')
    return None, error('imei or device_id required')


# -- browser endpoints -------------------------------------------------------

@bp.route('/carpos', methods=['GET'])
@login_required
def carpos():
    device, failure = _device()
    if failure:
        return failure
    row = devices.latest_log(device)
    if not row:
        return error('no records for device')
    return ok({
        'position': devices.position(row, track_mode=device.get('track_mode')),
        'accel_baseline': devices.accel_baseline(device),
        'track_mode': 1 if device.get('track_mode') else 0,
    })


@bp.route('/trackmode', methods=['GET', 'POST'])
@login_required
def trackmode():
    """The track-mode switch (firmware TRACK_MODE.md).

    POST ``{"on": 0|1}`` sets it and has to name the device; the device picks
    it up from its next reply, which while driving is at most APP_RESP_POLL_S
    away and in the mode at most APP_TRACK_RESP_INTERVAL_S.  GET reads it.
    """
    device, failure = _device(allow_default=request.method == 'GET')
    if failure:
        return failure
    if request.method == 'POST':
        data = request.get_json(silent=True) or {}
        on = 1 if data.get('on') else 0
        db.web.query('UPDATE `device` SET `track_mode` = %s WHERE `id` = %s',
                     (on, device['id']))
        device['track_mode'] = on
        # Switching a vehicle's GNSS off is worth a line in the audit log.
        audit('trackmode', 'on=%d imei=%s' % (on, device['imei']))
    return ok({'track_mode': 1 if device.get('track_mode') else 0})


@bp.route('/journeys', methods=['GET'])
@login_required
def journeys():
    device, failure = _device()
    if failure:
        return failure

    try:
        per_page = int(request.args.get('per_page', 50))
        page = int(request.args.get('page', 0))
    except ValueError:
        return error('page and per_page must be whole numbers')
    if page < 0 or per_page < 1:
        return error('page must be 0 or more, and per_page 1 or more')
    per_page = min(per_page, 200)

    rows = db.web.all(
        'SELECT `id`, `start_time`, `end_time`, `from_latitude`, `from_longitude`, '
        '`to_latitude`, `to_longitude`, `miles`, `from_place`, `to_place` '
        'FROM `journey` WHERE `device_id` = %s AND `end_time` IS NOT NULL '
        'ORDER BY `start_time` DESC LIMIT %s OFFSET %s',
        (device['id'], per_page, page * per_page),
    )

    return ok({'journeys': [{
        'id': r['id'],
        'start_time': r['start_time'].strftime('%Y-%m-%d %H:%M:%S') if r['start_time'] else None,
        'end_time': r['end_time'].strftime('%Y-%m-%d %H:%M:%S') if r['end_time'] else None,
        'from_latitude': devices.to_float(r['from_latitude']),
        'from_longitude': devices.to_float(r['from_longitude']),
        'to_latitude': devices.to_float(r['to_latitude']),
        'to_longitude': devices.to_float(r['to_longitude']),
        'miles': devices.to_float(r['miles']),
        'from_place': r['from_place'] or '',
        'to_place': r['to_place'] or '',
    } for r in rows]})


@bp.route('/journey/<int:journey_id>/points', methods=['GET'])
@login_required
def journey_points(journey_id):
    journey = db.web.one('SELECT * FROM `journey` WHERE `id` = %s', (journey_id,))
    if not journey:
        return error('journey not found')

    # The map page names the vehicle it follows.  Another vehicle's journey
    # is not part of that vehicle's history, so it is not found rather than
    # replayed on the wrong map.
    if devices.named():
        device = devices.from_request(allow_default=False)
        if not device or device['id'] != journey['device_id']:
            return error('journey not found')

    # Bound by log id rather than by time: the ids were recorded when the
    # journey opened and closed, so this cannot drift on clock skew or on a
    # record whose device timestamp lands outside the window.
    rows = db.web.all(
        'SELECT `latitude`, `longitude`, `speed`, `combined_speed`, `altitude`, '
        '`heading`, `timestamp`, `ignition_state`, `battery_level`, `mcc`, '
        '`mnc`, `rat`, `cell_location`, `track_mode`, '
        + ', '.join('`%s`' % c for c in
                    devices.OBD_LIVE_COLUMNS + devices.IMU_LIVE_COLUMNS)
        + ' FROM `log` '
        'WHERE `device_id` = %s AND `id` >= %s '
        'AND `id` <= %s ORDER BY `id`',
        (journey['device_id'], journey['start_log_id'], journey['end_log_id']),
    )

    # One operator lookup per distinct cell, not per point.
    cache = {}
    points = []
    for row in rows:
        key = (row.get('mcc'), row.get('mnc'))
        if key not in cache:
            cache[key] = db.lookup_operator(*key) or ''
        point = devices.position(row)
        point['operator'] = cache[key]
        points.append(point)

    return ok({'points': points})


# -- automation endpoints ----------------------------------------------------

@bp.route('/track', methods=['GET'])
def track_link():
    """Redirect to a map at the device's last known position.

    Exists so a phone shortcut or a home-automation rule can be a plain URL.
    """
    if not bearer_ok():
        return unauthorised()

    device, failure = _device()
    if failure:
        return failure
    row = devices.latest_log(device)
    if not row:
        return error('no records for device')

    coords = '%s,%s' % (row['latitude'], row['longitude'])
    if _flag('google'):
        url = 'https://maps.google.com/maps/place/%s/' % coords
    else:
        url = 'maps:ll=%s&q=%s' % (coords, device.get('name') or 'vehicle')

    if _flag('return'):
        return ok({'url': url})
    return redirect(url)


@bp.route('/home', methods=['POST'])
def home_check():
    """Report whether each listed vehicle's last fix is near its home.

    A stalled tracker parked at home is invisible from the inside: the last
    record still looks like a car sitting at home, which is exactly what a
    healthy tracker reports too.  An external cron calling this notices the
    other case — a vehicle that is not where it should be, with nothing having
    said so.  Every ``home_check`` entry is checked, or only the one ``imei``
    names.
    """
    if not bearer_ok():
        return unauthorised()
    checks = config.HOME_CHECKS
    if not checks:
        return error('home_check not configured')

    imei = devices.requested()[0]
    if imei:
        checks = [c for c in checks if c['imei'] == str(imei).strip()]
        if not checks:
            return error('no home_check entry for %s' % imei)

    return ok({'devices': [_check_home(check) for check in checks]})


def _check_home(check):
    """One home_check entry's verdict, notifying when the vehicle is away."""
    device = db.lookup_device(imei=check['imei'])
    if not device:
        return {'imei': check['imei'], 'name': None, 'error': 'device not found'}

    name = device.get('name') or device['imei']
    result = {'imei': device['imei'], 'name': name,
              'garage': bool(device.get('garage'))}
    row = devices.latest_log(device)
    if not row or row.get('latitude') is None or row.get('longitude') is None:
        result['error'] = 'no position recorded'
        return result

    distance_m = _distance_m(check['latitude'], check['longitude'],
                             float(row['latitude']), float(row['longitude']))
    result['at_home'] = distance_m <= check['radius_m']
    result['distance_m'] = round(distance_m, 1)

    if not result['at_home'] and not result['garage']:
        notify.send('%s: tracker may be stalled - vehicle is %dm from home'
                    % (name, int(distance_m)), title='Tracker home check')
    return result


def _distance_m(lat1, lon1, lat2, lon2):
    """Great-circle distance between two points, in metres."""
    phi1, phi2 = math.radians(lat1), math.radians(lat2)
    dphi = phi2 - phi1
    dlambda = math.radians(lon2 - lon1)
    h = (math.sin(dphi / 2) ** 2
         + math.cos(phi1) * math.cos(phi2) * math.sin(dlambda / 2) ** 2)
    return 2 * 6371000 * math.asin(math.sqrt(h))


@bp.route('/config', methods=['GET'])
def get_config():
    if not bearer_ok():
        return unauthorised()
    device, failure = _device()
    if failure:
        return failure

    defaults = {'ma': 1, 'oaf': 23, 'oat': 6}
    result = {}
    for key, column in CONFIG_FIELDS.items():
        value = device.get(column)
        result[key] = defaults.get(key, 0) if value is None else value
    return ok(result)


@bp.route('/config', methods=['POST'])
def update_config():
    if not bearer_ok():
        return unauthorised()

    data = request.get_json(silent=True)
    if not isinstance(data, dict):
        return error('invalid JSON body')

    device, failure = _device(allow_default=False)
    if failure:
        return failure

    updates = []
    for key, column in CONFIG_FIELDS.items():
        if key in data:
            value = _setting(column, data[key])
            if value is None:
                return _setting_error(key, column)
            updates.append((column, value))

    if updates:
        assignments = ', '.join('`%s` = %%s' % column for column, _ in updates)
        db.web.query(
            'UPDATE `device` SET %s WHERE `id` = %%s' % assignments,
            [value for _, value in updates] + [device['id']],
        )

    return ok()


def _command_keys(command):
    """The key names in a command string: 'int=3600,locate' -> {int, locate}."""
    keys = set()
    for part in command.split(','):
        part = part.strip()
        if not part:
            continue
        keys.add(part.split('=', 1)[0] if '=' in part else part)
    return keys


def _dedup(device_id, command):
    """Drop queued commands the new one supersedes."""
    conflicts = set()
    for key in _command_keys(command):
        conflicts.update(COMMAND_CONFLICTS.get(key, {key}))

    for row in db.web.all(
        'SELECT `id`, `command` FROM `command` WHERE `device_id` = %s',
        (device_id,),
    ):
        if _command_keys(row['command']) & conflicts:
            db.web.query('DELETE FROM `command` WHERE `id` = %s', (row['id'],))


@bp.route('/command', methods=['POST'])
def queue_command():
    """Queue a command for delivery on the device's next check-in.

    Nothing is pushed: the device is asleep almost all of the time, so a
    command waits in the queue until the device next reports and picks it up
    with the response it was already going to receive.
    """
    if not bearer_ok():
        return unauthorised()

    data = request.get_json(silent=True)
    if not isinstance(data, dict) or 'command' not in data:
        return error('command required')

    device, failure = _device(allow_default=False)
    if failure:
        return failure

    server_side = []
    for_device = []
    for part in data['command'].split(','):
        part = part.strip()
        if '=' in part and part.split('=', 1)[0] in SERVER_SIDE_SETTINGS:
            server_side.append(part)
        elif part:
            for_device.append(part)

    if server_side:
        assignments = []
        values = []
        for part in server_side:
            key, value = part.split('=', 1)
            assignments.append('`%s` = %%s' % SERVER_SIDE_SETTINGS[key])
            number = _setting(SERVER_SIDE_SETTINGS[key], value)
            if number is None:
                return _setting_error(key, SERVER_SIDE_SETTINGS[key])
            values.append(number)
        values.append(device['id'])
        db.web.query(
            'UPDATE `device` SET %s WHERE `id` = %%s' % ', '.join(assignments),
            values,
        )

    if for_device:
        command = ','.join(for_device)
        _dedup(device['id'], command)
        db.web.query(
            'INSERT INTO `command` (`device_id`, `timestamp`, `command`) '
            'VALUES (%s, NOW(6), %s)',
            (device['id'], command),
        )

    return ok()


@bp.route('/devices', methods=['GET'])
@login_required
def list_devices():
    """Every enrolled device with its latest record; see devices.summary()."""
    return ok({'devices': [devices.summary(device)
                           for device in devices.enrolled()]})


@bp.route('/status', methods=['GET'])
@login_required
def status():
    """Current settings and last-seen for one device."""
    device, failure = _device()
    if failure:
        return failure
    row = devices.latest_log(device)
    return ok({
        'imei': device['imei'],
        'name': device.get('name'),
        'settings': telemetry.device_config(device),
        'firmware': (row or {}).get('fw'),
        'last_seen': row['timestamp'].strftime('%Y-%m-%d %H:%M:%S')
        if row and row.get('timestamp') else None,
    })
