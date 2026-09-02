#!/usr/bin/env python3
"""
Log the tracker's serial console to a file (timestamped lines), for reading
the bench agent's printk output while the tests run.

    python3 can_bench/console_log.py /dev/cu.usbmodem1301 console.log &

Stop with ctrl-c / SIGTERM.  Bytes outside printable ASCII (ANSI colour
codes from the Zephyr logger) are stripped so the file greps cleanly.
"""
import re
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbmodem1301"
out = sys.argv[2] if len(sys.argv) > 2 else "console.log"
ansi = re.compile(rb"\x1b\[[0-9;]*[A-Za-z]")

with serial.Serial(port, 115200, timeout=0.2) as s, open(out, "ab", buffering=0) as f:
    buf = b""
    while True:
        chunk = s.read(4096)
        if not chunk:
            continue
        buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            line = ansi.sub(b"", line).replace(b"\r", b"")
            line = bytes(c for c in line if 0x20 <= c < 0x7F or c == 0x09)
            f.write(time.strftime("%H:%M:%S ").encode() + line + b"\n")
