"""Telemetry parsing, storage and response building.

Everything below the UDP listener lives here.  Once a datagram has been
decrypted and attributed to a device, the listener hands its plaintext lines
to :func:`process_lines`.

Wire format
-----------
One record per line, comma-separated::

    ts,lat,lon,spd,alt,hdg,hdop,sat,bat,ign,up,pon[,extras...]

``ts`` is the modem's own clock as ``dd/mm/yy,HH:MM:SS+NN`` — note that it
contains a comma, so the first two fields are rejoined before parsing.
``hdop`` arrives multiplied by ten, and ``up`` is seconds since boot, stored
in the ``waketime`` column; see docs/PROTOCOL.md.  (``waketime`` predates the
current meaning of that field and is unrelated to ``wake_ms`` below.)

``rid=`` is the device's own id for a record, consecutive within a boot and
seeded at random at each one, and ``wt=<rid>:<ms>`` is how long the wake that
sent record ``<rid>`` lasted.  That figure names its record because arrival
order cannot identify it: a wake whose own send failed leaves its record in
the device's backlog, which is flushed behind the live record describing it.

``rid=`` is also how a copy is recognised.  The device holds a datagram's
records until this server's reply to it comes back, and sends them again if it
never does, so a datagram that arrived but whose reply was lost arrives twice;
the second copy of a record (same rid, same fix time) is dropped.  ``age=<s>``
is on a record sent long after it was built — from the backlog, or again after
its first datagram went unanswered.  It is filed at the time it was built
rather than on arrival, and when something newer is already stored it is
history: it takes its place in the track and does nothing a live record would
(no ignition alert, no journey change, no settings or track-mode change).

Everything after the twelve fixed fields is an "extras" group: comma-separated
groups of ``key=value`` pairs joined by semicolons.  Extras are optional and
sparse by design.  The device pays for every byte in radio time, so fields
that rarely change (serving cell, firmware version) are sent only when they do
change or on the first record after a wake, and the server carries the last
known value forward so every row is still self-describing.

``mv=<gap>:<v0>:<steps>:<end>`` is on the first record after a gap between
fixes: the motion GNSS could not see, the ECU's speed and the gyro's heading
change for each second of it.  It is stored as sent, in ``motion``, and the
journey replay turns it into a path fitted between the two fixes; see
:mod:`tracker.deadreckon`.

A line beginning ``A,`` is an alert rather than a position record:
``A,<priority>,<message>``, with ``,aid=<n>`` on the end from firmware that
holds alerts until answered; a copy of one already relayed is dropped the same
way a record's is.  ``D,<code>,<code>...`` is the vehicle's complete
set of stored fault codes (``D,`` alone means none), and
``L,<uptime_ms>,<E|W>,<text>`` is a warning or error the firmware captured
between sends.
"""

import datetime
import math
import re
import threading
import time

from . import (config, db, deadreckon, device_events, firmware,
               interval_schedule, logs, notify)

# Fixed fields, in wire order, named for the columns they are stored in.  The
# eleventh is seconds since boot on current firmware; its column kept the
# older name, waketime.
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
    'bd':  'board',              # board the running build is for, e.g. v4.0p1+kline
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
    'tm':  'track_mode',         # 1 = built in track mode (GNSS off, fast poll)
    'acc': 'imu_burst',          # IMU samples since the previous record
    'mv':  'motion',             # the motion through the gap before this fix
    'rid': 'rec_id',             # the device's own id for this record
    'rsrp': 'rsrp',              # serving cell, dBm (AT%XMONITOR)
    'snr': 'snr',                # serving cell, dB
    'band': 'band',              # LTE band in use
    'pathloss': 'pathloss',      # downlink path loss, dB
    'ce': 'ce_level',            # LTE-M coverage enhancement level
    'txrep': 'tx_rep',           # estimated transmit repetitions
    # 'rsrq' is handled in parse_csv_line: sent in tenths of a dB.
    # 'wt' is handled in parse_csv_line: it is <rid>:<ms>, and one of the two
    # fields that describe another record rather than this one.
    # 'ws' is the other: <rid>:<rsrp>:<snr>:<band>, the signal read after
    # that record's send.
}

# Upper bound on a reported wake, in milliseconds.  The device's own ceiling
# on an engine-off wake is its registration search plus a fix attempt, minutes
# at most, so anything near a day is a corrupt field rather than a very
# patient tracker -- and storing it would skew every average over the column.
WAKE_MS_MAX = 24 * 60 * 60 * 1000

# What a ws= reading can hold: the firmware converts the 3GPP indices, which
# cover RSRP -140..-44 dBm and SNR -24..+24 dB, and names an LTE band.
WAKE_RSRP_RANGE = (-140, -44)
WAKE_SNR_RANGE = (-24, 24)
WAKE_BAND_RANGE = (1, 255)

# Config the device may report back, mirrored onto the `device` row so the
# server's view of a unit's settings tracks what the unit actually applied.
DEVICE_SYNC_KEYS = ['int', 'movement_alarm']

# Track mode ends with the drive: the switch is cleared by the ignition-off
# record, and — for a device that never sent one — once nothing has been
# heard for this long.  Otherwise a switch left on by a page that was never
# revisited would put the next drive, or a rebooted device, into the mode.
TRACK_MODE_IDLE_SECONDS = 3600

# Fields that describe a slowly-changing condition rather than this instant.
# When absent, the previous row's value is carried forward.
STICKY_KEYS = ('mcc', 'mnc', 'lac', 'cid', 'rat', 'fw')

# What the firmware may call its board (Kconfig APP_BOARD_ID plus the fitted
# interfaces): the column holds 32 characters and the database refuses more.
BOARD_RE = re.compile(r'^[A-Za-z0-9._+-]{1,31}$')

# Fields that mean "right now" and must never be carried forward.
PER_PACKET_KEYS = (
    'accel_x', 'accel_y', 'accel_z', 'gyro_x', 'gyro_y', 'gyro_z',
    'mcu_temp', 'imu_temp', 'uptime', 'waketime', 'dead_reckoning', 'vsys',
    'cell_location',
    # Signal quality is a measurement, not a condition: carrying it forward
    # would put a reading on a row where none was taken.
    'rsrp', 'snr', 'band', 'pathloss', 'rsrq', 'ce_level', 'tx_rep',
)

# Track mode (firmware TRACK_MODE.md).  A record built with GNSS off carries
# tm=1 and acc=<burst>: one ax/ay/az/gx/gy/gz group per IMU sample, groups
# joined by ':', oldest first, 26 Hz — accel in milli-g, gyro in raw LSB.
# The burst is stored verbatim and unpacked on the way out (devices.py); it
# is validated against this shape so nothing but integers reaches the row.
IMU_BURST_RE = re.compile(r'^-?\d+(/-?\d+){5}(:-?\d+(/-?\d+){5})*$')
IMU_BURST_MAX = 4000

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
    'dbg', 'rst', 'fw', 'board',
    'mcc', 'mnc', 'lac', 'cid', 'cell_location', 'rat',
    'accel_x', 'accel_y', 'accel_z', 'gyro_x', 'gyro_y', 'gyro_z',
    'waketime', 'uptime', 'mcu_temp', 'imu_temp', 'dead_reckoning', 'vsys',
] + [column for column, _, _ in OBD_FIELDS.values()] + [
    'combined_speed', 'track_mode', 'imu_burst', 'rec_id', 'wake_ms',
    'attach_ms', 'rsrp', 'snr', 'band',
    'pathloss', 'rsrq', 'ce_level', 'tx_rep', 'motion',
]

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
        if extra.startswith('tb='):
            # The built tunable values, code:value pairs joined by ';' like
            # tu=, so taken whole rather than split like the others.
            data['tunables_built'] = extra[3:]
            continue

        for pair in extra.split(';'):
            if '=' not in pair:
                continue
            key, value = pair.split('=', 1)
            if key == 'rsrq':
                # Tenths of a dB: the 3GPP index has half-dB steps, so a
                # whole number would throw half the resolution away.
                try:
                    data['rsrq'] = '%.1f' % (int(value) / 10.0)
                except (TypeError, ValueError):
                    pass
                continue
            if key == 'age':
                # Seconds between the device building the record and sending
                # it; not a column, but when the record belongs.
                data['age'] = value
                continue
            if key == 'wt':
                # <rid>:<ms>[:<attach_ms>] -- the record the figure belongs
                # to, how long that wake took, and how much of it was the LTE
                # attach.  The reference is kept apart from wake_ms's column
                # so it is never mistaken for a value to store on this row.
                bits = value.split(':')
                if len(bits) >= 2 and bits[1]:
                    data['wake_ref'] = bits[0]
                    data['wake_ms'] = bits[1]
                    if len(bits) >= 3 and bits[2]:
                        data['wake_attach_ms'] = bits[2]
                continue
            if key == 'bk':
                # <state>:<age_s>[|<state>:<age_s>...], oldest first: each
                # change of the GNSS backup supply and how long before this
                # record was built it happened.  Not a column: each change
                # is kept on the device row (`gnss_backup`).
                data['backup_changes'] = value
                continue
            if key == 'ap':
                # <int>:<ma>:<backup>:<track> -- the settings the unit is
                # running now, sent after a boot and whenever one changes.
                data['applied'] = value
                continue
            if key == 'ak':
                # <name>|<name>... -- commands the unit has run since the last
                # record that said so.
                data['acks'] = value
                continue
            if key == 'ws':
                # <rid>:<rsrp>:<snr>:<band> -- the serving cell as the wake
                # that sent record <rid> left it.  A wake from PSM builds its
                # record before the radio is up, and a sleeping modem has no
                # measurement to give, so the device reads it after the send.
                # Kept under names of its own for the same reason as wt=:
                # these are that record's figures, not this one's.
                bits = value.split(':')
                if len(bits) >= 4 and all(bits[:4]):
                    data['wake_signal_ref'] = bits[0]
                    data['wake_rsrp'] = bits[1]
                    data['wake_snr'] = bits[2]
                    data['wake_band'] = bits[3]
                continue
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
    # HDOP arrives in tenths — the firmware sends hdop x 10, the scale the
    # tracker it replaced used — and the column holds the HDOP itself.
    entry['hdop'] = '%.2f' % (float(data['hdop']) / 10)
    for key in ('latitude', 'longitude', 'altitude', 'heading',
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

    # The board goes with the build, as fw does, but is carried forward only
    # while the version is the same: a build too old to report one must leave
    # its rows empty, not wearing the previous build's board.
    board = data.get('board')
    if board is not None and not BOARD_RE.match(board):
        logs.app.warning('unusable bd=%r from %s', board,
                         device.get('imei', '?'))
        board = None
    if (board is None and previous is not None
            and entry.get('fw') == previous.get('fw')):
        board = previous.get('board')
    if board is None and entry.get('fw'):
        # Firmware too old to send bd=: the image published at that version
        # says which board it was made for.
        board = firmware.image_board(device.get('imei'), entry['fw'])
    if board is not None:
        entry['board'] = board

    for key in PER_PACKET_KEYS:
        if key in data:
            entry[key] = data[key]

    # The device's own id for this record: what lets a later record name it,
    # and what makes a gap in the sequence a record that never arrived.
    if 'rec_id' in data:
        try:
            entry['rec_id'] = int(data['rec_id'])
        except (TypeError, ValueError):
            logs.app.warning('unparsable rid=%r from %s', data['rec_id'],
                             device.get('imei', '?'))

    if 'track_mode' in data:
        try:
            entry['track_mode'] = 1 if int(data['track_mode']) else 0
        except (TypeError, ValueError):
            pass
    burst = data.get('imu_burst')
    if burst and len(burst) <= IMU_BURST_MAX and IMU_BURST_RE.match(burst):
        entry['imu_burst'] = burst

    # mv=: the ECU's speed and the gyro's heading change through the gap
    # since the previous record, stored as sent and turned into a path when
    # a journey is drawn (deadreckon.py).  Checked for shape first, so only
    # the field's own alphabet ever reaches the row.
    motion = data.get('motion')
    if motion and deadreckon.valid(motion):
        entry['motion'] = motion

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


def gnss_off_window_token(device, now=None):
    """The gw= value for ``device``: the GNSS backup off window as UTC whole
    hours, ``<from>-<to>`` (it may wrap midnight, 23-6), or ``off`` when it is
    disabled (``gnss_off_window`` 0, or both hours equal).  The settings are
    server local hours; the offset used is the one in force at ``now``."""
    if not device.get('gnss_off_window'):
        return 'off'
    hour_from = device.get('gnss_off_hour_from')
    hour_to = device.get('gnss_off_hour_to')
    hour_from = 0 if hour_from is None else int(hour_from)
    hour_to = 7 if hour_to is None else int(hour_to)
    if hour_from == hour_to:
        return 'off'
    now = (now or datetime.datetime.now()).astimezone()
    offset = now.utcoffset()
    shift = int(offset.total_seconds() // 3600) if offset else 0
    return '%d-%d' % ((hour_from - shift) % 24, (hour_to - shift) % 24)


# The runtime tunables: (token code, device column, low, high).  A NULL column
# is "no override" (the unit keeps the value it was built with).  The order is
# the order of the tu= token.  obd_alerts is text, so has no range.
TUNABLES = [
    ('xtt', 'tow_tilt_deg', 0, 30),
    ('xpi', 'parked_impact_mg', 300, 2500),
    ('xii', 'impact_immediate_mg', 0, 1200),
    ('xap', 'accel_alert_priority', -2, 2),
    ('xab', 'accel_alert_backoff_s', 0, 86400),
    ('xoa', 'obd_alerts', None, None),
    ('xis', 'ignition_on_sleep_s', 10, 600),
    ('xbw', 'battery_warning_mv', 11000, 12500),
    ('xbp', 'battery_poweroff_mv', 10500, 12200),
    ('xds', 'drive_stream', 0, 1),
    ('xdi', 'drive_stream_interval_s', 0, 900),
    ('xtp', 'track_period_ms', 200, 5000),
    ('xdd', 'databuf_drain_s', 1, 600),
    ('xns', 'network_search_s', 60, 86400),
    ('xrp', 'resp_poll_s', 5, 600),
    ('xlw', 'led_accel_wake', 0, 1),
]
TUNABLE_COLUMNS = [column for _, column, _, _ in TUNABLES]
TUNABLE_RANGES = {column: (low, high) for _, column, low, high in TUNABLES
                  if column != 'obd_alerts'}
OBD_ALERTS_MAX = 120

# One APP_OBD_ALERTS rule: <field><op><value>[/<hysteresis>], the operators
# > >= < <= =, the field a name (or record key), the numbers decimal.
_OBD_RULE = re.compile(
    r'[a-z][a-z0-9_]*(?:>=|<=|>|<|=)-?\d+(?:\.\d+)?(?:/\d+(?:\.\d+)?)?')


def valid_obd_alerts(text):
    """True if ``text`` is a rule list the unit can take: comma-separated
    APP_OBD_ALERTS rules, at most 120 printable ASCII characters, without
    ';' or '|' (the tu= separators) and without empty rules."""
    if not isinstance(text, str) or not text or len(text) > OBD_ALERTS_MAX:
        return False
    if any(not 0x20 < ord(c) < 0x7f or c in ';|' for c in text):
        return False
    return all(_OBD_RULE.fullmatch(rule) for rule in text.split(','))


TUNABLES_BUILT_MAX = 400


def parse_tunables_built(text):
    """The tb= value (``code:value`` pairs joined by ';'; xoa's commas sent as
    '|') as a dict of code -> int, or code -> str for xoa.  Unknown codes and
    bad values are left out, so a damaged report yields what is readable (or
    nothing).  Built values are not held to the override ranges: they are
    whatever the firmware was compiled with."""
    result = {}
    if not isinstance(text, str):
        return result
    codes = {code for code, _, _, _ in TUNABLES}
    for pair in text.split(';'):
        code, sep, value = pair.partition(':')
        if not sep or code not in codes or code in result:
            continue
        if code == 'xoa':
            value = value.replace('|', ',')
            if (len(value) > OBD_ALERTS_MAX
                    or any(not 0x20 < ord(c) < 0x7f for c in value)):
                continue
            result[code] = value
        else:
            try:
                result[code] = int(value)
            except ValueError:
                continue
    return result


def tunables_built_text(built):
    """The stored form of a parse_tunables_built() dict: the same grammar the
    unit sent, in table order, or None when it holds nothing."""
    pairs = []
    for code, _, _, _ in TUNABLES:
        if code not in built:
            continue
        value = built[code]
        if code == 'xoa':
            value = value.replace(',', '|')
        pairs.append('%s:%s' % (code, value))
    return ';'.join(pairs)[:TUNABLES_BUILT_MAX] or None


def tunables_built(device):
    """The built values the unit reported, keyed by token code, null for any
    it has not reported."""
    built = parse_tunables_built(device.get('tunables_built'))
    return {code: built.get(code) for code, _, _, _ in TUNABLES}


def tunables_effective(device):
    """The value in use per token code: the override if set, else the built
    value the unit reported, else null."""
    built = tunables_built(device)
    result = {}
    for code, column, _, _ in TUNABLES:
        value = device.get(column)
        if value is None:
            value = built[code]
        result[code] = value
    return result


def tunables_token(device):
    """The tu= value for ``device``: its non-NULL tunables as
    ``<code>:<value>`` joined by ';' in table order, or ``-`` when none is set.
    The obd_alerts commas go out as '|' because the reply is comma-joined."""
    pairs = []
    for code, column, _, _ in TUNABLES:
        value = device.get(column)
        if value is None or value == '':
            continue
        if column == 'obd_alerts':
            pairs.append('%s:%s' % (code, str(value).replace(',', '|')))
        else:
            pairs.append('%s:%d' % (code, int(value)))
    return ';'.join(pairs) or '-'


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
    # In time order, not arrival order: a record sent again after its first
    # datagram went unanswered arrives after later ones, and summed where it
    # landed it would add a detour the vehicle never drove.
    rows = database.all(
        'SELECT `latitude`, `longitude` FROM `log` WHERE `device_id` = %s '
        'AND `id` >= %s AND `id` <= %s ORDER BY `timestamp`, `id`',
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


def _wake_figure(data, imei):
    """The ``(rec_id, ms, attach_ms)`` a ``wt=`` carries, or three ``None``.

    Anything that is not a set of plausible integers is rejected: a corrupt
    figure cannot be told from a real one once stored, and would skew every
    average taken over the column.  The attach part is optional -- a wake that
    found the modem already registered paid none -- and is dropped on its own
    if it does not parse, since the total is still worth having.
    """
    if 'wake_ms' not in data or 'wake_ref' not in data:
        return None, None, None
    try:
        ref = int(data['wake_ref'])
        millis = int(data['wake_ms'])
    except (TypeError, ValueError):
        logs.app.warning('unparsable wt=%r:%r from %s', data.get('wake_ref'),
                         data.get('wake_ms'), imei)
        return None, None, None
    if ref <= 0 or not 0 < millis <= WAKE_MS_MAX:
        logs.app.warning('implausible wt=%d:%d from %s', ref, millis, imei)
        return None, None, None

    attach = None
    if 'wake_attach_ms' in data:
        try:
            attach = int(data['wake_attach_ms'])
        except (TypeError, ValueError):
            attach = None
        # The attach is part of the wake, so it cannot exceed it.
        if attach is None or not 0 <= attach <= millis:
            logs.app.warning('implausible attach %r in wt= from %s (wake %d ms)',
                             data.get('wake_attach_ms'), imei, millis)
            attach = None
    return ref, millis, attach


def _held_wake_for(device, rec_id):
    """The ``(ms, attach_ms)`` held for a record that had not arrived yet."""
    held = device.get('wake_pending_rec_id')
    if held is None or rec_id is None or int(held) != rec_id:
        return None
    return device.get('wake_pending_ms'), device.get('wake_pending_attach_ms')


def _store_wake_figure(database, device, data, rec_id):
    """File a record's ``wt=`` against the record it names.

    A wake cannot measure itself -- the device is still awake when it builds
    the record, and one that could not register spends most of its time after
    the send, waiting for a registration that never comes -- so the figure is
    finished once the wake is over and travels on a later record.

    It names its record rather than meaning "the one before" because arrival
    order does not identify it: a wake whose own send failed leaves its record
    in the device's backlog, which is flushed *behind* the live record
    describing it.  When the named record is not here yet the figure is held
    on the device row, and the insert path applies it as the record lands.  At
    most one is ever outstanding, since the device clears its own pending
    figure as it emits it.
    """
    ref, millis, attach = _wake_figure(data, device.get('imei', '?'))
    if ref is None:
        return
    if ref == rec_id:
        # A record cannot describe the wake that sent it; that is the whole
        # reason the figure travels separately.  Corrupt, not merely late.
        logs.app.warning('wt= names its own record (%d) from %s', ref,
                         device.get('imei', '?'))
        return

    # The newest match: ids are consecutive within a boot and seeded at random
    # at each one, so even a long-lived device has one plausible row per id.
    target = database.one(
        'SELECT `id`, `wake_ms` FROM `log` '
        'WHERE `device_id` = %s AND `rec_id` = %s ORDER BY `id` DESC LIMIT 1',
        (device['id'], ref),
    )
    if target is None:
        database.query(
            'UPDATE `device` SET `wake_pending_rec_id` = %s, '
            '`wake_pending_ms` = %s, `wake_pending_attach_ms` = %s '
            'WHERE `id` = %s',
            (ref, millis, attach, device['id']),
        )
        # The caller's copy of the row too: a datagram is read once and may
        # carry several records, and the record this figure is waiting for
        # can be one of them.
        device['wake_pending_rec_id'] = ref
        device['wake_pending_ms'] = millis
        device['wake_pending_attach_ms'] = attach
        return
    # Left alone if it already carries a figure, so a duplicate datagram
    # cannot overwrite one with a stale repeat of itself.
    if target['wake_ms'] is None:
        database.query(
            'UPDATE `log` SET `wake_ms` = %s, `attach_ms` = %s WHERE `id` = %s',
            (millis, attach, target['id']),
        )


def _wake_signal(data, imei):
    """The ``(rec_id, rsrp, snr, band)`` a ``ws=`` carries, or four ``None``.

    All four or nothing: a reading is only worth storing whole, and one
    outside what the modem can report is a corrupt field, not a measurement.
    """
    if 'wake_signal_ref' not in data:
        return None, None, None, None
    try:
        ref = int(data['wake_signal_ref'])
        rsrp = int(data['wake_rsrp'])
        snr = int(data['wake_snr'])
        band = int(data['wake_band'])
    except (TypeError, ValueError):
        logs.app.warning('unparsable ws=%r:%r:%r:%r from %s',
                         data.get('wake_signal_ref'), data.get('wake_rsrp'),
                         data.get('wake_snr'), data.get('wake_band'), imei)
        return None, None, None, None
    if (ref <= 0
            or not WAKE_RSRP_RANGE[0] <= rsrp <= WAKE_RSRP_RANGE[1]
            or not WAKE_SNR_RANGE[0] <= snr <= WAKE_SNR_RANGE[1]
            or not WAKE_BAND_RANGE[0] <= band <= WAKE_BAND_RANGE[1]):
        logs.app.warning('implausible ws=%d:%d:%d:%d from %s',
                         ref, rsrp, snr, band, imei)
        return None, None, None, None
    return ref, rsrp, snr, band


def _store_wake_signal(database, device, data, rec_id):
    """File a record's ``ws=`` against the record it names.

    The signal a wake read after its send, for a record that went out without
    a reading of its own: a wake from PSM builds its record before the radio
    is up.  It only ever fills a gap -- a record that carried its own reading
    keeps it -- and it is never held the way a wake figure is: the device
    reads it only after a send the server answered, so the record it names
    is already stored, and one that is not is dropped rather than waited for.
    """
    ref, rsrp, snr, band = _wake_signal(data, device.get('imei', '?'))
    if ref is None:
        return
    if ref == rec_id:
        logs.app.warning('ws= names its own record (%d) from %s', ref,
                         device.get('imei', '?'))
        return
    target = database.one(
        'SELECT `id`, `rsrp` FROM `log` '
        'WHERE `device_id` = %s AND `rec_id` = %s ORDER BY `id` DESC LIMIT 1',
        (device['id'], ref),
    )
    if target is None:
        logs.app.info('ws= names record %d, which is not stored, from %s',
                      ref, device.get('imei', '?'))
        return
    if target['rsrp'] is None:
        database.query(
            'UPDATE `log` SET `rsrp` = %s, `snr` = %s, `band` = %s '
            'WHERE `id` = %s',
            (rsrp, snr, band, target['id']),
        )


# Longest hold age= is believed for: well past any backlog the device can keep
# (it thins a long outage rather than stopping), well short of a garbled field
# putting a record in another year.
RECORD_AGE_MAX = 7 * 24 * 60 * 60


def _record_age(data, imei):
    """Seconds between the device building this record and sending it, from
    ``age=``, or None for a record sent fresh (or an unusable field)."""
    raw = data.get('age')
    if raw is None:
        return None
    try:
        age = int(raw)
    except (TypeError, ValueError):
        logs.app.warning('unparsable age=%r from %s', raw, imei)
        return None
    if age < 0 or age > RECORD_AGE_MAX:
        logs.app.warning('age=%d out of range from %s', age, imei)
        return None
    return age


def _latest_row(database, device_id):
    """The device's latest state: of its last few rows by arrival, the one
    built last.  Arrival order stopped saying that once late records were
    filed at the time they were built — a backlog flushed behind the live
    record, a record sent again after its datagram went unanswered."""
    rows = database.all(
        'SELECT `id`, `timestamp` FROM `log` WHERE `device_id` = %s '
        'ORDER BY `id` DESC LIMIT 50',
        (device_id,),
    )
    if not rows:
        return None
    newest = max(rows, key=lambda r: (r['timestamp'], r['id']))
    return database.one('SELECT * FROM `log` WHERE `id` = %s', (newest['id'],))


def _is_copy(database, device, data):
    """Whether this record is already stored: the device sent it again
    because the reply to its first datagram never reached it, and the first
    did arrive.  rid is consecutive within a boot and the fix time pins it to
    that boot, so the pair is the record."""
    try:
        rec_id = int(data['rec_id'])
    except (KeyError, TypeError, ValueError):
        return False
    stamp, _ = _parse_gsm_timestamp(data['gsm_timestamp'])
    return database.one(
        'SELECT `id` FROM `log` WHERE `device_id` = %s AND `rec_id` = %s '
        'AND `gsm_timestamp` = %s LIMIT 1',
        (device['id'], rec_id, stamp.strftime('%Y-%m-%d %H:%M:%S.%f')),
    ) is not None


# -- device events ------------------------------------------------------------
#
# What the phone app lists for a unit (device_events.py).  record() already
# keeps its own errors in, so a hook that is one call is written in place; these
# are the ones that need a loop or a read first.  Each keeps its whole body in
# a try, so that a line missing from the event log can never cost a record or
# a reply.

def _note_log_events(database, device, previous, entry, built):
    """The ignition change and the reboot a record just stored reports.
    `previous` is the row it was measured against, as process_record read it."""
    try:
        gsm = entry['gsm_timestamp']
        if (previous is not None
                and int(previous['ignition_state']) != entry['ignition_state']):
            device_events.record_ignition(
                database, device['id'], bool(entry['ignition_state']), built,
                device_events.ref('ign', gsm))
        if entry.get('rst'):
            device_events.record(
                database, device['id'], 'reboot',
                device_events.reset_text(entry['rst']), built,
                device_events.ref('rst', gsm))
    except Exception:
        logs.app.exception('device event failed for %s', device.get('imei'))


def _note_backup_events(database, device, raw, built):
    """The GNSS backup supply changes a record's bk= reports, oldest first.
    Called before the device dict takes the newest one, so the state the unit
    was last known in, and when, is still in it."""
    try:
        changes = []
        for item in raw.split('|'):
            try:
                state, age = item.split(':')
                state, age = int(state), int(age)
            except ValueError:
                continue
            if state not in (0, 1) or age < 0 or age > BACKUP_AGE_MAX:
                continue
            changes.append((built - datetime.timedelta(seconds=age), state))
        held = device.get('gnss_backup')
        guard = device.get('gnss_backup_at')
        for at, state in sorted(changes, key=lambda change: change[0]):
            if guard is not None and not at > guard:
                continue
            if state != held:
                device_events.record(
                    database, device['id'], 'setting',
                    device_events.backup_supply_text(state), at,
                    device_events.ref('bk', state, at.strftime('%Y-%m-%d %H:%M:%S')))
            held = state
    except Exception:
        logs.app.exception('device event failed for %s', device.get('imei'))


def record_command_events(database, device_id, what, commands, at=None):
    """One command event for each command in `commands` (strings, which may
    be comma-joined), `what` being how it went: queued, collected, ..."""
    try:
        for command in commands:
            for part in device_events.command_parts(command):
                device_events.record(database, device_id, 'command',
                                     device_events.command_text(what, part), at)
    except Exception:
        logs.app.exception('device event failed for device %s', device_id)


def record_setting_changes(database, device, changes):
    """One setting event for each (device column, new value) in `changes` whose
    value is not the one on `device`, the row as it was read before the write."""
    try:
        for column, value in dict(changes).items():
            if device.get(column) != value:
                device_events.record(database, device['id'], 'setting',
                                     device_events.setting_text(column, value))
    except Exception:
        logs.app.exception('device event failed for %s', device.get('imei'))


def _expiring_commands(database, device_id):
    """[(command, when it expired)] for the queued commands the sweep in
    build_response is about to drop; read before it, as they are gone after."""
    try:
        return [(row['command'],
                 row['timestamp'] + datetime.timedelta(hours=COMMAND_MAX_AGE_HOURS))
                for row in database.all(
                    'SELECT `id`, `command`, `timestamp` FROM `command` '
                    'WHERE `device_id` = %s '
                    'AND `timestamp` < NOW(6) - INTERVAL %s HOUR',
                    (device_id, COMMAND_MAX_AGE_HOURS)) or []]
    except Exception:
        logs.app.exception('device event failed for device %s', device_id)
        return []


def _note_expired(database, device_id, expired):
    for command, at in expired:
        record_command_events(database, device_id, 'expired', [command], at)


def _note_alert(database, device, message):
    try:
        event = device_events.alert_event(message)
        if event:
            device_events.record(database, device['id'], event[0], event[1])
    except Exception:
        logs.app.exception('device event failed for %s', device.get('imei'))


def _note_faults(database, device, raised, cleared):
    try:
        for code in raised:
            device_events.record(database, device['id'], 'other',
                                 device_events.fault_text(code, True))
        for code in cleared:
            device_events.record(database, device['id'], 'other',
                                 device_events.fault_text(code, False))
    except Exception:
        logs.app.exception('device event failed for %s', device.get('imei'))


def _note_versions(database, device, entry):
    """Keep the device row's firmware and board at what its newest live
    record says: the version the unit is running now and the board that
    build is for.  Written when either changes - a restart into an update,
    mostly - and not otherwise.  A record that carries no version, because
    the unit has never reported one, leaves the row as it is.  So does a
    build that names no board: the row holds the last board the unit
    reported, where the log row for that record is left empty."""
    fw, board = entry.get('fw'), entry.get('board')
    if fw is None:
        return
    if fw == device.get('fw') and (board is None or board == device.get('board')):
        return
    database.query(
        'UPDATE `device` SET `fw` = %s, `board` = COALESCE(%s, `board`) '
        'WHERE `id` = %s',
        (fw, board, device['id']),
    )
    if device.get('fw') not in (None, fw):
        device_events.record(
            database, device['id'], 'firmware',
            device_events.firmware_text('applied', fw, device.get('fw')),
            entry['timestamp'],
            device_events.ref('fw', fw, entry['gsm_timestamp']))
    device['fw'] = fw
    if board is not None:
        device['board'] = board


# A change older than a week was lost in a backlog; the record's own age is
# bounded the same way.
BACKUP_AGE_MAX = RECORD_AGE_MAX


def _store_backup_changes(database, device, data, built):
    """Keep the GNSS backup supply state a record reports (``bk=``) on the
    device row.

    Each item is ``state:age``: on or off, and the seconds before the record
    was built that it changed, so a midnight cut-off that a key-on reports
    hours later still counts from midnight.  Only the newest change matters:
    it becomes the state the replies hand back (``gb=``), unless a newer one
    was set from the API since.  An unusable item is skipped on its own."""
    raw = data.get('backup_changes')
    if not raw:
        return
    newest = None
    for item in raw.split('|'):
        try:
            state, age = item.split(':')
            state, age = int(state), int(age)
        except ValueError:
            logs.app.warning('unparsable bk=%r from %s', item,
                             device.get('imei', '?'))
            continue
        if state not in (0, 1) or age < 0 or age > BACKUP_AGE_MAX:
            logs.app.warning('implausible bk=%r from %s', item,
                             device.get('imei', '?'))
            continue
        at = built - datetime.timedelta(seconds=age)
        if newest is None or at >= newest[0]:
            newest = (at, state)
    if newest is None:
        return
    at, state = newest
    when = at.strftime('%Y-%m-%d %H:%M:%S')
    database.query(
        'UPDATE `device` SET `gnss_backup` = %s, `gnss_backup_at` = %s '
        'WHERE `id` = %s AND (`gnss_backup_at` IS NULL OR `gnss_backup_at` < %s)',
        (state, when, device['id'], when),
    )
    _note_backup_events(database, device, raw, built)
    held = device.get('gnss_backup_at')
    if held is None or held < at.replace(microsecond=0):
        device['gnss_backup'] = state
        device['gnss_backup_at'] = at.replace(microsecond=0)


# Commands not delivered within this long are dropped: a "locate now" from the
# morning is no use to a unit that wakes up in the evening.
COMMAND_MAX_AGE_HOURS = 6


def _store_applied(database, device, data, entry):
    """Keep what a live record says the unit is running (``ap=``) and which
    commands it has run (``ak=``) on its row, with the time.

    A restart that did not change the build is also taken as the acknowledgement
    of a ``reboot``: the unit cannot report one before it goes.  Not called for
    history, which can carry settings since changed."""
    now = datetime.datetime.now().strftime('%Y-%m-%d %H:%M:%S')
    assignments, values = [], []

    raw = data.get('applied')
    if raw:
        try:
            ints = [int(part) for part in raw.split(':')]
            if len(ints) != 4:
                raise ValueError(raw)
        except ValueError:
            logs.app.warning('unparsable ap=%r from %s', raw,
                             device.get('imei', '?'))
        else:
            assignments += ['`ap_int` = %s', '`ap_ma` = %s', '`ap_backup` = %s',
                            '`ap_track` = %s', '`ap_at` = %s']
            values += ints + [now]

    acks = [name for name in (data.get('acks') or '').split('|') if name]
    if entry.get('rst') == 'sw' and \
            entry.get('fw') in (None, device.get('fw')):
        acks.append('reboot')
    if acks:
        assignments += ['`ack_cmd` = %s', '`ack_at` = %s']
        values += ['|'.join(acks)[:48], now]

    if assignments:
        database.query(
            'UPDATE `device` SET %s WHERE `id` = %%s' % ', '.join(assignments),
            values + [device['id']],
        )
    record_command_events(database, device['id'], 'acknowledged', acks)


def process_record(data, device, ip, database=None):
    """Store one telemetry record.

    Returns the device's current config when the record asked for it
    (``ri=1``), otherwise None.
    """
    database = database or db.web

    if 'gsm_timestamp' not in data:
        raise ValueError('missing field: gsm_timestamp')

    # A copy is answered like any other record — the reply is the receipt
    # that stops the device sending it a third time — and stored once.
    if _is_copy(database, device, data):
        logs.udp.info('duplicate rid=%s from %s dropped', data.get('rec_id'),
                      device.get('imei', '?'))
        return None

    now = datetime.datetime.now()

    # Filed at the time it was built, not on arrival, when the device held it
    # (age=).  History when something built later is already stored.
    age = _record_age(data, device.get('imei', '?'))
    built = now - datetime.timedelta(seconds=age) if age is not None else now

    # The newest state stored before this record: what an ignition change is
    # measured against and sticky fields carry forward from.  History has its
    # own predecessor, the row before it in time.
    previous = _latest_row(database, device['id'])
    history = bool(
        age is not None and previous is not None and previous['timestamp']
        and built < previous['timestamp'] - datetime.timedelta(seconds=1)
    )
    if history:
        stamp, _ = _parse_gsm_timestamp(data['gsm_timestamp'])
        previous = database.one(
            'SELECT * FROM `log` WHERE `device_id` = %s AND `gsm_timestamp` <= %s '
            'ORDER BY `gsm_timestamp` DESC, `id` DESC LIMIT 1',
            (device['id'], stamp.strftime('%Y-%m-%d %H:%M:%S.%f')),
        )

    entry = _build_entry(data, device, ip, previous)
    entry['timestamp'] = built.strftime('%Y-%m-%d %H:%M:%S.%f')

    # A figure that arrived before the record it belongs to: this may be that
    # record, flushed out of the backlog behind the one describing it.
    held_wake = _held_wake_for(device, entry.get('rec_id'))
    if held_wake is not None:
        entry['wake_ms'], entry['attach_ms'] = held_wake

    # powered_on marks the record on which the ignition came up, so the
    # transition is queryable without comparing adjacent rows.  Only a record
    # that is the latest state can switch it on: a late ignition-on record
    # behind an ignition-off one is the drive that already ended, and
    # alerting on it would be a false alarm.
    powered_on = bool(
        not history
        and previous
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
    _note_log_events(database, device, previous, entry, built)

    # History fills in a journey's track (its miles are summed in time
    # order) but opens and closes nothing: a late ignition-on record would
    # reopen the journey that ended with the ignition-off already stored.
    if not history:
        try:
            _update_journey(database, device, entry, log_id, powered_on)
        except Exception:
            logs.app.exception('journey update failed for %s', device.get('imei'))

    # The held figure went onto the row just inserted, so stop holding it.
    if held_wake is not None:
        database.query(
            'UPDATE `device` SET `wake_pending_rec_id` = NULL, '
            '`wake_pending_ms` = NULL, `wake_pending_attach_ms` = NULL '
            'WHERE `id` = %s', (device['id'],)
        )
        device['wake_pending_rec_id'] = None
        device['wake_pending_ms'] = None
        device['wake_pending_attach_ms'] = None

    try:
        _store_backup_changes(database, device, data, built)
    except Exception:
        logs.app.exception('gnss backup update failed for %s', device.get('imei'))

    if not history:
        try:
            _store_applied(database, device, data, entry)
        except Exception:
            logs.app.exception('applied settings update failed for %s',
                               device.get('imei'))

    _store_wake_figure(database, device, data, entry.get('rec_id'))
    _store_wake_signal(database, device, data, entry.get('rec_id'))

    # Ignition off ends track mode; see TRACK_MODE_IDLE_SECONDS.  Not for
    # history: that drive is over, and a newer one may be in the mode.
    if (entry['ignition_state'] == 0 and device.get('track_mode')
            and not history):
        clear_track_mode(device, database, 'ignition off')

    # Mirror config the device reported back onto its row — from a live
    # record only, since a late one can carry a setting since changed.
    # With a reporting schedule the interval the unit reports is whichever one
    # it was last sent, not the plain one the schedule falls back to: not
    # mirrored, or the schedule would overwrite its own base.
    updates = [(key, int(data[key])) for key in DEVICE_SYNC_KEYS
               if key in data and not history
               and not (key == 'int' and device.get('interval_schedule'))]
    if updates:
        assignments = ', '.join('`%s` = %%s' % key for key, _ in updates)
        database.query(
            'UPDATE `device` SET %s WHERE `id` = %%s' % assignments,
            [value for _, value in updates] + [device['id']],
        )

    # And the version it is running, on the same terms: a late record can
    # carry the one before an update.
    if not history:
        try:
            _note_versions(database, device, entry)
        except Exception:
            logs.app.exception('version update failed for %s', device.get('imei'))

    if 'tunables_built' in data and not history:
        try:
            _note_tunables_built(database, device, data['tunables_built'])
        except Exception:
            logs.app.exception('tunables_built update failed for %s',
                               device.get('imei'))

    if 'request_int' in data:
        return device_config(device)

    return None


def _note_tunables_built(database, device, raw):
    """Keep the built tunable values the unit reported (tb=), when they parse
    and differ from what the row holds."""
    text = tunables_built_text(parse_tunables_built(raw))
    if text is None or text == device.get('tunables_built'):
        return
    database.query('UPDATE `device` SET `tunables_built` = %s WHERE `id` = %s',
                   (text, device['id']))
    device['tunables_built'] = text


def clear_track_mode(device, database, why):
    database.query('UPDATE `device` SET `track_mode` = 0 WHERE `id` = %s',
                   (device['id'],))
    if device.get('track_mode'):
        device_events.record(database, device['id'], 'setting',
                             device_events.track_mode_text(False, why))
    device['track_mode'] = 0
    logs.app.info('track mode off for %s: %s', device.get('imei'), why)


def expire_track_mode(device, database):
    """Clear a switch left on for a device that has gone quiet.

    The ignition-off record is the normal end; this catches a device that
    lost power or coverage before sending one, so the switch does not wait
    for the next drive to be found still on."""
    if not device.get('track_mode'):
        return
    row = _latest_row(database, device['id'])
    stamp = row.get('timestamp') if row else None
    if isinstance(stamp, str):
        try:
            stamp = datetime.datetime.strptime(stamp, '%Y-%m-%d %H:%M:%S.%f')
        except ValueError:
            stamp = datetime.datetime.strptime(stamp, '%Y-%m-%d %H:%M:%S')
    if stamp is None:
        return
    idle = (datetime.datetime.now() - stamp).total_seconds()
    if idle > TRACK_MODE_IDLE_SECONDS:
        clear_track_mode(device, database, 'idle %d s' % int(idle))


def device_config(device):
    """The settings the device syncs, with their defaults."""
    movement_alarm = device.get('movement_alarm')
    return {
        'int': device.get('int') or 0,
        'ma': 1 if movement_alarm is None else movement_alarm,
        'backup': device.get('backup_int') or 0,
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
    _note_faults(database, device, raised, [row['code'] for row in cleared])

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

# The device holds an alert until this server's reply to the datagram that
# carried it comes back, and sends it again if it never does, so an alert
# whose reply was lost arrives twice.  Each carries an id, ``,aid=<n>`` at the
# end of the line (consecutive within a boot from a random start), and the
# copy is dropped here rather than notified a second time.
#
# Remembered in memory: the listeners are threads of one process, and a copy
# follows its original by minutes (the next send) or at most a day or two
# (the next wake that gets through).  The message is part of the key, so two
# boots that happen to reuse an id cannot swallow a real alert.  A copy that
# arrives across a restart of this server is notified again.
_ALERT_SEEN_SECONDS = 7 * 86400
_ALERT_SEEN_MAX = 256                   # per device
_ALERT_ID_RE = re.compile(r',aid=(\d+)$')
_alert_seen = {}                        # device id -> {(aid, message): seen}
_alert_seen_lock = threading.Lock()


def _alert_copy(device_id, aid, message, now=None):
    """True if this alert was relayed already; otherwise remember it."""
    now = time.time() if now is None else now
    key = (aid, message)
    with _alert_seen_lock:
        seen = _alert_seen.setdefault(device_id, {})
        for old in [k for k, t in seen.items()
                    if now - t > _ALERT_SEEN_SECONDS]:
            del seen[old]
        if key in seen:
            return True
        if len(seen) >= _ALERT_SEEN_MAX:
            del seen[min(seen, key=seen.get)]
        seen[key] = now
        return False


def _handle_alert(line, device, database, log):
    """Relay a device-originated alert line
    (``A,<priority>,<message>[,aid=<n>]``)."""
    body = line[2:]
    aid = None
    m = _ALERT_ID_RE.search(body)
    if m:
        aid = int(m.group(1))
        body = body[:m.start()]
    parts = body.split(',', 1)
    if len(parts) == 2 and parts[0].lstrip('-').isdigit():
        priority, message = int(parts[0]), parts[1]
    else:
        priority, message = 0, body

    if aid is not None and _alert_copy(device['id'], aid, message):
        log.info('duplicate alert aid=%d from %s dropped', aid,
                 device['imei'])
        return

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
        latest = _latest_row(database, device['id'])
        if latest and str(latest['ignition_state']) == '1':
            suppress = True
        else:
            priority = 0

    log.info('alert from %s (pri=%d): %s%s', device['imei'], priority, message,
             ' [suppressed: engine running]' if suppress else '')
    if not suppress:
        _note_alert(database, device, message)
        notify.device_alert(device['name'], message, priority)


_REPORT_TITLES = {'selftest': 'self test', 'status': 'status',
                  'gnssstat': 'GNSS status', 'backlog': 'backlog'}


def _scan_summary(cells):
    """The strongest three cells of an F,scan, list as 'plmn rsrp dBm' lines."""
    out = []
    for cell in cells.split('|')[:3]:
        f = cell.split(':')
        if len(f) >= 5:
            out.append('%s  %s dBm  (tac %s, earfcn %s)' % (f[0], f[4], f[1], f[3]))
    return '\n'.join(out) or 'no cells found'


def _notify_report(device, title, body):
    """Tell the user a command's answer has arrived (the app has no push)."""
    name = (device.get('name') or device.get('imei') or '?').strip()
    notify.send(body[:1000], title='%s: %s' % (name, title))


# -- response ----------------------------------------------------------------

def build_response(device, database, log, firmware_version=None):
    """Build the reply the device reads after a successful send.

    Format: ``1,<interval>[,<movement_alarm>][,<commands>][,backup=<n>][,fota=<v>][,track=<0|1>]``.

    The leading ``1`` is the ack the firmware checks before clearing its send
    buffer.  Commands are appended as a comma-separated list and deleted as
    they are handed over — the device has no way to acknowledge them
    separately, so delivery is at-most-once by design: a command lost to a
    dropped reply is re-queued by whoever issued it, which is safer than
    replaying a `poweroff` the operator has since changed their mind about.
    """
    settings = device_config(device)
    # The engine-off interval for this moment: the plain one, or what the
    # device's reporting schedule says for the time of day (see
    # interval_schedule.py).  It is what the unit is told and so what it runs.
    settings['int'] = interval_schedule.interval_now(device)
    response = '1,%s,%s' % (settings['int'], settings['ma'])

    expired = _expiring_commands(database, device['id'])
    database.query(
        'DELETE FROM `command` WHERE `device_id` = %s '
        'AND `timestamp` < NOW(6) - INTERVAL %s HOUR',
        (device['id'], COMMAND_MAX_AGE_HOURS),
    )
    _note_expired(database, device['id'], expired)
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
        record_command_events(database, device['id'], 'collected', [command_string])

    # The interval for a tracker running on the backup module rides the
    # command field too, on every response, so a device that rebooted or
    # missed a reply converges on it.  The firmware applies it as it reads
    # the reply and takes it out of the string, so it is never run as a
    # command; firmware from before it passes over it.  Ahead of the advert
    # and the track switch so that those two stay last.
    response += ',backup=%d' % settings['backup']

    # The OTA indication rides the command field.
    if firmware_version:
        response += ',fota=%s' % firmware_version

    # Track mode rides the same way, on every response, so a device that
    # rebooted or missed a reply converges on the setting.  The firmware
    # acts only on a change, so the repeat costs nothing but the bytes.
    response += ',track=%d' % (1 if device.get('track_mode') else 0)

    # The GNSS backup supply state, once known (the unit reported it or the
    # API set it), on every reply like track=.  The unit follows it; a change
    # after the first one it sees is announced with an alert and an ack.
    if device.get('gnss_backup') in (0, 1):
        response += ',gb=%d' % device['gnss_backup']

    # The GNSS backup off window, on every reply after gb=: the unit works in
    # UTC, the setting is in the server's local hours, so it is converted
    # with the offset in force now (BST/GMT) as "<from>-<to>", or "off".
    response += ',gw=%s' % gnss_off_window_token(device)

    # Whether the unit's status LEDs may light, on every reply after gw=:
    # 1 unless the column says 0 (so a row without it reads as on).
    response += ',le=%d' % (0 if device.get('leds') == 0 else 1)

    # The runtime tunables, on every reply after le=: the non-NULL columns as
    # tu=<code>:<value>[;...], or tu=- when none is set.  The unit makes its
    # overrides exactly this set, so a column set back to NULL clears it.
    response += ',tu=%s' % tunables_token(device)

    # The unit that is answered applies what it is told, so the settings in
    # this reply are what it runs from now: filed as applied, ahead of its
    # own ap= (which describes it before it read this reply, and so reads
    # stale after a boot).  Only the settings it adopts as sent; a reply lost
    # on the way is put right by the unit's next ap=.
    try:
        database.query(
            'UPDATE `device` SET `ap_int` = %s, `ap_ma` = %s, `ap_backup` = %s, '
            '`ap_track` = %s, `ap_at` = NOW() WHERE `id` = %s',
            (settings['int'], settings['ma'], settings['backup'],
             1 if device.get('track_mode') else 0, device['id']),
        )
    except Exception:
        logs.app.exception('applied settings update failed for %s', device.get('imei'))

    return response


_UPTIME_RE = re.compile(r'(?:^|[,;])up=(\d+)(?:[,;]|$)')


def boot_wall_time(lines, now=None, device=None, database=None):
    """Estimate when the device booted, as a datetime, or None.

    Firmware log lines are stamped with uptime, which only means something
    against a boot time.  The telemetry records in the same batch carry up=
    (seconds since boot as of when each was built), so now minus the newest
    of them is the boot time to within a few seconds.  It has to be the
    newest: a batch that waited out a network outage starts with a record
    built long before the send, and taking that one stamps every log line
    in the batch late by the length of the outage.  With no record in the
    batch, fall back to the last stored row's uptime and receipt time; that
    is wrong if the device rebooted since, but the line's own uptime is
    printed too, so the reader can tell.
    """
    now = now or datetime.datetime.now()
    newest = None
    for line in lines:
        if line[:2] in ('A,', 'D,', 'L,'):
            continue
        m = _UPTIME_RE.search(line)
        if m and (newest is None or int(m.group(1)) > newest):
            newest = int(m.group(1))
    if newest is not None:
        return now - datetime.timedelta(seconds=newest)

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

    # A device heard from after a long silence with the switch still on.
    # Checked before the batch is stored, since the batch is what ends the
    # silence.
    try:
        expire_track_mode(device, database)
    except Exception:
        log.exception('track mode expiry failed for %s', device.get('imei'))

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
            elif line.startswith('F,fota,'):
                # Update lifecycle, reported by the device:
                #   F,fota,staged,<new>,<old>         rebooting into <new>
                #   F,fota,failed,<staged>,<running>  MCUboot reverted it
                parts = line.split(',')
                if len(parts) >= 4 and parts[2] == 'staged':
                    firmware.note_staged(device, parts[3], database, log)
                elif len(parts) >= 5 and parts[2] == 'failed':
                    firmware.note_failed(device, parts[3], parts[4],
                                         database, log)
                else:
                    log.warning('unparsed fota line from %s: %r',
                                device['imei'], line)
            elif line.startswith('F,rep,'):
                # Answer to a reporting command (selftest, status, gnssstat,
                # backlog, logdump):  F,rep,<name>,<text>
                parts = line.split(',', 3)
                if len(parts) == 4 and parts[2].isalnum():
                    database.query(
                        "INSERT INTO `device_report` (`device_id`, `name`, "
                        "`text`, `at`) VALUES (%s, %s, %s, NOW()) "
                        "ON DUPLICATE KEY UPDATE `text` = VALUES(`text`), "
                        "`at` = VALUES(`at`)",
                        (device['id'], parts[2][:16], parts[3][:600]))
                    device_events.record(database, device['id'], 'result',
                                         device_events.result_text(parts[2][:16]))
                    logs.device.info('%s report %s %s', device['imei'],
                                     parts[2], parts[3])
                    if parts[2] in _REPORT_TITLES:
                        _notify_report(device, _REPORT_TITLES[parts[2]],
                                       parts[3])
            elif line.startswith('F,scan,'):
                # Network scan (netscan command):
                #   F,scan,<n>,<mcc>-<mnc>:<tac>:<cell>:<earfcn>:<rsrp>:<rsrq10>|...
                parts = line.split(',', 3)
                cells = parts[3] if len(parts) == 4 else ''
                database.query(
                    "UPDATE `device` SET `net_scan` = %s, `net_scan_at` = NOW() "
                    "WHERE `id` = %s", (cells[:600], device['id']))
                device_events.record(database, device['id'], 'result',
                                     device_events.result_text('scan'))
                logs.device.info('%s netscan %s', device['imei'], line[7:])
                _notify_report(device, 'network scan', _scan_summary(cells))
            elif line.startswith('L,'):
                # A warning or error the firmware logged since its last
                # successful send.  File it; nothing to store or act on.
                if boot_wall is None:
                    boot_wall = boot_wall_time(lines, device=device,
                                               database=database)
                logs.device.info('%s', format_device_log(
                    line, device['imei'], boot_wall))
            else:
                parsed = parse_csv_line(line)
                process_record(parsed, device, ip, database=database)
                # fw= rides the first record after every restart.  Against
                # what we staged it says whether the update took, and it
                # works for a device whose reverted-to image is too old to
                # report the failure itself.
                if parsed.get('fw'):
                    firmware.check_running(device, parsed['fw'], database, log)
            processed += 1
        except ValueError as e:
            log.error('record error IMEI=%s: %s', device['imei'], e)
        except Exception:
            log.exception('unexpected error IMEI=%s', device['imei'])

    log.info('%d records from %s (%s)', processed, device['imei'], ip)

    # Re-read: the batch may have changed the device's own settings.
    device = database.one('SELECT * FROM `device` WHERE `id` = %s', (device['id'],))

    # An advertised version this device has already failed to boot is
    # withheld: dropping the advert is what stops it asking, since the
    # device only fetches a manifest when the advertised version is newer
    # than its own.  A version newer than the blocked one is offered
    # normally — that is the fix arriving.
    available = firmware.latest_version(device['imei'])
    if available and available == device.get('fw_blocked'):
        available = None

    return build_response(device, database, log, firmware_version=available)
