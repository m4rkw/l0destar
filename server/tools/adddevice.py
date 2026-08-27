#!/usr/bin/env python3
"""Enrol a device.

    tools/adddevice.py <imei> <name> [registration]

Generates a fresh ChaCha20-Poly1305 pre-shared key and prints it.  The same key
has to be built into that unit's firmware, so this is the one moment it is
printed — it is stored as hex in `device`.`psk` and never shown again.

Devices using the TLS or DTLS transports do not need the key, but there is no
cost to having one.
"""

import secrets
import sys

import _bootstrap  # noqa: F401

from tracker import db


def main():
    if len(sys.argv) < 3:
        print('usage: adddevice.py <imei> <name> [registration]', file=sys.stderr)
        return 1

    imei, name = sys.argv[1], sys.argv[2]
    registration = sys.argv[3] if len(sys.argv) > 3 else None

    if not imei.isdigit() or not 14 <= len(imei) <= 16:
        print('imei must be 14-16 digits', file=sys.stderr)
        return 1

    if db.lookup_device(imei=imei):
        print('device %s already enrolled' % imei, file=sys.stderr)
        return 1

    psk = secrets.token_hex(32)
    db.web.query(
        'INSERT INTO `device` (`imei`, `name`, `registration`, `psk`) '
        'VALUES (%s, %s, %s, %s)',
        (imei, name, registration, psk),
    )

    print('enrolled %s (%s)' % (name, imei))
    print('psk: %s' % psk)
    print()
    print('Build this key into the unit\'s firmware; it is not shown again.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
