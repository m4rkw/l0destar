"""The TLS listener's dispatch: firmware downloads always, telemetry frames
only when ``tls_telemetry`` is on.

Driven over a real socket like the firmware tests, with the handshake left
out: what matters here is which bytes get an answer.
"""

import socket
import struct
import threading

from tracker import config, db, telemetry
from tracker.listeners import tls

IMEI = '350000000000000'


def exchange(payload):
    """Send bytes into tls.serve() and return everything it sends back
    before closing the connection."""
    listener = socket.socket()
    listener.bind(('127.0.0.1', 0))
    listener.listen(1)
    errors = []

    def run():
        conn, _ = listener.accept()
        try:
            conn.settimeout(5)
            tls.serve(conn, '127.0.0.1')
        except Exception as e:      # pragma: no cover - surfaced below
            errors.append(e)
        finally:
            conn.close()

    thread = threading.Thread(target=run, daemon=True)
    thread.start()
    client = socket.create_connection(listener.getsockname())
    client.settimeout(5)
    client.sendall(payload)

    received = b''
    try:
        while True:
            chunk = client.recv(4096)
            if not chunk:
                break
            received += chunk
    except ConnectionResetError:
        # Closing with the rest of a refused frame unread resets the
        # connection rather than ending it cleanly; either way it is over.
        pass
    client.close()
    thread.join(5)
    listener.close()
    assert not errors
    return received


def frame(*lines):
    body = '\n'.join((IMEI,) + lines).encode('ascii')
    return struct.pack('>H', len(body)) + body


def test_telemetry_is_off_unless_configured():
    assert config.TLS_TELEMETRY is False


def test_telemetry_frame_is_refused_when_off(monkeypatch):
    calls = []
    monkeypatch.setattr(telemetry, 'process_lines',
                        lambda *args: calls.append(args) or '1,0,1')

    assert exchange(frame('12/08/26,00:00:00+01,51.5,-0.1')) == b''
    assert calls == []


def test_telemetry_frame_is_served_when_on(monkeypatch):
    monkeypatch.setattr(config, 'TLS_TELEMETRY', True)
    monkeypatch.setattr(db.tls, 'one',
                        lambda sql, params=None: {'id': 1, 'imei': IMEI, 'name': 'Car'})
    seen = []

    def process_lines(device, lines, ip, database, log):
        seen.append((device['imei'], lines))
        return '1,3600,1'

    monkeypatch.setattr(telemetry, 'process_lines', process_lines)

    assert exchange(frame('a record')) == b'1,3600,1'
    assert seen == [(IMEI, ['a record'])]


def test_firmware_downloads_do_not_depend_on_it(fw_dir):
    reply = exchange(b'GET /fw/published.txt HTTP/1.1\r\n'
                     b'Host: test\r\nConnection: close\r\n\r\n')
    assert reply.startswith(b'HTTP/1.1 200 OK\r\n')
