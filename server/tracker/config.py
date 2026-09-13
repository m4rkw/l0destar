"""Configuration loading.

All deployment-specific values live in ``config.yaml`` beside the package.
Nothing in the source tree carries a hostname, credential or coordinate; see
``config.yaml.example`` for the full set of keys and their defaults.
"""

import os
import yaml

APP_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

CONFIG_PATH = os.environ.get('TRACKER_CONFIG', os.path.join(APP_DIR, 'config.yaml'))


def _load():
    with open(CONFIG_PATH) as f:
        cfg = yaml.safe_load(f) or {}
    if not isinstance(cfg, dict):
        raise RuntimeError('%s must contain a YAML mapping' % CONFIG_PATH)
    return cfg


config = _load()


def get(key, default=None):
    return config.get(key, default)


def require(key):
    if key not in config or config[key] in (None, ''):
        raise RuntimeError("'%s' missing from %s" % (key, CONFIG_PATH))
    return config[key]


# -- web ---------------------------------------------------------------------

SESSION_SECRET = require('session_secret')
SESSION_LIFETIME_DAYS = int(get('session_lifetime_days', 30))
MAPS_API_KEY = get('google_maps_api_key', '')

# Device a read is about when the request names none: the map the web UI
# lands on, the bare /api/1.0/track redirect.  Optional.  Unset, or naming a
# device that is not enrolled, a request that names none falls back to the
# only enrolled device, and with several the web UI opens on the device list.
# A request that changes anything always has to name its device.
DEFAULT_DEVICE_IMEI = str(get('default_device', '') or '')

# Login rate limiting: authoptions requests permitted per IP per window.
RATE_LIMIT_REQUEST_COUNT = int(get('rate_limit_request_count', 5))
RATE_LIMIT_RESET_PERIOD = int(get('rate_limit_reset_period', 3600))

# -- engine-running heuristic ------------------------------------------------
# Ignition state alone cannot distinguish "key on, engine off" from "running".
# A charging alternator holds the bus above ENGINE_RUNNING_VOLTAGE, so the UI
# calls the engine running if any of the last ENGINE_STOPPED_COUNT readings was
# above it.  The hysteresis stops smart/regenerative charging systems, which
# deliberately swing the bus over a wide range, from flickering the display.
ENGINE_RUNNING_VOLTAGE = float(get('engine_running_voltage', 13.0))
ENGINE_STOPPED_COUNT = int(get('engine_stopped_count', 10))

# -- listeners ---------------------------------------------------------------

UDP_HOST = get('udp_host', '0.0.0.0')
UDP_PORT = int(get('udp_port', 65480))
UDP_ENABLED = bool(get('udp_enabled', True))
MAX_DGRAM = 2048

TLS_HOST = get('tls_host', '0.0.0.0')
TLS_PORT = int(get('tls_port', 65481))
TLS_CERT = get('tls_cert', '')
TLS_KEY = get('tls_key', '')

# Accept telemetry frames on the TLS port.  Off by default: that transport
# authenticates a device by nothing more than the IMEI it presents, and the
# port has to be reachable from the internet for firmware downloads, so with
# this on anyone who learns an IMEI can post positions and alerts as that
# device — and collect the commands queued for it.  Current firmware reports
# over UDP and only comes here for updates, which are served either way.
TLS_TELEMETRY = bool(get('tls_telemetry', False))

DTLS_HOST = get('dtls_host', '0.0.0.0')
DTLS_PORT = int(get('dtls_port', 65482))
DTLS_CERT = get('dtls_cert', '') or TLS_CERT
DTLS_KEY = get('dtls_key', '') or TLS_KEY
DTLS_LIB = get('dtls_lib', '')

# Drop the movement_alarm response field; shortens every reply.
SLIM_RESPONSE = bool(get('slim_response', False))

# -- storage -----------------------------------------------------------------

DATABASE = require('database')
LOG_DIR = get('log_dir', os.path.join(APP_DIR, 'logs'))
FW_DIR = get('fw_dir', os.path.join(APP_DIR, 'fw'))

# -- journeys ----------------------------------------------------------------
# A journey is closed when the ignition goes off.  A key-off/key-on inside
# JOURNEY_RESUME_SECONDS is treated as one journey with a stop in it (fuel,
# traffic light restart on a stop-start car) rather than two.
JOURNEY_ENABLED = bool(get('journeys', True))
JOURNEY_RESUME_SECONDS = int(get('journey_resume_seconds', 300))

# -- notifications -----------------------------------------------------------

NOTIFY = get('notify') or {}

# -- home check --------------------------------------------------------------
# Optional watchdog: an external cron POSTs /api/1.0/home and the server
# reports whether each listed vehicle's last fix is near its home.  Used to
# catch a tracker that has silently stopped reporting while parked at home —
# the last row keeps looking plausible, so only an external check notices.


def home_checks(raw):
    """``home_check`` as a list of ``{imei, latitude, longitude, radius_m}``.

    A single mapping is the original form and still accepted; a list covers
    several vehicles.  An entry missing its IMEI or a coordinate is a mistake
    to report rather than work around, so it stops the server at startup
    instead of leaving that vehicle silently unchecked.
    """
    if not raw:
        return []
    if isinstance(raw, dict):
        raw = [raw]
    if not isinstance(raw, list):
        raise RuntimeError('home_check in %s must be a mapping or a list of them'
                           % CONFIG_PATH)
    checks = []
    for number, entry in enumerate(raw, 1):
        if (not isinstance(entry, dict) or not entry.get('imei')
                or entry.get('latitude') is None or entry.get('longitude') is None):
            raise RuntimeError('home_check entry %d in %s needs imei, latitude '
                               'and longitude' % (number, CONFIG_PATH))
        checks.append({
            'imei': str(entry['imei']),
            'latitude': float(entry['latitude']),
            'longitude': float(entry['longitude']),
            'radius_m': float(entry.get('radius_m', 300)),
        })
    return checks


HOME_CHECKS = home_checks(get('home_check'))
