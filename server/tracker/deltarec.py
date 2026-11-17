"""Delta records: what the drive stream leaves out of a record because the one
before it in the same datagram already said it.

The stream sends several records in a datagram (firmware APP_DRIVE_STREAM_BATCH)
and most of what a record carries has not moved since the last one a few
hundred milliseconds ago: the altitude, HDOP and satellite count, the die
temperatures and supply, the signal reading, and the OBD figures the K wire only
refreshes once a second.  The first record of a datagram is sent whole.  Each
one after it may be sent as a delta:

* ``d=1`` is among its extras;
* a fixed field from ``spd`` on that equals the previous record's is empty;
* an extra of the sticky set (STICKY: ``it mt vs up``, the signal reading
  ``rsrp snr band pathloss rsrq ce txrep`` and the OBD keys ``o...``) that
  equals the previous record's is not sent;
* everything else (the timestamp, position, ``rid``, the accelerometer and the
  rest) is sent as usual.

``expand_lines`` turns such a record back into the text it was made from before
anything else reads the datagram, so parse_csv_line and everything after it see
only whole records.  Nothing is carried between datagrams: a datagram that is
lost, repeated or arrives late decodes exactly as it would have in order, which
is why a delta is only ever against a record of its own datagram.  The firmware
sends a record whole when it cannot be expressed as a delta (it lacks a sticky
extra the previous one had, which would otherwise read as "unchanged").

This is not the carrying forward the PER_PACKET_KEYS comment in telemetry.py
rules out.  That would put a reading on a row where none was taken.  Here the
device took the reading and it came out the same as the last one, and the row
gets the value it would have been sent with.

Standard library only: the same file runs inside the tracker package and as a
module beside the live main.py.
"""
import re

MARKER = 'd=1'
FIXED = 12                 # ts, lat, lon, spd, alt, hdg, hdop, sat, bat, ign, up, pon
BLANKABLE_FROM = 3         # fixed fields from here (spd) may be empty

# Extras that may be left out when unchanged.  Must match the firmware's
# record_delta.h; DELTA_VERSION says which list a d= marker means.
STICKY = frozenset((
    'it', 'mt', 'vs', 'up',
    'rsrp', 'snr', 'band', 'pathloss', 'rsrq', 'ce', 'txrep',
    'orpm', 'ormin', 'ormax', 'oravg', 'ospd', 'ocl', 'oit', 'old', 'oth',
    'omaf', 'otim', 'ostft', 'oltft', 'ofs', 'omil', 'odtc',
))
DELTA_VERSION = 1

# o<2 hex>: any other selected OBD PID (obd.pid_key).
_PID_KEY = re.compile(r'^o[0-9A-F]{2}$')

# Extras whose body is one value that may itself hold ';' or ':' pairs.  Never
# split, never carried.
OPAQUE = ('dbg=', 'rst=', 'ir=', 'ie=', 'fc=', 'fe=', 'osp=', 'tb=', 'wt=',
          'bk=', 'ap=', 'ak=', 'ws=', 'mv=', 'acc=')

# Lines of a datagram that are not records (telemetry.process_lines).
_NOT_RECORDS = ('A,', 'D,', 'F,', 'L,')


def is_sticky(key):
    return key in STICKY or _PID_KEY.match(key) is not None


def _split(line):
    """(fixed fields, extras): the timestamp's own comma kept inside the first."""
    parts = line.split(',')
    if len(parts) < FIXED + 1:
        raise ValueError('record has %d fields, expected at least %d'
                         % (len(parts) - 1, FIXED))
    return [parts[0] + ',' + parts[1]] + parts[2:FIXED + 1], parts[FIXED + 1:]


def _pairs(group):
    """The (key, pair text) of each ``key=value`` in an extra, none for an
    opaque one."""
    if group.startswith(OPAQUE):
        return []
    out = []
    for pair in group.split(';'):
        key, eq, _ = pair.partition('=')
        if eq:
            out.append((key, pair))
    return out


# What the sender blanks, by position: spd, alt, hdg, hdop, sat, bat, up.  The
# timestamp and position always go, and so do ignition and the powered-on flag,
# which are state the server acts on and cost a byte each.
BLANKED = (3, 4, 5, 6, 7, 8, 10)


def encode(previous, line):
    """``line`` as a delta against the whole record ``previous``, or ``line``
    itself when it cannot be one or would not come out shorter.

    The server never encodes.  This is the specification the firmware's
    record_delta.h is held to (tests/test_record_delta.py builds the C version
    and compares its output with this one), and what the round-trip tests use.
    """
    try:
        fixed, extras = _split(line)
        before, before_extras = _split(previous)
    except ValueError:
        return line
    if MARKER in extras or MARKER in before_extras:
        return line

    was = {key: pair.partition('=')[2] for group in before_extras
           for key, pair in _pairs(group) if is_sticky(key)}
    now = {key for group in extras for key, _ in _pairs(group)}
    if not was.keys() <= now:
        return line            # a sticky reading went away: "unchanged" would be a lie

    out = [value if i not in BLANKED or value != before[i] else ''
           for i, value in enumerate(fixed)]
    kept = []
    for group in extras:
        if group.startswith(OPAQUE):
            kept.append(group)
            continue
        pairs = [pair for pair in group.split(';')
                 if not ('=' in pair and is_sticky(pair.partition('=')[0])
                         and was.get(pair.partition('=')[0]) == pair.partition('=')[2])]
        if pairs:
            kept.append(';'.join(pairs))
    delta = ','.join(out + kept + [MARKER])
    return delta if len(delta) < len(line) else line


def encode_lines(lines):
    """A datagram's records as the stream sends them: the first whole, each
    later one a delta against the whole record before it."""
    out = []
    previous = None
    for line in lines:
        out.append(line if previous is None else encode(previous, line))
        if not line.startswith(_NOT_RECORDS):
            previous = line
    return out


def is_marked(line):
    """Whether a record line says it is a delta (and so cannot be read until it
    has been expanded)."""
    return ',' + MARKER in line and MARKER in line.split(',')[FIXED + 1:]


def expand(previous, line):
    """The whole record a delta stands for, given the whole record before it."""
    fixed, extras = _split(line)
    if MARKER not in extras:
        return line
    before, before_extras = _split(previous)

    out = []
    for i, value in enumerate(fixed):
        if value == '':
            if i < BLANKABLE_FROM:
                raise ValueError('delta record with no %s' % ('ts', 'lat', 'lon')[i])
            value = before[i]
        out.append(value)

    extras = [group for group in extras if group != MARKER]
    sent = {key for group in extras for key, _ in _pairs(group)}
    carried = [pair for group in before_extras for key, pair in _pairs(group)
               if is_sticky(key) and key not in sent]
    if carried:
        extras.append(';'.join(carried))
    return ','.join(out + extras)


def expand_lines(lines):
    """A datagram's lines with every delta record made whole.

    A delta that cannot be expanded (nothing before it to be a delta of, or a
    field it needs missing) is left as it came: it still carries its marker, so
    the parser refuses it and the record is logged and skipped like any other
    malformed one, rather than stored with holes in it."""
    out = []
    previous = None
    for line in lines:
        if not line.startswith(_NOT_RECORDS):
            if previous is not None and ',' + MARKER in line:
                try:
                    line = expand(previous, line)
                except ValueError:
                    out.append(line)
                    continue
            if not is_marked(line):
                previous = line
        out.append(line)
    return out
