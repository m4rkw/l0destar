"""Dead reckoning: the mv= field, and the path fitted through a gap with it.

The encoder below does what the firmware does (src/motion.c): whole-second
samples of speed and heading change, each the change in the rounded running
value, clamped to +/-31 with the rest carried into the next.
"""

import datetime
import math

import pytest

from tracker import deadreckon as dr
from tracker import telemetry

LAT0, LON0 = 51.39, -0.21


def encode(gap_s, speeds, headings, ecu=True):
    """An mv= value.  ``speeds`` and ``headings`` (km/h, degrees since the
    first fix) are given at 0, each whole second, and the second fix."""
    n = len(speeds) - 2
    out = []
    ev, eh = round(speeds[0]), 0
    for k in range(1, n + 1):
        if ecu:
            if speeds[k] is None:
                out.append(dr.UNMEASURED)
            else:
                d = max(-31, min(31, round(speeds[k]) - ev))
                ev += d
                out.append(dr.ALPHABET[d + 32])
        d = max(-31, min(31, round(headings[k]) - eh))
        eh += d
        out.append(dr.ALPHABET[d + 32])
    end = max(-31, min(31, round(headings[-1]) - eh))
    return '%d:%s:%s:%s' % (round(gap_s * 10), round(speeds[0]) if ecu else '',
                            ''.join(out), dr.ALPHABET[end + 32])


def drive(gap_s, speed, yaw, heading0=37.0, dt=0.001):
    """Ground truth for a drive through a gap: speed(t) km/h and yaw(t)
    deg/s clockwise.  Returns the truth at 0, each whole second and the gap,
    as (x, y, heading, speed) in metres east and north."""
    x = y = 0.0
    h = heading0
    times = list(range(0, math.ceil(gap_s))) + [gap_s]
    out = []
    t = 0.0
    for target in times:
        while t < target - 1e-9:
            h += yaw(t) * dt
            v = speed(t) / 3.6
            x += v * math.sin(math.radians(h)) * dt
            y += v * math.cos(math.radians(h)) * dt
            t += dt
        out.append((x, y, h, speed(target)))
    return times, out


def latlon(x, y):
    r = dr.EARTH_RADIUS_M
    return (LAT0 + math.degrees(y / r),
            LON0 + math.degrees(x / (r * math.cos(math.radians(LAT0)))))


def metres(lat, lon):
    r = dr.EARTH_RADIUS_M
    return (math.radians(lon - LON0) * r * math.cos(math.radians(LAT0)),
            math.radians(lat - LAT0) * r)


def fixes(truth, gap_s, t0=1000.0, hdop=None):
    (ax, ay, ah, av), (bx, by, bh, bv) = truth[0], truth[-1]
    la, lb = latlon(ax, ay), latlon(bx, by)
    return ({'lat': la[0], 'lon': la[1], 't': t0, 'speed': av, 'heading': ah % 360,
             'hdop': hdop},
            {'lat': lb[0], 'lon': lb[1], 't': t0 + gap_s, 'speed': bv, 'heading': bh % 360,
             'hdop': hdop})


def worst_error(points, truth):
    worst = 0.0
    for p, (x, y, _, _) in zip(points, truth[1:-1]):
        px, py = metres(p['lat'], p['lon'])
        worst = max(worst, math.hypot(px - x, py - y))
    return worst


def right_turn(t):
    return 18.0 if 4 < t < 9 else 0.0


# -- the field -----------------------------------------------------------------

def test_parse_reads_speeds_and_headings():
    m = dr.parse('34:30:gggmgy:t')
    assert m['gap'] == pytest.approx(3.4)
    assert m['times'] == [0.0, 1.0, 2.0, 3.0, 3.4]
    assert m['speeds'] == [30.0, 30.0, 30.0, 30.0]
    assert m['headings'] == [0.0, 0.0, 6.0, 24.0, 37.0]


def test_steps_have_to_fit_the_gap():
    # 12.4 s holds 12 whole seconds; three steps are some other gap's.
    assert dr.parse('124:30:gggmgy:t') is None


def test_parse_without_ecu_speeds():
    m = dr.parse('34::gmy:t')
    assert m['speeds'] is None
    assert m['headings'] == [0.0, 0.0, 6.0, 24.0, 37.0]


def test_unmeasured_speed():
    m = dr.parse('34:30:gaAgmg:g')
    assert m['speeds'] == [30.0, 30.0, None, 36.0]


@pytest.mark.parametrize('raw', [
    '', '34:30:ggg:t', '34:30:gg$g:t', '34:30:gggggg', 'x:30:gggggg:g',
    '34:1000:gggggg:g', '34:30:gggggg:gg', '5:30::g', '3' * 300,
])
def test_malformed_fields_are_refused(raw):
    assert dr.parse(raw) is None


def test_encoder_matches_firmware_alphabet():
    # 0 is "g", -32 (unmeasured) is "A", 31 is "_": the firmware's table.
    assert dr.ALPHABET[32] == 'g'
    assert dr.ALPHABET[0] == dr.UNMEASURED == 'A'
    assert dr.ALPHABET[63] == '_'


# -- the fill ------------------------------------------------------------------

def test_right_turn_is_followed_not_cut():
    gap = 12.4
    times, truth = drive(gap, lambda t: 30.0, right_turn)
    a, b = fixes(truth, gap)
    raw = encode(gap, [s for _, _, _, s in truth[:-1]] + [truth[-1][3]],
                 [h - truth[0][2] for _, _, h, _ in truth])
    points = dr.fill(a, b, raw)
    assert len(points) == 12
    assert worst_error(points, truth) < 2.0
    # The straight line it replaces misses the corner by tens of metres.
    chord = max(math.hypot(x - truth[0][0] - (truth[-1][0] - truth[0][0]) * t / gap,
                           y - truth[0][1] - (truth[-1][1] - truth[0][1]) * t / gap)
                for t, (x, y, _, _) in zip(times, truth))
    assert chord > 20


def test_headings_alone_with_speeds_from_the_fixes():
    gap = 13.5
    _, truth = drive(gap, lambda t: 25.0,
                     lambda t: 30.0 if 1 < t < 2.5 else (-32.0 if 2.5 < t < 10 else 0.0))
    a, b = fixes(truth, gap)
    raw = encode(gap, [25.0] * (len(truth) - 1) + [25.0],
                 [h - truth[0][2] for _, _, h, _ in truth], ecu=False)
    points = dr.fill(a, b, raw)
    assert points and worst_error(points, truth) < 6.0


def test_gyro_drift_is_taken_out():
    # The gyro reads 0.6 deg/s more clockwise than the truth; the fixes'
    # headings show it, and it is spread back out over the gap.
    gap = 12.4
    _, truth = drive(gap, lambda t: 30.0, right_turn)
    a, b = fixes(truth, gap)
    drifted = [h - truth[0][2] + 0.6 * t for (_, _, h, _), t in
               zip(truth, list(range(13)) + [gap])]
    raw = encode(gap, [30.0] * 13 + [30.0], drifted)
    points = dr.fill(a, b, raw)
    assert points and worst_error(points, truth) < 2.0


def test_heading_disagreement_is_not_trusted():
    gap = 12.4
    _, truth = drive(gap, lambda t: 30.0, right_turn)
    a, b = fixes(truth, gap)        # fixes of unknown quality
    # A turn the other way: 180 degrees off what the fixes say.
    raw = encode(gap, [30.0] * 13 + [30.0],
                 [-(h - truth[0][2]) for _, _, h, _ in truth])
    assert dr.fill(a, b, raw) == []


def test_mirrored_turn_is_not_trusted_between_good_fixes_either():
    # Setting the headings aside does not let a path through that leaves
    # along neither fix's heading nor arrives along the other's.
    gap = 12.4
    _, truth = drive(gap, lambda t: 30.0, right_turn)
    a, b = fixes(truth, gap, hdop=1.0)
    raw = encode(gap, [30.0] * 13 + [30.0],
                 [-(h - truth[0][2]) for _, _, h, _ in truth])
    assert dr.fill(a, b, raw) == []


def test_lagging_heading_is_set_aside_between_good_fixes():
    # After a tight turn a receiver's heading trails the car: 2026-10-04,
    # rid 41405 said 267 with the car already going about 220.  The gyro
    # says straight on, the headings say a 47 degree turn, and both fixes
    # are good, so the headings are set aside and the positions fit it.
    gap = 4.1
    _, truth = drive(gap, lambda t: 20.0, lambda t: 0.0, heading0=220.0)
    a, b = fixes(truth, gap, hdop=1.0)
    a['heading'] = 267.0
    raw = encode(gap, [20.0] * len(truth), [0.0] * len(truth))
    points = dr.fill(a, b, raw)
    assert len(points) == 4 and worst_error(points, truth) < 1.0
    assert points[0]['heading'] == pytest.approx(220.0, abs=1.0)


def test_disagreement_with_a_poor_fix_is_not_trusted():
    # The same disagreement with the second fix poor (rid 41403, HDOP 4.7,
    # plotted past the corner): the fix itself may be what is wrong.
    gap = 4.1
    _, truth = drive(gap, lambda t: 20.0, lambda t: 0.0, heading0=220.0)
    a, b = fixes(truth, gap, hdop=1.0)
    a['heading'] = 267.0
    b['hdop'] = 4.7
    raw = encode(gap, [20.0] * len(truth), [0.0] * len(truth))
    assert dr.fill(a, b, raw) == []


def test_speed_that_cannot_reach_the_second_fix_is_not_trusted():
    gap = 12.4
    _, truth = drive(gap, lambda t: 30.0, right_turn)
    a, b = fixes(truth, gap)
    raw = encode(gap, [60.0] * 13 + [30.0], [h - truth[0][2] for _, _, h, _ in truth])
    assert dr.fill(a, b, raw) == []


def test_field_for_another_gap_is_ignored():
    gap = 12.4
    _, truth = drive(gap, lambda t: 30.0, right_turn)
    a, b = fixes(truth, gap)
    b['t'] = a['t'] + 20.0          # the fixes are 20 s apart, the field says 12.4
    raw = encode(gap, [30.0] * 13 + [30.0], [h - truth[0][2] for _, _, h, _ in truth])
    assert dr.fill(a, b, raw) == []


def test_standing_still_needs_no_fill():
    gap = 12.4
    _, truth = drive(gap, lambda t: 0.0, lambda t: 0.0)
    a, b = fixes(truth, gap)
    assert dr.fill(a, b, encode(gap, [0.0] * 14, [0.0] * 14)) == []


def test_short_gap_between_batches():
    gap = 3.7
    _, truth = drive(gap, lambda t: 40.0, lambda t: 8.0)
    a, b = fixes(truth, gap)
    raw = encode(gap, [40.0] * 4 + [40.0], [h - truth[0][2] for _, _, h, _ in truth])
    points = dr.fill(a, b, raw)
    assert len(points) == 3 and worst_error(points, truth) < 1.0


# -- journeys ------------------------------------------------------------------

def row(rid, t, x, y, heading, speed_mph, motion=None, **extra):
    lat, lon = latlon(x, y)
    base = {
        'rec_id': rid, 'latitude': lat, 'longitude': lon,
        'gsm_timestamp': datetime.datetime(2026, 10, 3, 21, 55) + datetime.timedelta(seconds=t),
        'speed': speed_mph, 'obd_speed': speed_mph, 'heading': heading,
        'satellites': 8, 'cell_location': 0, 'motion': motion,
    }
    base.update(extra)
    return base


def journey_rows():
    gap = 12.4
    _, truth = drive(gap, lambda t: 30.0, right_turn)
    raw = encode(gap, [30.0] * 13 + [30.0], [h - truth[0][2] for _, _, h, _ in truth])
    (ax, ay, ah, _), (bx, by, bh, _) = truth[0], truth[-1]
    mph = 30.0 / dr.KMH_PER_MPH
    return [row(100, -1, ax - 3, ay - 7, ah, mph),
            row(101, 0, ax, ay, ah, mph),
            row(102, gap, bx, by, bh % 360, mph, motion=raw),
            row(103, gap + 1, bx + 8, by - 1, bh % 360, mph)]


def test_journey_fill_goes_between_the_two_records():
    fills = dr.journey_fill(journey_rows())
    assert list(fills) == [1]
    assert len(fills[1]) == 12


def test_journey_fill_needs_consecutive_records():
    rows = journey_rows()
    rows[2]['rec_id'] = 105         # a record went missing in between
    assert dr.journey_fill(rows) == {}


def test_journey_fill_needs_a_live_fix_each_end():
    rows = journey_rows()
    rows[1]['cell_location'] = 1    # built from the stored position
    assert dr.journey_fill(rows) == {}


def test_point_keeps_the_fix_readings_but_not_its_imu():
    base = {'latitude': 1.0, 'longitude': 2.0, 'speed': 10.0, 'combined_speed': 10.0,
            'heading': 0.0, 'timestamp': 'x', 'gsm_ts': 1000.0, 'battery_level': 13.9,
            'obd_rpm': 1800.0, 'accel_x': 12.0, 'gyro_z': 1.5, 'imu': [[1, 2, 3, 4, 5, 6]]}
    stamp = datetime.datetime(2026, 10, 3, 22, 55, 3)
    p = dr.point(base, {'lat': 51.5, 'lon': -0.2, 't': 2.0, 'speed': 48.28,
                        'heading': 91.5}, stamp)
    assert p['dr'] == 1
    assert p['latitude'] == 51.5 and p['speed'] == pytest.approx(30.0, abs=0.01)
    assert p['timestamp'] == '03.10.2026 22:55:05'
    assert p['gsm_ts'] == 1002.0
    assert p['battery_level'] == 13.9 and p['obd_rpm'] == 1800.0
    assert p['accel_x'] is None and p['gyro_z'] is None and p['imu'] is None
    assert base['latitude'] == 1.0       # the fix itself is left alone


# -- ingest --------------------------------------------------------------------

def test_mv_is_parsed_into_motion():
    line = ('03/10/26,21:55:54.499000+00,51.388465,-0.211870,26.89,45.20,123.45,14,6'
            ',13.88,0,26234,0,up=26234,rid=46182,mv=34:27:gggmgy:t')
    assert telemetry.parse_csv_line(line)['motion'] == '34:27:gggmgy:t'


def test_only_a_well_formed_motion_field_is_valid():
    assert dr.valid('34:27:gggmgy:t')
    assert not dr.valid('34:27:gg$g:t')
    assert not dr.valid("34:27:gggmgy:t' OR 1=1")
