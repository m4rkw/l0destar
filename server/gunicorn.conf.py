"""Gunicorn configuration.

Two things here are load-bearing rather than tuning.
"""

import os

# Stamp the master pid before gunicorn preloads the app, so wsgi.py can tell
# the master apart from any process that later inherits this environment
# (workers, and anything they fork).  This has to happen at config-load time:
# the on_starting hook fires after preload, by which point wsgi.py has already
# been imported and its guard has already run.
os.environ['GUNICORN_MASTER_PID'] = str(os.getpid())

bind = os.environ.get('TRACKER_BIND', '127.0.0.1:5000')
workers = int(os.environ.get('TRACKER_WORKERS', '2'))

# Import the app once in the master before forking, so the listener threads
# start in the master only.  Each worker then inherits the state without
# re-running module-level code, so the threads are neither duplicated per
# worker nor recreated when a worker recycles.
preload_app = True

# Long-lived WebSocket handlers sit in a poll loop and cannot drain on
# shutdown, so the default 30s graceful timeout holds the listen socket long
# enough for a restarting instance to fail its bind and bounce under a
# supervisor's restart policy.  Cut workers fast — browsers reconnect
# WebSockets on their own and ordinary HTTP requests are short.
graceful_timeout = 2


def post_fork(server, worker):
    """Drop database handles inherited across fork().

    The master opens a connection at import time.  A worker that inherits it
    is sharing one TCP socket between two processes, which confuses the client
    and the server alike — the symptom is the worker's first ping() stalling
    for the full TCP timeout before it reconnects.
    """
    del server, worker
    from tracker import db
    db.close_all()
