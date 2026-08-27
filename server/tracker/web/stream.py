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
            row = handle.one(
                'SELECT * FROM `log` WHERE `device_id` = %s AND `id` > %s '
                'ORDER BY `id` DESC LIMIT 1',
                (device['id'], last_id),
            )

            if row:
                last_id = row['id']
                ws.send(json.dumps(devices.position(row, database=handle),
                                   separators=(',', ':')))
                last_send = time.time()
            elif time.time() - last_send >= PING_INTERVAL:
                # The client drops and reconnects if it hears nothing for a
                # while, which is how it recovers from a proxy silently
                # dropping an idle connection.  Keep it fed.
                ws.send('{"ping":true}')
                last_send = time.time()

            time.sleep(POLL_INTERVAL)
    except Exception:
        logs.app.debug('websocket closed', exc_info=True)
    finally:
        handle.close()
