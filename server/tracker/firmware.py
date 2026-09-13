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

Failed updates
--------------
An image is swapped in as a test and reverted by MCUboot unless it confirms
itself, so a build that boots and dies takes the old one back — and the
server, knowing nothing about that, goes on advertising it while the device
goes on fetching it.  On a parked vehicle that loop is a flattened battery,
not a missed update.  So the lifecycle is tracked per device: what was
staged (``note_staged``), whether the version now running matches it
(``check_running``), and if it does not, the version is withheld from that
device until a newer one is published or an operator clears it
(``note_failed`` / ``blocked_version``).

Delivery
--------
Downloads are served over the same TLS port as telemetry.  See
``listeners/tls.py`` for why the two protocols can share a port, and
``docs/PROTOCOL.md`` for the request sequence the nRF91 FOTA stack produces.
"""

import datetime
import os
import re

from . import config, db, logs, notify

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


# How long after a reported stage a device is still expected to be rebooting.
# A record naming a different running version after this is a revert rather
# than a device that simply has not restarted yet.
STAGE_GRACE_S = 600


def _version_ok(version):
    return bool(version) and bool(VERSION_RE.match(str(version).strip()))


def note_staged(device, version, database, log=None):
    """The device says it has staged `version` and is rebooting into it."""
    log = log or logs.udp
    if not _version_ok(version):
        log.warning('fota staged report with bad version %r from %s',
                    version, device['imei'])
        return
    database.query(
        'UPDATE `device` SET `fw_staged` = %s, `fw_staged_at` = NOW() '
        'WHERE `id` = %s',
        (str(version).strip(), device['id']),
    )
    log.info('%s staged %s', device['imei'], version)


def note_failed(device, staged, running, database, log=None):
    """`staged` was applied and did not survive: MCUboot put `running` back.

    Reported by the device from the boot that finds itself running the old
    image, and inferred by check_running() for a device whose reverted-to
    image is too old to report it.  Idempotent either way: the device repeats
    the report until a send succeeds, and only the first one counts.
    """
    log = log or logs.udp
    if not _version_ok(staged):
        return
    staged = str(staged).strip()
    row = database.one(
        'SELECT `fw_blocked`, `fw_fail_count` FROM `device` WHERE `id` = %s',
        (device['id'],),
    ) or {}
    already = row.get('fw_blocked') == staged
    fails = (row.get('fw_fail_count') or 0) + (0 if already else 1)

    database.query(
        'UPDATE `device` SET `fw_staged` = NULL, `fw_staged_at` = NULL, '
        '`fw_blocked` = %s, `fw_fail_count` = %s WHERE `id` = %s',
        (staged, fails, device['id']),
    )
    log.error('%s: update to %s failed, running %s (attempt %d) — withholding it',
              device['imei'], staged, running, fails)

    # On the transition into blocked only.  The device repeats its report
    # until one gets through, and a notification per repeat is noise.
    if not already:
        try:
            notify.device_alert(
                device['name'],
                'fota: %s failed to boot (running %s) — updates withheld '
                'until retried' % (staged, running), 1)
        except Exception:
            log.exception('alert failed for fota failure')


def check_running(device, running, database, log=None):
    """Reconcile the version a record reports against what we staged.

    Same version means the update took; a different one, once the reboot has
    had time to happen, means it was reverted.
    """
    log = log or logs.udp
    staged = device.get('fw_staged')
    if not running or not staged:
        return
    running = str(running).strip()

    if running == staged:
        database.query(
            'UPDATE `device` SET `fw_staged` = NULL, `fw_staged_at` = NULL, '
            '`fw_blocked` = NULL, `fw_fail_count` = 0 WHERE `id` = %s',
            (device['id'],),
        )
        log.info('%s is running %s — update confirmed', device['imei'], staged)
        # The "updated to" notification belongs here rather than on the
        # device.  The device can only raise it on the one boot that writes
        # the MCUboot confirm flag, and on that boot it is a queued alert in
        # RAM needing a working link before the next reset; lost there, it is
        # never re-raised, so a missing notification said nothing about
        # whether the update worked.  The version a device reports is state:
        # it survives reboots and arrives with the first record that gets
        # through.  Clearing fw_staged above makes this fire exactly once.
        try:
            notify.device_alert(device['name'],
                                'fota: updated to %s' % staged, 0)
        except Exception:
            log.exception('alert failed for fota success')
        return

    staged_at = device.get('fw_staged_at')
    if staged_at is not None:
        age = (datetime.datetime.now() - staged_at).total_seconds()
        if age < STAGE_GRACE_S:
            return       # still rebooting, or the record predates the swap
    note_failed(device, staged, running, database, log)


def note_check_in(imei, running, log=None):
    """Confirm a staged update from the boot check, and only confirm.

    A device fetches its manifest with v=<running version> moments after
    booting, which settles a successful update long before telemetry does.
    But the request says nothing trustworthy about *failure*: the publisher
    verifies every push with v=0.0.0 from the build host, and reading that
    as "the device is running 0.0.0" marked a perfectly good update as
    failed and withheld it.  So only an exact match with the staged version
    counts here.  A revert is reported by the device itself
    (F,fota,failed) or inferred from the version in its telemetry.
    """
    log = log or logs.tls
    imei = str(imei or '')
    if not imei.isdigit() or not _version_ok(running):
        return
    database = db.tls
    try:
        device = database.one('SELECT * FROM `device` WHERE `imei` = %s', (imei,))
        if device and device.get('fw_staged') == str(running).strip():
            check_running(device, running, database, log)
    except Exception:
        log.exception('check-in reconciliation failed for %s', imei)


def blocked_version(imei, database=None):
    """Version withheld from this IMEI, or None."""
    imei = str(imei or '')
    if not imei.isdigit():
        return None
    # Asked from the TLS connection threads, which own a connection each on
    # this handle; a fresh DB() per manifest request was never closed.
    database = database or db.tls
    try:
        row = database.one('SELECT `fw_blocked` FROM `device` WHERE `imei` = %s',
                           (imei,))
    except Exception:
        logs.udp.exception('fw_blocked lookup failed for %s', imei)
        return None
    return (row or {}).get('fw_blocked')


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
            req_imei = _query_param(query, 'imei')

            # The power-on check carries v=<running version>, which settles
            # a staged update without waiting for telemetry.  That matters:
            # after a reboot the device's engine-off interval is back at its
            # compiled default until a response restores it, so on a parked
            # vehicle the next record can be a long way off — and the update
            # verdict, including the notification, should not wait for a
            # GNSS fix in a car park.  A no-op unless a version is staged.
            note_check_in(req_imei, _query_param(query, 'v'), log)

            # Withheld from this device after it failed to boot here.  The
            # power-on check fetches a manifest unconditionally, so silence
            # is not enough: the device has to be told, and told which
            # version, so it can scope the refusal to that one build and
            # still take a newer one.  Firmware predating status= sees
            # version=<blocked>, which is never newer than what it is
            # running, so it does nothing either.
            # Version-scoped, like every other part of this: the refusal
            # covers the build that failed here, not the device.  Once a
            # newer one is published it is served normally — that is the
            # fix arriving, and blocking it would strand the unit on the
            # broken build with no way back.
            blocked = blocked_version(req_imei)
            if blocked and latest_version(req_imei) == blocked:
                body = ('version=%s\nstatus=blocked\n'
                        'reason=failed to boot on this device\n'
                        % blocked).encode()
                log.info('fw http: %s %s from %s -> blocked (%s)',
                         method, target, ip, blocked)
                _respond(conn, '200 OK', body, 'text/plain',
                         keep=keep, head_only=head_only)
                if not keep:
                    return
                continue

            per_device = manifest_path(req_imei)
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
