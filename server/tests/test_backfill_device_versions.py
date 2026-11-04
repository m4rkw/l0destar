"""tools/backfill_device_versions.py: the firmware and board a device row is
filled with, from the device's newest record.

The first groups need no database; the last needs one (see conftest.py).
"""

import datetime
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
    """The tool loaded as a module; it imports its sibling, so tools/ is
    importable while it loads, as it is when the script runs."""
    sys.path.insert(0, TOOLS)
    try:
        spec = importlib.util.spec_from_file_location(
            'backfill_device_versions', os.path.join(TOOLS, 'backfill_device_versions.py'))
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
    finally:
        sys.path.remove(TOOLS)
    return module


def at(minute):
    return datetime.datetime(2026, 11, 1, 12, minute)


# -- the newest record -----------------------------------------------------------

def test_the_newest_record_is_the_one_built_last(tool):
    # A backlog flushed behind the live record arrives last and was built first.
    rows = [(12, at(5), '0.4.96', 'v4.0+kline'),
            (11, at(9), '0.4.98', 'v4.0p1+kline')]
    assert tool.newest_version(rows) == ('0.4.98', 'v4.0p1+kline')


def test_records_built_together_go_by_arrival(tool):
    rows = [(3, at(9), '0.4.98', 'b'), (4, at(9), '0.4.99', 'c'), (2, at(9), '0.4.97', 'a')]
    assert tool.newest_version(rows) == ('0.4.99', 'c')


def test_no_records_or_no_version_gives_nothing(tool):
    assert tool.newest_version([]) is None
    assert tool.newest_version([(1, at(0), None, None)]) is None


def test_a_version_with_no_board_is_still_a_version(tool):
    assert tool.newest_version([(1, at(0), '0.4.0', None)]) == ('0.4.0', None)


# -- what a device row is given ---------------------------------------------------

def test_an_empty_device_takes_what_was_found(tool):
    assert tool.plan({'fw': None, 'board': None}, ('0.4.98', 'v4.0p1+kline')) == ('0.4.98', 'v4.0p1+kline')
    assert tool.plan({'fw': None, 'board': None}, ('0.4.0', None)) == ('0.4.0', None)


def test_a_device_the_server_has_filled_is_left_alone(tool):
    # The server keeps the row current, so the newest record can only agree or be older.
    assert tool.plan({'fw': '0.4.98', 'board': 'v4.0p1+kline'}, ('0.4.96', 'v4.0+kline')) is None
    assert tool.plan({'fw': '0.4.98', 'board': 'v4.0p1+kline'}, ('0.4.98', 'v4.0p1+kline')) is None


def test_a_board_is_filled_only_for_the_firmware_the_row_has(tool):
    assert tool.plan({'fw': '0.4.98', 'board': None}, ('0.4.98', 'v4.0p1+kline')) == ('0.4.98', 'v4.0p1+kline')
    assert tool.plan({'fw': '0.4.99', 'board': None}, ('0.4.98', 'v4.0p1+kline')) is None
    assert tool.plan({'fw': '0.4.98', 'board': None}, ('0.4.98', None)) is None


def test_a_device_that_never_reported_stays_empty(tool):
    assert tool.plan({'fw': None, 'board': None}, None) is None


# -- against a database ------------------------------------------------------------

def send(device, *lines):
    telemetry.process_lines(device, list(lines), '203.0.113.7', db.web, logs.udp)


def stored(device):
    row = db.lookup_device(imei=device['imei'])
    return row['fw'], row['board']


def forget(database):
    """The device rows as they were before the server kept a version on them."""
    database.query('UPDATE `device` SET `fw` = NULL, `board` = NULL')


def args(*more):
    return ['--config', os.environ['TRACKER_CONFIG']] + list(more)


@needs_db
def test_the_tool_fills_a_quiet_device_and_can_be_run_again(tool, device, second_device, database, capsys):
    send(device, record(0, 51.5, -0.1, 0, extras=',fw=0.4.97,bd=v4.0p1+kline'),
         record(1, 51.5, -0.1, 0), record(2, 51.5, -0.1, 0))
    forget(database)

    assert tool.main(args()) == 0                                   # shows, writes nothing
    out = capsys.readouterr().out
    assert stored(device) == (None, None)
    assert 'would set firmware 0.4.97, board v4.0p1+kline' in out
    assert 'has not reported a version' in out                      # the van
    assert 'nothing written' in out

    assert tool.main(args('--apply')) == 0
    assert stored(device) == ('0.4.97', 'v4.0p1+kline')
    assert stored(second_device) == (None, None)
    assert '1 device written' in capsys.readouterr().out

    assert tool.main(args('--apply')) == 0
    out = capsys.readouterr().out
    assert '0 devices written' in out and 'already has firmware 0.4.97, board v4.0p1+kline' in out


@needs_db
def test_the_tool_reads_the_record_built_last(tool, device, database):
    # The live record, then a backlog from before an update behind it.
    send(device, record(1, 51.5, -0.1, 0, extras=',rid=101,fw=0.4.98,bd=v4.0p1+kline'))
    send(device, record(0, 51.5, -0.1, 0, extras=',rid=100,fw=0.4.96,bd=v4.0+kline,age=35'))
    forget(database)

    assert tool.main(args('--apply')) == 0
    assert stored(device) == ('0.4.98', 'v4.0p1+kline')


@needs_db
def test_a_device_whose_records_name_no_board_gets_the_firmware_alone(tool, device, database, capsys):
    # The bench: builds from before bd=, and no images published for it.
    send(device, record(0, 51.5, -0.1, 0, extras=',fw=0.4.0'))
    forget(database)

    assert tool.main(args('--apply')) == 0
    assert stored(device) == ('0.4.0', None)
    assert 'firmware 0.4.0 (no board in its records)' in capsys.readouterr().out


@needs_db
def test_the_tool_keeps_a_board_set_by_hand(tool, device, database):
    send(device, record(0, 51.5, -0.1, 0, extras=',fw=0.4.0'))
    database.query("UPDATE `device` SET `fw` = NULL, `board` = 'v4.0+kline' WHERE `id` = %s",
                   (device['id'],))

    assert tool.main(args('--apply')) == 0
    assert stored(device) == ('0.4.0', 'v4.0+kline')


@needs_db
def test_the_tool_leaves_a_version_the_server_wrote_alone(tool, device, database):
    send(device, record(0, 51.5, -0.1, 0, extras=',fw=0.4.97,bd=v4.0p1+kline'))
    database.query("UPDATE `device` SET `fw` = '0.4.98', `board` = 'v4.1+kline' WHERE `id` = %s",
                   (device['id'],))

    assert tool.main(args('--apply')) == 0
    assert stored(device) == ('0.4.98', 'v4.1+kline')


@needs_db
def test_the_tool_fills_a_missing_board_for_the_same_firmware(tool, device, database):
    send(device, record(0, 51.5, -0.1, 0, extras=',fw=0.4.97,bd=v4.0p1+kline'))
    database.query('UPDATE `device` SET `board` = NULL WHERE `id` = %s', (device['id'],))

    assert tool.main(args('--apply')) == 0
    assert stored(device) == ('0.4.97', 'v4.0p1+kline')


@needs_db
def test_a_version_that_arrives_after_the_read_is_never_replaced(tool, device, database, monkeypatch):
    # The server writes the row between the tool reading it and writing: the
    # statement itself must refuse, not just the check before it.
    send(device, record(0, 51.5, -0.1, 0, extras=',fw=0.4.97,bd=v4.0p1+kline'))
    database.query("UPDATE `device` SET `fw` = '0.4.99', `board` = 'v4.1+kline' WHERE `id` = %s",
                   (device['id'],))
    monkeypatch.setattr(tool, 'plan', lambda row, found: ('0.4.97', 'v4.0p1+kline'))

    assert tool.main(args('--apply')) == 0
    assert stored(device) == ('0.4.99', 'v4.1+kline')


@needs_db
def test_the_tool_wants_the_columns_before_it_writes(tool, device, database, capsys):
    send(device, record(0, 51.5, -0.1, 0, extras=',fw=0.4.97,bd=v4.0p1+kline'))
    # Rebuilt each way: an instant drop and add leaves the dropped columns
    # counted against the row size, and a database that has run this enough
    # times refuses the add.
    database.query('ALTER TABLE `device` DROP COLUMN `fw`, DROP COLUMN `board`, ALGORITHM=COPY')
    try:
        refused = tool.main(args('--apply'))
        refusal = capsys.readouterr().out
        shown = tool.main(args())
        preview = capsys.readouterr().out
    finally:
        database.query('ALTER TABLE `device` ADD COLUMN `fw` VARCHAR(16) DEFAULT NULL, '
                       'ADD COLUMN `board` VARCHAR(32) DEFAULT NULL, ALGORITHM=COPY')

    assert refused == 1
    assert 'migrations/2026-11-01_device_fw_board.sql' in refusal
    assert shown == 0
    assert 'do not exist yet' in preview and 'would set firmware 0.4.97' in preview
