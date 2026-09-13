"""Make the tracker package importable when running a tool directly, and close
the tool's database connections when it exits.  A connection dropped without
saying goodbye is logged by the database server as an aborted connection, once
for every tool run."""

import atexit
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


@atexit.register
def _close_database():
    db = sys.modules.get('tracker.db')
    if db is not None:
        db.close_all()
