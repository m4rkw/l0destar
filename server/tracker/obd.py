"""OBD-II PID selection and the supported-PID scan.

One catalogue of the Mode 01 PIDs a unit can be asked to send, and the helpers
for the two things that travel as hex text: the selection (``xop``, the PIDs
the unit captures, as concatenated 2-digit hex in the user's order) and the
supported-PID bitmaps (``osp=``, what the car's ECU says it can answer).

The firmware keeps the same table (scale included).  The wire integer for a PID
is ``round(value * scale)``; the server divides it back.  Sixteen keys predate
the catalogue (``orpm``, ``ocl``, ...): they keep their own ``log`` columns and
scales (telemetry.OBD_FIELDS), and a catalogue entry names them in
``legacy_keys``.  Every other PID travels as ``o<2-hex>=<int>`` and is stored
in ``log.obd_extra``.
"""

import re

MAX_SELECTED = 12
DEFAULT_SELECTION = ['0C', '05']
SUPPORTED_MAX_CHARS = 56        # 7 words of 8 hex digits: PIDs 0x01-0xE0

# Kept by name so the catalogue below reads as a table.
_PCT = '%'


def _e(pid, name, unit, size, scale, *legacy):
    return {'pid': '%02X' % pid, 'name': name, 'unit': unit, 'size': size,
            'scale': scale, 'legacy_keys': list(legacy)}


CATALOGUE = [
    _e(0x01, 'Monitor status', '', 4, 1, 'omil', 'odtc'),
    _e(0x03, 'Fuel system status', 'bitmap', 2, 1, 'ofs'),
    _e(0x04, 'Engine load', _PCT, 1, 10, 'old'),
    _e(0x05, 'Coolant temperature', '°C', 1, 1, 'ocl'),
    _e(0x06, 'Short fuel trim, bank 1', _PCT, 1, 10, 'ostft'),
    _e(0x07, 'Long fuel trim, bank 1', _PCT, 1, 10, 'oltft'),
    _e(0x08, 'Short fuel trim, bank 2', _PCT, 1, 10),
    _e(0x09, 'Long fuel trim, bank 2', _PCT, 1, 10),
    _e(0x0A, 'Fuel pressure', 'kPa', 1, 1),
    _e(0x0B, 'Intake manifold pressure', 'kPa', 1, 1),
    _e(0x0C, 'Engine speed', 'rpm', 2, 1, 'orpm', 'ormin', 'ormax', 'oravg'),
    _e(0x0D, 'Vehicle speed', 'km/h', 1, 1, 'ospd'),
    _e(0x0E, 'Timing advance', '°', 1, 10, 'otim'),
    _e(0x0F, 'Intake air temperature', '°C', 1, 1, 'oit'),
    _e(0x10, 'Mass air flow', 'g/s', 2, 100, 'omaf'),
    _e(0x11, 'Throttle position', _PCT, 1, 10, 'oth'),
    _e(0x14, 'O2 sensor 1 (B1S1) voltage', 'V', 2, 1000),
    _e(0x15, 'O2 sensor 2 (B1S2) voltage', 'V', 2, 1000),
    _e(0x16, 'O2 sensor 3 (B1S3) voltage', 'V', 2, 1000),
    _e(0x17, 'O2 sensor 4 (B1S4) voltage', 'V', 2, 1000),
    _e(0x18, 'O2 sensor 5 (B2S1) voltage', 'V', 2, 1000),
    _e(0x19, 'O2 sensor 6 (B2S2) voltage', 'V', 2, 1000),
    _e(0x1A, 'O2 sensor 7 (B2S3) voltage', 'V', 2, 1000),
    _e(0x1B, 'O2 sensor 8 (B2S4) voltage', 'V', 2, 1000),
    _e(0x1F, 'Run time since engine start', 's', 2, 1),
    _e(0x21, 'Distance with MIL on', 'km', 2, 1),
    _e(0x22, 'Fuel rail pressure (relative to manifold)', 'kPa', 2, 10),
    _e(0x23, 'Fuel rail gauge pressure', 'kPa', 2, 1),
    _e(0x2C, 'Commanded EGR', _PCT, 1, 10),
    _e(0x2D, 'EGR error', _PCT, 1, 10),
    _e(0x2E, 'Commanded evaporative purge', _PCT, 1, 10),
    _e(0x2F, 'Fuel level', _PCT, 1, 10),
    _e(0x30, 'Warm-ups since codes cleared', 'count', 1, 1),
    _e(0x31, 'Distance since codes cleared', 'km', 2, 1),
    _e(0x33, 'Barometric pressure', 'kPa', 1, 1),
    _e(0x3C, 'Catalyst temperature, bank 1 sensor 1', '°C', 2, 10),
    _e(0x3D, 'Catalyst temperature, bank 2 sensor 1', '°C', 2, 10),
    _e(0x3E, 'Catalyst temperature, bank 1 sensor 2', '°C', 2, 10),
    _e(0x3F, 'Catalyst temperature, bank 2 sensor 2', '°C', 2, 10),
    _e(0x42, 'Control module voltage', 'V', 2, 1000),
    _e(0x43, 'Absolute load value', _PCT, 2, 10),
    _e(0x44, 'Commanded equivalence ratio', 'lambda', 2, 1000),
    _e(0x45, 'Relative throttle position', _PCT, 1, 10),
    _e(0x46, 'Ambient air temperature', '°C', 1, 1),
    _e(0x47, 'Absolute throttle position B', _PCT, 1, 10),
    _e(0x48, 'Absolute throttle position C', _PCT, 1, 10),
    _e(0x49, 'Accelerator pedal position D', _PCT, 1, 10),
    _e(0x4A, 'Accelerator pedal position E', _PCT, 1, 10),
    _e(0x4B, 'Accelerator pedal position F', _PCT, 1, 10),
    _e(0x4C, 'Commanded throttle actuator', _PCT, 1, 10),
    _e(0x4D, 'Time run with MIL on', 'min', 2, 1),
    _e(0x4E, 'Time since codes cleared', 'min', 2, 1),
    _e(0x52, 'Ethanol fuel', _PCT, 1, 10),
    _e(0x5C, 'Engine oil temperature', '°C', 1, 1),
    _e(0x5E, 'Engine fuel rate', 'L/h', 2, 100),
]
BY_PID = {entry['pid']: entry for entry in CATALOGUE}

_PID_KEY = re.compile(r'^o([0-9A-F]{2})$')
_HEX_PID = re.compile(r'[0-9A-Fa-f]{2}')


def catalogue():
    """The catalogue as the API shows it: pid, name, unit, scale, size, and
    legacy_key (the first of the entry's own record keys, or null)."""
    return [{'pid': e['pid'], 'name': e['name'], 'unit': e['unit'],
             'scale': e['scale'], 'size': e['size'],
             'legacy_key': e['legacy_keys'][0] if e['legacy_keys'] else None}
            for e in CATALOGUE]


def pid_key(key):
    """The PID ('0B') an ``o<2hex>`` record key names, or None for any other
    key, the legacy ones included."""
    match = _PID_KEY.match(key)
    return match.group(1) if match else None


def extra_value(pid, raw):
    """A generic o<pid>= value as the number it stands for (wire int / scale),
    or None when the PID is unknown, is one of the legacy ones (those have
    their own columns) or ``raw`` is not a whole number."""
    entry = BY_PID.get(pid)
    if entry is None or entry['legacy_keys']:
        return None
    try:
        return round(int(raw) / entry['scale'], 4)
    except (TypeError, ValueError):
        return None


# -- the selection -----------------------------------------------------------------

def parse_selection(value):
    """The PIDs in ``value`` as a list of uppercase 2-digit hex strings in the
    order given, duplicates dropped.  ``value`` is a list or a string; a string
    may separate PIDs with commas, '|', ';' or spaces, or run them together
    (``0C05``).  ValueError for anything else, a PID the catalogue lacks or
    more than MAX_SELECTED PIDs.  An empty selection is [] (it means the
    default)."""
    if isinstance(value, str):
        value = re.sub(r'[\s,|;]+', '', value)
        if not re.fullmatch(r'(?:[0-9A-Fa-f]{2})*', value):
            raise ValueError('PIDs are 2-digit hex, for example 0C,05,0B')
        items = _HEX_PID.findall(value)
    elif isinstance(value, (list, tuple)):
        items = []
        for item in value:
            if not isinstance(item, str) or not re.fullmatch(r'[0-9A-Fa-f]{2}', item.strip()):
                raise ValueError('PIDs are 2-digit hex, for example 0C,05,0B')
            items.append(item.strip())
    else:
        raise ValueError('PIDs must be a list or a comma-separated string')
    pids = []
    for item in items:
        pid = item.upper()
        if pid not in BY_PID:
            raise ValueError('PID %s is not in the catalogue (GET /api/1.0/obd/pids)' % pid)
        if pid not in pids:
            pids.append(pid)
    if len(pids) > MAX_SELECTED:
        raise ValueError('at most %d PIDs can be selected' % MAX_SELECTED)
    return pids


def selection_text(pids):
    """The stored / on-the-wire form of a selection: ['0C', '05'] -> '0C05'."""
    return ''.join(pids)


def selection_list(text):
    """The PIDs in a stored or reported selection ('0C05'), tolerantly: bad
    pairs and duplicates are left out.  [] for NULL or empty."""
    if not isinstance(text, str):
        return []
    pids = []
    for pair in re.findall(r'[0-9A-F]{2}', text.upper()):
        if pair not in pids:
            pids.append(pair)
    return pids[:MAX_SELECTED]


def valid_selection_text(text):
    """True for a tb= xop value: up to 12 uppercase hex pairs, may be empty."""
    return (isinstance(text, str) and len(text) <= 2 * MAX_SELECTED
            and re.fullmatch(r'(?:[0-9A-F]{2})*', text) is not None)


def effective_selection(device, built=None):
    """(the PIDs the unit captures, whether that is the default).  The
    override on the device row, else the value the unit reported being built
    with (``built``, the tb= xop text), else the minimal default."""
    override = selection_list(device.get('obd_pids'))
    if override:
        return override, False
    reported = selection_list(built)
    return (reported or list(DEFAULT_SELECTION)), True


def selectable(supported):
    """The PIDs that may be selected: only the default ones until a scan has
    said what the car supports, then the supported ones the catalogue knows."""
    if supported is None:
        return list(DEFAULT_SELECTION)
    known = {entry['pid'] for entry in catalogue()}
    return [pid for pid in supported if pid in known]


# -- the supported-PID bitmaps -------------------------------------------------------

def valid_supported(text):
    """True for an osp= value: whole 8-digit words of uppercase hex, at least
    one and at most 7 (SUPPORTED_MAX_CHARS)."""
    return (isinstance(text, str) and 8 <= len(text) <= SUPPORTED_MAX_CHARS
            and len(text) % 8 == 0
            and re.fullmatch(r'[0-9A-F]+', text) is not None)


def supported_pids(text):
    """The PIDs set in an osp= bitmap string as uppercase hex strings in
    order, or None when it is not a valid one.  The chain bits (the last bit
    of each word: PIDs 20, 40, ... say another word follows) are not PIDs a
    unit can read and are left out."""
    if not valid_supported(text):
        return None
    pids = []
    for index in range(len(text) // 8):
        word = int(text[index * 8:index * 8 + 8], 16)
        for bit in range(32):
            pid = index * 32 + bit + 1
            if word >> (31 - bit) & 1 and pid % 32:
                pids.append('%02X' % pid)
    return pids
