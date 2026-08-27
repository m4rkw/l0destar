"""JSON API.

Two audiences with different authentication:

* Browser endpoints (``/api/1.0/carpos``, ``journeys``, ``journey/<id>/points``)
  are session-authenticated and only ever read.
* Automation endpoints (``track``, ``home``, ``config``, ``command``) take a
  bearer token from the ``api_token`` table.  Tokens are opaque, minted by
  ``tools/gentoken.py``, and carry no scopes — anything holding one can queue a
  command, so treat one as equivalent to console access.
"""

import math

from flask import Blueprint, redirect, request

from .. import config, db, notify, telemetry
from . import devices, error, ok, unauthorised
from .auth import login_required

bp = Blueprint('api', __name__, url_prefix='/api/1.0')

# Commands that supersede one another.  Queuing `locate` when a `locate` is
# already pending should not make the device send two positions.
COMMAND_CONFLICTS = {
    'locate':    {'locate', 'locatenow'},
    'locatenow': {'locate', 'locatenow'},
    'tomtom':    {'tomtom', 'tomtomnow'},
    'tomtomnow': {'tomtom', 'tomtomnow'},
    'poweroff':  {'poweroff', 'alwayson'},
    'alwayson':  {'alwayson', 'poweroff'},
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
    'ao': 'always_on',
    'ma': 'movement_alarm',
    'al': 'alarm',
    'ga': 'garage',
    'oa': 'overnight_alarm',
    'oaf': 'overnight_alarm_hour_from',
    'oat': 'overnight_alarm_hour_to',
}


def bearer_ok():
    header = request.headers.get('Authorization', '')
    if not header.startswith('Bearer '):
        return False
    token = header[7:].strip()
    if not token:
        return False
    return db.web.one('SELECT `id` FROM `api_token` WHERE `token` = %s',
                      (token,)) is not None


# -- browser endpoints -------------------------------------------------------

@bp.route('/carpos', methods=['GET'])
@login_required
def carpos():
    device = devices.from_request()
    if not device:
        return error('device not found')
    row = devices.latest_log(device)
    if not row:
        return error('no records for device')
    return ok({'position': devices.position(row)})


@bp.route('/journeys', methods=['GET'])
@login_required
def journeys():
    device = devices.from_request()
    if not device:
        return error('device not found')

    per_page = min(int(request.args.get('per_page', 50)), 200)
    page = max(int(request.args.get('page', 0)), 0)

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

    # Bound by log id rather than by time: the ids were recorded when the
    # journey opened and closed, so this cannot drift on clock skew or on a
    # record whose device timestamp lands outside the window.
    rows = db.web.all(
        'SELECT `latitude`, `longitude`, `speed`, `altitude`, `heading`, '
        '`timestamp`, `ignition_state`, `battery_level`, `mcc`, `mnc`, `rat`, '
        '`cell_location` FROM `log` WHERE `device_id` = %s AND `id` >= %s '
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

    device = devices.from_request()
    if not device:
        return error('device not found')
    row = devices.latest_log(device)
    if not row:
        return error('no records for device')

    coords = '%s,%s' % (row['latitude'], row['longitude'])
    if request.args.get('google'):
        url = 'https://maps.google.com/maps/place/%s/' % coords
    else:
        url = 'maps:ll=%s&q=%s' % (coords, device.get('name') or 'vehicle')

    if request.args.get('return'):
        return ok({'url': url})
    return redirect(url)


@bp.route('/home', methods=['POST'])
def home_check():
    """Report whether the vehicle's last fix is near the reference point.

    A stalled tracker parked at home is invisible from the inside: the last
    record still looks like a car sitting at home, which is exactly what a
    healthy tracker reports too.  An external cron calling this notices the
    other case — the vehicle is not where it should be and nothing has said so.
    """
    if not bearer_ok():
        return unauthorised()
    if (not config.HOME_CHECK_IMEI
            or config.HOME_CHECK_LAT is None
            or config.HOME_CHECK_LON is None):
        return error('home_check not configured')

    device = db.lookup_device(imei=config.HOME_CHECK_IMEI)
    if not device:
        return error('home_check device not found')
    row = devices.latest_log(device)
    if not row:
        return error('no records for device')

    lat1 = math.radians(float(config.HOME_CHECK_LAT))
    lat2 = math.radians(devices.to_float(row['latitude']))
    dlat = lat2 - lat1
    dlon = math.radians(devices.to_float(row['longitude']) - float(config.HOME_CHECK_LON))
    h = (math.sin(dlat / 2) ** 2
         + math.cos(lat1) * math.cos(lat2) * math.sin(dlon / 2) ** 2)
    distance_m = 2 * 6371000 * math.asin(math.sqrt(h))

    at_home = distance_m <= config.HOME_CHECK_RADIUS_M
    garage = bool(device.get('garage'))

    if not at_home and not garage:
        notify.send(
            'Tracker may be stalled - vehicle is %dm from home' % int(distance_m),
            title='Tracker home check',
        )

    return ok({'at_home': at_home, 'distance_m': round(distance_m, 1),
               'garage': garage})


@bp.route('/config', methods=['GET'])
def get_config():
    if not bearer_ok():
        return unauthorised()
    device = devices.from_request()
    if not device:
        return error('device not found')

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

    device = devices.from_request()
    if not device:
        return error('device not found')

    updates = []
    for key, column in CONFIG_FIELDS.items():
        if key in data:
            try:
                updates.append((column, int(data[key])))
            except (TypeError, ValueError):
                return error('%s must be an integer' % key)

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

    device = devices.from_request(allow_default=False)
    if not device:
        return error('device not found')

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
            try:
                values.append(int(value))
            except ValueError:
                return error('%s must be an integer' % key)
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
    rows = db.web.all(
        'SELECT `id`, `imei`, `name`, `registration` FROM `device` ORDER BY `name`')
    return ok({'devices': [dict(r) for r in rows]})


@bp.route('/status', methods=['GET'])
@login_required
def status():
    """Current settings and last-seen for one device."""
    device = devices.from_request()
    if not device:
        return error('device not found')
    row = devices.latest_log(device)
    return ok({
        'imei': device['imei'],
        'name': device.get('name'),
        'settings': telemetry.device_config(device),
        'firmware': (row or {}).get('fw'),
        'last_seen': row['timestamp'].strftime('%Y-%m-%d %H:%M:%S')
        if row and row.get('timestamp') else None,
    })
