#!/usr/bin/env python3
"""Look after enrolled devices.

    tools/device.py list
    tools/device.py show <imei>
    tools/device.py rename <imei> <name> [registration]
    tools/device.py rekey <imei> [--psk <64 hex characters>]
    tools/device.py remove <imei> --yes

Enrolment itself is tools/adddevice.py; this is everything after it.

rekey replaces a device's pre-shared key and prints the new one, once.  From
that moment the server accepts only datagrams sealed with the new key, so the
unit needs firmware built with it.  On the bench, reflash it.  For a unit in
the field, publish an image built with a key of your choosing while the old
key still works, wait for the unit to report the new version, then run rekey
with --psk and that key; telemetry sent in between is lost.

remove deletes the device and everything recorded for it - positions,
journeys, fault codes, queued commands - and wants --yes, because there is no
undo.  A firmware manifest published for it is left for you to delete.
"""

import argparse
import os
import re
import secrets
import sys

import _bootstrap  # noqa: F401

from tracker import db, firmware

PSK_RE = re.compile(r'^[0-9a-fA-F]{64}$')


def _find(imei):
    device = db.lookup_device(imei=imei)
    if device is None:
        print('no device with imei %s' % imei, file=sys.stderr)
    return device


def _latest(device):
    return db.web.one(
        'SELECT * FROM `log` WHERE `device_id` = %s ORDER BY `id` DESC LIMIT 1',
        (device['id'],),
    ) or {}


def _when(stamp):
    return stamp.strftime('%Y-%m-%d %H:%M:%S') if stamp else 'never'


def _volts(value):
    return '-' if value is None else '%.2fV' % value


def _ignition(log):
    if not log:
        return '-'
    return 'on' if log.get('ignition_state') == 1 else 'off'


def list_devices(args):
    devices = db.web.all('SELECT * FROM `device` ORDER BY `name`, `id`')
    if not devices:
        print('no devices enrolled - see tools/adddevice.py')
        return 0

    rows = [('imei', 'name', 'registration', 'last seen', 'firmware',
             'battery', 'ignition')]
    for device in devices:
        log = _latest(device)
        rows.append((
            device['imei'],
            device['name'] or '-',
            device['registration'] or '-',
            _when(log.get('timestamp')),
            log.get('fw') or '-',
            _volts(log.get('battery_level')),
            _ignition(log),
        ))
    widths = [max(len(row[i]) for row in rows) for i in range(len(rows[0]))]
    for row in rows:
        print('  '.join(value.ljust(width)
                        for value, width in zip(row, widths)).rstrip())
    return 0


def show(args):
    device = _find(args.imei)
    if device is None:
        return 1
    log = _latest(device)

    fields = [
        ('registration', device['registration'] or '-'),
        ('enrolled', _when(device['created_at'])),
        # Only whether there is one: the key is printed once, at enrolment.
        ('pre-shared key', 'set' if device['psk'] else 'not set'),
        ('engine-off interval',
         '%ds' % device['int'] if device['int'] else '0 (no timed wakes)'),
        ('movement alarm', device['movement_alarm']),
        ('track mode', device['track_mode']),
        ('ignition alarm', device['alarm']),
        ('garage', device['garage']),
        ('overnight alarm', '%s, %s:00 to %s:00' % (
            device['overnight_alarm'], device['overnight_alarm_hour_from'],
            device['overnight_alarm_hour_to'])),
        ('published firmware', firmware.latest_version(device['imei']) or '-'),
        ('staged update', device['fw_staged'] or '-'),
        ('withheld update', '%s, after %d failed attempt(s)' % (
            device['fw_blocked'], device['fw_fail_count'])
         if device['fw_blocked'] else '-'),
    ]
    if log:
        network = [db.lookup_operator(log.get('mcc'), log.get('mnc')),
                   log.get('rat')]
        fields += [
            ('last seen', _when(log.get('timestamp'))),
            ('position', '%s, %s' % (log.get('latitude'), log.get('longitude'))),
            ('battery', _volts(log.get('battery_level'))),
            ('ignition', _ignition(log)),
            ('firmware', log.get('fw') or '-'),
            ('network', ' '.join(v for v in network if v) or '-'),
        ]
    else:
        fields.append(('last seen', 'never'))

    commands = db.web.all(
        'SELECT `command` FROM `command` WHERE `device_id` = %s ORDER BY `id`',
        (device['id'],),
    )
    fields.append(('queued commands',
                   ', '.join(c['command'] for c in commands) or 'none'))

    print('%s (%s)' % (device['name'] or '-', device['imei']))
    width = max(len(name) for name, _ in fields)
    for name, value in fields:
        print('  %s  %s' % (name.ljust(width), value))
    return 0


def rename(args):
    device = _find(args.imei)
    if device is None:
        return 1
    name = args.name.strip()
    if not name:
        print('the name must not be empty', file=sys.stderr)
        return 1
    if args.registration is None:
        registration = device['registration']
    else:
        registration = args.registration.strip() or None

    db.web.query(
        'UPDATE `device` SET `name` = %s, `registration` = %s WHERE `id` = %s',
        (name, registration, device['id']),
    )
    print('%s is now %s%s' % (device['imei'], name,
                              ' (%s)' % registration if registration else ''))
    return 0


def rekey(args):
    device = _find(args.imei)
    if device is None:
        return 1
    if args.psk is not None and not PSK_RE.match(args.psk):
        print('--psk must be 64 hex characters (32 bytes)', file=sys.stderr)
        return 1

    psk = (args.psk or secrets.token_hex(32)).lower()
    db.web.query('UPDATE `device` SET `psk` = %s WHERE `id` = %s',
                 (psk, device['id']))

    print('rekeyed %s (%s)' % (device['name'] or '-', device['imei']))
    print('psk: %s' % psk)
    print()
    print('The server accepts only this key from now on.  Build it into the '
          "unit's firmware; it is not shown again.")
    return 0


def remove(args):
    device = _find(args.imei)
    if device is None:
        return 1
    records = db.web.one(
        'SELECT COUNT(*) AS `n` FROM `log` WHERE `device_id` = %s',
        (device['id'],),
    )['n']
    label = '%s (%s)' % (device['name'] or '-', device['imei'])

    if not args.yes:
        print('this deletes %s with its %d record(s), journeys, fault codes and '
              'queued commands, and there is no undo; run again with --yes'
              % (label, records), file=sys.stderr)
        return 1

    # The log, journey, dtc and command rows go with it (ON DELETE CASCADE).
    db.web.query('DELETE FROM `device` WHERE `id` = %s', (device['id'],))
    print('removed %s and %d record(s)' % (label, records))

    manifest = firmware.manifest_path(device['imei'])
    if manifest and os.path.exists(manifest):
        print('%s is still published: delete it, and the image it names, if '
              'the unit is gone for good' % manifest)
    return 0


ACTIONS = {
    'list': list_devices,
    'show': show,
    'rename': rename,
    'rekey': rekey,
    'remove': remove,
}


def main(argv=None):
    parser = argparse.ArgumentParser(
        prog='device.py',
        description='Look after enrolled devices.',
        epilog='Enrol new devices with tools/adddevice.py.',
    )
    actions = parser.add_subparsers(dest='action', required=True)

    actions.add_parser('list', help='every device and when it last reported')

    show_parser = actions.add_parser(
        'show', help='settings, update state, last record and queued commands')
    show_parser.add_argument('imei')

    rename_parser = actions.add_parser(
        'rename', help='change the name shown in the UI and in alerts')
    rename_parser.add_argument('imei')
    rename_parser.add_argument('name')
    rename_parser.add_argument('registration', nargs='?',
                               help='vehicle registration; unchanged if omitted')

    rekey_parser = actions.add_parser(
        'rekey', help='replace the pre-shared key and print it')
    rekey_parser.add_argument('imei')
    rekey_parser.add_argument('--psk',
                              help='set this key (64 hex characters) instead '
                                   'of generating one')

    remove_parser = actions.add_parser(
        'remove', help='delete the device and everything recorded for it')
    remove_parser.add_argument('imei')
    remove_parser.add_argument('--yes', action='store_true',
                               help='confirm: there is no undo')

    args = parser.parse_args(argv)
    return ACTIONS[args.action](args)


if __name__ == '__main__':
    sys.exit(main())
