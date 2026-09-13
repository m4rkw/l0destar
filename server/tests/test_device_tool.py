"""tools/device.py against a real database.

Skipped unless ``TRACKER_TEST_DB`` is set; see conftest.py.
"""

import importlib.util
import os
import sys

import pytest

from conftest import needs_db, record

from tracker import db, logs, telemetry

pytestmark = needs_db

TOOLS = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                     'tools')


@pytest.fixture(scope='module')
def tool():
    """tools/device.py loaded as a module, with tools/ importable as it is
    when the script runs."""
    if TOOLS not in sys.path:
        sys.path.insert(0, TOOLS)
    spec = importlib.util.spec_from_file_location(
        'device_tool', os.path.join(TOOLS, 'device.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def run(tool, capsys, *argv):
    code = tool.main(list(argv))
    captured = capsys.readouterr()
    return code, captured.out, captured.err


def send(device, *lines):
    telemetry.process_lines(device, list(lines), '203.0.113.7', db.web, logs.udp)


def test_list_shows_every_device(tool, capsys, device, second_device):
    send(device, record(0, 51.5, -0.1, 1, extras=',fw=0.4.12'))
    code, out, _ = run(tool, capsys, 'list')

    assert code == 0
    assert device['imei'] in out and second_device['imei'] in out
    assert '0.4.12' in out and '12.40V' in out
    assert 'never' in out           # the second device has not reported


def test_show_prints_settings_but_never_the_key(tool, capsys, device, database):
    database.query("INSERT INTO `command` (`device_id`, `timestamp`, `command`) "
                   "VALUES (%s, NOW(6), 'locate')", (device['id'],))
    code, out, _ = run(tool, capsys, 'show', device['imei'])

    assert code == 0
    assert 'AB12CDE' in out and '3600s' in out and 'locate' in out
    assert device['psk'] not in out


def test_rename_keeps_the_registration_unless_given(tool, capsys, device):
    assert run(tool, capsys, 'rename', device['imei'], 'Estate')[0] == 0
    row = db.lookup_device(imei=device['imei'])
    assert (row['name'], row['registration']) == ('Estate', 'AB12CDE')

    assert run(tool, capsys, 'rename', device['imei'], 'Estate', 'CD56EFG')[0] == 0
    assert db.lookup_device(imei=device['imei'])['registration'] == 'CD56EFG'


def test_rekey_generates_a_key_or_sets_the_one_given(tool, capsys, device):
    code, out, _ = run(tool, capsys, 'rekey', device['imei'])
    generated = db.lookup_device(imei=device['imei'])['psk']
    assert code == 0 and len(generated) == 64
    assert generated != device['psk'] and generated in out

    assert run(tool, capsys, 'rekey', device['imei'], '--psk', 'AB' * 32)[0] == 0
    assert db.lookup_device(imei=device['imei'])['psk'] == 'ab' * 32

    assert run(tool, capsys, 'rekey', device['imei'], '--psk', 'not-a-key')[0] == 1
    assert db.lookup_device(imei=device['imei'])['psk'] == 'ab' * 32


def test_remove_wants_confirmation_and_takes_the_history(tool, capsys, device):
    send(device, record(0, 51.5, -0.1, 0))

    assert run(tool, capsys, 'remove', device['imei'])[0] == 1
    assert db.lookup_device(imei=device['imei']) is not None

    assert run(tool, capsys, 'remove', device['imei'], '--yes')[0] == 0
    assert db.lookup_device(imei=device['imei']) is None
    assert db.web.one('SELECT COUNT(*) AS `n` FROM `log`')['n'] == 0


def test_unknown_device_is_an_error(tool, capsys, database):
    code, _, err = run(tool, capsys, 'show', '999999999999999')
    assert code == 1 and 'no device' in err
