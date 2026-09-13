"""Database handles are per thread.

pymysql connections are not thread-safe, and the server queries from many
threads at once: gunicorn's request threads, the WebSocket streams, and one
thread per TLS connection.  Two of them sharing a connection interleave their
queries on one socket.
"""

import threading

from conftest import needs_db

from tracker import db


def test_every_module_handle_is_per_thread():
    for handle in db.ALL:
        assert isinstance(handle, db.PerThread)


def test_each_thread_gets_its_own_connection_handle():
    handle = db.PerThread()
    seen = []
    thread = threading.Thread(target=lambda: seen.append(handle.handle()))
    thread.start()
    thread.join()

    mine = handle.handle()
    assert mine is handle.handle()
    assert seen[0] is not mine


def test_close_only_touches_the_calling_thread():
    handle = db.PerThread()
    theirs = []
    ready = threading.Event()
    done = threading.Event()

    def other():
        theirs.append(handle.handle())
        theirs[0]._conn = 'open'        # stands in for a live connection
        ready.set()
        done.wait(5)

    thread = threading.Thread(target=other)
    thread.start()
    ready.wait(5)
    handle.close()                      # nothing of this thread's to close
    assert theirs[0]._conn == 'open'
    done.set()
    thread.join()


@needs_db
def test_threads_do_not_share_a_connection(database):
    ids = []

    def query():
        ids.append(db.web.one('SELECT CONNECTION_ID() AS `id`')['id'])
        db.web.close()

    thread = threading.Thread(target=query)
    thread.start()
    thread.join()
    ids.append(db.web.one('SELECT CONNECTION_ID() AS `id`')['id'])

    assert ids[0] != ids[1]
