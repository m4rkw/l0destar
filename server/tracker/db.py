"""Minimal MySQL wrapper.

One :class:`DB` per thread.  pymysql connections are not thread-safe and the
listeners run concurrently with the Flask workers, so each gets its own handle
rather than sharing a pool — the query volume is a few per device per wake, and
a pool would be more moving parts than the load justifies.
"""

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


# One handle per thread of execution.
web = DB()
udp = DB()
tls = DB()
dtls = DB()

ALL = (web, udp, tls, dtls)


def close_all():
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
