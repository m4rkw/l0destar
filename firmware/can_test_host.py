#!/usr/bin/env python3
"""
Host-side CAN adapter server for the l0destar CAN bench tests.

Runs as root (the gs_usb adapter needs it on macOS) and hands the CANable
2.0 to unprivileged test scripts over a localhost socket — see
can_bench/server.py (protocol), can_bench/client.py (wrapper) and
can_bench/run_all.py (the test suite).

The firmware's boot-time CAN test (CONFIG_APP_CAN_TEST=y) still works with
this running: the server answers PING (ID 0x100) with PONG (ID 0x101) by
default.

Usage:
    sudo /Users/mark/.venv/bin/python3 can_test_host.py

Stop it with ctrl-c, or by creating a file named "can_stop" in the working
directory (it is removed on the way out).

Requires: pyusb, libusb (MacPorts /opt/local/lib/libusb-1.0.dylib)
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "can_bench"))

from server import serve   # noqa: E402

if __name__ == "__main__":
    serve()
