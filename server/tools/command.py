#!/usr/bin/env python3
"""Queue a command for a device.

    tools/command.py <imei> <command> [command...]
    tools/command.py 350000000000000 int=3600
    tools/command.py 350000000000000 locate

Nothing is pushed.  The device is asleep almost all of the time, so the command
waits until it next reads a reply and rides back on it.  A parked unit reads one
on each timed engine-off wake, so with no engine-off interval set a command can
wait until the ignition next comes on.

Commands the firmware carries out
---------------------------------
    int=<seconds>          engine-off reporting interval; 0 = no timed wakes
    movealarm=0|1          set the movement alarm flag (stored and reported
                           back; firmware 0.4.x raises movement alerts either way)
    locate                 alert with the last known position
    locatenow              take and send a position, then alert with it
    tomtom / tomtomnow     as locate / locatenow, as a TomTom link
    config                 alert with firmware, settings, battery, ignition, uptime
    movereset              reset the movement alert cooldown
    reboot                 restart the device
    fota                   check for an update now, clearing the device's own
                           block and retry holdoff
    fota-retry             clear the version withheld on the server, then queue fota

Settings the server acts on, applied immediately and never sent to the device:

    alarm=0|1              notify when the ignition comes on
    garage=0|1             expected to be moved; urgent alerts are demoted
    overnightalarm=0|1     notify when the ignition comes on overnight
    overnight_alarm_hour_from=0-23
    overnight_alarm_hour_to=0-23

Track mode is switched from the map page or POST /api/1.0/trackmode rather than
queued here: every reply already carries the server's switch, so a queued
track= would be undone by the next one.

fota-retry is both halves of a manual retry: it clears the version withheld
from this device after a failed update, then queues the bare `fota` command,
which is what tells the device to drop its own block and check immediately
rather than wait for the next advert.
"""

import sys

import requests

import _bootstrap  # noqa: F401

from tracker import config, db


def fota_retry(imei):
    """Clear a withheld firmware version so the device may try it again."""
    row = db.web.one('SELECT `fw_blocked`, `fw_fail_count` FROM `device` '
                     'WHERE `imei` = %s', (imei,))
    if row is None:
        print('no device with imei %s' % imei, file=sys.stderr)
        return False
    if row['fw_blocked']:
        print('%s: clearing block on %s (%d failed attempt(s))'
              % (imei, row['fw_blocked'], row['fw_fail_count']))
    else:
        print('%s: nothing withheld' % imei)
    db.web.query('UPDATE `device` SET `fw_blocked` = NULL, `fw_fail_count` = 0, '
                 '`fw_staged` = NULL, `fw_staged_at` = NULL WHERE `imei` = %s',
                 (imei,))
    return True


def main():
    if len(sys.argv) < 3:
        print((__doc__ or '').strip(), file=sys.stderr)
        return 1

    imei = sys.argv[1]

    if sys.argv[2] == 'fota-retry':
        if not fota_retry(imei):
            return 1
        command = 'fota'
    else:
        command = ','.join(sys.argv[2:])

    row = db.web.one('SELECT `token` FROM `api_token` ORDER BY `id` LIMIT 1')
    if not row:
        print('no token in api_token - run tools/gentoken.py first', file=sys.stderr)
        return 1

    base = config.get('api_base', 'http://127.0.0.1:5000')
    try:
        response = requests.post(
            base + '/api/1.0/command',
            json={'imei': imei, 'command': command},
            headers={'Authorization': 'Bearer %s' % row['token']},
            timeout=10,
        )
    except requests.RequestException as e:
        print('request failed: %s' % e, file=sys.stderr)
        return 1

    try:
        data = response.json()
    except ValueError:
        print('invalid response (HTTP %d): %s'
              % (response.status_code, response.text[:200]), file=sys.stderr)
        return 1

    if data.get('status') == 'ok':
        print('queued: %s' % command)
        return 0

    print('error: %s' % data.get('message', 'unknown'), file=sys.stderr)
    return 1


if __name__ == '__main__':
    sys.exit(main())
