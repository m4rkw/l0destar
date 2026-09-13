"""Firmware downloads over TLS.

The device's modem terminates TLS itself, so the server only has to speak
plain TLS 1.2 over TCP, and over it the small part of HTTP that the nRF91 FOTA
stack uses (``firmware.serve_http``).  The certificate is issued by the CA
compiled into the firmware, which is how a device knows the manifest and the
image come from its own server.

Telemetry does not come here: it arrives over UDP, where every datagram is
authenticated with the device's own key.
"""

import socket
import ssl
import threading
import time

from .. import config, db, firmware, logs


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
        # Handshake done, back to a short budget until the request arrives, so
        # a client that connects and says nothing does not hold a thread.
        # serve_http() widens it again for the download itself.
        conn.settimeout(int(config.get('tls_read_timeout', 10)))
        serve(conn, ip)
    except Exception:
        logs.tls.exception('TLS connection error from %s', ip)
    finally:
        try:
            conn.close()
        except OSError:
            pass
        # This thread ends here, and its database connection with it, rather
        # than lingering until the database server times it out.
        db.tls.close()


def serve(conn, ip):
    """Serve one established connection.

    The firmware's downloader only ever sends GET and HEAD, so the first two
    bytes are enough to turn anything else away before it can hold the thread
    for a download's long read timeout.  Kept apart from the handshake so the
    dispatch can be driven over a plain socket in the tests.
    """
    header = recv_exact(conn, 2)
    if not header:
        return

    if header not in (b'GE', b'HE'):
        logs.tls.warning('not a firmware request from %s', ip)
        return

    firmware.serve_http(conn, ip, header, log=logs.tls)


def _context():
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(config.TLS_CERT, config.TLS_KEY)
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


def run(ready=None):
    """Accept connections until the process exits.

    ``ready``, if given, is set once the socket is listening, or once it is
    clear that it will not be; see ``wsgi.start_listeners``.
    """
    try:
        if not config.TLS_CERT or not config.TLS_KEY:
            logs.tls.info('TLS cert/key not configured, TLS listener disabled')
            return

        ctx = _context()
        sock = _bind()
        if sock is None:
            return

        sock.listen(int(config.get('tls_backlog', 8)))
        logs.tls.info('TLS listening on %s:%d', config.TLS_HOST, config.TLS_PORT)
    finally:
        if ready is not None:
            ready.set()

    # Handshake budget, deliberately generous.  A device on LTE-M in weak
    # signal has to get the server's certificate chain across before it can
    # reply, and the read timeout is far too tight for that — the symptom is
    # repeated "handshake operation timed out" on FOTA attempts that never
    # reach the HTTP layer at all.
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
