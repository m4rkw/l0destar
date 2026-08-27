"""OTA manifests and the firmware HTTP server.

The HTTP tests drive ``serve_http`` over a real socket pair rather than a
mock, because what is being checked is byte-level: the nRF91's downloader is
strict about ``206`` and ``Content-Range``, and a framing bug shows up as a
device that downloads a corrupt image and refuses to boot it.
"""

import os
import socket
import threading

import pytest

from tracker import firmware

IMEI = '350000000000000'
IMAGE_SIZE = 10240


@pytest.fixture
def published(fw_dir):
    """A manifest, its image, and one older image."""
    with open(os.path.join(fw_dir, 'manifest-%s.txt' % IMEI), 'w') as f:
        f.write('version=0.4.12\nfile=l0destar-0.4.12-%s.bin\nboard=l0destar_v3.2\n' % IMEI)
    image = os.path.join(fw_dir, 'l0destar-0.4.12-%s.bin' % IMEI)
    with open(image, 'wb') as f:
        f.write(bytes(range(256)) * (IMAGE_SIZE // 256))
    with open(os.path.join(fw_dir, 'l0destar-0.4.9-%s.bin' % IMEI), 'wb') as f:
        f.write(b'old')
    firmware._manifest_cache.clear()
    return image


def test_latest_version(published):
    assert firmware.latest_version(IMEI) == '0.4.12'


def test_unknown_device_gets_no_update(fw_dir):
    # The safe direction: a device not in the publisher's list simply never
    # updates, rather than being offered somebody else's image.
    assert firmware.latest_version('999999999999999') is None


@pytest.mark.parametrize('imei', ['', None, '../etc/passwd', 'abc'])
def test_unusable_imei_rejected(fw_dir, imei):
    assert firmware.manifest_path(imei) is None
    assert firmware.latest_version(imei) is None


def test_junk_version_never_forwarded(fw_dir):
    # The device parses this with a strict %u.%u.%u; anything else would be
    # read as garbage on the far side.
    with open(os.path.join(fw_dir, 'manifest-111111111111111.txt'), 'w') as f:
        f.write('version=nightly-build\n')
    firmware._manifest_cache.clear()
    assert firmware.latest_version('111111111111111') is None


def test_published_versions_sort_numerically(published):
    # Lexical sort would put 0.4.12 before 0.4.9 and the publisher would
    # reuse a patch number.
    assert firmware.published_versions() == ['0.4.9', '0.4.12']


def test_manifest_cache_follows_mtime(published, fw_dir):
    assert firmware.latest_version(IMEI) == '0.4.12'
    path = os.path.join(fw_dir, 'manifest-%s.txt' % IMEI)
    with open(path, 'w') as f:
        f.write('version=0.4.13\n')
    os.utime(path, (0, 0))
    assert firmware.latest_version(IMEI) == '0.4.13'


# -- HTTP --------------------------------------------------------------------

class Session:
    """A live TLS-less connection into serve_http, plus a tiny HTTP client."""

    def __init__(self):
        self.listener = socket.socket()
        self.listener.bind(('127.0.0.1', 0))
        self.listener.listen(1)
        self.error = None
        threading.Thread(target=self._serve, daemon=True).start()
        self.conn = socket.create_connection(self.listener.getsockname())
        self.conn.settimeout(5)

    def _serve(self):
        conn, _ = self.listener.accept()
        try:
            firmware.serve_http(conn, '127.0.0.1', conn.recv(2))
        except Exception as e:      # pragma: no cover - surfaced by the test
            self.error = e
        finally:
            conn.close()

    def request(self, line, headers='', body_expected=True):
        self.conn.sendall(('%s HTTP/1.1\r\nHost: test\r\n%s\r\n'
                           % (line, headers)).encode())
        buf = b''
        while b'\r\n\r\n' not in buf:
            buf += self.conn.recv(4096)
        head, _, body = buf.partition(b'\r\n\r\n')
        head = head.decode()
        length = 0
        for header in head.split('\r\n')[1:]:
            if header.lower().startswith('content-length:'):
                length = int(header.split(':', 1)[1])
        if body_expected:
            while len(body) < length:
                body += self.conn.recv(4096)
        return head, body

    def close(self):
        self.conn.close()
        self.listener.close()


@pytest.fixture
def http(published):
    session = Session()
    yield session
    session.close()
    assert session.error is None


def test_manifest_is_per_device(http):
    head, body = http.request('GET /fw/manifest.txt?imei=%s&v=0.4.11' % IMEI)
    assert head.startswith('HTTP/1.1 200')
    assert b'version=0.4.12' in body


def test_manifest_without_imei_is_404(http):
    head, _ = http.request('GET /fw/manifest.txt')
    assert head.startswith('HTTP/1.1 404')


def test_published_txt(http):
    _, body = http.request('GET /fw/published.txt')
    assert body == b'0.4.9\n0.4.12\n'


def test_ranged_get(http, published):
    head, body = http.request('GET /fw/%s' % os.path.basename(published),
                              'Range: bytes=0-2047\r\n')
    assert head.startswith('HTTP/1.1 206')
    assert 'Content-Range: bytes 0-2047/%d' % IMAGE_SIZE in head
    assert len(body) == 2048
    with open(published, 'rb') as f:
        assert body == f.read(2048)


def test_open_ended_range(http, published):
    head, body = http.request('GET /fw/%s' % os.path.basename(published),
                              'Range: bytes=10176-\r\n')
    assert head.startswith('HTTP/1.1 206')
    assert len(body) == 64


def test_range_past_eof(http, published):
    head, _ = http.request('GET /fw/%s' % os.path.basename(published),
                           'Range: bytes=%d-\r\n' % IMAGE_SIZE)
    assert head.startswith('HTTP/1.1 416')


def test_head_reports_length_without_body(http, published):
    head, body = http.request('HEAD /fw/%s' % os.path.basename(published),
                              body_expected=False)
    assert head.startswith('HTTP/1.1 200')
    assert 'Content-Length: %d' % IMAGE_SIZE in head
    assert body == b''


@pytest.mark.parametrize('target', [
    '/fw/../../etc/passwd',
    '/etc/passwd',
    '/fw/nonexistent.bin',
])
def test_only_fw_files_are_served(http, target):
    head, _ = http.request('GET %s' % target)
    assert head.startswith('HTTP/1.1 404')


def test_non_get_rejected(http):
    head, _ = http.request('POST /fw/anything')
    assert head.startswith('HTTP/1.1 405')


def test_keep_alive_across_many_ranges(http, published):
    # An image is hundreds of sequential ranged GETs on one connection; if
    # keep-alive breaks, every download restarts from byte zero.
    name = os.path.basename(published)
    for start in range(0, IMAGE_SIZE, 2048):
        head, body = http.request('GET /fw/%s' % name,
                                  'Range: bytes=%d-%d\r\n' % (start, start + 2047))
        assert head.startswith('HTTP/1.1 206')
        assert len(body) == 2048
