"""The TLS listener's dispatch: firmware requests are served, and anything
else is turned away — including the length-prefixed telemetry frames the
listener once accepted.

Driven over a real socket like the firmware tests, with the handshake left
out: what matters here is which bytes get an answer.
"""

import socket
import struct
import threading

from tracker import telemetry
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
        # Closing with the rest of a refused request unread resets the
        # connection rather than ending it cleanly; either way it is over.
        pass
    client.close()
    thread.join(5)
    listener.close()
    assert not errors
    return received


def test_firmware_requests_are_served(fw_dir):
    reply = exchange(b'GET /fw/published.txt HTTP/1.1\r\n'
                     b'Host: test\r\nConnection: close\r\n\r\n')
    assert reply.startswith(b'HTTP/1.1 200 OK\r\n')


def test_telemetry_frames_are_turned_away(monkeypatch):
    calls = []
    monkeypatch.setattr(telemetry, 'process_lines',
                        lambda *args: calls.append(args) or '1,0,1')

    body = '\n'.join((IMEI, '12/08/26,00:00:00+01,51.5,-0.1')).encode('ascii')
    assert exchange(struct.pack('>H', len(body)) + body) == b''
    assert calls == []


def test_other_http_methods_are_turned_away(fw_dir):
    assert exchange(b'POST /fw/published.txt HTTP/1.1\r\n'
                    b'Host: test\r\nConnection: close\r\n\r\n') == b''
