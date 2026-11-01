"""tools/backfill_board.py: the board a row's build was made for, read from the
images that were published.

The first groups need no database; the last needs one (see conftest.py).
"""

import importlib.util
import os
import sys

import pytest

from conftest import needs_db, record

from tracker import db, logs, telemetry

TOOLS = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                     'tools')


@pytest.fixture(scope='module')
def tool():
    spec = importlib.util.spec_from_file_location(
        'backfill_board', os.path.join(TOOLS, 'backfill_board.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def blob(*strings):
    """Bytes the way a binary holds C strings: NUL-terminated, among noise."""
    return b'\x00\x13\x37' + b''.join(b'\x00' + s.encode() + b'\x00' for s in strings) + b'\xff\x00'


# -- reading an image ----------------------------------------------------------

def test_the_board_is_read_from_among_the_other_version_strings(tool):
    # Every image has v3.3 and v4.3 in it from somewhere else.
    assert tool.image_board(blob('v4.3', 'v3.3', 'v3.4+kline', '0.4.60')) == 'v3.4+kline'


@pytest.mark.parametrize('board', ['v4.0p1+kline', 'v4.0+can+kline', 'v2.1m+aio', 'bench+kline',
                                   'v3.3+can'])
def test_boards_as_the_firmware_spells_them(tool, board):
    assert tool.image_board(blob('v4.3', board)) == board


def test_an_image_that_does_not_say_gives_nothing(tool):
    assert tool.image_board(blob('v4.3', 'v3.3', 'v2.2', 'v0.4')) is None


def test_an_id_with_no_interface_cannot_be_told_from_the_noise(tool):
    assert tool.image_board(blob('v3.4')) is None


def test_two_different_boards_in_one_image_give_nothing(tool):
    assert tool.image_board(blob('v3.4+kline', 'v4.0+kline')) is None


def test_the_same_board_twice_is_still_that_board(tool):
    assert tool.image_board(blob('v3.4+kline', 'v3.4+kline')) == 'v3.4+kline'


def test_a_string_that_is_part_of_a_longer_one_is_not_a_board(tool):
    assert tool.image_board(b'\x00xv3.4+kline\x00') is None
    assert tool.image_board(b'\x00v3.4+kline-extra\x00') is None


def test_the_tool_reads_images_as_the_server_does(tool):
    # The tool runs without the package, so it carries its own copy.
    from tracker import firmware
    assert tool.BOARD_IN_IMAGE.pattern == firmware.BOARD_IN_IMAGE.pattern


def test_only_per_device_images_are_read(tool, tmp_path):
    (tmp_path / 'l0destar-0.4.60-350000000000000.bin').write_bytes(blob('v3.4+kline'))
    (tmp_path / 'l0destar-0.4.61-350000000000000.bin').write_bytes(blob('v3.3'))   # says nothing
    (tmp_path / 'l0destar-0.4.6.bin').write_bytes(blob('v3.0+kline'))               # not per device
    (tmp_path / 'manifest-350000000000000.txt').write_text('version=0.4.61\n')
    (tmp_path / 'l0destar-0.4.62-350000000000001.bin').write_bytes(blob('v4.0+kline'))

    assert tool.published_boards(str(tmp_path)) == {
        ('350000000000000', '0.4.60'): 'v3.4+kline',
        ('350000000000001', '0.4.62'): 'v4.0+kline',
    }


# -- which rows get what ------------------------------------------------------------

def rows_of(*runs):
    """[(fw, n)] -> [(id, fw, board)] with consecutive ids."""
    out = []
    for fw, n in runs:
        for _ in range(n):
            out.append((len(out) + 1, fw, None))
    return out


KNOWN = {'0.4.60': 'v3.3+kline', '0.4.61': 'v3.3+kline', '0.4.70': 'v3.4+kline'}


def test_a_row_takes_the_board_of_its_versions_image(tool):
    updates, tally = tool.plan(rows_of(('0.4.60', 3), ('0.4.70', 2)), KNOWN)

    assert updates == [('v3.3+kline', 1, 3), ('v3.4+kline', 4, 5)]
    assert tally['image'] == 5


def test_a_build_between_two_runs_of_the_same_board_takes_it(tool):
    # 0.4.0 is a local build: it says nothing of its own.
    updates, tally = tool.plan(rows_of(('0.4.60', 2), ('0.4.0', 3), ('0.4.61', 2)), KNOWN)

    assert ('v3.3+kline', 3, 5) in updates
    assert (tally['image'], tally['between']) == (4, 3)


def test_a_build_between_two_boards_stays_empty(tool):
    updates, tally = tool.plan(rows_of(('0.4.60', 2), ('0.4.0', 3), ('0.4.70', 2)), KNOWN)

    assert all(first not in (3, 4, 5) for _, first, _ in updates)
    assert tally['between boards'] == 3


def test_a_build_at_the_start_of_the_history_stays_empty(tool):
    # Nothing says what the unit had before its first known build.
    updates, tally = tool.plan(rows_of(('0.4.5', 2), ('0.4.60', 2)), KNOWN)

    assert updates == [('v3.3+kline', 3, 4)]
    assert tally['at an end'] == 2


def test_rows_from_before_a_version_was_reported_stay_empty(tool):
    updates, tally = tool.plan(rows_of((None, 4), ('0.4.60', 2)), KNOWN)

    assert updates == [('v3.3+kline', 5, 6)]
    assert tally['unversioned'] == 4


def test_an_unversioned_gap_between_the_same_board_is_filled(tool):
    updates, tally = tool.plan(rows_of(('0.4.60', 2), (None, 2), ('0.4.61', 2)), KNOWN)

    assert tally['between'] == 2


def test_a_board_the_device_reported_is_left_alone(tool):
    rows = [(1, '0.4.60', 'v3.3+kline'), (2, '0.4.60', None), (3, '0.4.60', 'v3.3+kline')]
    updates, tally = tool.plan(rows, KNOWN)

    assert updates == [('v3.3+kline', 1, 3)]       # the statement only writes where empty
    assert (tally['reported'], tally['image']) == (2, 1)


def test_nothing_to_do_once_every_row_has_its_board(tool):
    rows = [(1, '0.4.60', 'v3.3+kline'), (2, '0.4.70', 'v3.4+kline')]
    updates, tally = tool.plan(rows, KNOWN)

    assert updates == []
    assert tally['reported'] == 2


# -- against a database ----------------------------------------------------------

def send(device, *lines):
    telemetry.process_lines(device, list(lines), '203.0.113.7', db.web, logs.udp)


def boards(database):
    return [(r['fw'], r['board']) for r in
            database.all('SELECT `fw`, `board` FROM `log` ORDER BY `id`')]


@needs_db
def test_the_tool_fills_the_rows_and_can_be_run_again(tool, device, database, tmp_path, capsys):
    fw = tmp_path / 'fw'
    fw.mkdir()
    imei = device['imei']
    (fw / ('l0destar-0.4.60-%s.bin' % imei)).write_bytes(blob('v4.3', 'v3.3+kline'))
    (fw / ('l0destar-0.4.70-%s.bin' % imei)).write_bytes(blob('v4.3', 'v3.4+kline'))

    send(device, record(0, 51.5, -0.1, 0))                                  # before a version was reported
    send(device, record(1, 51.5, -0.1, 0, extras=',fw=0.4.60'))
    send(device, record(2, 51.5, -0.1, 0))
    send(device, record(3, 51.5, -0.1, 0, extras=',fw=0.4.70,bd=v9.9+reported'))   # the device said so
    send(device, record(4, 51.5, -0.1, 0))
    database.query('UPDATE `log` SET `board` = NULL ORDER BY `id` DESC LIMIT 1')   # one empty row among reported ones
    assert boards(database)[0] == (None, None)

    config = os.environ['TRACKER_CONFIG']
    args = ['--config', config, '--fw-dir', str(fw)]

    assert tool.main(args) == 0                                              # shows, writes nothing
    assert all(board is None for _, board in boards(database)[:3])
    assert 'nothing written' in capsys.readouterr().out

    assert tool.main(args + ['--apply']) == 0
    assert boards(database) == [
        (None, None),
        ('0.4.60', 'v3.3+kline'), ('0.4.60', 'v3.3+kline'),
        ('0.4.70', 'v9.9+reported'), ('0.4.70', 'v3.4+kline'),               # the reported one never replaced
    ]
    capsys.readouterr()

    assert tool.main(args + ['--apply']) == 0
    assert '0 rows written' in capsys.readouterr().out


@needs_db
def test_the_tool_wants_the_column_before_it_writes(tool, device, database, tmp_path, capsys):
    (tmp_path / ('l0destar-0.4.60-%s.bin' % device['imei'])).write_bytes(blob('v3.3+kline'))
    database.query('ALTER TABLE `log` DROP COLUMN `board`')
    try:
        code = tool.main(['--config', os.environ['TRACKER_CONFIG'], '--fw-dir', str(tmp_path), '--apply'])
    finally:
        database.query("ALTER TABLE `log` ADD COLUMN `board` VARCHAR(32) DEFAULT NULL")

    assert code == 1
    assert 'migrations/2026-11-01_log_board.sql' in capsys.readouterr().out
