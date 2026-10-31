#!/usr/bin/env python3
"""u-blox AssistNow state, per device.

    tools/assistnow.py status
    tools/assistnow.py refresh <imei>
    tools/assistnow.py reset <imei> --yes

status lists every device that has registered a receiver: the receiver's chip
id, when the orbits were last downloaded and what day they run to, when the
next attempt is allowed after a failure, and the last error.  The credential
u-blox issued for a receiver is never shown.

refresh downloads the orbits for one device now, whatever the schedule says.
It uses one of the device's requests to u-blox (an evaluation profile allows
300 a month each, a commercial one 2 a day).

reset forgets a device's registration, orbits and all.  Its next request for
orbits is answered 404 and it registers again, with whichever receiver it has
then: this is what to run when a unit's receiver has been replaced, since a
device is held to the receiver it first registered.

Needs the `assistnow` section of config.yaml (see config.yaml.example) for
refresh; status and reset only need the database.
"""

import argparse
import sys

import _bootstrap  # noqa: F401

from tracker import assistnow, db


def _find(imei):
    device = db.lookup_device(imei=imei)
    if device is None:
        print('no device with imei %s' % imei, file=sys.stderr)
    return device


def status(_args):
    rows = db.web.all(
        'SELECT d.`imei`, d.`name`, a.`uniqid`, (a.`chipcode` IS NOT NULL) AS `registered`, '
        'a.`registered_at`, a.`fetched_at`, a.`blob_until`, a.`blob_days`, a.`next_try_at`, '
        'a.`last_error`, COALESCE(LENGTH(a.`blob`), 0) AS `blob_size` '
        'FROM `device_assist` a JOIN `device` d ON d.`id` = a.`device_id` ORDER BY d.`imei`')
    if not rows:
        print('no device has registered a receiver')
        return 0
    if not assistnow.enabled():
        print('(assistnow is not configured: the server answers /assist/ with 501)\n')
    for r in rows:
        print('%s  %s' % (r['imei'], r['name']))
        print('    receiver   %s  %s' % (r['uniqid'] or '-',
                                       'registered %s' % r['registered_at'] if r['registered'] else 'credential refused: will register again'))
        print('    orbits     %s' % ('%d bytes, %s days, downloaded %s, to %s'
                                    % (r['blob_size'], r['blob_days'], r['fetched_at'], r['blob_until'])
                                    if r['blob_size'] else 'none yet'))
        if r['next_try_at']:
            print('    next try   not before %s' % r['next_try_at'])
        if r['last_error']:
            print('    last error %s' % r['last_error'])
    return 0


def refresh(args):
    device = _find(args.imei)
    if device is None:
        return 1
    if not assistnow.enabled():
        print('assistnow is not configured: no ztp_token in config.yaml', file=sys.stderr)
        return 1
    if assistnow.refresh(device, assistnow.Store(db.web), force=True):
        print('downloaded')
        return 0
    row = assistnow.Store(db.web).get(device['id'])
    if not row or not row['chipcode']:
        print('%s has no registration: it registers itself the next time it asks' % args.imei,
              file=sys.stderr)
    else:
        print('not downloaded: %s' % (row['last_error'] or 'no reason recorded'), file=sys.stderr)
    return 1


def reset(args):
    device = _find(args.imei)
    if device is None:
        return 1
    if not args.yes:
        print('this forgets %s\'s registration and orbits; run again with --yes' % args.imei,
              file=sys.stderr)
        return 1
    assistnow.Store(db.web).reset(device['id'])
    print('reset')
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = parser.add_subparsers(dest='command', required=True)
    sub.add_parser('status').set_defaults(run=status)
    p = sub.add_parser('refresh')
    p.add_argument('imei')
    p.set_defaults(run=refresh)
    p = sub.add_parser('reset')
    p.add_argument('imei')
    p.add_argument('--yes', action='store_true')
    p.set_defaults(run=reset)
    args = parser.parse_args()
    return args.run(args)


if __name__ == '__main__':
    sys.exit(main())
