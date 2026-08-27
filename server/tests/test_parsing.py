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
        {'int': None, 'always_on': None, 'movement_alarm': None}
    ) == {'int': 0, 'ao': 0, 'ma': 1}


def test_device_config_preserves_explicit_zero():
    assert telemetry.device_config(
        {'int': 900, 'always_on': 1, 'movement_alarm': 0}
    ) == {'int': 900, 'ao': 1, 'ma': 0}
