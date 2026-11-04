#!/usr/bin/env python3
"""Fill device.fw and device.board for devices that reported a version before
the device row kept one, from their newest record.

The server writes both onto the device row as a device's records arrive, so a
unit that is reporting needs nothing.  One that is switched off, or that has
not sent a record since the columns were added, has nothing there - but its
log does: every row carries the version forward from the one before, so the
newest row says what the unit was running when it last reported.

  * a device with no version on its row gets its newest record's firmware and
    board (a board already on the row stays when the records name none);
  * one with that same firmware and no board gets the board;
  * one whose row already has a version is left alone: the server keeps it
    current, and the newest record can only agree or be older;
  * one that never reported a version stays empty.

The newest record is the one built last among the device's last few rows, as
the server itself reads it, so a backlog flushed behind the live record is not
mistaken for it.  Run tools/backfill_board.py first, so that the rows being
read have their boards.

    python backfill_device_versions.py            show what would be filled
    python backfill_device_versions.py --apply    fill it

It reads the server's config.yaml (TRACKER_CONFIG, else the one in the current
directory, else the one beside tools/), or the file named with --config.
device.fw and device.board must exist first
(migrations/2026-11-01_device_fw_board.sql).
"""

import argparse
import datetime
import sys

from backfill_board import connect, default_config

# How far back to look for the newest record, as the server does.
RECENT = 50


def newest_version(rows):
    """(fw, board) from the record built last among rows, [(id, timestamp,
    fw, board)], or None when there are none or it reports no version."""
    if not rows:
        return None
    _, _, fw, board = max(rows, key=lambda r: (r[1] or datetime.datetime.min, r[0]))
    return (fw, board) if fw else None


def plan(device, found):
    """What to write to a device row, as (fw, board), or None.  device is
    {'fw', 'board'} as stored; found is newest_version()."""
    if found is None:
        return None
    fw, board = found
    if device['fw'] is None:
        return fw, board
    if device['fw'] == fw and device['board'] is None and board is not None:
        return fw, board
    return None


def has_columns(cursor, table, names):
    cursor.execute('SHOW COLUMNS FROM `%s`' % table)
    present = {row[0] for row in cursor.fetchall()}
    return all(name in present for name in names)


def describe(fw, board):
    if board is None:
        return 'firmware %s (no board in its records)' % fw
    return 'firmware %s, board %s' % (fw, board)


def fill(connection, apply):
    cursor = connection.cursor()

    has_device = has_columns(cursor, 'device', ('fw', 'board'))
    if apply and not has_device:
        print('device.fw and device.board do not exist: '
              'run migrations/2026-11-01_device_fw_board.sql first')
        return 1
    if not has_device:
        print('device.fw and device.board do not exist yet; '
              'showing the plan as if every device were empty\n')
    has_log_board = has_columns(cursor, 'log', ('board',))

    cursor.execute('SELECT `id`, `imei`, `name`, %s FROM `device` ORDER BY `id`'
                   % ('`fw`, `board`' if has_device else 'NULL, NULL'))
    devices = [dict(zip(('id', 'imei', 'name', 'fw', 'board'), row))
               for row in cursor.fetchall()]

    written = 0
    for device in devices:
        cursor.execute(
            'SELECT `id`, `timestamp`, `fw`, %s FROM `log` WHERE `device_id` = %%s '
            'ORDER BY `id` DESC LIMIT %d' % ('`board`' if has_log_board else 'NULL', RECENT),
            (device['id'],))
        found = newest_version(cursor.fetchall())
        label = '%s (%s)' % (device['name'] or '-', device['imei'])
        change = plan(device, found)
        if change:
            print('%s: %s %s' % (label, 'sets' if apply else 'would set', describe(*change)))
            if apply:
                written += cursor.execute(
                    'UPDATE `device` SET `fw` = %s, `board` = COALESCE(%s, `board`) '
                    'WHERE `id` = %s '
                    'AND (`fw` IS NULL OR (`fw` = %s AND `board` IS NULL))',
                    (change[0], change[1], device['id'], change[0]))
        elif device['fw'] is not None:
            print('%s: already has firmware %s, %s' % (
                label, device['fw'], 'board ' + device['board'] if device['board'] else 'no board'))
        else:
            print('%s: has not reported a version' % label)

    if apply:
        connection.commit()
        print('\n%d device%s written' % (written, '' if written == 1 else 's'))
    else:
        print('\nnothing written; --apply fills these')
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('--config', default=default_config())
    parser.add_argument('--apply', action='store_true', help='write the versions (default: only show)')
    args = parser.parse_args(argv)

    # Closed whatever happens: the open transaction would otherwise hold a
    # lock on the table against anything that wants to alter it.
    _, connection = connect(args.config)
    try:
        return fill(connection, args.apply)
    finally:
        connection.close()


if __name__ == '__main__':
    sys.exit(main())
