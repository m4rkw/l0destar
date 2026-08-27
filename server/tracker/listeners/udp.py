"""UDP telemetry with ChaCha20-Poly1305.

Wire format, one envelope per datagram::

    request:   [1] imei_len  [imei_len] IMEI ASCII  [12] nonce  [N] ct  [16] tag
    response:  [12] nonce  [N] ct  [16] tag

The AAD is the device IMEI on both directions, and the response additionally
binds the request's nonce, so a response captured from one exchange cannot be
replayed into another.  The plaintext is the same newline-separated CSV that
the TLS and DTLS transports carry, so nothing downstream of decryption differs
between them.

The IMEI travels in the clear because the server needs it to pick a key.  That
is a real privacy cost — an observer on path learns which device is reporting,
though not where it is — and it is the price of not paying for a handshake.  A
deployment that cares more about that than about radio time should use the DTLS
transport instead.
"""

import secrets
import socket
import threading
import time

from cryptography.exceptions import InvalidTag
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305

from .. import config, db, logs, telemetry

NONCE_BYTES = 12
TAG_BYTES = 16

# Replay window per device.  The most recent nonce is also persisted so that a
# restart cannot open a hole for the single most-recently captured packet.
_NONCE_WINDOW = 1024
_seen_nonces = {}
_seen_lock = threading.Lock()


def _nonce_seen(device_id, nonce):
    """True if this nonce has already been used by this device."""
    with _seen_lock:
        window = _seen_nonces.get(device_id)
        if window is None:
            row = db.udp.one('SELECT `last_nonce` FROM `device` WHERE `id` = %s',
                             (device_id,))
            seed = row.get('last_nonce') if row else None
            window = [bytes(seed)] if seed else []
            _seen_nonces[device_id] = window

        if nonce in window:
            return True
        window.append(nonce)
        if len(window) > _NONCE_WINDOW:
            del window[0]

    try:
        db.udp.query('UPDATE `device` SET `last_nonce` = %s WHERE `id` = %s',
                     (nonce, device_id))
    except Exception:
        logs.udp.exception('failed to persist last_nonce for device %s', device_id)
    return False


def _aead(device):
    psk = device.get('psk') or ''
    if len(psk) != 64:
        return None
    try:
        return ChaCha20Poly1305(bytes.fromhex(psk))
    except ValueError:
        return None


def decrypt_request(raw):
    """Parse and decrypt a request envelope.

    Returns (device, plaintext, nonce), or (None, None, None) on any failure.
    The failures are deliberately not distinguished: malformed, unknown IMEI,
    missing key, bad tag and replay all look the same from outside, so the
    responses cannot be used to enumerate which IMEIs are enrolled.
    """
    if len(raw) < 1 + NONCE_BYTES + TAG_BYTES:
        return None, None, None
    imei_len = raw[0]
    if not 14 <= imei_len <= 16:
        return None, None, None
    if len(raw) < 1 + imei_len + NONCE_BYTES + TAG_BYTES:
        return None, None, None

    imei_bytes = bytes(raw[1:1 + imei_len])
    try:
        imei = imei_bytes.decode('ascii')
    except UnicodeDecodeError:
        return None, None, None
    if not imei.isdigit():
        return None, None, None

    nonce = bytes(raw[1 + imei_len:1 + imei_len + NONCE_BYTES])
    sealed = bytes(raw[1 + imei_len + NONCE_BYTES:])

    device = db.udp.one('SELECT * FROM `device` WHERE `imei` = %s', (imei,))
    if not device:
        return None, None, None

    aead = _aead(device)
    if aead is None:
        return None, None, None

    try:
        plaintext = aead.decrypt(nonce, sealed, imei_bytes)
    except InvalidTag:
        return None, None, None

    # Only after the tag verifies — an unauthenticated packet must not be able
    # to poison the replay window with a nonce the real device will use later.
    if _nonce_seen(device['id'], nonce):
        return None, None, None

    return device, plaintext, nonce


def encrypt_response(device, plaintext, request_nonce):
    """Seal a response bound to the request's nonce."""
    aead = _aead(device)
    if aead is None:
        return None
    nonce = secrets.token_bytes(NONCE_BYTES)
    aad = device['imei'].encode('ascii') + request_nonce
    return nonce + aead.encrypt(nonce, plaintext.encode('ascii'), aad)


# Rate-limit the failure log.  Anyone can send this port a datagram, and a
# spoofed flood would otherwise fill the disk with one line per packet.
_fail_counters = {}
_fail_lock = threading.Lock()
_FAIL_WINDOW_SEC = 60
_FAIL_LOG_EVERY = 20


def _should_log_failure(ip):
    now = time.monotonic()
    with _fail_lock:
        entry = _fail_counters.get(ip)
        if entry is None or now - entry[0] >= _FAIL_WINDOW_SEC:
            _fail_counters[ip] = [now, 1]
            return True
        entry[1] += 1
        return entry[1] % _FAIL_LOG_EVERY == 0


def handle_datagram(raw, addr):
    """Process one datagram; returns response bytes or None."""
    ip = addr[0]
    device, plaintext, request_nonce = decrypt_request(raw)
    if device is None or plaintext is None:
        if _should_log_failure(ip):
            logs.udp.warning('decrypt failed from %s (%d bytes)', ip, len(raw))
        return None

    text = plaintext.decode('ascii', errors='replace')
    lines = [line for line in text.split('\n') if line.strip()]
    response = telemetry.process_lines(device, lines, ip, db.udp, logs.udp)
    return encrypt_response(device, response, request_nonce)


def _bind():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    delay = 1
    for attempt in range(1, 31):
        try:
            sock.bind((config.UDP_HOST, config.UDP_PORT))
            return sock
        except OSError as e:
            # Usually the previous instance has not released the port yet.
            logs.udp.error('UDP bind failed (attempt %d/30): %s', attempt, e)
            time.sleep(min(delay, 30))
            delay *= 2
    logs.udp.critical('UDP bind failed after 30 attempts')
    return None


def run():
    if not config.UDP_ENABLED:
        logs.udp.info('UDP listener disabled')
        return

    sock = _bind()
    if sock is None:
        return

    logs.udp.info('UDP listening on %s:%d', config.UDP_HOST, config.UDP_PORT)

    while True:
        try:
            data, addr = sock.recvfrom(config.MAX_DGRAM)
            response = handle_datagram(data, addr)
            if response:
                sock.sendto(response, addr)
        except OSError:
            break
        except Exception:
            logs.udp.exception('UDP loop error')
