#!/usr/bin/env python3
"""Enrol a device.

    tools/adddevice.py <imei> <name> [registration] [--psk <64 hex characters>]

Generates a fresh ChaCha20-Poly1305 pre-shared key and prints it, or stores the
key given with --psk, for a unit whose firmware was already built with a key of
its own (`openssl rand -hex 32`).  Either way the same key has to be in that
unit's firmware.  It is stored as hex in `device`.`psk` and never shown again.
"""

import argparse
import re
import secrets
import sys

import _bootstrap  # noqa: F401

from tracker import db

PSK_RE = re.compile(r'^[0-9a-fA-F]{64}$')


def main():
    parser = argparse.ArgumentParser(description='Enrol a device.')
    parser.add_argument('imei')
    parser.add_argument('name')
    parser.add_argument('registration', nargs='?')
    parser.add_argument('--psk', help='the key already built into the firmware '
                                      '(64 hex characters), instead of a new one')
    args = parser.parse_args()

    if not args.imei.isdigit() or not 14 <= len(args.imei) <= 16:
        print('imei must be 14-16 digits', file=sys.stderr)
        return 1
    if args.psk is not None and not PSK_RE.match(args.psk):
        print('--psk must be 64 hex characters (32 bytes)', file=sys.stderr)
        return 1

    if db.lookup_device(imei=args.imei):
        print('device %s already enrolled' % args.imei, file=sys.stderr)
        return 1

    psk = (args.psk or secrets.token_hex(32)).lower()
    db.web.query(
        'INSERT INTO `device` (`imei`, `name`, `registration`, `psk`) '
        'VALUES (%s, %s, %s, %s)',
        (args.imei, args.name, args.registration, psk),
    )

    print('enrolled %s (%s)' % (args.name, args.imei))
    if args.psk is None:
        print('psk: %s' % psk)
        print()
        print('Build this key into the unit\'s firmware; it is not shown again.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
