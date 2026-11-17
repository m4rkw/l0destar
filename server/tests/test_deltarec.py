"""Delta records (tracker/deltarec.py): the drive stream's records after the
first of a datagram, with what the one before already said left out."""

import random

import pytest

from tracker import deltarec, telemetry

# A streamed record as the firmware builds it, and the one 250 ms after.
A = ('16/11/26,20:01:23.456789+00,51.401515,-0.195071,38.88,62.00,227.45,110,14,13.86,0,2015,0,'
     'ax=819;ay=573;az=-158,gx=-18;gy=77;gz=-144,it=18.3,mt=23.0,vs=4.22,'
     'rsrp=-65;snr=19;band=20,orpm=1375;ospd=22;ocl=85,up=2015,rid=1234567')
B = ('16/11/26,20:01:23.711234+00,51.401530,-0.195090,39.10,62.00,227.50,110,14,13.86,0,2015,0,'
     'ax=812;ay=570;az=-160,gx=-20;gy=75;gz=-140,it=18.3,mt=23.0,vs=4.22,'
     'rsrp=-65;snr=19;band=20,orpm=1375;ospd=22;ocl=85,up=2015,rid=1234568')
B_DELTA = ('16/11/26,20:01:23.711234+00,51.401530,-0.195090,39.10,,227.50,,,,0,,0,'
           'ax=812;ay=570;az=-160,gx=-20;gy=75;gz=-140,rid=1234568,d=1')


def same(a, b):
    """The same record: what the parser makes of them is equal.  The carried
    extras come back as one group at the end, so the text may be ordered
    differently."""
    return telemetry.parse_csv_line(a) == telemetry.parse_csv_line(b)


# -- expanding -------------------------------------------------------------------------------

def test_a_delta_is_made_whole():
    out = deltarec.expand(A, B_DELTA)
    assert same(out, B)
    assert ',d=1' not in out


def test_the_encoder_writes_what_the_decoder_reads():
    assert deltarec.encode(A, B) == B_DELTA


def test_blank_fixed_fields_take_the_previous_record_s_value():
    out = deltarec.expand(A, B_DELTA).split(',')
    assert out[5:6] == ['62.00']                       # altitude: ts has a comma, so index 5
    d = telemetry.parse_csv_line(deltarec.expand(A, B_DELTA))
    assert (d['altitude'], d['hdop'], d['satellites'], d['battery_level'], d['waketime']) == \
        ('62.00', '110', '14', '13.86', '2015')


def test_what_the_delta_sends_wins_over_the_previous_record():
    out = telemetry.parse_csv_line(deltarec.expand(A, B_DELTA))
    assert out['speed'] == '39.10' and out['heading'] == '227.50'
    assert out['accel_x'] == '812' and out['gyro_z'] == '-140'
    assert out['rec_id'] == '1234568'


def test_sticky_extras_not_sent_are_carried():
    d = telemetry.parse_csv_line(deltarec.expand(A, B_DELTA))
    assert (d['imu_temp'], d['mcu_temp'], d['vsys']) == ('18.3', '23.0', '4.22')
    assert (d['rsrp'], d['snr'], d['band']) == ('-65', '19', '20')
    assert (d['obd_rpm'], d['obd_speed'], d['obd_coolant']) == ('1375', '22', '85')
    assert d['uptime'] == '2015'


def test_a_sticky_extra_that_changed_is_the_one_sent():
    b = B.replace('orpm=1375', 'orpm=1410')
    delta = deltarec.encode(A, b)
    assert 'orpm=1410' in delta and 'ospd' not in delta
    assert same(deltarec.expand(A, delta), b)


def test_extras_that_are_not_sticky_are_never_carried():
    # ax, rid, fl, age are per record: absent from the delta means absent
    a = A + ',fl=1,age=40'
    d = telemetry.parse_csv_line(deltarec.expand(a, B_DELTA))
    assert 'flash' not in d and 'age' not in d


def test_opaque_extras_are_neither_carried_nor_touched():
    a = A + ',tb=xtt:8;xpi:900;xoa:coolant>=95|rpm>6500,dbg=a:1;b:2;rst=wdt'
    d = telemetry.parse_csv_line(deltarec.expand(a, B_DELTA))
    assert 'tunables_built' not in d and 'dbg' not in d and 'rst' not in d
    # and one sent with the delta goes through whole
    b = B + ',tb=xtt:8;xpi:900;xoa:coolant>=95|rpm>6500'
    out = deltarec.encode(a, b)
    assert 'tb=xtt:8;xpi:900;xoa:coolant>=95|rpm>6500' in out
    assert same(deltarec.expand(a, out), b)


def test_nothing_is_carried_out_of_an_opaque_extra_even_if_it_looks_sticky():
    # the body of dbg= and the like is one value: whatever is inside it is not a reading
    a = A + ',dbg=a:1;vs=9.99;it=77'
    d = telemetry.parse_csv_line(deltarec.expand(a, B_DELTA))
    assert d['vsys'] == '4.22' and d['imu_temp'] == '18.3'      # the real ones, from the plain extras
    only = A.replace(',it=18.3,mt=23.0,vs=4.22', '') + ',dbg=a:1;vs=9.99;it=77'
    delta = B_DELTA
    out = telemetry.parse_csv_line(deltarec.expand(only, delta))
    assert 'vsys' not in out and 'imu_temp' not in out


def test_an_opaque_extra_that_looks_sticky_is_sent_whole_and_not_dropped():
    a = A + ',dbg=a:1;vs=4.22'
    b = B + ',dbg=a:1;vs=4.22'
    out = deltarec.encode(a, b)
    assert 'dbg=a:1;vs=4.22' in out
    assert same(deltarec.expand(a, out), b)


def test_the_marker_is_not_left_in_what_is_read():
    assert not deltarec.is_marked(deltarec.expand(A, B_DELTA))
    assert deltarec.is_marked(B_DELTA)
    assert not deltarec.is_marked(A)


def test_a_record_that_is_not_a_delta_is_returned_as_it_is():
    assert deltarec.expand(A, B) == B


def test_a_chain_of_deltas_each_against_the_whole_one_before():
    c = B.replace('.711234', '.966789').replace('51.401530', '51.401549') \
         .replace('rid=1234568', 'rid=1234569').replace('62.00', '62.40')
    lines = deltarec.encode_lines([A, B, c])
    assert lines[0] == A and deltarec.is_marked(lines[1]) and deltarec.is_marked(lines[2])
    out = deltarec.expand_lines(lines)
    assert [telemetry.parse_csv_line(x) for x in out] == \
        [telemetry.parse_csv_line(x) for x in (A, B, c)]


# -- what a datagram holds ----------------------------------------------------------------------

def test_other_lines_pass_through_and_do_not_break_the_chain():
    lines = [A, 'L,1200,W,main: something', B_DELTA]
    out = deltarec.expand_lines(lines)
    assert out[1] == lines[1] and same(out[2], B)


def test_a_whole_datagram_of_whole_records_is_untouched():
    assert deltarec.expand_lines([A, B]) == [A, B]


def test_a_delta_with_no_record_before_it_is_refused_by_the_parser():
    out = deltarec.expand_lines([B_DELTA])
    assert out == [B_DELTA]
    with pytest.raises(ValueError):
        telemetry.parse_csv_line(out[0])


def test_a_delta_that_lost_its_position_is_refused_and_the_chain_goes_on():
    broken = B_DELTA.replace('51.401530', '')
    out = deltarec.expand_lines([A, broken, B_DELTA])
    assert out[1] == broken
    with pytest.raises(ValueError):
        telemetry.parse_csv_line(out[1])
    assert same(out[2], B)                      # still against A, the last whole record


def test_a_delta_is_never_read_against_another_datagram():
    # nothing is carried between calls
    deltarec.expand_lines([A])
    assert deltarec.expand_lines([B_DELTA]) == [B_DELTA]


def test_a_short_previous_record_cannot_be_a_baseline():
    out = deltarec.expand_lines(['16/11/26,20:01:23.4+00,51.4,-0.1', B_DELTA])
    assert out[1] == B_DELTA


# -- the encoder -----------------------------------------------------------------------------

def test_a_record_that_lost_a_sticky_reading_is_sent_whole():
    # the signal reading went stale: "unchanged" would put it back
    b = B.replace('rsrp=-65;snr=19;band=20,', '')
    assert deltarec.encode(A, b) == b


def test_a_new_sticky_reading_is_just_sent():
    a = A.replace('rsrp=-65;snr=19;band=20,', '')
    out = deltarec.encode(a, B)
    assert 'rsrp=-65;snr=19;band=20' in out
    assert same(deltarec.expand(a, out), B)


def test_position_ignition_and_power_flag_are_never_blanked():
    still = B.replace('51.401530', '51.401515').replace('-0.195090', '-0.195071')
    fields = deltarec.encode(A, still).split(',')
    assert fields[2:4] == ['51.401515', '-0.195071']
    assert fields[10] == '0' and fields[12] == '0'     # ignition (index 10) and pon (12)


def test_a_delta_is_never_longer_than_the_record():
    assert len(deltarec.encode(A, B)) < len(B)
    other = B.replace('62.00', '70.00').replace('110', '90').replace('14', '12') \
             .replace('13.86', '13.90').replace('2015', '2016') \
             .replace('it=18.3', 'it=18.4').replace('mt=23.0', 'mt=23.1') \
             .replace('vs=4.22', 'vs=4.23').replace('rsrp=-65', 'rsrp=-66') \
             .replace('orpm=1375', 'orpm=1400').replace('ospd=22', 'ospd=23').replace('ocl=85', 'ocl=86')
    out = deltarec.encode(A, other)
    assert len(out) <= len(other)
    assert same(deltarec.expand(A, out), other)


def test_an_already_marked_record_is_not_encoded_again():
    assert deltarec.encode(A, B_DELTA) == B_DELTA
    assert deltarec.encode(B_DELTA, B) == B


@pytest.mark.parametrize('key', ['o0B', 'o5E', 'oFF'])
def test_other_obd_pids_are_sticky(key):
    a = A + ',%s=95' % key
    b = B + ',%s=95' % key
    out = deltarec.encode(a, b)
    assert key not in out
    assert telemetry.parse_csv_line(deltarec.expand(a, out))['obd_extra'] == \
        telemetry.parse_csv_line(b)['obd_extra']


def test_a_lower_case_pid_key_is_not_one():
    # obd.pid_key takes upper case only; anything else is an unknown extra
    assert not deltarec.is_sticky('o0b')
    assert deltarec.is_sticky('o0B')


# -- the whole thing, on random streams ------------------------------------------------------------

def _random_stream(rng, n):
    lines = []
    alt, hdop, sat, bat, up = 62.0, 110, 14, 13.86, 2015
    imu_t, mt, vs = 18.3, 23.0, 4.22
    rpm, spd_e, cool = 1375, 22, 85
    sig = (-65, 19, 20)
    for i in range(n):
        if rng.random() < 0.1:
            alt += rng.choice((-0.4, 0.4))
        if rng.random() < 0.2:
            hdop += rng.choice((-10, 10))
        if rng.random() < 0.4:
            sat += rng.choice((-1, 1))
        if rng.random() < 0.15:
            bat += rng.choice((-0.01, 0.01))
        if rng.random() < 0.25:
            up += 1
        if rng.random() < 0.2:
            imu_t += 0.1
        if rng.random() < 0.05:
            mt += 0.1
        if rng.random() < 0.3:
            rpm += rng.randint(-40, 40)
            spd_e += rng.choice((-1, 0, 1))
        if rng.random() < 0.01:
            cool += 1
        if rng.random() < 0.3:
            sig = (sig[0] + rng.choice((-1, 1)), sig[1], sig[2])
        extras = ',ax=%d;ay=%d;az=%d' % tuple(rng.randint(-900, 900) for _ in range(3))
        if rng.random() < 0.3:
            extras += ',gx=%d;gy=%d;gz=%d' % tuple(rng.randint(-300, 300) for _ in range(3))
        extras += ',it=%.1f,mt=%.1f,vs=%.2f' % (imu_t, mt, vs)
        if rng.random() < 0.85:                      # a reading goes stale now and then
            extras += ',rsrp=%d;snr=%d;band=%d' % sig
        if rng.random() < 0.95:                      # the K wire drops out now and then
            extras += ',orpm=%d;ospd=%d;ocl=%d' % (rpm, spd_e, cool)
        if rng.random() < 0.1:
            extras += ',o0B=%d' % rng.randint(90, 100)
        extras += ',up=%d,rid=%d' % (up, 1000 + i)
        if rng.random() < 0.05:
            extras += ',age=%d' % rng.randint(1, 90)
        lines.append('16/11/26,20:01:%02d.%06d+00,%.6f,%.6f,%.2f,%.2f,%.2f,%d,%d,%.2f,0,%d,0%s' % (
            i // 4, (i % 4) * 250000, 51.4 + i * 1e-5, -0.19 - i * 1e-5,
            rng.uniform(20, 60), alt, rng.uniform(0, 360), hdop, sat, bat, up, extras))
    return lines


@pytest.mark.parametrize('seed', range(30))
def test_random_streams_round_trip(seed):
    rng = random.Random(seed)
    lines = _random_stream(rng, 40)
    for start in range(0, len(lines), 4):             # datagrams of four
        batch = lines[start:start + 4]
        wire = deltarec.encode_lines(batch)
        assert all(len(w) <= len(r) for w, r in zip(wire, batch))
        got = deltarec.expand_lines(wire)
        assert [telemetry.parse_csv_line(x) for x in got] == \
            [telemetry.parse_csv_line(x) for x in batch]


def test_a_record_with_nothing_to_leave_out_is_sent_as_it_is():
    # every blankable field and every sticky reading differs: the marker would make it longer
    b = ('16/11/26,20:01:23.711234+00,51.401530,-0.195090,39.10,63.00,228.50,120,15,13.90,0,2016,0,'
         'ax=812;ay=570;az=-160,gx=-20;gy=75;gz=-140,it=18.4,mt=23.1,vs=4.23,'
         'rsrp=-66;snr=18;band=3,orpm=1400;ospd=23;ocl=86,up=2016,rid=1234568')
    assert deltarec.encode(A, b) == b


def test_a_delta_that_could_not_be_expanded_is_not_a_baseline_for_the_next():
    out = deltarec.expand_lines([B_DELTA, B_DELTA])
    assert out == [B_DELTA, B_DELTA]


def test_the_stream_gets_smaller():
    lines = _random_stream(random.Random(7), 400)
    whole = sum(len(x) for x in lines)
    sent = 0
    for start in range(0, len(lines), 4):
        sent += sum(len(x) for x in deltarec.encode_lines(lines[start:start + 4]))
    assert sent < whole * 0.85
