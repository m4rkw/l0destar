#!/usr/bin/env python3
"""Fill log.board for the rows from before devices reported it (bd=), from the
firmware images that were published to them.

Every image carries the board id it was built for (Kconfig APP_BOARD_ID plus
the fitted interfaces, for example v3.4+kline) as a string the firmware
compares the update manifest against, and the server keeps one image per
version per device (fw/l0destar-<version>-<imei>.bin).  A row's fw says which
version the unit was running, so the image says which board that build was
made for:

  * rows whose version has an image get that image's board;
  * rows from a build that has none (a build flashed over USB reports 0.4.0
    whatever it was made from) get a board only when the nearest rows either
    side that do have one agree: a unit does not change boards inside a run of
    builds for the same one;
  * everything else stays empty - the rows from before the unit reported a
    version at all, builds older than the board id, and a build that sits
    between two boards.

Only rows whose board is empty are written, so it can be run again and never
replaces a board a device reported itself.  The manifest is not used: it names
the board of the unit the update is offered to, which for a bridge release is
the board the unit is leaving, where the image names the board it is made for.

    python backfill_board.py            show what would be filled
    python backfill_board.py --apply    fill it

It reads the server's config.yaml (TRACKER_CONFIG, else the one in the current
directory, else the one beside tools/), or the file named with --config.
log.board must exist first (migrations/2026-11-01_log_board.sql).
"""

import argparse
import os
import re
import sys

import pymysql
import yaml

IMAGE_RE = re.compile(r'^l0destar-(\d{1,3}\.\d{1,3}\.\d{1,3})-(\d+)\.bin$')

# A board id as the firmware spells it: the board, then each fitted interface.
# It is a C string, so the bytes before and after it are NUL.  An id with no
# interface (v3.4 on its own) cannot be told from the other version-like
# strings in a binary, so such an image is not read.
BOARD_IN_IMAGE = re.compile(
    rb'(?<=\x00)((?:bench|v[0-9]+\.[0-9]+(?:[a-z][0-9]*)?)'
    rb'(?:\+(?:can|kline|aio))+)(?=\x00)')


def image_board(data):
    """The board an image was built for, or None when it does not say or says
    more than one thing."""
    found = {m.group(1) for m in BOARD_IN_IMAGE.finditer(data)}
    return found.pop().decode('ascii') if len(found) == 1 else None


def published_boards(fw_dir):
    """{(imei, version): board} for every per-device image whose board is readable."""
    boards = {}
    for name in sorted(os.listdir(fw_dir)):
        match = IMAGE_RE.match(name)
        if not match:
            continue
        with open(os.path.join(fw_dir, name), 'rb') as handle:
            board = image_board(handle.read())
        if board:
            boards[(match.group(2), match.group(1))] = board
    return boards


def plan(rows, known):
    """Which rows get which board.

    rows is [(id, fw, board)] for one device in id order; known is {version:
    board} from its images.  Returns (updates, tally): updates is [(board,
    first id, last id)], one per run of rows with the same version that gets a
    board and has an empty row in it; tally counts rows by outcome.
    """
    runs = []
    for row_id, fw, board in rows:
        if runs and runs[-1]['fw'] == fw:
            run = runs[-1]
            run['last'] = row_id
        else:
            run = {'fw': fw, 'first': row_id, 'last': row_id, 'n': 0, 'empty': 0}
            runs.append(run)
        run['n'] += 1
        run['empty'] += board is None

    # Runs that have a board of their own, then the ones between two that agree.
    for run in runs:
        run['board'] = known.get(run['fw']) if run['fw'] is not None else None
        run['how'] = 'image' if run['board'] else None
    before, after = [None] * len(runs), [None] * len(runs)
    seen = None
    for i, run in enumerate(runs):
        before[i] = seen
        seen = run['board'] or seen
    seen = None
    for i in range(len(runs) - 1, -1, -1):
        after[i] = seen
        seen = runs[i]['board'] or seen
    for i, run in enumerate(runs):
        if run['board'] is None and before[i] and before[i] == after[i]:
            run['board'], run['how'] = before[i], 'between'

    updates = []
    tally = {'reported': 0, 'image': 0, 'between': 0, 'unversioned': 0,
             'no image': 0, 'between boards': 0, 'at an end': 0}
    for i, run in enumerate(runs):
        tally['reported'] += run['n'] - run['empty']
        if not run['empty']:
            continue
        if run['board']:
            tally[run['how']] += run['empty']
            updates.append((run['board'], run['first'], run['last']))
        elif run['fw'] is None:
            tally['unversioned'] += run['empty']
        elif before[i] and after[i]:
            tally['between boards'] += run['empty']
        elif before[i] or after[i]:
            tally['at an end'] += run['empty']
        else:
            tally['no image'] += run['empty']
    return updates, tally


def default_config():
    if os.environ.get('TRACKER_CONFIG'):
        return os.environ['TRACKER_CONFIG']
    if os.path.exists('config.yaml'):
        return 'config.yaml'
    return os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'config.yaml')


def connect(config_path):
    with open(config_path) as handle:
        config = yaml.safe_load(handle)
    settings = config['database']
    connection = pymysql.connect(
        host=settings['host'], port=int(settings.get('port', 3306)),
        user=settings['user'], password=settings['password'],
        database=settings['database'], autocommit=False)
    return config, connection


def report(device, total, tally):
    print('%s (%s): %s rows' % (device['name'] or '-', device['imei'], format(total, ',')))
    lines = [
        ('reported by the device', tally['reported']),
        ('filled from the version\'s image', tally['image']),
        ('filled, between builds for the same board', tally['between']),
        ('left: before the unit reported a version', tally['unversioned']),
        ('left: build with no image, between two boards', tally['between boards']),
        ('left: build with no image, at the edge of the history', tally['at an end']),
        ('left: build with no image', tally['no image']),
    ]
    for label, count in lines:
        if count:
            print('  %-54s %s' % (label, format(count, ',')))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('--config', default=default_config())
    parser.add_argument('--fw-dir', help='default: fw_dir from the config, else fw/ beside it')
    parser.add_argument('--apply', action='store_true', help='write the boards (default: only show)')
    args = parser.parse_args(argv)

    config, connection = connect(args.config)
    fw_dir = args.fw_dir or config.get('fw_dir') or os.path.join(
        os.path.dirname(os.path.abspath(args.config)), 'fw')
    boards = published_boards(fw_dir)
    if not boards:
        print('no readable images in %s' % fw_dir)
        return 1

    cursor = connection.cursor()
    cursor.execute("SHOW COLUMNS FROM `log` LIKE 'board'")
    has_column = cursor.fetchone() is not None
    if args.apply and not has_column:
        print('log.board does not exist: run migrations/2026-11-01_log_board.sql first')
        return 1
    if not has_column:
        print('log.board does not exist yet; showing the plan as if every row were empty\n')

    cursor.execute('SELECT `id`, `imei`, `name` FROM `device` ORDER BY `id`')
    devices = [dict(zip(('id', 'imei', 'name'), row)) for row in cursor.fetchall()]

    statements = 0
    written = 0
    for device in devices:
        known = {version: board for (imei, version), board in boards.items()
                 if imei == device['imei']}
        if not known:
            continue
        cursor.execute(
            'SELECT `id`, `fw`, %s FROM `log` WHERE `device_id` = %%s ORDER BY `id`'
            % ('`board`' if has_column else 'NULL'), (device['id'],))
        rows = cursor.fetchall()
        updates, tally = plan(rows, known)
        report(device, len(rows), tally)
        if args.apply:
            for board, first, last in updates:
                statements += 1
                written += cursor.execute(
                    'UPDATE `log` SET `board` = %s WHERE `device_id` = %s '
                    'AND `id` BETWEEN %s AND %s AND `board` IS NULL',
                    (board, device['id'], first, last))
    if args.apply:
        connection.commit()
        print('\n%s rows written in %d statements' % (format(written, ','), statements))
    else:
        print('\nnothing written; --apply fills these')
    connection.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
