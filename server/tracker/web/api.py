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

from .. import config, db, deadreckon, device_events, notify, telemetry
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
#
# backup_interval is the odd one out: the device does act on it, but it gets it
# from the backup= field every response carries (like track mode), so setting
# it is writing the column, and a queued command would only be undone by the
# next response.  Its name keeps clear of "int=", which the firmware finds
# anywhere in a command string.
SERVER_SIDE_SETTINGS = {
    'alarm': 'alarm',
    'garage': 'garage',
    'overnightalarm': 'overnight_alarm',
    'overnight_alarm_hour_from': 'overnight_alarm_hour_from',
    'overnight_alarm_hour_to': 'overnight_alarm_hour_to',
    'gnss_off_window': 'gnss_off_window',
    'gnss_off_hour_from': 'gnss_off_hour_from',
    'gnss_off_hour_to': 'gnss_off_hour_to',
    'leds': 'leds',
    'backup_interval': 'backup_int',
    # The runtime tunables, by column name; a value of "default" (or nothing)
    # clears one back to NULL, "the unit keeps the value it was built with".
    # obd_alerts takes its rules separated by '|', as a comma would end the
    # command: obd_alerts=coolant>=95|rpm>6500.
    **{column: column for column in telemetry.TUNABLE_COLUMNS},
}

CONFIG_FIELDS = {
    'int': 'int',
    'backup': 'backup_int',
    'ma': 'movement_alarm',
    'al': 'alarm',
    'ga': 'garage',
    'oa': 'overnight_alarm',
    'oaf': 'overnight_alarm_hour_from',
    'oat': 'overnight_alarm_hour_to',
    'gw': 'gnss_off_window',
    'gwf': 'gnss_off_hour_from',
    'gwt': 'gnss_off_hour_to',
    'le': 'leds',
    **{code: column for code, column, _, _ in telemetry.TUNABLES},
}

# Intervals the unit would raise to this anyway (the cell behind them is
# finite): 0 means off, anything from 1 to 9 is refused.
NONZERO_MINIMUM = {'int': 10, 'backup_int': 10}

# The values each setting's column can hold.
SETTING_RANGES = {
    'int': (0, 4294967295),
    'backup_int': (0, 4294967295),
    'movement_alarm': (0, 1),
    'alarm': (0, 1),
    'garage': (0, 1),
    'overnight_alarm': (0, 1),
    'overnight_alarm_hour_from': (0, 23),
    'overnight_alarm_hour_to': (0, 23),
    'gnss_off_window': (0, 1),
    'gnss_off_hour_from': (0, 23),
    'gnss_off_hour_to': (0, 23),
    'leds': (0, 1),
    **telemetry.TUNABLE_RANGES,
}

BATTERY_COLUMNS = ('battery_warning_mv', 'battery_poweroff_mv')


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
    if column in NONZERO_MINIMUM and 0 < value < NONZERO_MINIMUM[column]:
        return None
    return value if low <= value <= high else None


def _tunable(column, value):
    """(True, the value to store) for a runtime tunable, (False, None) if it is
    refused.  None, the string "default" and (for text) "" clear it to NULL;
    obd_alerts is a rule list, the others whole numbers in their range."""
    if value is None or (isinstance(value, str)
                         and value.strip() in ('', 'default')
                         and column == 'obd_alerts'):
        return True, None
    if column == 'obd_alerts':
        return (True, value) if telemetry.valid_obd_alerts(value) else (False, None)
    number = _setting(column, value)
    return (False, None) if number is None else (True, number)


def _tunable_error(key, column):
    if column == 'obd_alerts':
        return error('%s must be null, "" or up to %d characters of comma-'
                     'separated rules like coolant>=95,rpm>6500/200 (printable '
                     'ASCII, no ";" or "|")' % (key, telemetry.OBD_ALERTS_MAX))
    return _setting_error(key, column)


def _battery_conflict(device, updates):
    """An error response if the battery power-off level would not sit below
    the warning level once ``updates`` ({column: value}) are applied to
    ``device``, else None.  Only checked when both are set."""
    if not set(BATTERY_COLUMNS) & set(updates):
        return None
    merged = {c: updates[c] if c in updates else device.get(c)
              for c in BATTERY_COLUMNS}
    warning, poweroff = (merged[c] for c in BATTERY_COLUMNS)
    if warning is not None and poweroff is not None and poweroff >= warning:
        return error('battery_poweroff_mv (xbp) must be below '
                     'battery_warning_mv (xbw)')
    return None


def _setting_error(key, column):
    low, high = SETTING_RANGES[column]
    if column in NONZERO_MINIMUM:
        return error('%s must be 0 or a whole number from %d to %d'
                     % (key, NONZERO_MINIMUM[column], high))
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
        if bool(device.get('track_mode')) != bool(on):
            device_events.record(db.web, device['id'], 'setting',
                                 device_events.track_mode_text(bool(on)))
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
    # record whose device timestamp lands outside the window.  In time order,
    # as the journey's miles are summed: a record sent again after its first
    # datagram went unanswered, or flushed from the backlog after an outage,
    # arrives behind later ones and is filed at the time it was built.
    rows = db.web.all(
        'SELECT `latitude`, `longitude`, `speed`, `combined_speed`, `altitude`, '
        '`heading`, `timestamp`, `gsm_timestamp`, `ignition_state`, '
        '`battery_level`, `mcc`, `mnc`, `rat`, `cell_location`, `track_mode`, '
        '`rec_id`, `satellites`, `hdop`, `obd_speed`, `motion`, '
        + ', '.join('`%s`' % c for c in
                    devices.OBD_LIVE_COLUMNS + devices.IMU_LIVE_COLUMNS)
        + ' FROM `log` '
        'WHERE `device_id` = %s AND `id` >= %s '
        'AND `id` <= %s ORDER BY `timestamp`, `id`',
        (journey['device_id'], journey['start_log_id'], journey['end_log_id']),
    )

    # The paths through the gaps between fixes, from the motion the device
    # measured across them (mv=): dashed on the map, between the fixes.
    fills = deadreckon.journey_fill(rows)

    # One operator lookup per distinct cell, not per point.
    cache = {}
    points = []
    for i, row in enumerate(rows):
        key = (row.get('mcc'), row.get('mnc'))
        if key not in cache:
            cache[key] = db.lookup_operator(*key) or ''
        point = devices.position(row)
        point['operator'] = cache[key]
        points.append(point)
        for p in fills.get(i, ()):
            points.append(deadreckon.point(point, p, row.get('timestamp')))

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

    defaults = {'ma': 1, 'oaf': 23, 'oat': 6, 'gwt': 7, 'le': 1}
    result = {}
    for key, column in CONFIG_FIELDS.items():
        value = device.get(column)
        if column in telemetry.TUNABLE_COLUMNS:
            result[key] = value   # null: no override, the unit's built value
        else:
            result[key] = defaults.get(key, 0) if value is None else value
    # What the unit reported it was built with, and the value in use (the
    # override if set, else the built value, else null).
    result['tunables_built'] = telemetry.tunables_built(device)
    result['tunables'] = telemetry.tunables_effective(device)
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

    unknown = sorted(set(data) - set(CONFIG_FIELDS) - {'imei', 'device_id'})
    if unknown:
        return error('unknown setting: %s' % ', '.join(str(k) for k in unknown))

    updates = []
    for key, column in CONFIG_FIELDS.items():
        if key in data:
            if column in telemetry.TUNABLE_COLUMNS:
                good, value = _tunable(column, data[key])
                if not good:
                    return _tunable_error(key, column)
            else:
                value = _setting(column, data[key])
                if value is None:
                    return _setting_error(key, column)
            updates.append((column, value))

    failure = _battery_conflict(device, dict(updates))
    if failure:
        return failure

    if updates:
        assignments = ', '.join('`%s` = %%s' % column for column, _ in updates)
        db.web.query(
            'UPDATE `device` SET %s WHERE `id` = %%s' % assignments,
            [value for _, value in updates] + [device['id']],
        )
        telemetry.record_setting_changes(db.web, device, updates)

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
            telemetry.record_command_events(db.web, device_id, 'replaced',
                                            [row['command']])


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
            if part.startswith('int=') and _setting('int', part[4:]) is None:
                return _setting_error('int', 'int')
            if part.startswith('gnssbk='):
                if part[7:] not in ('0', '1'):
                    return error('gnssbk must be 0 or 1')
                # Not queued: the state is on the device row and every reply
                # carries it (gb=); the unit announces a change it sees there.
                db.web.query(
                    'UPDATE `device` SET `gnss_backup` = %s, '
                    '`gnss_backup_at` = NOW() WHERE `id` = %s',
                    (int(part[7:]), device['id']),
                )
                telemetry.record_setting_changes(
                    db.web, device, [('gnss_backup', int(part[7:]))])
                continue
            for_device.append(part)

    if server_side:
        assignments = []
        values = []
        for part in server_side:
            key, value = part.split('=', 1)
            column = SERVER_SIDE_SETTINGS[key]
            assignments.append('`%s` = %%s' % column)
            if column in telemetry.TUNABLE_COLUMNS:
                value = value.strip()
                if value == 'default' or (value == '' and column != 'obd_alerts'):
                    value = None
                elif column == 'obd_alerts':
                    value = value.replace('|', ',')
                good, number = _tunable(column, value)
                if not good:
                    return _tunable_error(key, column)
            else:
                number = _setting(column, value)
                if number is None:
                    return _setting_error(key, column)
            values.append(number)
        failure = _battery_conflict(device, {
            SERVER_SIDE_SETTINGS[part.split('=', 1)[0]]: value
            for part, value in zip(server_side, values)})
        if failure:
            return failure
        values.append(device['id'])
        db.web.query(
            'UPDATE `device` SET %s WHERE `id` = %%s' % ', '.join(assignments),
            values,
        )
        telemetry.record_setting_changes(
            db.web, device,
            [(SERVER_SIDE_SETTINGS[part.split('=', 1)[0]], value)
             for part, value in zip(server_side, values)])

    if for_device:
        command = ','.join(for_device)
        _dedup(device['id'], command)
        db.web.query(
            'INSERT INTO `command` (`device_id`, `timestamp`, `command`) '
            'VALUES (%s, NOW(6), %s)',
            (device['id'], command),
        )
        telemetry.record_command_events(db.web, device['id'], 'queued', [command])

    return ok()


def _cancel_queued(device_id, name):
    """Drop `name` from the device's queued commands; returns how many went.

    A row can hold several commands ('int=60,locate'); only the matching parts
    are removed and a row left empty is deleted.
    """
    cancelled = 0
    for row in db.web.all(
        'SELECT `id`, `command` FROM `command` WHERE `device_id` = %s',
        (device_id,),
    ):
        parts = [p.strip() for p in row['command'].split(',') if p.strip()]
        keep = [p for p in parts if p.split('=', 1)[0] != name]
        if len(keep) == len(parts):
            continue
        cancelled += len(parts) - len(keep)
        if keep:
            db.web.query('UPDATE `command` SET `command` = %s WHERE `id` = %s',
                         (','.join(keep), row['id']))
        else:
            db.web.query('DELETE FROM `command` WHERE `id` = %s', (row['id'],))
        telemetry.record_command_events(
            db.web, device_id, 'cancelled',
            [p for p in parts if p.split('=', 1)[0] == name])
    return cancelled


@bp.route('/command/cancel', methods=['POST'])
def cancel_command():
    """Remove a still-queued command (by name, any value) before the device
    collects it.  0 cancelled is not an error: it may already have gone."""
    if not bearer_ok():
        return unauthorised()

    data = request.get_json(silent=True)
    name = data.get('command') if isinstance(data, dict) else None
    if not isinstance(name, str) or not name.strip():
        return error('command required')

    device, failure = _device(allow_default=False)
    if failure:
        return failure

    return ok({'success': True,
                'cancelled': _cancel_queued(device['id'], name.strip())})


@bp.route('/devices', methods=['GET'])
@login_required
def list_devices():
    """Every enrolled device with its latest record; see devices.summary()."""
    return ok({'devices': [devices.summary(device)
                           for device in devices.enrolled()]})


@bp.route('/devices/events', methods=['GET'])
@login_required
def device_event_list():
    """A device's event log, newest first; see device_events.page()."""
    try:
        limit, before = device_events.parse_query(request.args.get('limit'),
                                                  request.args.get('before'))
    except ValueError as e:
        return error(str(e))
    device, failure = _device()
    if failure:
        return failure
    return ok({'events': device_events.page(db.web, device['id'], limit, before)})


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
        'firmware': device.get('fw'),
        'board': device.get('board'),
        'last_seen': row['timestamp'].strftime('%Y-%m-%d %H:%M:%S')
        if row and row.get('timestamp') else None,
    })
