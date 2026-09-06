"""Log channels.

Each transport gets its own file so a problem on one does not have to be
picked out of the others' traffic, and telemetry never has to be grepped out
of application errors.  ``debug`` is deliberately separate: the firmware only
emits debug counters and reset causes after something went wrong, so that file
is a short chronological incident list rather than bulk traffic.
"""

import logging
import os

from . import config


def _handler(name, fmt):
    path = os.path.join(config.LOG_DIR, name)
    handler = logging.FileHandler(path)
    handler.setFormatter(fmt)
    return handler


def _channel(name, filename, console=False, propagate=False, stamped=True):
    if stamped:
        fmt = logging.Formatter('%(asctime)s %(message)s', datefmt='%Y-%m-%d %H:%M:%S')
    else:
        fmt = logging.Formatter('%(message)s')
    log = logging.getLogger(name)
    log.setLevel(logging.INFO)
    log.propagate = propagate
    if not log.handlers:
        log.addHandler(_handler(filename, fmt))
        if console:
            console_handler = logging.StreamHandler()
            console_handler.setFormatter(fmt)
            log.addHandler(console_handler)
    return log


os.makedirs(config.LOG_DIR, exist_ok=True)

logging.basicConfig(
    filename=os.path.join(config.LOG_DIR, 'app.log'),
    level=logging.ERROR,
    format='%(asctime)s - %(levelname)s - %(message)s',
)

app = logging.getLogger('tracker')
udp = _channel('tracker.udp', 'udp.log', console=True)
tls = _channel('tracker.tls', 'tls.log', console=True)
dtls = _channel('tracker.dtls', 'dtls.log', console=True)
debug = _channel('tracker.debug', 'debug.log')
# Warnings and errors the firmware captured between sends ("L," records),
# appended on receipt so an outage can be read back afterwards.  Each line
# carries the time the device logged it, so the channel adds no stamp of
# its own.
device = _channel('tracker.device', 'device.log', stamped=False)

AUDIT_PATH = os.path.join(config.LOG_DIR, 'audit.log')
