"""Device resolution for web and API requests.

One server can track several vehicles, so which one a request is about has
to be settled wherever a guess would do harm.  A read may fall back to a
default: a map opened without naming a vehicle still lands somewhere useful.
A write never does — a command queued, a setting changed or track mode
switched on for the wrong vehicle is worse than an error.
"""

import datetime

from flask import request

from .. import config, db


def _named(value):
    return value is not None and str(value).strip() != ''


def requested():
    """The identifiers a request carries, as ``(imei, device_id, header)``:
    the first two from the query string or a JSON body, whichever the request
    used, the last from ``X-Imei``."""
    body = request.get_json(silent=True) if request.is_json else None
    body = body if isinstance(body, dict) else {}
    return (request.args.get('imei') or body.get('imei'),
            request.args.get('device_id') or body.get('device_id'),
            request.headers.get('X-Imei'))


def named():
    """Whether the request names a device at all."""
    return any(_named(value) for value in requested())


def enrolled(database=None):
    """Every enrolled device, in the order the device list shows them."""
    database = database or db.web
    return database.all('SELECT * FROM `device` ORDER BY `name`, `id`')


def count(database=None):
    database = database or db.web
    return database.one('SELECT COUNT(*) AS `n` FROM `device`')['n']


def default_device(database=None):
    """The device a request that names none is about, or None.

    ``default_device`` from the config when it is set and enrolled, otherwise
    the only enrolled device.  With several enrolled and no default there is
    no right answer, so none is given.
    """
    database = database or db.web
    if config.DEFAULT_DEVICE_IMEI:
        device = db.lookup_device(database, imei=config.DEFAULT_DEVICE_IMEI)
        if device:
            return device
    rows = database.all('SELECT * FROM `device` ORDER BY `id` LIMIT 2')
    return rows[0] if len(rows) == 1 else None


def resolve(imei=None, device_id=None, header_imei=None, allow_default=True):
    """Find the device a request is about.

    An explicit ``imei`` wins, then ``device_id``, then the ``X-Imei``
    header.  A request that names a device gets that device or nothing:
    answering with another vehicle's data because an IMEI was mistyped would
    be quietly wrong.  Only a request that names none falls back, and only
    when ``allow_default`` is set — see :func:`default_device`.
    """
    if _named(imei):
        return db.lookup_device(imei=str(imei).strip())
    if _named(device_id):
        return db.lookup_device(device_id=device_id)
    if _named(header_imei):
        return db.lookup_device(imei=header_imei.strip())
    if allow_default:
        return default_device()
    return None


def from_request(allow_default=True):
    """Resolve the device the current request names.

    Pass ``allow_default=False`` for anything that changes state.
    """
    imei, device_id, header_imei = requested()
    return resolve(imei=imei, device_id=device_id, header_imei=header_imei,
                   allow_default=allow_default)


# How far ahead of the present a record's device time may be before the
# clock that stamped it is taken to be wrong.  The firmware writes UTC from
# the network's time (docs/PROTOCOL.md), so an hour is drift, not time zones.
DEVICE_CLOCK_SLACK = datetime.timedelta(hours=1)

_EPOCH = datetime.datetime(1970, 1, 1)


def _clock_limit():
    """The latest device time a record can plausibly carry right now."""
    return (datetime.datetime.now(datetime.timezone.utc).replace(tzinfo=None)
            + DEVICE_CLOCK_SLACK)


def gsm_epoch(row):
    """When the device built a record, as seconds since the epoch.

    The page keeps the newest record on screen by this rather than by
    arrival: a backlog sent after an outage is stored behind the live record,
    and by arrival the older record would replace the newer one.  None when
    the row has no usable device time — including one dated in the future,
    which the page would otherwise keep on screen for good.
    """
    stamp = row.get('gsm_timestamp')
    if not isinstance(stamp, datetime.datetime) or stamp > _clock_limit():
        return None
    return (stamp - _EPOCH).total_seconds()


def latest_log(device, database=None):
    """The device's most recent record: the newest by when the device built
    it, not by arrival.

    The two differ after an outage.  The backlog is flushed behind the live
    record, so it is stored last, and by arrival the map would show an old
    position until the next live record came in.

    A device clock can be wrong — unset before the network has supplied the
    time — but a wrong clock is nearly always in the past, where it sorts
    behind every genuine record and does no harm.  One in the future would
    pin the display, so rows more than DEVICE_CLOCK_SLACK ahead of the
    present are left out here, and a device with nothing else falls back
    to the last arrival.
    """
    database = database or db.web
    return database.one(
        'SELECT * FROM `log` WHERE `device_id` = %s AND `gsm_timestamp` <= %s '
        'ORDER BY `gsm_timestamp` DESC, `id` DESC LIMIT 1',
        (device['id'], _clock_limit()),
    ) or database.one(
        'SELECT * FROM `log` WHERE `device_id` = %s ORDER BY `id` DESC LIMIT 1',
        (device['id'],),
    )


def to_float(value, default=0.0):
    try:
        return float(value)
    except (TypeError, ValueError):
        return default


def combined_speed(row):
    """The speed the interface shows, as a float in mph.

    Rows written since the column existed carry it directly; older rows fall
    back to the GNSS speed so history still replays.
    """
    value = row.get('combined_speed')
    if value is None:
        value = row.get('speed')
    return to_float(value)


# The ECU readings the page shows alongside the position.  Every column is
# nullable and only K-wire builds fill them, so the page checks for null
# rather than for zero.  DECIMAL columns come back as Decimal, which json
# refuses, hence to_float.
OBD_LIVE_COLUMNS = (
    'obd_rpm', 'obd_coolant', 'obd_intake', 'obd_load', 'obd_throttle',
    'obd_maf', 'obd_timing', 'obd_stft', 'obd_ltft', 'obd_fuel_status',
    'obd_mil', 'obd_dtc_count',
)

# IMU readings.  Accel is stored in milli-g; the gyro as raw LSB at the
# +/-250 dps full scale (8.75 mdps per LSB) and is converted to deg/s here so
# the page never needs to know the sensor.
GYRO_DPS_PER_LSB = 0.00875
IMU_LIVE_COLUMNS = ('accel_x', 'accel_y', 'accel_z',
                    'gyro_x', 'gyro_y', 'gyro_z', 'imu_temp')


def _nullable_float(value):
    return None if value is None else to_float(value)


def imu_burst(raw):
    """A track-mode record's IMU burst as a list of [ax, ay, az, gx, gy, gz]:
    accel in milli-g, gyro in deg/s, oldest first, 26 Hz.  None when the
    row has none."""
    if not raw:
        return None
    out = []
    for group in raw.split(':'):
        parts = group.split('/')
        if len(parts) != 6:
            continue
        try:
            v = [int(x) for x in parts]
        except ValueError:
            continue
        out.append([v[0], v[1], v[2],
                    round(v[3] * GYRO_DPS_PER_LSB, 2),
                    round(v[4] * GYRO_DPS_PER_LSB, 2),
                    round(v[5] * GYRO_DPS_PER_LSB, 2)])
    return out or None


def accel_baseline(device, database=None):
    """Gravity as the resting device sees it: the mean accel vector over the
    last twenty stationary records.  The page subtracts it from live samples
    to get the dynamic (driving) acceleration and measures tilt against it.
    None until the device has reported a stationary sample."""
    database = database or db.web
    row = database.one(
        'SELECT AVG(accel_x) AS x, AVG(accel_y) AS y, AVG(accel_z) AS z FROM ('
        '  SELECT accel_x, accel_y, accel_z FROM `log`'
        '  WHERE device_id = %s AND accel_x IS NOT NULL'
        '  AND COALESCE(combined_speed, speed, 0) < 1'
        '  ORDER BY id DESC LIMIT 20) t',
        (device['id'],),
    )
    if not row or row.get('x') is None:
        return None
    return {'x': to_float(row['x']), 'y': to_float(row['y']), 'z': to_float(row['z'])}


def position(row, database=None, track_mode=None):
    """Serialise a log row into the shape the map understands.

    Shared by the polling endpoint, the journey replay and the live stream so
    the client has one format to parse whichever way a point reached it.
    ``track_mode`` is the device's switch, when the caller knows it; the
    live paths pass it so the page can change view on any message.
    """
    stamp = row.get('timestamp')
    out = {
        'latitude': to_float(row.get('latitude')),
        'longitude': to_float(row.get('longitude')),
        'speed': to_float(row.get('speed')),
        # ECU road speed when the device reported one, else GNSS.
        'combined_speed': combined_speed(row),
        'altitude': to_float(row.get('altitude')),
        'heading': to_float(row.get('heading')),
        'timestamp': stamp.strftime('%d.%m.%Y %H:%M:%S') if stamp else '',
        # When the device built it, for the page to keep the newest on screen.
        'gsm_ts': gsm_epoch(row),
        'battery_level': to_float(row.get('battery_level')),
        'ignition_state': row.get('ignition_state'),
        'operator': db.lookup_operator(row.get('mcc'), row.get('mnc'),
                                       database=database) or '',
        'rat': row.get('rat') or '',
        'cell_location': row.get('cell_location') or 0,
        # Whether this record was built in track mode, and its IMU burst.
        'track': 1 if row.get('track_mode') else 0,
        'imu': imu_burst(row.get('imu_burst')),
    }
    # ECU readings on K-wire builds, null otherwise.  The map uses the RPM
    # ahead of voltage to call the engine running and shows the rest.
    for column in OBD_LIVE_COLUMNS:
        out[column] = _nullable_float(row.get(column))
    for column in IMU_LIVE_COLUMNS:
        value = row.get(column)
        if value is not None and column.startswith('gyro_'):
            value = round(to_float(value) * GYRO_DPS_PER_LSB, 2)
        out[column] = _nullable_float(value)
    if track_mode is not None:
        out['track_mode'] = 1 if track_mode else 0
    return out


def engine_running(device, log, database=None):
    """Whether the engine appears to be running.

    Ignition state cannot tell "key on, engine off" from "running".  When the
    device reports engine RPM from the ECU (a K-wire build) that is a direct
    measurement and settles it outright.  Otherwise fall back to voltage with
    hysteresis: a charging alternator holds the bus above the threshold, so
    any reading above it in the recent window counts as running — smart and
    regenerative charging systems deliberately let the bus sag, and a noisy
    rail can put a single reading a volt low, so without the window the
    display would flicker between states on a perfectly healthy car.
    """
    if not log or log.get('ignition_state') != 1:
        return False
    rpm = log.get('obd_rpm')
    if rpm is not None:
        try:
            return int(rpm) > 0
        except (TypeError, ValueError):
            pass
    database = database or db.web
    recent = database.all(
        'SELECT `battery_level` FROM `log` WHERE `device_id` = %s '
        'ORDER BY `id` DESC LIMIT %s',
        (device['id'], config.ENGINE_STOPPED_COUNT),
    )
    for row in recent:
        try:
            if float(row['battery_level']) >= config.ENGINE_RUNNING_VOLTAGE:
                return True
        except (TypeError, ValueError):
            continue
    return False


def summary(device, database=None, log=None):
    """A device and its latest record, as the device list shows them.

    ``log`` is that record when the caller already has it — an empty dict
    for a device that has never reported — and is looked up otherwise.
    """
    database = database or db.web
    if log is None:
        log = latest_log(device, database) or {}
    stamp = log.get('timestamp')
    return {
        'id': device['id'],
        'imei': device['imei'],
        'name': device.get('name') or '',
        'registration': device.get('registration') or '',
        'last_seen': stamp.strftime('%Y-%m-%d %H:%M:%S') if stamp else None,
        'latitude': _nullable_float(log.get('latitude')),
        'longitude': _nullable_float(log.get('longitude')),
        'speed': combined_speed(log) if log else None,
        'battery_level': _nullable_float(log.get('battery_level')),
        'ignition_state': log.get('ignition_state'),
        'fw': log.get('fw'),
        'rat': log.get('rat') or '',
        'operator': db.lookup_operator(log.get('mcc'), log.get('mnc'),
                                       database=database) or '',
        'track_mode': 1 if device.get('track_mode') else 0,
    }
