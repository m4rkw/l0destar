"""Dead reckoning: the path a device drove between two of its GNSS fixes.

GNSS gets nothing while LTE holds the radio, and every send holds it: about
2.5 s after the reply on most cells the car sees, 10 s on others, 25 s or
more through a rejected tracking-area update.  So a track is runs of
one-second fixes with gaps between them, which a map draws as straight lines
across whatever the vehicle actually drove.

The device measures what it cannot see.  The record that ends a gap carries
``mv=<gap>:<v0>:<steps>:<end>`` (firmware src/motion.c): the ECU's road speed
and the gyro's heading change for every second of the gap.  That is the shape
of the path, everything but its rotation and scale, which the fixes at either
end supply: the shape is integrated from the first fix, then turned and
stretched about it until it ends on the second.

The field
---------
``gap``    tenths of a second between the fixes, by the device's clock
``v0``     km/h at the first fix, or empty when the device has no ECU speed
``steps``  one entry per whole second after the first fix and before the
           second: a speed change then a heading change when ``v0`` is given,
           the heading change alone when it is not
``end``    the heading change from the last whole second to the second fix

Each change is one base64url character, the value plus 32 ("g" is 0); -32
("A") in a speed slot means not measured.  Speeds are whole km/h, headings
whole degrees clockwise, and each step is the change in the rounded running
value.

Nothing here is stored.  The fill is worked out when a journey is drawn, so a
better fit applies to every journey already recorded, and a fill that fails
its checks costs nothing but the straight line that was there before.
"""

import calendar
import datetime
import math
import re

ALPHABET = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_'
_VALUE = {c: i - 32 for i, c in enumerate(ALPHABET)}
UNMEASURED = 'A'

MOTION_RE = re.compile(r'^(\d{1,4}):(\d{0,3}):([A-Za-z0-9_-]*):([A-Za-z0-9_-])$')
MOTION_MAX = 255

KMH_PER_MPH = 1.609344
EARTH_RADIUS_M = 6371008.8

# A fix's GNSS heading only means something on the move; below this it is
# the receiver's noise.
HEADING_MIN_KMH = 10.0
HEADING_MIN_SATS = 5
# The gyro's heading change across a gap against the fixes' own.  A
# difference up to this is drift, spread over the gap.  Beyond it one of the
# two is wrong, and it is usually the fixes': a receiver's heading lags a
# tight turn at low speed, 40 degrees behind the car after the left turn home
# on 2026-10-04 (rid 41405), while the gyro is current.  So with two good
# fixes the headings are set aside and the positions fit the path alone.
HEADING_DRIFT_MAX = 45.0
# A fix this poor (HDOP) can be the wrong one instead: rid 41403, HDOP 4.7,
# was plotted 15 m past the corner the gyro had already turned.  When the
# headings disagree and either end is this poor, nothing is trusted.
HDOP_TRUST = 3.0
# The path is turned and stretched about the first fix to end on the second.
# The stretch is the speed's error: an ECU's is a few percent, speeds
# interpolated between the fixes much worse.
SCALE_ECU = (0.7, 1.4)
SCALE_FIXES = (0.5, 2.0)
# With a heading at the first fix the turn is that heading's error, and
# beyond this the path is not the vehicle's.  With the headings set aside the
# fitted path still has to set off along the first fix's heading or arrive
# along the second's, within this: one of them lagging is the reason they
# were set aside, both wrong is a path that is not the vehicle's.
TURN_MAX = 45.0
# Fixes this close together say nothing about direction: stood still.
MIN_MOVE_M = 5.0
# A turn and a stretch need some distance to be measured over.  Below it the
# path is only shifted onto the second fix, and only if it nearly ends there.
MIN_FIT_M = 20.0
SHIFT_MAX_M = 15.0
# The device's clock and the GNSS one have to agree about the gap, or the
# field belongs to some other pair of fixes.
GAP_TOLERANCE_S = 1.5


def valid(raw):
    """Whether ``raw`` is shaped like an mv= field (before it is stored)."""
    return bool(raw) and len(raw) <= MOTION_MAX and MOTION_RE.match(raw) is not None


def parse(raw):
    """The field as ``{'gap', 'times', 'speeds', 'headings'}``, or None.

    ``times`` are seconds after the first fix: 0, each whole second, then the
    gap itself.  ``speeds`` (km/h, None where not measured) has an entry for
    every time but the last, or is None without an ECU; ``headings`` (degrees
    since the first fix) has one for every time.
    """
    if not valid(raw):
        return None
    match = MOTION_RE.match(raw)
    gap = int(match.group(1)) / 10.0
    has_speed = match.group(2) != ''
    steps = match.group(3)
    per = 2 if has_speed else 1
    if len(steps) % per:
        return None
    n = len(steps) // per
    # n whole seconds lie strictly inside a gap of more than n and at most
    # n + 1 seconds; the device rounds the gap to a tenth.
    if n < 1 or not n - 0.05 <= gap <= n + 1.05:
        return None

    speeds = None
    if has_speed:
        speed = int(match.group(2))
        speeds = [float(speed)]
    heading = 0
    headings = [0.0]
    for k in range(n):
        if has_speed:
            c = steps[2 * k]
            if c == UNMEASURED:
                speeds.append(None)
            else:
                speed += _VALUE[c]
                speeds.append(float(speed))
        heading += _VALUE[steps[per * k + per - 1]]
        headings.append(float(heading))
    headings.append(float(heading + _VALUE[match.group(4)]))

    return {
        'gap': gap,
        'times': [float(k) for k in range(n + 1)] + [gap],
        'speeds': speeds,
        'headings': headings,
    }


def fill(a, b, raw):
    """The points between fixes ``a`` and ``b`` that the field describes.

    ``a`` and ``b`` are ``{'lat', 'lon', 't', 'speed', 'heading'}``, with an
    optional ``'hdop'``: degrees, seconds on any one clock, km/h, a heading
    in degrees or None where it cannot be trusted, and the HDOP itself (not
    in tenths; None or absent when not known, which counts as poor).
    Returns ``{'lat', 'lon', 't', 'speed', 'heading'}`` for every whole
    second of the gap, ``t`` seconds after ``a``; an empty list when the
    field is not for these two fixes or fails a check.
    """
    m = parse(raw)
    if m is None:
        return []
    gap = m['gap']
    span = b['t'] - a['t']
    if span <= 0 or abs(span - gap) > max(GAP_TOLERANCE_S, 0.1 * gap):
        return []

    times = m['times']
    n = len(times) - 2
    ecu = m['speeds'] is not None
    speeds = (list(m['speeds']) if ecu else [None] * (n + 1)) + [b['speed']]
    if speeds[0] is None:
        speeds[0] = a['speed']
    speeds = _bridge(times, speeds)
    if speeds is None:
        return []

    headings = m['headings']
    # The fixes' headings, for checking the fit against: set aside below when
    # they disagree with the gyro but the fixes themselves are good.
    head_a, head_b = a['heading'], b['heading']
    set_aside = None
    if head_a is not None and head_b is not None:
        # What the gyro turned against what the fixes say was turned: the
        # difference is the gyro's drift, taken out evenly over the gap.
        drift = _wrap(head_b - head_a - headings[-1])
        if abs(drift) <= HEADING_DRIFT_MAX:
            headings = [h + drift * t / gap for h, t in zip(headings, times)]
        elif max(_hdop(a), _hdop(b)) > HDOP_TRUST:
            return []
        else:
            set_aside = (head_a, head_b)
            head_a = head_b = None

    # The shape, setting off along bearing 0.
    xs, ys = [0.0], [0.0]
    for i in range(1, len(times)):
        dist = (speeds[i - 1] + speeds[i]) / 2 / 3.6 * (times[i] - times[i - 1])
        bearing = math.radians((headings[i - 1] + headings[i]) / 2)
        xs.append(xs[-1] + dist * math.sin(bearing))
        ys.append(ys[-1] + dist * math.cos(bearing))

    bx, by = _local(a, b['lat'], b['lon'])
    e_len = math.hypot(xs[-1], ys[-1])
    b_len = math.hypot(bx, by)
    if e_len < MIN_MOVE_M and b_len < MIN_MOVE_M:
        return []

    if e_len >= MIN_FIT_M and b_len >= MIN_FIT_M:
        turn = _wrap(math.degrees(math.atan2(bx, by) - math.atan2(xs[-1], ys[-1])))
        stretch = b_len / e_len
        low, high = SCALE_ECU if ecu else SCALE_FIXES
        if not low <= stretch <= high:
            return []
        if head_a is not None and abs(_wrap(turn - head_a)) > TURN_MAX:
            return []
        if set_aside is not None:
            # Set aside for disagreeing: the fitted path still has to leave
            # along one fix's heading or arrive along the other's.
            leave, arrive = set_aside
            if (abs(_wrap(turn - leave)) > TURN_MAX
                    and abs(_wrap(turn + headings[-1] - arrive)) > TURN_MAX):
                return []
        path = [_turn(x, y, turn, stretch) for x, y in zip(xs, ys)]
        offset = turn
    else:
        if head_a is None:
            return []
        offset = head_a
        path = [_turn(x, y, offset, 1.0) for x, y in zip(xs, ys)]
        shift_x, shift_y = bx - path[-1][0], by - path[-1][1]
        if math.hypot(shift_x, shift_y) > SHIFT_MAX_M:
            return []
        path = [(x + shift_x * t / gap, y + shift_y * t / gap)
                for (x, y), t in zip(path, times)]

    clock = span / gap
    points = []
    for i in range(1, n + 1):
        lat, lon = _global(a, *path[i])
        points.append({
            'lat': lat,
            'lon': lon,
            't': times[i] * clock,
            'speed': speeds[i],
            'heading': round((offset + headings[i]) % 360.0, 2),
        })
    return points


def journey_fill(rows):
    """Fills for a journey's rows, in time order: ``{index: points}``.

    The points go between row ``index`` and the next.  A row carries the
    motion through the gap before it, so the next row has to be the record
    built straight after this one (its ``rec_id`` one more) and both have to
    be live fixes.  Rows are ``log`` rows: ``rec_id``, ``gsm_timestamp`` (a
    naive UTC datetime), ``latitude``, ``longitude``, ``speed`` and
    ``obd_speed`` (mph), ``heading``, ``satellites``, ``hdop`` (the HDOP
    itself, not tenths), ``cell_location`` and ``motion``.
    """
    fills = {}
    for i in range(len(rows) - 1):
        a, b = rows[i], rows[i + 1]
        if not b.get('motion'):
            continue
        try:
            consecutive = int(b['rec_id']) == int(a['rec_id']) + 1
        except (KeyError, TypeError, ValueError):
            continue
        fix_a, fix_b = _fix(a), _fix(b)
        if not consecutive or fix_a is None or fix_b is None:
            continue
        points = fill(fix_a, fix_b, b['motion'])
        if points:
            fills[i] = points
    return fills


def point(base, p, stamp):
    """Fill point ``p`` in the shape the map takes, made from ``base``: the
    fix before the gap as the map got it.  Position, speed, heading and time
    are the fill's, ``dr`` marks it, and the rest — ECU and IMU readings,
    battery, cell — stays as the fix had it, which is what the map does with
    a reading it does not get.  ``stamp`` is the fix's own ``timestamp``."""
    mph = round(p['speed'] / KMH_PER_MPH, 2)
    out = dict(base)
    out.update({
        'latitude': round(p['lat'], 6),
        'longitude': round(p['lon'], 6),
        'speed': mph,
        'combined_speed': mph,
        'heading': p['heading'],
        'timestamp': ((stamp + datetime.timedelta(seconds=p['t']))
                      .strftime('%d.%m.%Y %H:%M:%S') if stamp else ''),
        'dr': 1,
    })
    if out.get('gsm_ts') is not None:
        out['gsm_ts'] = out['gsm_ts'] + p['t']
    # The IMU readings were the fix's instant, not this one; the engine and
    # battery readings stand for a few seconds either side well enough.
    for key in list(out):
        if key == 'imu' or key == 'imu_temp' or key.startswith(('accel_', 'gyro_')):
            out[key] = None
    return out


def _fix(row):
    """A row as fill() takes a fix, or None for one that is not a live fix."""
    try:
        if int(row.get('cell_location') or 0) == 1 or int(row['satellites'] or 0) <= 0:
            return None
        lat, lon = float(row['latitude']), float(row['longitude'])
        stamp = row['gsm_timestamp']
        t = calendar.timegm(stamp.timetuple()) + stamp.microsecond / 1e6
        gnss_kmh = float(row['speed'] or 0) * KMH_PER_MPH
        obd = row.get('obd_speed')
        speed = float(obd) * KMH_PER_MPH if obd is not None else gnss_kmh
        heading = None
        if (gnss_kmh >= HEADING_MIN_KMH and int(row['satellites']) >= HEADING_MIN_SATS
                and row.get('heading') is not None):
            heading = float(row['heading'])
        hdop = float(row['hdop']) if row.get('hdop') is not None else None
    except (KeyError, TypeError, ValueError, AttributeError):
        return None
    if lat == 0.0 and lon == 0.0:
        return None
    return {'lat': lat, 'lon': lon, 't': t, 'speed': speed, 'heading': heading,
            'hdop': hdop}


def _hdop(fix):
    """A fix's HDOP, or infinity when it is not known: not known is poor."""
    hdop = fix.get('hdop')
    return float('inf') if hdop is None else hdop


def _bridge(times, values):
    """Unmeasured values interpolated in time between the measured ones."""
    known = [i for i, v in enumerate(values) if v is not None]
    if not known:
        return None
    out = list(values)
    for i in range(len(out)):
        if out[i] is not None:
            continue
        before = max((k for k in known if k < i), default=None)
        after = min((k for k in known if k > i), default=None)
        if before is None:
            out[i] = values[after]
        elif after is None:
            out[i] = values[before]
        else:
            f = (times[i] - times[before]) / (times[after] - times[before])
            out[i] = values[before] + (values[after] - values[before]) * f
    return out


def _wrap(degrees):
    return (degrees + 180.0) % 360.0 - 180.0


def _turn(x, y, degrees, stretch):
    """(x, y) turned clockwise by ``degrees`` about the origin and stretched."""
    r = math.radians(degrees)
    return (stretch * (x * math.cos(r) + y * math.sin(r)),
            stretch * (y * math.cos(r) - x * math.sin(r)))


def _local(origin, lat, lon):
    """Metres east and north of ``origin``."""
    return (math.radians(lon - origin['lon']) * EARTH_RADIUS_M
            * math.cos(math.radians(origin['lat'])),
            math.radians(lat - origin['lat']) * EARTH_RADIUS_M)


def _global(origin, x, y):
    lat = origin['lat'] + math.degrees(y / EARTH_RADIUS_M)
    lon = origin['lon'] + math.degrees(
        x / (EARTH_RADIUS_M * math.cos(math.radians(origin['lat']))))
    return lat, lon
