"""WSGI entrypoint.

The listener threads belong to exactly one process.  Under gunicorn with
``preload_app``, this module is imported once in the master before any worker
is forked, so the guard below starts them there and the workers inherit the
state without re-running it.  Starting them per worker would mean N processes
racing to bind the same port, N copies of the nonce replay window, and N
notifications per alert.
"""

import os
import threading

from tracker import logs
from tracker.listeners import dtls, tls, udp
from tracker.web import create_app

app = create_app()


def start_listeners():
    for name, target in (('udp', udp.run), ('tls', tls.run), ('dtls', dtls.run)):
        threading.Thread(target=target, name='listener-%s' % name,
                         daemon=True).start()
    logs.app.info('listener threads started in pid %d', os.getpid())


# GUNICORN_MASTER_PID is stamped by gunicorn.conf.py at config-load time,
# before preload imports this module.  Running standalone (flask run, or
# python wsgi.py) there is no master, so start them here.
_master = os.environ.get('GUNICORN_MASTER_PID')
if _master is None or _master == str(os.getpid()):
    start_listeners()


if __name__ == '__main__':
    app.run(host='127.0.0.1', port=5000, debug=False)
