"""Live position WebSocket.

The map page follows the vehicle without polling.  New rows are found by
watching the log's auto-increment id: the listener threads insert, this loop
notices anything past the last id it sent.  Polling the database once a second
is not elegant, but it needs no coordination between the listener threads and
the web workers — which are separate processes under gunicorn — and one
indexed lookup per second per open map is not a load worth engineering around.
"""

import json
import time

from flask import session

from .. import db, logs
from . import devices, sock

POLL_INTERVAL = 1.0
TRACK_POLL_INTERVAL = 0.25
PING_INTERVAL = 10.0


@sock.route('/ws/carpos')
def carpos(ws):
    if 'username' not in session:
        ws.close(1008, 'unauthorised')
        return

    device = devices.from_request()
    if not device:
        ws.close(1008, 'device not found')
        return

    # This handler outlives a request, so it gets its own connection rather
    # than sharing the one the request handlers use.
    handle = db.DB()
    last_id = 0
    last_send = time.time()

    try:
        while ws.connected:
            # The track-mode switch, read fresh on every pass — a primary-key
            # lookup, four times a second at most — so a row is never stamped
            # with a value the page has already moved on from.
            switch = handle.one(
                'SELECT `track_mode` FROM `device` WHERE `id` = %s', (device['id'],))
            track_mode = 1 if switch and switch.get('track_mode') else 0

            # Every row since the last one, oldest first.  Track mode writes
            # two a second, each with its own IMU burst, so skipping to the
            # newest would drop samples.  On first connect only the newest
            # row is wanted.
            if last_id == 0:
                rows = handle.all(
                    'SELECT * FROM `log` WHERE `device_id` = %s '
                    'ORDER BY `id` DESC LIMIT 1',
                    (device['id'],),
                )
            else:
                rows = handle.all(
                    'SELECT * FROM `log` WHERE `device_id` = %s AND `id` > %s '
                    'ORDER BY `id` ASC LIMIT 20',
                    (device['id'], last_id),
                )

            for row in rows:
                last_id = row['id']
                ws.send(json.dumps(
                    devices.position(row, database=handle, track_mode=track_mode),
                    separators=(',', ':')))
                last_send = time.time()
            if not rows and time.time() - last_send >= PING_INTERVAL:
                # The client drops and reconnects if it hears nothing for a
                # while, which is how it recovers from a proxy silently
                # dropping an idle connection.  Keep it fed.  The switch
                # rides on the ping too, so a toggled page with a silent
                # device still changes view.
                ws.send(json.dumps({'ping': True, 'track_mode': track_mode},
                                   separators=(',', ':')))
                last_send = time.time()

            # Track-mode records arrive twice a second; a quarter-second
            # poll keeps the page within a frame of them.
            time.sleep(TRACK_POLL_INTERVAL if track_mode else POLL_INTERVAL)
    except Exception:
        logs.app.debug('websocket closed', exc_info=True)
    finally:
        handle.close()
