"""The device event log.

One row in `device_event` for each thing that happens to a unit that the phone
app might list: ignition on and off, a firmware update staged, applied or
failed, a command queued, collected, answered, cancelled or expired, a reboot,
a setting changed, a result arriving.  page() reads it back, newest first, for
GET /api/1.0/devices/events.

Writing never gets in the way.  record() and record_ignition() catch every
error and log it: a line missing from the log must not cost a telemetry record
or fail an API call.

A row may carry a `ref`, unique per device.  Something derived from a stored
record - a reboot, an ignition change, a firmware change - is recorded under a
ref built from the record (ref()), so it is stored once however often it is
looked at: when the record arrives, and again when
tools/backfill_device_events.py reads the log.  Events with no natural key (a
command queued) have no ref.

`at` is server-local time, as in the log table's `timestamp`.

This file is the same in the live tracker (device_events.py) and in the public
package (tracker/device_events.py).  It uses the standard library and the
handle it is given (.one / .all / .query, as in db.py) and nothing else.
"""

import datetime
import logging
import re

log = logging.getLogger(__name__)

# What the app is told an event is.
KINDS = ('firmware', 'ignition', 'command', 'reboot', 'result', 'setting', 'other')

TEXT_MAX = 200          # the column's width
DEFAULT_LIMIT = 200
MAX_LIMIT = 500


# -- keys and times -----------------------------------------------------------

def stamp(when):
    """'YYYY-MM-DD HH:MM:SS', the form every 'at' in the API takes.  A string
    already in the log's form ('YYYY-MM-DD HH:MM:SS.ffffff') is cut to it."""
    if isinstance(when, str):
        return when[:19]
    return when.strftime('%Y-%m-%d %H:%M:%S')


def ref(prefix, *parts):
    """A row key from a prefix and the facts that identify the event, e.g.
    ref('ign', fix_time) or ref('fw', '0.4.104', fix_time).  A datetime is
    written to the microsecond, so the key is the same whether it comes from a
    record being processed or from the stored column."""
    out = [prefix]
    for part in parts:
        if isinstance(part, datetime.datetime):
            part = part.strftime('%Y-%m-%d %H:%M:%S.%f')
        out.append(str(part))
    return ':'.join(out)[:96]


# -- writing -----------------------------------------------------------------

def record(database, device_id, kind, text, at=None, ref=None):
    """Add an event: kind is one of KINDS, text the short line the app shows,
    at a datetime (default now), ref the key described above.  A ref that is
    already stored adds nothing.  Returns False only when the insert failed."""
    if kind not in KINDS:
        log.warning('device event of unknown kind %r: %s', kind, text)
        kind = 'other'
    try:
        database.query(
            "INSERT INTO `device_event` (`device_id`, `at`, `type`, `text`, `ref`) "
            "VALUES (%s, %s, %s, %s, %s) "
            "ON DUPLICATE KEY UPDATE `id` = `id`",
            (device_id, stamp(at or datetime.datetime.now()), kind,
             text[:TEXT_MAX], ref))
    except Exception as error:
        _failed('device event not recorded for device %s: %s' % (device_id, text), error)
        return False
    return True


def _failed(what, error):
    """One line in the log for a write that failed.  No traceback: with the
    table missing every event fails, and a traceback for each is only noise."""
    log.warning('%s (%s: %s)', what, type(error).__name__, error)


def record_ignition(database, device_id, on, at, ref=None):
    """Record ignition going on (True) or off (False) at `at`, if that is a
    change.

    Records do not always arrive in the order they were built: a backlog is
    flushed behind the live record.  So "is it a change" is judged against the
    ignition events around `at`, not the newest one.  Nothing is added when the
    event before `at` already says the same; and when the first event after it
    does, that one is no longer a change - the state turned over earlier than it
    was told - and is removed.  Returns True when an event was added."""
    text = ignition_text(on)
    try:
        if ref and database.one(
                "SELECT `id` FROM `device_event` WHERE `device_id` = %s AND `ref` = %s",
                (device_id, ref)):
            return False
        when = stamp(at)
        before = database.one(
            "SELECT `text` FROM `device_event` WHERE `device_id` = %s "
            "AND `type` = 'ignition' AND `at` <= %s ORDER BY `at` DESC, `id` DESC LIMIT 1",
            (device_id, when))
        if before and before['text'] == text:
            return False
        after = database.one(
            "SELECT `id`, `text` FROM `device_event` WHERE `device_id` = %s "
            "AND `type` = 'ignition' AND `at` > %s ORDER BY `at`, `id` LIMIT 1",
            (device_id, when))
    except Exception as error:
        _failed('ignition event not recorded for device %s' % device_id, error)
        return False
    if not record(database, device_id, 'ignition', text, at, ref):
        return False
    if after and after['text'] == text:
        try:
            database.query("DELETE FROM `device_event` WHERE `id` = %s", (after['id'],))
        except Exception as error:
            _failed('ignition event not tidied for device %s' % device_id, error)
    return True


# -- reading -----------------------------------------------------------------

def parse_query(limit, before):
    """(limit, before) from the raw query-string values: limit defaults to
    DEFAULT_LIMIT and is capped at MAX_LIMIT, before is an event id or None.
    Raises ValueError, with the message to send back, for anything else."""
    if limit in (None, ''):
        limit = DEFAULT_LIMIT
    else:
        limit = _whole(limit)
        if limit < 1:
            raise ValueError('limit must be a whole number from 1 to %d' % MAX_LIMIT)
        limit = min(limit, MAX_LIMIT)
    if before in (None, ''):
        return limit, None
    before = _whole(before)
    if before < 1:
        raise ValueError('before must be an event id')
    return limit, before


def _whole(value):
    """The number in a string of plain digits, else 0 (int() alone would take
    '1_0', ' 5 ' and digits of other scripts as well)."""
    text = str(value)
    return int(text) if text.isascii() and text.isdigit() else 0


def page(database, device_id, limit=DEFAULT_LIMIT, before=None):
    """The device's events, newest first: [{'id', 'at', 'type', 'text'}].

    Ordered by time then id.  `before` is the id of the oldest event the
    caller has; the page is the events that follow it in that order.  (The
    ids are not in time order once old events have been filled in behind newer
    ones, which is why the cursor is an event, not a number to count from.)"""
    where = "`device_id` = %s"
    args = [device_id]
    if before is not None:
        cursor = database.one(
            "SELECT `at`, `id` FROM `device_event` WHERE `id` = %s AND `device_id` = %s",
            (before, device_id))
        if cursor:
            where += " AND (`at` < %s OR (`at` = %s AND `id` < %s))"
            args += [cursor['at'], cursor['at'], cursor['id']]
        else:
            where += " AND `id` < %s"
            args.append(before)
    rows = database.all(
        "SELECT `id`, `at`, `type`, `text` FROM `device_event` WHERE " + where +
        " ORDER BY `at` DESC, `id` DESC LIMIT %s", args + [limit])
    return [{'id': r['id'], 'at': stamp(r['at']), 'type': r['type'], 'text': r['text']}
            for r in rows]


# -- the words ---------------------------------------------------------------

def ignition_text(on):
    return 'Ignition on' if on else 'Ignition off'


# What the reset-cause flags (rst=, from the unit's reset register) say, most
# telling first.  Flags are sticky, so 'pin+sw+wdt' is the worst of them.
_RESET_WORDS = (('wdt', 'watchdog'), ('lockup', 'CPU lockup'),
                ('sw', 'software'), ('pin', 'hardware reset'))


def reset_text(rst):
    """'Rebooted (watchdog)' from a record's rst= value, e.g. 'pin+sw+wdt' or
    'wdt+stall:4aa02/3e254'.  Bare 'Rebooted' when no flag is known."""
    names = {part.split(':')[0] for part in (rst or '').split('+') if part}
    for key, word in _RESET_WORDS:
        if key in names:
            if key == 'wdt' and 'stall' in names:
                word += ', stalled'
            return 'Rebooted (%s)' % word
    return 'Rebooted'


def version_tuple(version):
    """(0, 4, 104) from '0.4.104', None for anything else."""
    if not isinstance(version, str) or not re.fullmatch(r'\d+(\.\d+)*', version.strip()):
        return None
    return tuple(int(p) for p in version.strip().split('.'))


def firmware_text(what, version, other=None):
    """The line for a firmware event.  what is 'downloading', 'staged',
    'applied' or 'failed'.  For 'applied', other is the version it replaced:
    an older one makes it 'restored' (the unit went back after a failed
    update).  For 'failed', other is the version that is still running."""
    if what == 'applied':
        new, old = version_tuple(version), version_tuple(other)
        if new and old and new < old:
            what = 'restored'
    text = 'Firmware %s %s' % (version, what)
    if what == 'failed' and other:
        text += ', still on %s' % other
    return text


def command_text(what, command):
    """The line for a command event.  what is 'queued', 'collected' (the
    unit was given it in a reply), 'acknowledged' (the unit said it ran it),
    'cancelled', 'replaced' (a newer command took its place) or 'expired'
    (nobody collected it in time).  command is one command, e.g. 'netscan' or
    'int=600'."""
    return 'Command %s: %s' % (what, command)


def command_parts(command):
    """The commands in a comma-joined command string, one event each."""
    return [p.strip() for p in (command or '').split(',') if p.strip()]


def setting_text(column, value):
    """The line for a setting changed, from the device column and its new
    value (an int)."""
    on = 'on' if value else 'off'
    if column == 'int':
        return 'Report interval set to %d s' % value if value else 'Report interval off'
    if column == 'backup_int':
        return ('Backup report interval set to %d s' % value if value
                else 'Backup report interval follows the normal one')
    if column == 'movement_alarm':
        return 'Movement alarm ' + on
    if column == 'alarm':
        return 'Ignition alarm ' + on
    if column == 'garage':
        return 'Garage mode ' + on
    if column == 'overnight_alarm':
        return 'Overnight alarm ' + on
    if column == 'overnight_alarm_hour_from':
        return 'Overnight alarm starts at %02d:00' % value
    if column == 'overnight_alarm_hour_to':
        return 'Overnight alarm ends at %02d:00' % value
    if column == 'gnss_off_window':
        return 'GNSS backup off window ' + on
    if column == 'gnss_off_hour_from':
        return 'GNSS backup off window starts at %02d:00' % value
    if column == 'gnss_off_hour_to':
        return 'GNSS backup off window ends at %02d:00' % value
    if column == 'gnss_backup':
        return 'GNSS backup supply set to ' + on
    return 'Setting changed: %s=%s' % (column, value)


def track_mode_text(on, why=None):
    text = 'Track mode ' + ('on' if on else 'off')
    return '%s (%s)' % (text, why) if why else text


def backup_supply_text(on):
    """The unit's own report (bk=) that its GNSS backup supply went on or off."""
    return 'GNSS backup supply ' + ('on' if on else 'off')


_REPORT_TITLES = {'scan': 'Network scan', 'selftest': 'Self test', 'status': 'Status',
                  'gnssstat': 'GNSS status', 'backlog': 'Backlog', 'logdump': 'Log dump'}


def result_text(name):
    """The line for an answer arriving: name is the F,rep,<name> report, or
    'scan' for the F,scan, network scan."""
    return '%s result received' % _REPORT_TITLES.get(name, name.capitalize())


def fault_text(code, raised):
    return 'Fault code %s %s' % (code, 'raised' if raised else 'cleared')


_FOTA_ALERT = re.compile(r'fota: \S+ -> (\d+(?:\.\d+)*)\b.*\b(downloading|rebooting)\s*$')
_FOTA_GAVE_UP = re.compile(r'fota: \S+ -> (\d+(?:\.\d+)*) failed after (\d+) attempts')
# Said twice by the unit: the failed-boot alert comes with the F,fota,failed
# report, and 'check queued' with the command's own acknowledgement.
_FOTA_REPEATED = re.compile(r'fota: (?:\S+ failed to boot, reverted to |check queued\s*$)')
_LOCATION_ALERT = re.compile(r'(google|apple|tomtom): ')


def alert_event(message):
    """(kind, text) for an alert line (A,) from the unit, or None when the
    log has it another way.

      'fota: A -> B available, downloading'   firmware: downloading B
      'fota: A -> B, rebooting'               None (the staged report says it)
      'fota: A -> B failed after N attempts'  firmware: B download failed
      'fota: B failed to boot, reverted to A' None (the failed report says it)
      'fota: check queued'                    None (the command's ack says it)
      'google: lat,lon' (and apple, tomtom)   result: Location received
      'gnss backup supply ON'                 None (the unit's bk= report says it)
      anything else                           other: 'Alert: <message>'"""
    message = (message or '').strip()
    if not message:
        return None
    m = _FOTA_ALERT.match(message)
    if m:
        return ('firmware', firmware_text('downloading', m.group(1))) \
            if m.group(2) == 'downloading' else None
    m = _FOTA_GAVE_UP.match(message)
    if m:
        return 'firmware', 'Firmware %s download failed after %s attempts' % m.groups()
    if _FOTA_REPEATED.match(message):
        return None
    if _LOCATION_ALERT.match(message):
        return 'result', 'Location received'
    if message.lower().startswith('gnss backup supply'):
        return None
    return 'other', 'Alert: ' + message
