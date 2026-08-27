#!/usr/bin/env python3
"""Populate the `plmn` table from a CSV of MCC/MNC assignments.

    tools/import_plmn.py <file.csv>

Expected columns, with a header row: mcc, mnc, operator, country (country
optional).  Public lists of these are maintained in several places; none is
bundled here because their licensing varies and the table is optional anyway —
an empty one just means no operator name in the UI.
"""

import csv
import sys

import _bootstrap  # noqa: F401

from tracker import db


def main():
    if len(sys.argv) != 2:
        print('usage: import_plmn.py <file.csv>', file=sys.stderr)
        return 1

    imported = 0
    with open(sys.argv[1], newline='') as f:
        for row in csv.DictReader(f):
            mcc = (row.get('mcc') or '').strip()
            mnc = (row.get('mnc') or '').strip()
            operator = (row.get('operator') or '').strip()
            if not mcc or not mnc or not operator:
                continue
            db.web.query(
                'INSERT INTO `plmn` (`mcc`, `mnc`, `operator`, `country`) '
                'VALUES (%s, %s, %s, %s) '
                'ON DUPLICATE KEY UPDATE `operator` = %s, `country` = %s',
                (mcc, mnc, operator, (row.get('country') or '').strip() or None,
                 operator, (row.get('country') or '').strip() or None),
            )
            imported += 1

    print('imported %d entries' % imported)
    return 0


if __name__ == '__main__':
    sys.exit(main())
