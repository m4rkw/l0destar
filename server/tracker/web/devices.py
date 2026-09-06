"""Device resolution for web and API requests."""

from flask import request

from .. import config, db


def resolve(imei=None, device_id=None, allow_default=True):
    """Find the device a request is about.

    Explicit ``imei`` wins, then ``device_id``, then the ``X-Imei`` header,
    then the configured default.  Returns None if nothing matches.
    """
    device = db.lookup_device(imei=imei, device_id=device_id)
    if device:
        return device

    header_imei = request.headers.get('X-Imei')
    if header_imei:
        device = db.lookup_device(imei=header_imei)
        if device:
            return device

    if allow_default and config.DEFAULT_DEVICE_IMEI:
        return db.lookup_device(imei=config.DEFAULT_DEVICE_IMEI)

    return None


def from_request(allow_default=True):
    """Resolve from query string or JSON body, whichever the request used."""
    body = request.get_json(silent=True) if request.is_json else None
    body = body if isinstance(body, dict) else {}
    return resolve(
        imei=request.args.get('imei') or body.get('imei'),
        device_id=request.args.get('device_id') or body.get('device_id'),
        allow_default=allow_default,
    )


def latest_log(device, database=None):
    """The device's most recent record.

    Ordered by row id, not by the modem's own timestamp: id is arrival order,
    which is what "latest" means here.  Ordering by the device clock would let
    one record with a wrong or unset clock — which happens on a cold boot
    before the network supplies the time — pin the display to a bogus row.
    """
    database = database or db.web
    return database.one(
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
