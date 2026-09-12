"""OTA manifests and the firmware HTTP server.

The HTTP tests drive ``serve_http`` over a real socket pair rather than a
mock, because what is being checked is byte-level: the nRF91's downloader is
strict about ``206`` and ``Content-Range``, and a framing bug shows up as a
device that downloads a corrupt image and refuses to boot it.
"""

import datetime
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


# -- failed updates ----------------------------------------------------------
#
# An image that boots and dies is reverted by MCUboot, and the loop that
# follows — advertise, download, revert, advertise — is what flattens a parked
# vehicle's battery.  These drive the state machine directly with a fake
# database, so they run without one; the SQL itself is covered by the
# integration tests.

class FakeDB:
    """One device row, matched by the fragment of SQL each query carries."""

    def __init__(self, **row):
        self.row = {'fw_staged': None, 'fw_staged_at': None,
                    'fw_blocked': None, 'fw_fail_count': 0}
        self.row.update(row)

    def one(self, sql, args=None):
        return dict(self.row)

    def query(self, sql, args=None):
        if '`fw_staged` = %s' in sql:
            self.row.update(fw_staged=args[0],
                            fw_staged_at=datetime.datetime.now())
        elif '`fw_blocked` = %s' in sql:
            self.row.update(fw_staged=None, fw_staged_at=None,
                            fw_blocked=args[0], fw_fail_count=args[1])
        elif '`fw_blocked` = NULL' in sql:
            self.row.update(fw_staged=None, fw_staged_at=None,
                            fw_blocked=None, fw_fail_count=0)


@pytest.fixture
def alerts(monkeypatch):
    sent = []
    monkeypatch.setattr(firmware.notify, 'device_alert',
                        lambda name, msg, pri: sent.append((msg, pri)))
    return sent


DEV = {'id': 1, 'imei': IMEI, 'name': 'Car'}


def _dev(fake):
    return {**DEV, **fake.row}


def test_staged_then_running_confirms(alerts):
    fake = FakeDB()
    firmware.note_staged(DEV, '0.4.13', fake)
    assert fake.row['fw_staged'] == '0.4.13'

    firmware.check_running(_dev(fake), '0.4.13', fake)
    assert fake.row['fw_staged'] is None
    assert fake.row['fw_blocked'] is None
    # The device cannot reliably raise this itself: it only reaches the
    # confirm on one boot, and the alert is queued in RAM until a send.
    assert alerts == [('fota: updated to 0.4.13', 0)]


def test_success_notified_once(alerts):
    fake = FakeDB()
    firmware.note_staged(DEV, '0.4.13', fake)
    firmware.check_running(_dev(fake), '0.4.13', fake)
    firmware.check_running(_dev(fake), '0.4.13', fake)
    assert len(alerts) == 1


def test_reboot_with_nothing_staged_is_silent(alerts):
    fake = FakeDB()
    firmware.check_running(_dev(fake), '0.4.12', fake)
    assert alerts == []
    assert fake.row['fw_blocked'] is None


def test_reported_revert_blocks_the_version(alerts):
    fake = FakeDB()
    firmware.note_staged(DEV, '0.4.13', fake)
    firmware.note_failed(DEV, '0.4.13', '0.4.12', fake)
    assert fake.row['fw_blocked'] == '0.4.13'
    assert fake.row['fw_fail_count'] == 1
    assert len(alerts) == 1 and 'failed to boot' in alerts[0][0]


def test_repeated_failure_report_is_idempotent(alerts):
    # The device repeats the report until a send lands; each repeat must not
    # count as another failure or wake anyone up again.
    fake = FakeDB()
    firmware.note_staged(DEV, '0.4.13', fake)
    firmware.note_failed(DEV, '0.4.13', '0.4.12', fake)
    firmware.note_failed(DEV, '0.4.13', '0.4.12', fake)
    assert fake.row['fw_fail_count'] == 1
    assert len(alerts) == 1


def test_revert_inferred_only_after_the_grace_period(alerts):
    # Covers a device whose reverted-to image is too old to report anything:
    # the running version in its telemetry is the only evidence.
    fake = FakeDB()
    firmware.note_staged(DEV, '0.4.13', fake)

    firmware.check_running(_dev(fake), '0.4.12', fake)
    assert fake.row['fw_blocked'] is None, 'still rebooting'
    assert alerts == []

    stale = _dev(fake)
    stale['fw_staged_at'] = (datetime.datetime.now()
                             - datetime.timedelta(seconds=firmware.STAGE_GRACE_S + 1))
    firmware.check_running(stale, '0.4.12', fake)
    assert fake.row['fw_blocked'] == '0.4.13'
    assert len(alerts) == 1


def test_newer_version_clears_the_block(alerts):
    fake = FakeDB(fw_blocked='0.4.13', fw_fail_count=2)
    firmware.note_staged(DEV, '0.4.14', fake)
    firmware.check_running(_dev(fake), '0.4.14', fake)
    assert fake.row['fw_blocked'] is None
    assert fake.row['fw_fail_count'] == 0


def test_bad_version_in_report_ignored(alerts):
    fake = FakeDB()
    firmware.note_staged(DEV, 'nightly', fake)
    assert fake.row['fw_staged'] is None
    firmware.note_failed(DEV, 'nightly', '0.4.12', fake)
    assert fake.row['fw_blocked'] is None


def test_blocked_manifest_refuses_without_a_404(monkeypatch, published):
    # The power-on check fetches a manifest unconditionally, so the refusal
    # has to name the version: that is what lets the device scope the block
    # to one build and still take a newer one.
    monkeypatch.setattr(firmware, 'blocked_version', lambda imei, database=None: '0.4.12')
    session = Session()
    try:
        head, body = session.request(
            'GET /fw/manifest.txt?imei=%s&v=0.4.9' % IMEI)
    finally:
        session.close()
    assert '200 OK' in head
    assert b'version=0.4.12' in body
    assert b'status=blocked' in body
    assert b'reason=' in body


def test_manifest_check_in_ignores_a_mismatch(monkeypatch, published):
    # The publisher verifies every push with v=0.0.0 from the build host.
    # Read as a device check-in that would mark the staged version failed
    # and withhold it — which is exactly what happened on 2026-09-12.
    called = []
    monkeypatch.setattr(firmware, 'blocked_version', lambda imei, database=None: None)
    monkeypatch.setattr(firmware, 'check_running',
                        lambda *a, **k: called.append(a))

    class Staged:
        def one(self, sql, args=None):
            return {'id': 1, 'imei': args[0], 'name': 'Car', 'fw_staged': '0.4.12'}

    monkeypatch.setattr(firmware.db, 'tls', Staged())
    session = Session()
    try:
        session.request('GET /fw/manifest.txt?imei=%s&v=0.0.0' % IMEI)
    finally:
        session.close()
    assert called == []


def test_blocked_manifest_yields_to_a_newer_build(monkeypatch, published, alerts):
    # 0.4.12 is published and 0.4.9 is blocked: the refusal is scoped to the
    # build that failed, so the newer one is served normally.  Blocking it
    # would strand the unit on the broken build with no way back.
    monkeypatch.setattr(firmware, 'blocked_version', lambda imei, database=None: '0.4.9')
    monkeypatch.setattr(firmware, 'note_check_in', lambda *a, **k: None)
    session = Session()
    try:
        head, body = session.request('GET /fw/manifest.txt?imei=%s&v=0.4.9' % IMEI)
    finally:
        session.close()
    assert '200 OK' in head
    assert b'status=blocked' not in body
    assert b'version=0.4.12' in body


def test_manifest_check_in_confirms_the_update(monkeypatch, published, alerts):
    # The boot check is the earliest thing a freshly updated device does, and
    # it carries v=<running>.  Waiting for telemetry instead means waiting for
    # a GNSS fix, and after a reboot the engine-off interval is back at its
    # default until a response restores it — on a parked car that is a long
    # wait for a notification about something that already happened.
    seen = {}
    monkeypatch.setattr(firmware, 'blocked_version', lambda imei, database=None: None)
    monkeypatch.setattr(firmware, 'check_running',
                        lambda device, running, database, log=None:
                            seen.update(imei=device['imei'], running=running))

    class OneDevice:
        def one(self, sql, args=None):
            return {'id': 1, 'imei': args[0], 'name': 'Car', 'fw_staged': '0.4.12'}


    monkeypatch.setattr(firmware.db, 'tls', OneDevice())
    session = Session()
    try:
        session.request('GET /fw/manifest.txt?imei=%s&v=0.4.12' % IMEI)
    finally:
        session.close()
    assert seen == {'imei': IMEI, 'running': '0.4.12'}


def test_manifest_check_in_ignores_junk(monkeypatch, published):
    called = []
    monkeypatch.setattr(firmware, 'blocked_version', lambda imei, database=None: None)
    monkeypatch.setattr(firmware, 'check_running',
                        lambda *a, **k: called.append(a))

    class Boom:
        def one(self, sql, args=None):
            raise AssertionError('should not reach the database')

    monkeypatch.setattr(firmware.db, 'tls', Boom())
    session = Session()
    try:
        session.request('GET /fw/manifest.txt?imei=%s&v=nightly' % IMEI)
    finally:
        session.close()
    assert called == []
