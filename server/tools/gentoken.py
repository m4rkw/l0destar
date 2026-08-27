#!/usr/bin/env python3
"""Mint a bearer token for the automation API.

    tools/gentoken.py <name>

Prints the token on stdout; it is not recoverable afterwards in any friendlier
form than reading the table.  Any row in `api_token` is valid, and tokens carry
no scopes — one is equivalent to console access over the device, so treat it
that way and give each consumer its own so you can revoke them separately.

    curl -H "Authorization: Bearer <token>" https://host/api/1.0/config?imei=...
"""

import secrets
import sys
import time

import _bootstrap  # noqa: F401

from tracker import db


def main():
    if len(sys.argv) != 2:
        print('usage: gentoken.py <name>', file=sys.stderr)
        return 1

    token = secrets.token_urlsafe(32)
    db.web.query(
        'INSERT INTO `api_token` (`name`, `token`, `created_at`) VALUES (%s, %s, %s)',
        (sys.argv[1], token, int(time.time())),
    )
    print(token)
    return 0


if __name__ == '__main__':
    sys.exit(main())
