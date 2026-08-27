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


def position(row, database=None):
    """Serialise a log row into the shape the map understands.

    Shared by the polling endpoint, the journey replay and the live stream so
    the client has one format to parse whichever way a point reached it.
    """
    stamp = row.get('timestamp')
    return {
        'latitude': to_float(row.get('latitude')),
        'longitude': to_float(row.get('longitude')),
        'speed': to_float(row.get('speed')),
        'altitude': to_float(row.get('altitude')),
        'heading': to_float(row.get('heading')),
        'timestamp': stamp.strftime('%d.%m.%Y %H:%M:%S') if stamp else '',
        'battery_level': to_float(row.get('battery_level')),
        'ignition_state': row.get('ignition_state'),
        'operator': db.lookup_operator(row.get('mcc'), row.get('mnc'),
                                       database=database) or '',
        'rat': row.get('rat') or '',
        'cell_location': row.get('cell_location') or 0,
    }


def engine_running(device, log, database=None):
    """Whether the engine appears to be running, with hysteresis.

    Ignition state cannot tell "key on, engine off" from "running".  A charging
    alternator holds the bus above the threshold, so any reading above it in
    the recent window counts as running — smart and regenerative charging
    systems deliberately let the bus sag, and without the window the display
    would flicker between states on a perfectly healthy car.
    """
    if not log or log.get('ignition_state') != 1:
        return False
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
