"""Telemetry parsing, storage and response building.

Everything below the transports lives here: the three listeners (UDP, TLS,
DTLS) differ only in how bytes arrive and how they are authenticated.  Once a
datagram or frame has been decrypted and attributed to a device, they all hand
the same list of plaintext lines to :func:`process_lines`.

Wire format
-----------
One record per line, comma-separated::

    ts,lat,lon,spd,alt,hdg,hdop,sat,bat,ign,waketime,pon[,extras...]

``ts`` is the modem's own clock as ``dd/mm/yy,HH:MM:SS+NN`` — note that it
contains a comma, so the first two fields are rejoined before parsing.

Everything after the twelve fixed fields is an "extras" group: comma-separated
groups of ``key=value`` pairs joined by semicolons.  Extras are optional and
sparse by design.  The device pays for every byte in radio time, so fields
that rarely change (serving cell, firmware version) are sent only when they do
change or on the first record after a wake, and the server carries the last
known value forward so every row is still self-describing.

A line beginning ``A,`` is an alert rather than a position record:
``A,<priority>,<message>``.  ``D,<code>,<code>...`` is the vehicle's complete
set of stored fault codes (``D,`` alone means none), and
``L,<uptime_ms>,<E|W>,<text>`` is a warning or error the firmware captured
between sends.
"""

import datetime
import math
import re

from . import config, db, logs, notify

# Fixed fields, in wire order.
CSV_FIELDS = [
    'gsm_timestamp', 'latitude', 'longitude', 'speed', 'altitude',
    'heading', 'hdop', 'satellites',
    'battery_level', 'ignition_state', 'waketime', 'powered_on',
]

# Fields a record cannot be stored without.
REQUIRED_KEYS = [
    'latitude', 'longitude', 'speed', 'altitude', 'heading',
    'hdop', 'satellites', 'battery_level', 'gsm_timestamp', 'ignition_state',
]

# Extras: wire key -> column name.
EXTRA_KEYS = {
    'ri':  'request_int',        # device is asking for its config back
    'int': 'int',                # reporting interval the device believes it has
    'ma':  'movement_alarm',
    'fw':  'fw',                 # running firmware version, e.g. 0.4.12
    'mcc': 'mcc',
    'mnc': 'mnc',
    'lac': 'lac',
    'cid': 'cid',
    'cl':  'cell_location',      # 1 = position is cell-derived, not GNSS
    'rat': 'rat',                # CATM1 | NBIOT
    'ax':  'accel_x',            # raw LSB
    'ay':  'accel_y',
    'az':  'accel_z',
    'gx':  'gyro_x',             # raw LSB at +/-250 dps
    'gy':  'gyro_y',
    'gz':  'gyro_z',
    'up':  'uptime',             # seconds since boot
    'mt':  'mcu_temp',           # degrees C
    'it':  'imu_temp',           # degrees C
    'vs':  'vsys',               # SiP supply rail, volts (not vehicle battery)
    'dr':  'dead_reckoning',
}

# Config the device may report back, mirrored onto the `device` row so the
# server's view of a unit's settings tracks what the unit actually applied.
DEVICE_SYNC_KEYS = ['int', 'movement_alarm']

# Fields that describe a slowly-changing condition rather than this instant.
# When absent, the previous row's value is carried forward.
STICKY_KEYS = ('mcc', 'mnc', 'lac', 'cid', 'rat', 'fw')

# Fields that mean "right now" and must never be carried forward.
PER_PACKET_KEYS = (
    'accel_x', 'accel_y', 'accel_z', 'gyro_x', 'gyro_y', 'gyro_z',
    'mcu_temp', 'imu_temp', 'uptime', 'waketime', 'dead_reckoning', 'vsys',
    'cell_location',
)

KM_PER_HOUR_TO_MPH = 0.6213712

# OBD-II values read over the K wire.  The device sends integers only, with
# these scale factors applied, so the packet never carries a decimal point.
# Each entry is (column, divisor, unit factor): the stored value is
# int(v) / divisor * factor.  Keys are the short forms the firmware emits in
# the trailing k=v;k=v extras.
#
# The ECU reports speed in km/h (SAE J1979 PID 0x0D, on any market's
# vehicle).  It is converted here, with the same constant and in the same
# place as the GNSS speed, so both speed columns are mph and directly
# comparable.
OBD_FIELDS = {
    'orpm':  ('obd_rpm',         1,   1),
    'ormin': ('obd_rpm_min',     1,   1),
    'ormax': ('obd_rpm_max',     1,   1),
    'oravg': ('obd_rpm_avg',     1,   1),
    'ospd':  ('obd_speed',       1,   KM_PER_HOUR_TO_MPH),
    'ocl':   ('obd_coolant',     1,   1),
    'oit':   ('obd_intake',      1,   1),
    'old':   ('obd_load',       10,   1),
    'oth':   ('obd_throttle',   10,   1),
    'omaf':  ('obd_maf',       100,   1),
    'otim':  ('obd_timing',     10,   1),
    'ostft': ('obd_stft',       10,   1),
    'oltft': ('obd_ltft',       10,   1),
    'ofs':   ('obd_fuel_status', 1,   1),
    'omil':  ('obd_mil',         1,   1),
    'odtc':  ('obd_dtc_count',   1,   1),
}

# A diagnostic trouble code as the device reports it: P/C/B/U, then a digit
# 0-3, then three hex digits.  Anything else is a firmware or bus fault and
# is dropped rather than stored.
DTC_RE = re.compile(r'^[PCBU][0-3][0-9A-F]{3}$')

LOG_COLUMNS = [
    'device_id', 'ip', 'timestamp', 'gsm_timestamp', 'gsm_timestamp_offset',
    'latitude', 'longitude', 'altitude', 'speed', 'heading', 'hdop',
    'satellites', 'ignition_state', 'battery_level', 'powered_on',
    'dbg', 'rst', 'fw',
    'mcc', 'mnc', 'lac', 'cid', 'cell_location', 'rat',
    'accel_x', 'accel_y', 'accel_z', 'gyro_x', 'gyro_y', 'gyro_z',
    'waketime', 'uptime', 'mcu_temp', 'imu_temp', 'dead_reckoning', 'vsys',
] + [column for column, _, _ in OBD_FIELDS.values()] + ['combined_speed']

_TIMESTAMP_RE = re.compile(
    r'^(\d+)/(\d+)/(\d+),(\d{2}):(\d{2}):(\d{2})(?:\.(\d{1,6}))?([+-])(\d+)$'
)


# -- parsing -----------------------------------------------------------------

def parse_csv_line(line):
    """Parse one telemetry line into a dict of column names to raw strings."""
    parts = line.split(',')
    if len(parts) > 1:
        # Rejoin the timestamp, which contains the first comma.
        parts = [parts[0] + ',' + parts[1]] + parts[2:]

    if len(parts) < len(CSV_FIELDS):
        raise ValueError('record has %d fields, expected at least %d'
                         % (len(parts), len(CSV_FIELDS)))

    data = dict(zip(CSV_FIELDS, parts))

    for extra in parts[len(CSV_FIELDS):]:
        # dbg= is a compound field whose own body uses semicolons, so it is
        # taken whole rather than split like the others.  The firmware may
        # append ;rst=<cause> to it on the record after a reset.
        if extra.startswith('dbg='):
            body = extra[4:]
            if ';rst=' in body:
                data['dbg'], data['rst'] = body.rsplit(';rst=', 1)
            else:
                data['dbg'] = body
            continue
        if extra.startswith('rst='):
            data['rst'] = extra[4:]
            continue

        for pair in extra.split(';'):
            if '=' not in pair:
                continue
            key, value = pair.split('=', 1)
            column = EXTRA_KEYS.get(key)
            if column:
                data[column] = value
            elif key in OBD_FIELDS:
                data[OBD_FIELDS[key][0]] = value

    return data


def _parse_gsm_timestamp(raw):
    """Return (datetime, utc_offset_hours) from the modem's clock string."""
    match = _TIMESTAMP_RE.match(raw)
    if not match:
        raise ValueError('invalid gsm_timestamp format')

    fraction = match.group(7)
    microsecond = int(fraction.ljust(6, '0')) if fraction else 0
    stamp = datetime.datetime(
        int(match.group(3)) + 2000, int(match.group(2)), int(match.group(1)),
        int(match.group(4)), int(match.group(5)), int(match.group(6)),
        microsecond,
    )
    offset = match.group(9) if match.group(8) == '+' else '-' + match.group(9)
    return stamp, offset


# -- storage -----------------------------------------------------------------

def _build_entry(data, device, ip, previous):
    entry = {'ip': ip, 'device_id': device['id']}

    for key in REQUIRED_KEYS:
        if key not in data:
            raise ValueError('missing field: %s' % key)

    # The device reports km/h; everything downstream is mph.
    entry['speed'] = '%.2f' % (float(data['speed']) * KM_PER_HOUR_TO_MPH)
    for key in ('latitude', 'longitude', 'altitude', 'heading', 'hdop',
                'satellites', 'battery_level'):
        entry[key] = str(data[key])

    stamp, offset = _parse_gsm_timestamp(data['gsm_timestamp'])
    entry['gsm_timestamp'] = stamp.strftime('%Y-%m-%d %H:%M:%S.%f')
    entry['gsm_timestamp_offset'] = offset
    entry['ignition_state'] = 1 if int(data['ignition_state']) != 0 else 0
    entry['timestamp'] = datetime.datetime.now().strftime('%Y-%m-%d %H:%M:%S.%f')

    for key in ('dbg', 'rst'):
        if key in data:
            entry[key] = data[key]

    for key in STICKY_KEYS:
        if key in data:
            entry[key] = data[key]
        elif previous is not None and previous.get(key) is not None:
            entry[key] = previous[key]

    for key in PER_PACKET_KEYS:
        if key in data:
            entry[key] = data[key]

    # OBD-II data read over the K wire.  Only present when the vehicle has a
    # K interface and the firmware is configured to poll it, and only for the
    # PIDs that ECU actually supports — so every field is independently
    # optional and nothing is carried forward from the previous row.
    for column, scale, factor in OBD_FIELDS.values():
        if column not in data:
            continue
        try:
            value = int(data[column])
        except (TypeError, ValueError):
            continue
        if scale == 1 and factor == 1:
            entry[column] = value
        else:
            entry[column] = round(value / scale * factor, 2)

    # The speed the interface shows.  The ECU's road speed, when the device
    # reports one, is the vehicle's own figure: it does not wander with a poor
    # fix and reads a clean zero when stationary.  Without it, the GNSS speed.
    # Both are mph by this point.  Stored rather than derived on read so
    # queries and exports see one column.
    if entry.get('obd_speed') is not None:
        entry['combined_speed'] = entry['obd_speed']
    else:
        entry['combined_speed'] = entry['speed']

    return entry


def _ignition_alerts(device, entry, now):
    """Alerts raised by an ignition-off to ignition-on transition."""
    # `garage` marks a vehicle that is somewhere its ignition coming on is
    # expected — a workshop, a driveway with the car being moved daily — and
    # downgrades what would otherwise be an acknowledge-me alert.
    priority = 0 if device.get('garage') else 2
    battery = entry['battery_level']

    if device.get('alarm'):
        notify.send('%s ignition on, battery %sV' % (device['name'], battery),
                    priority=priority)

    if device.get('overnight_alarm'):
        hour_from = device.get('overnight_alarm_hour_from')
        hour_to = device.get('overnight_alarm_hour_to')
        hour_from = 23 if hour_from is None else int(hour_from)
        hour_to = 6 if hour_to is None else int(hour_to)
        hour = now.hour
        if hour_from <= hour_to:
            in_window = hour_from <= hour < hour_to
        else:
            # Window wraps midnight.
            in_window = hour >= hour_from or hour < hour_to
        if in_window:
            notify.send(
                '%s overnight ignition on, battery %sV' % (device['name'], battery),
                priority=priority,
            )


def _update_journey(database, device, entry, log_id, powered_on):
    """Maintain the `journey` row for this device.

    A journey opens on the first ignition-on record and closes when the
    ignition goes off.  A restart within ``journey_resume_seconds`` reopens the
    journey that just closed rather than starting a new one, so a fuel stop or
    a stop-start engine cycle does not fragment the history.
    """
    if not config.JOURNEY_ENABLED:
        return

    open_journey = database.one(
        'SELECT * FROM `journey` WHERE `device_id` = %s ORDER BY `id` DESC LIMIT 1',
        (device['id'],),
    )
    ignition_on = entry['ignition_state'] == 1

    if ignition_on:
        if open_journey is not None and open_journey['end_time'] is None:
            return  # already running
        if open_journey is not None and open_journey['end_time'] is not None:
            gap = datetime.datetime.now() - open_journey['end_time']
            if gap.total_seconds() <= config.JOURNEY_RESUME_SECONDS:
                database.query(
                    'UPDATE `journey` SET `end_time` = NULL, `to_latitude` = NULL, '
                    '`to_longitude` = NULL WHERE `id` = %s',
                    (open_journey['id'],),
                )
                return
        database.query(
            'INSERT INTO `journey` (`device_id`, `start_time`, `from_latitude`, '
            '`from_longitude`, `start_log_id`) VALUES (%s, %s, %s, %s, %s)',
            (device['id'], entry['timestamp'], entry['latitude'],
             entry['longitude'], log_id),
        )
        return

    # Ignition off: close the open journey, if there is one.
    if open_journey is None or open_journey['end_time'] is not None:
        return
    if powered_on:
        return

    miles = _journey_miles(database, device['id'], open_journey['start_log_id'], log_id)
    database.query(
        'UPDATE `journey` SET `end_time` = %s, `to_latitude` = %s, '
        '`to_longitude` = %s, `end_log_id` = %s, `miles` = %s WHERE `id` = %s',
        (entry['timestamp'], entry['latitude'], entry['longitude'], log_id,
         miles, open_journey['id']),
    )


def _journey_miles(database, device_id, start_log_id, end_log_id):
    """Great-circle distance along the journey's recorded fixes.

    Summing point-to-point is closer to the truth than a start-to-end straight
    line, and it costs one query per journey rather than any per-record work.
    It still under-reads on a sparse trace: at a long reporting interval a
    curve becomes a chord.
    """
    if start_log_id is None:
        return None
    rows = database.all(
        'SELECT `latitude`, `longitude` FROM `log` WHERE `device_id` = %s '
        'AND `id` >= %s AND `id` <= %s ORDER BY `id`',
        (device_id, start_log_id, end_log_id),
    )
    total_km = 0.0
    previous = None
    for row in rows:
        try:
            point = (float(row['latitude']), float(row['longitude']))
        except (TypeError, ValueError):
            continue
        if point == (0.0, 0.0):
            continue
        if previous is not None:
            total_km += _haversine_km(previous, point)
        previous = point
    return round(total_km * 0.621371, 2)


def _haversine_km(a, b):
    lat1, lat2 = math.radians(a[0]), math.radians(b[0])
    dlat = lat2 - lat1
    dlon = math.radians(b[1] - a[1])
    h = (math.sin(dlat / 2) ** 2
         + math.cos(lat1) * math.cos(lat2) * math.sin(dlon / 2) ** 2)
    return 2 * 6371.0088 * math.asin(math.sqrt(h))


def process_record(data, device, ip, database=None):
    """Store one telemetry record.

    Returns the device's current config when the record asked for it
    (``ri=1``), otherwise None.
    """
    database = database or db.web

    previous = database.one(
        'SELECT * FROM `log` WHERE `device_id` = %s ORDER BY `id` DESC LIMIT 1',
        (device['id'],),
    )

    entry = _build_entry(data, device, ip, previous)
    now = datetime.datetime.now()

    # powered_on marks the record on which the ignition came up, so the
    # transition is queryable without comparing adjacent rows.
    powered_on = bool(
        previous
        and str(previous['ignition_state']) == '0'
        and entry['ignition_state'] == 1
    )

    if 'dbg' in entry or 'rst' in entry:
        logs.debug.info(
            'IMEI=%s up=%s ign=%s bat=%s%s%s',
            device.get('imei', '?'), entry.get('uptime', '?'),
            entry['ignition_state'], entry['battery_level'],
            (' dbg=' + entry['dbg']) if 'dbg' in entry else '',
            (' rst=' + entry['rst']) if 'rst' in entry else '',
        )

    if powered_on:
        try:
            _ignition_alerts(device, entry, now)
        except Exception:
            logs.app.exception('ignition alert failed for %s', device.get('imei'))

    values = [entry.get(column) for column in LOG_COLUMNS]
    values[LOG_COLUMNS.index('powered_on')] = powered_on
    placeholders = ', '.join(['%s'] * len(values))
    columns = ', '.join('`%s`' % c for c in LOG_COLUMNS)
    log_id = database.query(
        'INSERT INTO `log` (%s) VALUES (%s)' % (columns, placeholders), values
    )

    try:
        _update_journey(database, device, entry, log_id, powered_on)
    except Exception:
        logs.app.exception('journey update failed for %s', device.get('imei'))

    # Mirror config the device reported back onto its row.
    updates = [(key, int(data[key])) for key in DEVICE_SYNC_KEYS if key in data]
    if updates:
        assignments = ', '.join('`%s` = %%s' % key for key, _ in updates)
        database.query(
            'UPDATE `device` SET %s WHERE `id` = %%s' % assignments,
            [value for _, value in updates] + [device['id']],
        )

    if 'request_int' in data:
        return device_config(device)

    return None


def device_config(device):
    """The settings the device syncs, with their defaults."""
    movement_alarm = device.get('movement_alarm')
    return {
        'int': device.get('int') or 0,
        'ma': 1 if movement_alarm is None else movement_alarm,
    }


# -- fault codes -------------------------------------------------------------

def process_dtc_report(device, codes, database, log):
    """Reconcile a reported set of fault codes against the `dtc` table.

    The device always reports its complete current set, so this is a set
    difference rather than an append: a code present in the report but not
    already active is newly raised, and a code active in the table but absent
    from the report has been cleared — by a garage, a battery disconnect, or
    the ECU itself once the fault stopped recurring.  Rows are never deleted,
    so the table is a history of when each fault appeared and disappeared.

    An empty report is meaningful and clears everything; that is how a device
    says "no stored codes".  Returns (raised, cleared) counts.
    """
    now = datetime.datetime.now().strftime('%Y-%m-%d %H:%M:%S.%f')

    reported = []
    for code in codes:
        code = code.strip().upper()
        if not code:
            continue
        if not DTC_RE.match(code):
            log.warning('ignoring malformed DTC %r from %s', code, device['imei'])
            continue
        if code not in reported:
            reported.append(code)

    active = database.all(
        'SELECT `id`, `code` FROM `dtc` WHERE `device_id` = %s AND `active` = 1',
        (device['id'],),
    ) or []
    active_codes = {row['code'] for row in active}

    raised = [c for c in reported if c not in active_codes]
    cleared = [row for row in active if row['code'] not in reported]

    for code in raised:
        database.query(
            'INSERT INTO `dtc` (`device_id`, `code`, `raised_at`, `active`) '
            'VALUES (%s, %s, %s, 1)',
            (device['id'], code, now),
        )
    if cleared:
        ids = [row['id'] for row in cleared]
        placeholders = ', '.join(['%s'] * len(ids))
        database.query(
            'UPDATE `dtc` SET `active` = 0, `cleared_at` = %%s '
            'WHERE `id` IN (%s)' % placeholders,
            [now] + ids,
        )

    if raised or cleared:
        log.info('DTC %s: raised=%s cleared=%s', device['imei'],
                 ','.join(raised) or '-',
                 ','.join(row['code'] for row in cleared) or '-')

    # One notification per event, not per code.  A single root fault
    # routinely raises three or four codes at once, and four separate pushes
    # for one problem is how people learn to ignore the alerts.  A new fault
    # is worth waking someone for; one going away is not.
    name = device.get('name') or device.get('imei')
    if raised:
        word = 'fault code' if len(raised) == 1 else 'fault codes'
        notify.send('%s: %s %s raised' % (name, word, ', '.join(raised)),
                    priority=1)
    if cleared:
        names = ', '.join(row['code'] for row in cleared)
        word = 'fault code' if len(cleared) == 1 else 'fault codes'
        notify.send('%s: %s %s cleared' % (name, word, names), priority=0)

    return len(raised), len(cleared)


# -- alerts ------------------------------------------------------------------

def _handle_alert(line, device, database, log):
    """Relay a device-originated alert line (``A,<priority>,<message>``)."""
    body = line[2:]
    parts = body.split(',', 1)
    if len(parts) == 2 and parts[0].lstrip('-').isdigit():
        priority, message = int(parts[0]), parts[1]
    else:
        priority, message = 0, body

    if device.get('garage') and priority == 2:
        priority = 0

    # Low-battery alerts are only meaningful with the engine off.  While
    # running, smart and regenerative charging systems swing the bus from
    # roughly 11.8 V to 14.9 V by design, and the device tests an
    # instantaneous voltage with no engine gate — so a low reading mid-drive
    # is a false alarm.  Suppress it while running; a genuine resting low
    # reading still relays, at normal priority.
    suppress = False
    if message.lower().startswith('low battery'):
        latest = database.one(
            'SELECT `ignition_state` FROM `log` WHERE `device_id` = %s '
            'ORDER BY `id` DESC LIMIT 1',
            (device['id'],),
        )
        if latest and str(latest['ignition_state']) == '1':
            suppress = True
        else:
            priority = 0

    log.info('alert from %s (pri=%d): %s%s', device['imei'], priority, message,
             ' [suppressed: engine running]' if suppress else '')
    if not suppress:
        notify.device_alert(device['name'], message, priority)


# -- response ----------------------------------------------------------------

def build_response(device, database, log, firmware_version=None):
    """Build the reply the device reads after a successful send.

    Format: ``1,<interval>[,<movement_alarm>][,<commands>]``.

    The leading ``1`` is the ack the firmware checks before clearing its send
    buffer.  Commands are appended as a comma-separated list and deleted as
    they are handed over — the device has no way to acknowledge them
    separately, so delivery is at-most-once by design: a command lost to a
    dropped reply is re-queued by whoever issued it, which is safer than
    replaying a `poweroff` the operator has since changed their mind about.
    """
    settings = device_config(device)
    if config.SLIM_RESPONSE:
        response = '1,%s' % settings['int']
    else:
        response = '1,%s,%s' % (settings['int'], settings['ma'])

    commands = database.all(
        'SELECT `id`, `command` FROM `command` WHERE `device_id` = %s ORDER BY `id`',
        (device['id'],),
    )
    if commands:
        command_string = ','.join(c['command'] for c in commands)
        response += ',' + command_string
        ids = [c['id'] for c in commands]
        placeholders = ','.join(['%s'] * len(ids))
        database.query('DELETE FROM `command` WHERE `id` IN (%s)' % placeholders, ids)
        log.info('delivered %d commands to %s: %s',
                 len(commands), device['imei'], command_string)

    # The OTA indication rides the command field, so a slim response — which
    # has no command field — cannot carry it.
    if firmware_version and not config.SLIM_RESPONSE:
        response += ',fota=%s' % firmware_version

    return response


_UPTIME_RE = re.compile(r'(?:^|[,;])up=(\d+)(?:[,;]|$)')


def boot_wall_time(lines, now=None, device=None, database=None):
    """Estimate when the device booted, as a datetime, or None.

    Firmware log lines are stamped with uptime, which only means something
    against a boot time.  The telemetry record in the same batch carries up=
    (seconds since boot as of moments before the send), so now minus that is
    the boot time to within a few seconds.  With no record in the batch, fall
    back to the last stored row's uptime and receipt time; that is wrong if
    the device rebooted since, but the line's own uptime is printed too, so
    the reader can tell.
    """
    now = now or datetime.datetime.now()
    for line in lines:
        if line[:2] in ('A,', 'D,', 'L,'):
            continue
        m = _UPTIME_RE.search(line)
        if m:
            return now - datetime.timedelta(seconds=int(m.group(1)))

    if device is None or database is None:
        return None
    try:
        row = database.one(
            'SELECT `timestamp`, `uptime` FROM `log` '
            'WHERE `device_id` = %s ORDER BY `id` DESC LIMIT 1',
            (device['id'],),
        )
    except Exception:
        logs.app.exception('failed to read last uptime for %s', device.get('imei'))
        return None
    if not row or row.get('uptime') is None or not row.get('timestamp'):
        return None
    stamp = row['timestamp']
    if isinstance(stamp, str):
        try:
            stamp = datetime.datetime.strptime(stamp, '%Y-%m-%d %H:%M:%S.%f')
        except ValueError:
            stamp = datetime.datetime.strptime(stamp, '%Y-%m-%d %H:%M:%S')
    return stamp - datetime.timedelta(seconds=int(float(row['uptime'])))


def format_device_log(line, imei, boot_wall, now=None):
    """Render one "L,<uptime_ms>,<E|W>,<module>: <text>" record for the
    device log.  Stamped with the time the device logged it; with no boot
    reference, the receipt time marked "(rx)".  Raises ValueError on a
    malformed line."""
    parts = line.split(',', 3)
    if len(parts) < 4 or not parts[1].isdigit():
        raise ValueError('malformed firmware log line: %r' % line[:80])
    _, ms, level, text = parts
    up_ms = int(ms)
    if boot_wall is not None:
        when = (boot_wall + datetime.timedelta(milliseconds=up_ms)) \
            .strftime('%Y-%m-%d %H:%M:%S')
    else:
        when = (now or datetime.datetime.now()).strftime('%Y-%m-%d %H:%M:%S') + '(rx)'
    return '%s IMEI=%s up=%d.%03d %s %s' % (
        when, imei, up_ms // 1000, up_ms % 1000, level, text)


def process_lines(device, lines, ip, database, log):
    """Process a batch of plaintext lines and return the response string.

    Shared by all three transports.  A bad record is logged and skipped rather
    than failing the batch: the device has already spent the radio time, and
    dropping eleven good fixes because the twelfth was malformed would be the
    worse outcome.
    """
    from . import firmware

    processed = 0
    boot_wall = None      # worked out on the first L, line, if any
    for line in lines:
        try:
            if line.startswith('A,'):
                _handle_alert(line, device, database, log)
            elif line.startswith('D,'):
                # Complete current set of stored fault codes; "D," alone means
                # the ECU reported none, which clears anything still active.
                process_dtc_report(device, line[2:].split(','), database, log)
            elif line.startswith('L,'):
                # A warning or error the firmware logged since its last
                # successful send.  File it; nothing to store or act on.
                if boot_wall is None:
                    boot_wall = boot_wall_time(lines, device=device,
                                               database=database)
                logs.device.info('%s', format_device_log(
                    line, device['imei'], boot_wall))
            else:
                process_record(parse_csv_line(line), device, ip, database=database)
            processed += 1
        except ValueError as e:
            log.error('record error IMEI=%s: %s', device['imei'], e)
        except Exception:
            log.exception('unexpected error IMEI=%s', device['imei'])

    log.info('%d records from %s (%s)', processed, device['imei'], ip)

    # Re-read: the batch may have changed the device's own settings.
    device = database.one('SELECT * FROM `device` WHERE `id` = %s', (device['id'],))

    return build_response(device, database, log,
                          firmware_version=firmware.latest_version(device['imei']))
