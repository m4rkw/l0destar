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
import time

from tracker import logs
from tracker.listeners import tls, udp
from tracker.web import create_app

app = create_app()


def start_listeners():
    ready = []
    for name, target in (('udp', udp.run), ('tls', tls.run)):
        event = threading.Event()
        threading.Thread(target=target, args=(event,), name='listener-%s' % name,
                         daemon=True).start()
        ready.append(event)

    # gunicorn forks the workers as soon as this module has been imported, and
    # a forked worker has only the thread that forked it: a lock any other
    # thread holds at that moment stays held in the worker for good.  A worker
    # forked while a listener was still loading its certificate or binding
    # could hang before its first heartbeat, until gunicorn killed it.  Once
    # bound, the listeners wait in accept() and recvfrom() holding nothing.
    # The wait is bounded because a port the previous instance has not yet
    # released keeps a listener retrying for longer than the web application
    # should be held up.
    deadline = time.monotonic() + 10
    for event in ready:
        event.wait(max(0, deadline - time.monotonic()))
    logs.app.info('listener threads started in pid %d', os.getpid())


# GUNICORN_MASTER_PID is stamped by gunicorn.conf.py at config-load time,
# before preload imports this module.  Running standalone (flask run, or
# python wsgi.py) there is no master, so start them here.
_master = os.environ.get('GUNICORN_MASTER_PID')
if _master is None or _master == str(os.getpid()):
    start_listeners()


if __name__ == '__main__':
    app.run(host='127.0.0.1', port=5000, debug=False)
