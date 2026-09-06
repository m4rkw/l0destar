"""Record parsing.

These are the tests worth having most: a parser bug corrupts history silently,
and the device is not in a position to notice or complain.
"""

import pytest

from tracker import telemetry

FULL = ('12/08/26,17:30:45+04,51.5074,-0.1278,48.3,35.0,182.5,0.9,11,12.64,1,7,0'
        ',mcc=234;mnc=10;lac=1a2b;cid=00112233;rat=CATM1'
        ',ax=12;ay=-4;az=1010,gx=1;gy=2;gz=3'
        ',up=86400;mt=31.5;it=30.2;vs=4.54'
        ',ri=1;int=3600;ao=0;ma=1;fw=0.4.12'
        ',dbg=a:1;b:2;rst=WDT')


def test_timestamp_is_rejoined():
    # The modem's clock format contains a comma, so a naive split puts the
    # time in the latitude field and every subsequent field shifts by one.
    assert telemetry.parse_csv_line(FULL)['gsm_timestamp'] == '12/08/26,17:30:45+04'


def test_fixed_fields():
    d = telemetry.parse_csv_line(FULL)
    assert d['latitude'] == '51.5074'
    assert d['longitude'] == '-0.1278'
    assert d['battery_level'] == '12.64'
    assert d['ignition_state'] == '1'


def test_extras():
    d = telemetry.parse_csv_line(FULL)
    assert d['mcc'] == '234'
    assert d['rat'] == 'CATM1'
    assert d['accel_z'] == '1010'
    assert d['gyro_y'] == '2'
    assert d['vsys'] == '4.54'
    assert d['imu_temp'] == '30.2'
    assert d['mcu_temp'] == '31.5'
    assert d['fw'] == '0.4.12'
    assert d['request_int'] == '1'


def test_dbg_is_compound():
    # dbg's own body uses semicolons, so it must not be split on them like
    # every other extras group.
    d = telemetry.parse_csv_line(FULL)
    assert d['dbg'] == 'a:1;b:2'
    assert d['rst'] == 'WDT'


def test_dbg_without_rst():
    line = '12/08/26,17:30:45+04,0,0,0,0,0,0,0,12.1,0,3,0,dbg=x:1;y:2'
    d = telemetry.parse_csv_line(line)
    assert d['dbg'] == 'x:1;y:2'
    assert 'rst' not in d


def test_bare_rst():
    line = '12/08/26,17:30:45+04,0,0,0,0,0,0,0,12.1,0,3,0,rst=POR'
    assert telemetry.parse_csv_line(line)['rst'] == 'POR'


def test_minimal_record():
    d = telemetry.parse_csv_line('12/08/26,17:30:45+04,0,0,0,0,0,0,0,12.1,0,3,0')
    assert d['battery_level'] == '12.1'
    assert 'rat' not in d


def test_unknown_extras_ignored():
    line = '12/08/26,17:30:45+04,0,0,0,0,0,0,0,12.1,0,3,0,zz=1;vs=4.2'
    d = telemetry.parse_csv_line(line)
    assert 'zz' not in d
    assert d['vsys'] == '4.2'


def test_short_record_rejected():
    with pytest.raises(ValueError):
        telemetry.parse_csv_line('12/08/26,17:30:45+04,1,2')


@pytest.mark.parametrize('raw,hour,offset', [
    ('12/08/26,17:30:45+04', 17, '04'),
    ('12/08/26,17:30:45-20', 17, '-20'),
    ('01/01/25,00:00:00.5+00', 0, '00'),
])
def test_gsm_timestamp(raw, hour, offset):
    stamp, parsed_offset = telemetry._parse_gsm_timestamp(raw)
    assert stamp.hour == hour
    assert parsed_offset == offset


def test_gsm_timestamp_century():
    stamp, _ = telemetry._parse_gsm_timestamp('12/08/26,17:30:45+04')
    assert (stamp.year, stamp.month, stamp.day) == (2026, 8, 12)


def test_gsm_timestamp_rejects_junk():
    with pytest.raises(ValueError):
        telemetry._parse_gsm_timestamp('nonsense')


def test_haversine():
    # London to Manchester, about 262 km.
    km = telemetry._haversine_km((51.5074, -0.1278), (53.4808, -2.2426))
    assert 255 < km < 270
    assert telemetry._haversine_km((1.0, 1.0), (1.0, 1.0)) == 0.0


def test_device_config_defaults():
    # movement_alarm defaults on: a device that has never been configured
    # should still raise the alarm rather than silently not.
    assert telemetry.device_config(
        {'int': None, 'movement_alarm': None}
    ) == {'int': 0, 'ma': 1}


def test_device_config_preserves_explicit_zero():
    assert telemetry.device_config(
        {'int': 900, 'movement_alarm': 0}
    ) == {'int': 900, 'ma': 0}


# -- firmware log lines -------------------------------------------------------

def test_device_log_line_is_dated_from_record_uptime():
    import datetime
    now = datetime.datetime(2026, 9, 6, 12, 0, 0)
    batch = [FULL, 'L,86395500,E,transport: send failed: -116']
    boot = telemetry.boot_wall_time(batch, now=now)
    # FULL says up=86400, so the device booted a day before `now` and the
    # line, at 86395.5 s of uptime, was logged 4.5 s before the record.
    assert boot == now - datetime.timedelta(seconds=86400)
    out = telemetry.format_device_log(batch[1], '355025936386877', boot)
    assert out == ('2026-09-06 11:59:55 IMEI=355025936386877 up=86395.500 '
                   'E transport: send failed: -116')


def test_device_log_text_keeps_its_commas():
    import datetime
    rx = datetime.datetime(2026, 9, 6, 12, 0, 0)
    out = telemetry.format_device_log('L,12345,W,modem: +CEREG: 2,0', 'x', None, now=rx)
    assert out == '2026-09-06 12:00:00(rx) IMEI=x up=12.345 W modem: +CEREG: 2,0'


def test_device_log_rejects_junk():
    with pytest.raises(ValueError):
        telemetry.format_device_log('L,abc,W', 'x', None)


def test_boot_wall_time_needs_a_record():
    assert telemetry.boot_wall_time(['L,1,E,x', 'A,0,hello']) is None


# -- OBD-II extras --------------------------------------------------------------

def test_obd_keys_map_to_columns():
    d = telemetry.parse_csv_line(FULL + ',orpm=850;ospd=48;old=235;omaf=1234;omil=0')
    assert d['obd_rpm'] == '850'
    assert d['obd_speed'] == '48'
    assert d['obd_load'] == '235'
    assert d['obd_maf'] == '1234'
    assert d['obd_mil'] == '0'


def test_obd_values_are_unscaled_and_speed_converted():
    d = telemetry.parse_csv_line(FULL + ',orpm=850;ospd=48;old=235;omaf=1234')
    e = telemetry._build_entry(d, {'id': 1}, '203.0.113.7', None)
    assert e['obd_rpm'] == 850
    assert e['obd_speed'] == 29.83          # 48 km/h
    assert e['obd_load'] == 23.5
    assert e['obd_maf'] == 12.34
    # The ECU's road speed is what the interface shows when it exists.
    assert e['combined_speed'] == 29.83


def test_combined_speed_falls_back_to_gnss():
    e = telemetry._build_entry(telemetry.parse_csv_line(FULL), {'id': 1},
                               '203.0.113.7', None)
    assert 'obd_speed' not in e
    assert e['combined_speed'] == e['speed']


def test_dtc_pattern():
    assert telemetry.DTC_RE.match('P0133')
    assert telemetry.DTC_RE.match('U3FFF')
    assert not telemetry.DTC_RE.match('P4000')
    assert not telemetry.DTC_RE.match('X0133')
    assert not telemetry.DTC_RE.match('P01334')
