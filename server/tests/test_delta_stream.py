"""The drive stream's delta records and short replies, through process_lines
against a real database (skipped unless TRACKER_TEST_DB is set; see conftest)."""

import pytest

from conftest import needs_db

from tracker import db, deltarec, logs, reply_policy, telemetry

pytestmark = needs_db

IP = '203.0.113.7'

BASE = ('16/11/26,20:01:%02d.%06d+00,51.4015%02d,-0.1950%02d,%.2f,%s,227.45,%d,14,13.86,0,%d,0,'
        'ax=%d;ay=573;az=-158,gx=-18;gy=77;gz=-144,it=%s,mt=23.0,vs=4.22,'
        'rsrp=-65;snr=19;band=20,orpm=%d;ospd=22;ocl=85,up=%d,rid=%d')


def stream(n=4, start=0, rpm=1375):
    """n records 250 ms apart, the way the stream builds them."""
    out = []
    for i in range(n):
        k = start + i
        out.append(BASE % (23 + k // 4, (k % 4) * 250000 + 456789 % 250000, k, k, 38.5 + k / 10,
                           '62.00', 110, 2015 + k // 4, 800 + k, '18.3', rpm + (k // 4) * 10,
                           2015 + k // 4, 1234567 + k))
    return out


def rows(database, device):
    """What was stored for a device, without what differs by design."""
    out = list(database.all('SELECT * FROM `log` WHERE `device_id` = %s ORDER BY `gsm_timestamp`',
                            (device['id'],)))
    for row in out:
        for key in ('id', 'device_id', 'timestamp', 'ip'):
            row.pop(key, None)
    return out


def send(device, lines):
    return telemetry.process_lines(device, list(lines), IP, db.web, logs.udp)


@pytest.fixture(autouse=True)
def fresh_gate(monkeypatch):
    monkeypatch.setattr(reply_policy, 'gate', reply_policy.ReplyGate())


# -- storage ------------------------------------------------------------------------------------

def test_a_delta_datagram_stores_the_rows_the_whole_one_does(device, second_device, database):
    lines = stream(8)
    for start in (0, 4):
        send(device, lines[start:start + 4])
        send(second_device, deltarec.encode_lines(lines[start:start + 4]))

    whole, deltas = rows(database, device), rows(database, second_device)
    assert len(whole) == len(deltas) == 8
    assert whole == deltas


def test_the_delta_rows_really_were_deltas(device, second_device, database):
    lines = stream(4)
    wire = deltarec.encode_lines(lines)
    assert [deltarec.is_marked(x) for x in wire] == [False, True, True, True]
    assert sum(len(x) for x in wire) < sum(len(x) for x in lines)
    send(second_device, wire)
    last = database.one('SELECT * FROM `log` WHERE `device_id` = %s ORDER BY `gsm_timestamp` DESC',
                        (second_device['id'],))
    assert (last['satellites'], float(last['mcu_temp']), float(last['vsys']),
            last['obd_coolant']) == (14, 23.0, 4.22, 85)


def test_a_delta_with_nothing_to_be_a_delta_of_is_refused_not_stored(device, database):
    wire = deltarec.encode_lines(stream(2))
    send(device, wire[1:])                            # the first record never arrived
    assert rows(database, device) == []


def test_a_lost_first_record_does_not_cost_the_whole_datagram_its_neighbours(device, database):
    # the broken delta is skipped, the whole record after it is stored
    wire = deltarec.encode_lines(stream(2))
    send(device, [wire[1], stream(1, start=9)[0]])
    assert len(rows(database, device)) == 1


def test_a_datagram_sent_twice_is_stored_once(device, database):
    wire = deltarec.encode_lines(stream(4))
    send(device, wire)
    send(device, wire)
    assert len(rows(database, device)) == 4


def test_a_log_line_after_the_records_does_not_disturb_them(device, database):
    wire = deltarec.encode_lines(stream(3)) + ['L,1200,W,main: something']
    send(device, wire)
    assert len(rows(database, device)) == 3


# -- replies ------------------------------------------------------------------------------------

def test_the_reply_is_whole_then_short_then_whole_again_when_the_unit_boots(device, database):
    first = send(device, stream(1))
    assert first.startswith('1,3600,1,backup=') and first.endswith(',gx=off,os=1')

    second = send(device, stream(1, start=10))
    assert second == '1,3600,1'

    boot = stream(1, start=20)[0] + ',fw=0.4.129'
    third = send(device, [boot])
    assert ',backup=' in third


def test_a_changed_setting_goes_out_in_the_next_reply(device, database):
    send(device, stream(1))
    assert send(device, stream(1, start=10)) == '1,3600,1'
    database.query('UPDATE `device` SET `leds` = 0 WHERE `id` = %s', (device['id'],))
    assert ',le=0' in send(device, stream(1, start=20))
    assert send(device, stream(1, start=30)) == '1,3600,1'


def test_a_queued_command_rides_the_short_reply(device, database):
    send(device, stream(1))
    database.query("INSERT INTO `command` (`device_id`, `command`, `timestamp`) "
                   "VALUES (%s, 'locate', NOW(6))", (device['id'],))
    assert send(device, stream(1, start=10)) == '1,3600,1,locate'


def test_a_published_firmware_is_told_to_the_unit_at_once(device, database, fw_dir):
    import os
    from tracker import firmware

    send(device, stream(1))
    assert send(device, stream(1, start=10)) == '1,3600,1'
    with open(os.path.join(fw_dir, 'manifest-%s.txt' % device['imei']), 'w') as f:
        f.write('version=0.4.129\nfile=l0destar-0.4.129-%s.bin\nboard=test\n' % device['imei'])
    firmware._manifest_cache.clear()
    assert ',fota=0.4.129' in send(device, stream(1, start=20))


def test_the_settings_are_repeated_after_a_minute(device, database, monkeypatch):
    clock = [1000.0]
    monkeypatch.setattr(reply_policy, 'gate', reply_policy.ReplyGate(clock=lambda: clock[0]))
    send(device, stream(1))
    assert send(device, stream(1, start=10)) == '1,3600,1'
    clock[0] += 61
    assert ',backup=' in send(device, stream(1, start=20))
