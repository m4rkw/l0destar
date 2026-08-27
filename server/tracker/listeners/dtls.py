"""DTLS 1.2 with Connection ID.

Why CID
-------
LTE-M's Release Assistance Indication lets the device drop the radio the
instant it has nothing more to send, which is most of what makes multi-week
standby possible.  The cost is that the operator's NAT rebinds the device to a
new source port on the next transmission — often on every single wake.  A
plain DTLS session is keyed on the 4-tuple and dies there, so every wake would
pay for a fresh handshake, which is the most expensive thing the device does.

Connection ID (RFC 9146) puts an explicit session identifier in each record, so
the server matches records by CID rather than by address.  The session then
survives an arbitrary number of rebinds and the device pays for one handshake
across its whole deployment.

Implementation
--------------
Python's ``ssl`` module has no DTLS support at all, and no maintained binding
exposes CID.  This module therefore drives a small C shared library that wraps
mbedTLS with ``MBEDTLS_SSL_DTLS_CONNECTION_ID`` enabled, through ctypes.

    The library is not yet part of this repository.  Point ``dtls_lib`` in
    config.yaml at a build of it, or leave it unset — the listener disables
    itself cleanly and the UDP and TLS transports are unaffected.

Expected ABI::

    void *dtls_cid_init(const char *cert, const char *key,
                        const char *host, int port);
    void *dtls_cid_accept(void *ctx, int timeout_ms);
    int   dtls_cid_read(void *session, void *buf, int len, int timeout_ms);
    int   dtls_cid_write(void *session, const char *buf, int len);
    void  dtls_cid_session_free(void *session);
    void  dtls_cid_free(void *ctx);

``dtls_cid_read`` returns the number of bytes read, 0 on timeout, negative on
error.  Payload framing inside the DTLS record matches the TLS transport: a
2-byte big-endian length followed by IMEI and records.
"""

import ctypes
import struct
import time

from .. import config, db, logs, telemetry

_lib = None

_SIGNATURES = {
    'dtls_cid_init': ([ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p,
                       ctypes.c_int], ctypes.c_void_p),
    'dtls_cid_accept': ([ctypes.c_void_p, ctypes.c_int], ctypes.c_void_p),
    'dtls_cid_read': ([ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int,
                       ctypes.c_int], ctypes.c_int),
    'dtls_cid_write': ([ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int],
                       ctypes.c_int),
    'dtls_cid_session_free': ([ctypes.c_void_p], None),
    'dtls_cid_free': ([ctypes.c_void_p], None),
}


def _load():
    """Load and bind the CID library, or return None if unavailable."""
    if not config.DTLS_LIB:
        return None
    try:
        lib = ctypes.CDLL(config.DTLS_LIB)
        for name, (argtypes, restype) in _SIGNATURES.items():
            fn = getattr(lib, name)
            fn.argtypes = argtypes
            fn.restype = restype
    except (OSError, AttributeError) as e:
        logs.dtls.warning('DTLS CID library unavailable (%s): %s',
                          config.DTLS_LIB, e)
        return None
    logs.dtls.info('DTLS CID library loaded from %s', config.DTLS_LIB)
    return lib


def _read_one(session):
    """Read and attribute one datagram.  Returns (device, lines) or None."""
    buf = ctypes.create_string_buffer(config.MAX_DGRAM)
    n = _lib.dtls_cid_read(session, buf, config.MAX_DGRAM, 5000)
    if n <= 0:
        return None

    data = buf.raw[:n]
    if len(data) < 2:
        return None

    payload_len = struct.unpack('>H', data[:2])[0]
    if not 1 <= payload_len <= 8192:
        logs.dtls.warning('bad payload length %d', payload_len)
        return None
    if len(data) < 2 + payload_len:
        logs.dtls.warning('truncated payload')
        return None

    text = data[2:2 + payload_len].decode('ascii', errors='replace')
    lines = [line for line in text.split('\n') if line.strip()]
    if not lines:
        return None

    imei = lines[0].strip()
    if not imei.isdigit() or not 14 <= len(imei) <= 16:
        logs.dtls.warning('invalid IMEI: %r', imei[:20])
        return None

    device = db.dtls.one('SELECT * FROM `device` WHERE `imei` = %s', (imei,))
    if not device:
        logs.dtls.warning('unknown IMEI %s', imei)
        return None

    return device, lines[1:]


def _handle(session):
    """Serve one session until it goes idle or the device disconnects."""
    count = 0
    try:
        while True:
            result = _read_one(session)
            if not result:
                break
            device, lines = result
            response = telemetry.process_lines(device, lines, 'dtls',
                                               db.dtls, logs.dtls)
            encoded = response.encode('ascii')
            _lib.dtls_cid_write(session, encoded, len(encoded))
            count += 1
    except Exception:
        logs.dtls.exception('DTLS session error')
    finally:
        if count:
            logs.dtls.info('DTLS session ended after %d datagrams', count)


def run():
    global _lib

    _lib = _load()
    if _lib is None:
        logs.dtls.info('DTLS listener disabled')
        return
    if not config.DTLS_CERT or not config.DTLS_KEY:
        logs.dtls.info('DTLS cert/key not configured, DTLS listener disabled')
        return

    ctx = _lib.dtls_cid_init(
        config.DTLS_CERT.encode(), config.DTLS_KEY.encode(),
        config.DTLS_HOST.encode(), config.DTLS_PORT,
    )
    if not ctx:
        logs.dtls.error('dtls_cid_init failed')
        return

    logs.dtls.info('DTLS-CID listening on %s:%d', config.DTLS_HOST, config.DTLS_PORT)

    while True:
        session = None
        try:
            session = _lib.dtls_cid_accept(ctx, 5000)
            if not session:
                continue
            logs.dtls.info('DTLS-CID session established')
            _handle(session)
        except OSError as e:
            logs.dtls.error('DTLS socket error: %s', e)
            time.sleep(1)
        except Exception:
            logs.dtls.exception('DTLS listener error')
        finally:
            if session:
                _lib.dtls_cid_session_free(session)
