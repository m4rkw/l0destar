#!/usr/bin/env python3
"""Fill the device event log (device_event) with what the server already
holds about each unit, from before it recorded events itself.

Three places still say something:

  * the device's log rows, read in time order of the unit's own clock: an
    ignition on or off wherever the state differs from the row before, a
    reboot for each row that carries a reset cause, and a firmware change
    wherever the version differs from the last one reported (named "restored"
    when it went back to an older one).  These carry a key built from the row,
    exactly as the server builds it when the row arrives, so a row that was
    recorded live is not added again;
  * the app log (--app-log): the lines that say a firmware update was staged,
    that commands were handed to the unit, and the alerts it raised, each at
    the time of its line.  A GNSS backup supply alert is a setting event here:
    the log table does not hold the flips the unit reports in bk=, only the
    newest.  These lines have no key of their own, so each gets one from its
    time and text, which makes a second run add nothing, and only the lines
    older than --before are taken, so that nothing is added twice once the
    server records them itself;
  * the current-state tables: the latest of each report a unit has answered
    (device_report) and its latest network scan, each as a result, before the
    same time.  The device keeps only the latest of each, so that is all there is.

Nothing else was ever recorded.  Commands queued, cancelled or replaced,
settings changed, track mode switched and fault codes raised left no trace
anywhere, so there is nothing to restore for them.  A log row with no device
time (from before the column existed) cannot be keyed and is not read.

    python backfill_device_events.py --before '2026-11-06 09:00:00'
                                                show what would be added
    python backfill_device_events.py --before '2026-11-06 09:00:00' --apply
                                                add it

--before is the time the server started recording events itself: it is
required with --apply, and in a dry run it defaults to now.  --device (more
than once if need be) limits the run to those device ids, and --app-log names
the log to read (default /var/log/tracker/app.log; one that is missing is
reported and skipped).  It can be run again: an event that is already there is
left as it is.

It reads the server's config.yaml (TRACKER_CONFIG, else the one in the current
directory, else the one beside tools/), or the file named with --config.
device_event must exist first (migrations/2026-11-06_device_event.sql).
"""

import argparse
import datetime
import os
import re
import sys

from backfill_board import connect, default_config

try:
    import device_events
except ImportError:
    # Run from the repository: the module is in the package, and importing the
    # bootstrap first puts the package on the path.
    import importlib
    importlib.import_module('_bootstrap')
    from tracker import device_events

DEFAULT_APP_LOG = '/var/log/tracker/app.log'
MIGRATION = 'migrations/2026-11-06_device_event.sql'

# Log rows are read this many at a time, by key, so a device with a hundred
# thousand of them is never held whole.
CHUNK = 20000
# Rows inserted per statement.
BATCH = 500

INSERT = ('INSERT INTO `device_event` (`device_id`, `at`, `type`, `text`, `ref`) '
          'VALUES (%s, %s, %s, %s, %s) ON DUPLICATE KEY UPDATE `id` = `id`')

# An event is (at, kind, text, ref).


# -- the log table ---------------------------------------------------------------

def plan_log(rows):
    """Events from a device's log rows, [(id, timestamp, gsm_timestamp,
    ignition_state, rst, fw)] in order of gsm_timestamp then id.  A generator,
    so that the rows can be read as they are needed."""
    state = None
    version = None
    for _, timestamp, gsm, ignition, rst, fw in rows:
        if ignition is not None:
            ignition = int(ignition) != 0
            if state is not None and ignition != state:
                yield (timestamp, 'ignition', device_events.ignition_text(ignition),
                       device_events.ref('ign', gsm))
            state = ignition
        if rst:
            yield (timestamp, 'reboot', device_events.reset_text(rst),
                   device_events.ref('rst', gsm))
        if fw:
            if version is not None and fw != version:
                yield (timestamp, 'firmware',
                       device_events.firmware_text('applied', fw, version),
                       device_events.ref('fw', fw, gsm))
            version = fw


def read_log(cursor, device_id, chunk=CHUNK):
    """The device's log rows with only the columns plan_log reads, in order,
    fetched `chunk` at a time."""
    columns = ('SELECT `id`, `timestamp`, `gsm_timestamp`, `ignition_state`, '
               '`rst`, `fw` FROM `log` WHERE `device_id` = %s ')
    order = 'ORDER BY `gsm_timestamp`, `id` LIMIT %s'
    last = None
    while True:
        if last is None:
            cursor.execute(columns + 'AND `gsm_timestamp` IS NOT NULL ' + order,
                           (device_id, chunk))
        else:
            cursor.execute(
                columns + 'AND (`gsm_timestamp` > %s OR (`gsm_timestamp` = %s '
                'AND `id` > %s)) ' + order,
                (device_id, last[0], last[0], last[1], chunk))
        rows = cursor.fetchall()
        yield from rows
        if len(rows) < chunk:
            return
        last = (rows[-1][2], rows[-1][0])


# -- the app log -----------------------------------------------------------------

# '2026-11-04 13:05:43,326 - INFO - <message>'
LINE_RE = re.compile(r'^(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d),(\d{3}) - \w+ - (.*?)\s*$')
# The unit as the line names it: its IMEI, then the name it had at the time
# ([l0destar], [LK56AAF], or none), which is not used.
UNIT = r'(\d{15})(?:\[[^\]]*\])?'
STAGED_RE = re.compile(UNIT + r' staged (\S+)$')
DELIVERED_RE = re.compile(r'delivered \d+ commands to ' + UNIT + r': (.*)$')
ALERT_RE = re.compile(r'alert from ' + UNIT + r' \(pri=-?\d+\): (.*)$')
BACKUP_RE = re.compile(r'gnss backup supply (ON|OFF)$', re.IGNORECASE)


def app_event_ref(written, text):
    return device_events.ref('app', written, text[:40])


def parse_app_line(line):
    """(imei, time, events) for a line of the app log that says something
    about a unit, else None.  time is a datetime with the line's milliseconds;
    events is [(kind, text, ref)]."""
    m = LINE_RE.match(line.rstrip('\r\n'))
    if not m:
        return None
    written = '%s,%s' % (m.group(1), m.group(2))
    when = datetime.datetime.strptime(m.group(1), '%Y-%m-%d %H:%M:%S').replace(
        microsecond=int(m.group(2)) * 1000)
    message = m.group(3)

    found = []
    m = STAGED_RE.match(message)
    if m:
        imei = m.group(1)
        found.append(('firmware', device_events.firmware_text('staged', m.group(2))))
    else:
        m = DELIVERED_RE.match(message)
        if m:
            imei = m.group(1)
            for command in device_events.command_parts(m.group(2)):
                found.append(('command', device_events.command_text('collected', command)))
        else:
            m = ALERT_RE.match(message)
            if not m:
                return None
            imei = m.group(1)
            backup = BACKUP_RE.match(m.group(2).strip())
            if backup:
                # The alert is the only place the unit's own flip is kept; live,
                # its bk= report records it instead.
                text = device_events.backup_supply_text(backup.group(1).upper() == 'ON')
                return imei, when, [('setting', text,
                                     device_events.ref('bk', 'alert', written))]
            event = device_events.alert_event(m.group(2))
            if event:
                found.append(event)
    return imei, when, [(kind, text, app_event_ref(written, text))
                        for kind, text in found]


def plan_app_log(lines, imeis, before):
    """{imei: [event]} for the lines older than `before` that are about one of
    `imeis`.  Each event is (at, kind, text, ref)."""
    out = {}
    for line in lines:
        parsed = parse_app_line(line)
        if parsed is None:
            continue
        imei, when, events = parsed
        if imei not in imeis or not when < before:
            continue
        if events:
            out.setdefault(imei, []).extend(
                (when, kind, text, ref) for kind, text, ref in events)
    return out


# -- the current-state tables ------------------------------------------------------

def plan_state(reports, scan_at, before):
    """Events from a device's latest answers, [(name, at)], and the time of its
    latest network scan (or None); only those older than `before`."""
    events = []
    for name, at in reports:
        if at is not None and at < before:
            events.append((at, 'result', device_events.result_text(name),
                           device_events.ref('rep', name, at)))
    if scan_at is not None and scan_at < before:
        events.append((scan_at, 'result', device_events.result_text('scan'),
                       device_events.ref('scan', scan_at)))
    return events


# -- running it --------------------------------------------------------------------

def tally(events):
    """{kind: count} in the order the app lists the kinds."""
    counts = {}
    for _, kind, _, _ in events:
        counts[kind] = counts.get(kind, 0) + 1
    return {kind: counts[kind] for kind in device_events.KINDS if kind in counts}


def show(events, n=3):
    """The first and last `n` events of each kind, with their times."""
    for kind in tally(events):
        mine = [e for e in events if e[1] == kind]
        head, tail = (mine[:n], mine[-n:]) if len(mine) > 2 * n else (mine, [])
        for at, _, text, _ in head:
            print('    %s  %-8s %s' % (device_events.stamp(at), kind, text))
        if tail:
            print('    ... %d more' % (len(mine) - 2 * n))
            for at, _, text, _ in tail:
                print('    %s  %-8s %s' % (device_events.stamp(at), kind, text))


def has_table(cursor, name):
    cursor.execute('SHOW TABLES LIKE %s', (name,))
    return cursor.fetchone() is not None


def has_log(cursor, device_id):
    cursor.execute('SELECT 1 FROM `log` WHERE `device_id` = %s LIMIT 1', (device_id,))
    return cursor.fetchone() is not None


def has_columns(cursor, table, names):
    cursor.execute('SHOW COLUMNS FROM `%s`' % table)
    present = {row[0] for row in cursor.fetchall()}
    return all(name in present for name in names)


def insert(cursor, device_id, events):
    """Add the events, oldest first, in batches; how many were new."""
    added = 0
    for start in range(0, len(events), BATCH):
        cursor.executemany(INSERT, [
            (device_id, device_events.stamp(at), kind,
             text[:device_events.TEXT_MAX], ref)
            for at, kind, text, ref in events[start:start + BATCH]])
        added += cursor.rowcount
    return added


def backfill(connection, apply, device_ids, app_log, before):
    cursor = connection.cursor()

    has_event = has_table(cursor, 'device_event')
    if apply and not has_event:
        print('run %s first' % MIGRATION)
        return 1
    if not has_event:
        print('device_event does not exist yet (%s); showing the plan\n' % MIGRATION)

    cursor.execute('SELECT `id`, `imei`, `name` FROM `device` ORDER BY `id`')
    devices = [dict(zip(('id', 'imei', 'name'), row)) for row in cursor.fetchall()]

    if device_ids:
        known = {device['id'] for device in devices}
        missing = [i for i in device_ids if i not in known]
        if missing:
            print('no such device: %s' % ', '.join(str(i) for i in missing))
            return 1
        devices = [device for device in devices if device['id'] in device_ids]
    else:
        # Every device that has a log.
        devices = [device for device in devices if has_log(cursor, device['id'])]

    scans = {}
    if has_columns(cursor, 'device', ('net_scan_at',)):
        cursor.execute('SELECT `id`, `net_scan_at` FROM `device`')
        scans = dict(cursor.fetchall())
    reports = {}
    if has_table(cursor, 'device_report'):
        cursor.execute('SELECT `device_id`, `name`, `text`, `at` FROM `device_report`')
        for device_id, name, _, at in cursor.fetchall():
            reports.setdefault(device_id, []).append((name, at))

    from_app = {}
    if os.path.exists(app_log):
        with open(app_log, errors='replace') as handle:
            from_app = plan_app_log(handle, {device['imei'] for device in devices}, before)
    else:
        print('%s does not exist: no app log lines to read\n' % app_log)

    total = added = 0
    for device in devices:
        events = list(plan_log(read_log(cursor, device['id'])))
        events += from_app.get(device['imei'], [])
        events += plan_state(reports.get(device['id'], []), scans.get(device['id']), before)
        events.sort(key=lambda event: event[0])
        total += len(events)

        counts = tally(events)
        print('%s (%s): %d events%s' % (
            device['name'] or '-', device['imei'], len(events),
            ' (%s)' % ', '.join('%s %d' % item for item in counts.items()) if counts else ''))
        if apply:
            added += insert(cursor, device['id'], events)
            connection.commit()
        else:
            show(events)

    if apply:
        print('\n%d events found, %d added' % (total, added))
    else:
        print('\n%d events found; nothing written; --apply adds them' % total)
    return 0


def parse_time(text):
    try:
        return datetime.datetime.strptime(text, '%Y-%m-%d %H:%M:%S')
    except ValueError:
        raise argparse.ArgumentTypeError(
            "%r is not a time of the form 'YYYY-MM-DD HH:MM:SS'" % text)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('--config', default=default_config())
    parser.add_argument('--apply', action='store_true', help='add the events (default: only show)')
    parser.add_argument('--device', type=int, action='append', metavar='ID',
                        help='a device id to fill (more than once for several; '
                        'default: every device that has log rows)')
    parser.add_argument('--app-log', default=DEFAULT_APP_LOG, metavar='PATH',
                        help='default: %(default)s')
    parser.add_argument('--before', type=parse_time, metavar='YYYY-MM-DD HH:MM:SS',
                        help='only events older than this are taken from the app log '
                        'and the current-state tables; required with --apply')
    args = parser.parse_args(argv)
    if args.apply and args.before is None:
        parser.error('--before is required with --apply: give the time the server '
                     'started recording events itself, so none are added twice')
    before = args.before or datetime.datetime.now().replace(microsecond=0)

    # Closed whatever happens: the open transaction would otherwise hold a
    # lock on the table against anything that wants to alter it.
    _, connection = connect(args.config)
    try:
        return backfill(connection, args.apply, args.device, args.app_log, before)
    finally:
        connection.close()


if __name__ == '__main__':
    sys.exit(main())
