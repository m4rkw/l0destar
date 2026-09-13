"""Minimal MySQL wrapper.

One connection per thread.  pymysql connections are not thread-safe, and this
process runs a good many threads: the UDP listener, one per TLS connection
downloading firmware, and a pool of request threads in each gunicorn worker.
So the module-level handles below hand every thread its own connection rather
than sharing a pool — the query volume is a few per device per wake, and a
pool would be more moving parts than the load justifies.
"""

import threading

import pymysql
import pymysql.cursors

from . import config


class DB:
    def __init__(self, settings=None):
        self._settings = settings or config.DATABASE
        self._conn = None

    def _dial(self):
        return pymysql.connect(
            host=self._settings.get('host', '127.0.0.1'),
            port=int(self._settings.get('port', 3306)),
            database=self._settings['database'],
            user=self._settings['user'],
            password=self._settings.get('password', ''),
            cursorclass=pymysql.cursors.DictCursor,
            autocommit=True,
        )

    def _connect(self):
        # A device wakes, sends, and sleeps for an hour; the connection has
        # usually been idle long enough for the server to have dropped it. Ping
        # first and redial on failure rather than discovering it mid-INSERT
        # while the device is holding its radio open waiting for the response.
        if self._conn is not None:
            try:
                self._conn.ping(reconnect=False)
            except Exception:
                self.close()
        if self._conn is None:
            self._conn = self._dial()
        return self._conn

    def close(self):
        """Drop the handle; the next call dials a fresh connection.

        Called after fork() — a connection inherited across fork() is the same
        TCP socket in two processes, which confuses client and server alike.
        """
        if self._conn is not None:
            try:
                self._conn.close()
            except Exception:
                pass
            self._conn = None

    def one(self, sql, params=None):
        with self._connect().cursor() as cur:
            cur.execute(sql, params)
            return cur.fetchone()

    def all(self, sql, params=None):
        with self._connect().cursor() as cur:
            cur.execute(sql, params)
            return cur.fetchall()

    def query(self, sql, params=None):
        with self._connect().cursor() as cur:
            cur.execute(sql, params)
            return cur.lastrowid


class PerThread:
    """A :class:`DB` for each thread that queries through this handle.

    The module-level handles are shared by everything that imports them.
    Shared as plain connections, two request threads in one worker — or two
    TLS connections downloading firmware at once — interleave their queries on
    one socket and corrupt both result sets.  Each thread dials its own
    connection the first time it queries instead, and keeps it for its life.
    """

    def __init__(self, settings=None):
        self._settings = settings
        self._local = threading.local()

    def handle(self):
        """The calling thread's :class:`DB`, created on first use."""
        handle = getattr(self._local, 'db', None)
        if handle is None:
            handle = self._local.db = DB(self._settings)
        return handle

    def close(self):
        """Close the calling thread's connection, if it has one.  Other
        threads' connections are theirs to close."""
        handle = getattr(self._local, 'db', None)
        if handle is not None:
            handle.close()

    def one(self, sql, params=None):
        return self.handle().one(sql, params)

    def all(self, sql, params=None):
        return self.handle().all(sql, params)

    def query(self, sql, params=None):
        return self.handle().query(sql, params)


# One handle per consumer, each a connection per thread underneath.
web = PerThread()
udp = PerThread()
tls = PerThread()

ALL = (web, udp, tls)


def close_all():
    """Close the calling thread's connection on every handle.

    Called after fork(), where the forking thread is the only one the child
    has: its connections are the ones inherited from the parent.
    """
    for handle in ALL:
        handle.close()


def lookup_device(database=None, imei=None, device_id=None):
    """Resolve a device by IMEI or row id.  Returns None if neither matches."""
    database = database or web
    if imei:
        return database.one('SELECT * FROM `device` WHERE `imei` = %s', (str(imei),))
    if device_id is not None:
        return database.one('SELECT * FROM `device` WHERE `id` = %s', (device_id,))
    return None


def lookup_operator(mcc, mnc, database=None):
    """Resolve an mcc/mnc pair to a carrier name via the `plmn` table.

    The table is optional — an empty one just means the UI shows no operator
    name.  ``tools/import_plmn.py`` loads it from a public MCC/MNC list.
    """
    if mcc in (None, '') or mnc in (None, ''):
        return None
    database = database or web
    try:
        row = database.one(
            'SELECT `operator` FROM `plmn` WHERE `mcc` = %s AND `mnc` = %s LIMIT 1',
            (mcc, mnc),
        )
    except Exception:
        return None
    return row['operator'] if row else None
