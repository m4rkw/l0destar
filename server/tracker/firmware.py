"""OTA firmware indication and delivery.

Indication
----------
Every telemetry response carries ``fota=<version>`` when a newer build is
published for that unit.  The device compares it against its own running
version locally and only fetches the manifest and image when there is
something newer, so the steady state costs no extra requests from the field —
which matters when the fleet is on LTE-M and every byte is radio time.

Per-device images
-----------------
Each unit gets its own build.  Carrier board revision and fitted interfaces
(CAN vs K-line) differ between units, and an image for the wrong one installs
cleanly — MCUboot verifies the signature, not the hardware — and then
misbehaves in the field.  So the publisher writes ``l0destar-<ver>-<imei>.bin``
plus ``manifest-<imei>.txt`` per device, and a device with no manifest is told
about no update at all.  Failing to update is the safe direction.

Delivery
--------
Downloads are served over the same TLS port as telemetry.  See
``listeners/tls.py`` for why the two protocols can share a port, and
``docs/PROTOCOL.md`` for the request sequence the nRF91 FOTA stack produces.
"""

import os
import re

from . import config, logs

VERSION_RE = re.compile(r'^\d{1,3}\.\d{1,3}\.\d{1,3}$')
IMAGE_RE = re.compile(r'^l0destar-(\d{1,3}\.\d{1,3}\.\d{1,3})-\d+\.bin$')

# path -> (mtime, version).  Consulted once per telemetry exchange, so it is
# worth not reading the file every time; keyed on mtime so a republish is
# picked up without a restart.
_manifest_cache = {}


def manifest_path(imei):
    """Path to one device's manifest, or None if the IMEI is unusable."""
    imei = str(imei or '')
    if not imei.isdigit():
        return None
    return os.path.join(config.FW_DIR, 'manifest-%s.txt' % imei)


def latest_version(imei):
    """Version this device's manifest advertises, or None."""
    path = manifest_path(imei)
    if path is None:
        return None
    try:
        mtime = os.stat(path).st_mtime
    except OSError:
        return None

    cached = _manifest_cache.get(path)
    if cached is None or cached[0] != mtime:
        version = None
        try:
            with open(path) as f:
                for line in f:
                    if line.startswith('version='):
                        version = line[len('version='):].strip()
                        break
        except OSError:
            pass
        # The device parses this with a strict %u.%u.%u — never forward junk.
        if version and not VERSION_RE.match(version):
            logs.app.error('manifest %s advertises unusable version %r',
                           path, version)
            version = None
        cached = (mtime, version)
        _manifest_cache[path] = cached

    return cached[1]


def published_versions():
    """Every version ever published, oldest first.

    The publisher reads this to pick the next patch number, so the fleet's own
    state is the counter rather than something tracked in the firmware repo.
    Image filenames count as well as manifests: a manifest is overwritten on
    each publish, an image file never is, so the filenames are the durable
    record of what has actually gone out.
    """
    found = set()
    try:
        names = os.listdir(config.FW_DIR)
    except OSError:
        return []

    for name in names:
        match = IMAGE_RE.match(name)
        if match:
            found.add(match.group(1))
            continue
        if name.startswith('manifest') and name.endswith('.txt'):
            try:
                with open(os.path.join(config.FW_DIR, name)) as f:
                    for line in f:
                        if line.startswith('version='):
                            value = line[len('version='):].strip()
                            if VERSION_RE.match(value):
                                found.add(value)
                            break
            except OSError:
                pass

    return sorted(found, key=lambda v: [int(n) for n in v.split('.')])


# -- HTTP delivery -----------------------------------------------------------
# Only what the nRF91 FOTA stack actually issues: GET and HEAD under /fw/,
# HTTP/1.1 keep-alive, and Range.  The modem decodes about 2 KB per TLS record,
# so the downloader fetches an image as a long run of sequential 2048-byte
# ranged GETs on one connection and expects 206 plus Content-Range for each.

def _respond(conn, status, body=b'', ctype='application/octet-stream',
             extra='', keep=True, head_only=False):
    header = ('HTTP/1.1 %s\r\n'
              'Content-Type: %s\r\n'
              'Content-Length: %d\r\n'
              '%s'
              'Connection: %s\r\n'
              '\r\n' % (status, ctype, len(body), extra,
                        'keep-alive' if keep else 'close'))
    conn.sendall(header.encode('ascii') + (b'' if head_only else body))


def _parse_request(buf):
    """Split one request off the front of ``buf``.

    Returns (method, target, headers, remainder) or None if the head is not
    complete or is malformed.
    """
    head, sep, remainder = buf.partition(b'\r\n\r\n')
    if not sep:
        return None
    lines = head.decode('ascii', errors='replace').split('\r\n')
    parts = lines[0].split(' ')
    if len(parts) < 3:
        return None
    headers = {}
    for line in lines[1:]:
        if ':' in line:
            key, value = line.split(':', 1)
            headers[key.strip().lower()] = value.strip()
    return parts[0], parts[1], headers, remainder


def _query_param(query, name):
    for pair in query.split('&'):
        key, _, value = pair.partition('=')
        if key == name:
            return value
    return ''


def serve_http(conn, ip, first_bytes, log=None):
    """Serve firmware requests on an already-established TLS connection."""
    log = log or logs.tls

    # The telemetry read timeout is deliberately short: one brief exchange,
    # anything slower is a stalled or hostile client holding a thread.  An
    # image is hundreds of sequential ranged GETs on this one connection and
    # the device goes quiet between them for as long as the radio makes it.  A
    # single RRC re-establishment in weak signal outlasts the telemetry budget,
    # and because the downloader has no resume, one read timeout costs the
    # whole transfer and the next attempt restarts at byte zero.
    conn.settimeout(int(config.get('fw_download_timeout', 120)))

    buf = first_bytes
    while True:
        parsed = None
        while parsed is None:
            if len(buf) > 4096:
                return
            parsed = _parse_request(buf)
            if parsed is not None:
                break
            chunk = conn.recv(1024)
            if not chunk:
                return
            buf += chunk

        method, target, headers, buf = parsed
        keep = headers.get('connection', 'keep-alive').lower() != 'close'

        if method not in ('GET', 'HEAD'):
            _respond(conn, '405 Method Not Allowed', keep=False)
            return
        head_only = method == 'HEAD'

        path, _, query = target.partition('?')

        if path == '/fw/published.txt':
            versions = published_versions()
            body = ''.join(v + '\n' for v in versions).encode()
            log.info('fw http: %s %s from %s (%d versions)',
                     method, path, ip, len(versions))
            _respond(conn, '200 OK', body, 'text/plain',
                     keep=keep, head_only=head_only)
            if not keep:
                return
            continue

        # The device asks for /fw/manifest.txt?imei=<imei>&v=<running version>.
        # Redirect that onto the manifest built for that unit; a request with
        # no usable IMEI gets a 404 rather than somebody else's image.
        if path == '/fw/manifest.txt':
            per_device = manifest_path(_query_param(query, 'imei'))
            if per_device is None:
                log.info('fw http: %s %s from %s -> 404 (no usable imei)',
                         method, target, ip)
                _respond(conn, '404 Not Found', b'not found\n', 'text/plain',
                         keep=keep, head_only=head_only)
                if not keep:
                    return
                continue
            path = '/fw/' + os.path.basename(per_device)

        # basename() flattens any traversal attempt; only /fw/<file> exists.
        fpath = os.path.join(config.FW_DIR, os.path.basename(path))
        if not path.startswith('/fw/') or not os.path.isfile(fpath):
            log.info('fw http: %s %s from %s -> 404', method, target, ip)
            _respond(conn, '404 Not Found', b'not found\n', 'text/plain',
                     keep=keep, head_only=head_only)
            if not keep:
                return
            continue

        size = os.path.getsize(fpath)
        ctype = 'text/plain' if fpath.endswith('.txt') else 'application/octet-stream'
        rng = headers.get('range', '')
        match = re.fullmatch(r'bytes=(\d+)-(\d*)', rng) if rng else None

        if match:
            start = int(match.group(1))
            end = int(match.group(2)) if match.group(2) else size - 1
            end = min(end, size - 1)
            if start >= size or start > end:
                _respond(conn, '416 Range Not Satisfiable', b'',
                         extra='Content-Range: bytes */%d\r\n' % size,
                         keep=keep, head_only=head_only)
            else:
                with open(fpath, 'rb') as f:
                    f.seek(start)
                    body = f.read(end - start + 1)
                # Log the first and last range only, not all few hundred.
                if start == 0 or end == size - 1:
                    log.info('fw http: %s %s bytes=%d-%d/%d from %s',
                             method, path, start, end, size, ip)
                _respond(conn, '206 Partial Content', body, ctype,
                         extra='Content-Range: bytes %d-%d/%d\r\n' % (start, end, size),
                         keep=keep, head_only=head_only)
        else:
            log.info('fw http: %s %s (%d bytes) from %s', method, path, size, ip)
            with open(fpath, 'rb') as f:
                body = f.read()
            _respond(conn, '200 OK', body, ctype, keep=keep, head_only=head_only)

        if not keep:
            return
