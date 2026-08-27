#!/usr/bin/env python3
"""Mint a single-use passkey enrolment URL.

    tools/regtoken.py <username> <hostname>

Valid for 24 hours and consumed on first successful registration.  There is no
self-service signup: the only correct number of accounts on a tracking server
is the number the operator created deliberately.

Re-running this for an existing username lets that user replace their
credential, which is how a lost authenticator is recovered.
"""

import hashlib
import secrets
import sys
import time

import _bootstrap  # noqa: F401

from tracker import db


def main():
    if len(sys.argv) != 3:
        print('usage: regtoken.py <username> <hostname>', file=sys.stderr)
        return 1

    username, hostname = sys.argv[1], sys.argv[2]
    token = hashlib.sha256(secrets.token_bytes(32)).hexdigest()

    db.web.query(
        'INSERT INTO `registration` (`username`, `token`, `timestamp`) '
        'VALUES (%s, %s, %s)',
        (username, token, int(time.time())),
    )

    print('https://%s/register?username=%s&token=%s' % (hostname, username, token))
    return 0


if __name__ == '__main__':
    sys.exit(main())
