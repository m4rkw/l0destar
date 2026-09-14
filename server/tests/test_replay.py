"""Replay protection, across a restart too.

Skipped unless ``TRACKER_TEST_DB`` is set; see conftest.py.
"""

import importlib
import secrets
import time

from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305

from conftest import needs_db, record

pytestmark = needs_db

IMEI = '350000000000000'


def datagram():
    nonce = secrets.token_bytes(12)
    sealed = ChaCha20Poly1305(bytes.fromhex('aa' * 32)).encrypt(
        nonce, record(0, 51.5, -0.1, 0).encode(), IMEI.encode())
    return bytes([len(IMEI)]) + IMEI.encode() + nonce + sealed


def test_a_replayed_datagram_is_refused(device):
    from tracker.listeners import udp

    raw = datagram()
    assert udp.decrypt_request(raw)[0] is not None
    assert udp.decrypt_request(raw) == (None, None, None)


def test_the_refusal_survives_a_restart(device):
    from tracker.listeners import udp

    raw = datagram()
    assert udp.decrypt_request(raw)[0] is not None
    importlib.reload(udp)   # a restarted listener remembers nothing in memory
    assert udp.decrypt_request(raw) == (None, None, None)


def test_nonces_past_the_retention_are_pruned(device, database):
    from tracker.listeners import udp

    stale = int(time.time()) - udp.NONCE_RETENTION_SECONDS - 60
    database.query('INSERT INTO `device_nonce` (`device_id`, `nonce`, `seen_at`) '
                   'VALUES (%s, %s, %s)', (device['id'], b'\x00' * 12, stale))
    udp._last_prune = 0.0
    assert udp.decrypt_request(datagram())[0] is not None
    assert database.one('SELECT COUNT(*) AS n FROM `device_nonce` WHERE `seen_at` = %s',
                        (stale,))['n'] == 0
