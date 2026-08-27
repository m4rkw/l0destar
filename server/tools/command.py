#!/usr/bin/env python3
"""Queue a command for a device.

    tools/command.py <imei> <command> [command...]
    tools/command.py 350000000000000 int=3600
    tools/command.py 350000000000000 locate

Nothing is pushed.  The device is asleep almost all of the time, so the command
waits until it next reports and rides back on the response it was already going
to receive.  At the default reporting interval that can be an hour away.

Commands
--------
    int=<seconds>          reporting interval
    movealarm=0|1          wake and report on accelerometer trigger
    alwayson=0|1           never enter deep sleep (mains-powered installs)
    locate                 report position on next check-in
    locatenow              wake and report position immediately
    config                 report current settings
    reboot                 restart the device
    poweroff               enter deep sleep until externally woken

    alarm=0|1              notify on ignition on          (server-side)
    garage=0|1             expected to be moved           (server-side)
    overnightalarm=0|1     notify on overnight ignition   (server-side)
    overnight_alarm_hour_from=0-23                        (server-side)
    overnight_alarm_hour_to=0-23                          (server-side)

Server-side settings are applied immediately and never reach the device.
"""

import sys

import requests

import _bootstrap  # noqa: F401

from tracker import config, db


def main():
    if len(sys.argv) < 3:
        print((__doc__ or '').strip(), file=sys.stderr)
        return 1

    imei = sys.argv[1]
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
