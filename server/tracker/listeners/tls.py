"""TLS telemetry, and firmware downloads on the same port.

The device's modem terminates TLS itself, so the server only has to speak
plain TLS 1.2 over TCP.  A telemetry exchange is one length-prefixed frame in
and one plaintext response out::

    [2] payload length, big-endian, <= 8192
    [N] payload:  IMEI '\\n' record '\\n' record ...

Sharing the port with firmware downloads
----------------------------------------
The public HTTPS name may well terminate somewhere else, but this port is
already open and already has a certificate the device trusts, so downloads ride
it too.  The two protocols cannot be confused: a telemetry frame starts with a
big-endian length capped at 8192, while an HTTP request starts with ``GE`` or
``HE`` — 0x4745 and 0x4845, far above the cap.  The first two bytes decide.

Authentication
--------------
The device proves nothing beyond presenting an enrolled IMEI, and the IMEI is
not a secret.  This transport therefore assumes the TLS listener is reachable
only from where the operator expects, or that mutual TLS is configured
(``tls_client_ca``); with neither, anyone who learns an IMEI can post
telemetry as that device.  The UDP transport's per-device PSK is stronger in
that respect.
"""

import socket
import ssl
import struct
import threading
import time

from .. import config, db, firmware, logs, telemetry


def recv_exact(conn, n):
    """Read exactly n bytes, or None if the peer closed first."""
    buf = b''
    while len(buf) < n:
        chunk = conn.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


def handle_connection(ctx, conn, addr):
    ip = addr[0]
    try:
        conn = ctx.wrap_socket(conn, server_side=True)
    except (ssl.SSLError, OSError) as e:
        # A failed handshake — a port scan, a probe, an abandoned client —
        # kills this connection only, never the listener.
        logs.tls.warning('TLS handshake failed from %s: %s', ip, e)
        try:
            conn.close()
        except OSError:
            pass
        return

    try:
        # Handshake done, back to the short budget.  Telemetry is one brief
        # exchange, so anything slower is a stalled or hostile client holding a
        # thread.  serve_http() widens it again for image downloads.
        conn.settimeout(int(config.get('tls_read_timeout', 10)))

        header = recv_exact(conn, 2)
        if not header:
            return

        if header in (b'GE', b'HE'):
            firmware.serve_http(conn, ip, header, log=logs.tls)
            return

        payload_len = struct.unpack('>H', header)[0]
        if not 1 <= payload_len <= 8192:
            logs.tls.warning('bad payload length %d from %s', payload_len, ip)
            return

        payload = recv_exact(conn, payload_len)
        if not payload:
            logs.tls.warning('truncated payload from %s', ip)
            return

        text = payload.decode('ascii', errors='replace')
        lines = [line for line in text.split('\n') if line.strip()]
        if not lines:
            return

        imei = lines[0].strip()
        if not imei.isdigit() or not 14 <= len(imei) <= 16:
            logs.tls.warning('invalid IMEI from %s: %r', ip, imei[:20])
            return

        device = db.tls.one('SELECT * FROM `device` WHERE `imei` = %s', (imei,))
        if not device:
            logs.tls.warning('unknown IMEI %s from %s', imei, ip)
            return

        response = telemetry.process_lines(device, lines[1:], ip, db.tls, logs.tls)
        conn.sendall(response.encode('ascii'))
    except Exception:
        logs.tls.exception('TLS connection error from %s', ip)
    finally:
        try:
            conn.close()
        except OSError:
            pass


def _context():
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(config.TLS_CERT, config.TLS_KEY)
    client_ca = config.get('tls_client_ca')
    if client_ca:
        # Optional mutual TLS: with a per-device client certificate the
        # transport authenticates the device rather than trusting the IMEI.
        ctx.verify_mode = ssl.CERT_REQUIRED
        ctx.load_verify_locations(client_ca)
    return ctx


def _bind():
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    delay = 1
    for attempt in range(1, 31):
        try:
            sock.bind((config.TLS_HOST, config.TLS_PORT))
            return sock
        except OSError as e:
            logs.tls.error('TLS bind failed (attempt %d/30): %s', attempt, e)
            time.sleep(min(delay, 30))
            delay *= 2
    logs.tls.critical('TLS bind failed after 30 attempts')
    return None


def run():
    if not config.TLS_CERT or not config.TLS_KEY:
        logs.tls.info('TLS cert/key not configured, TLS listener disabled')
        return

    ctx = _context()
    sock = _bind()
    if sock is None:
        return

    sock.listen(int(config.get('tls_backlog', 8)))
    logs.tls.info('TLS listening on %s:%d', config.TLS_HOST, config.TLS_PORT)

    # Handshake budget, deliberately generous.  A device on LTE-M in weak
    # signal has to get the server's certificate chain across before it can
    # reply, and the telemetry read timeout is far too tight for that — the
    # symptom is repeated "handshake operation timed out" on FOTA attempts that
    # never reach the HTTP layer at all.
    handshake_timeout = int(config.get('tls_handshake_timeout', 45))

    while True:
        try:
            conn, addr = sock.accept()
        except OSError:
            # A transient accept error — an aborted client, FD pressure — must
            # never tear down the listener.
            logs.tls.exception('TLS accept error')
            time.sleep(1)
            continue
        conn.settimeout(handshake_timeout)
        threading.Thread(target=handle_connection, args=(ctx, conn, addr),
                         daemon=True).start()
