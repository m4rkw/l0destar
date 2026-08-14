#!/usr/bin/env python3
"""
Host-side CAN test responder for the l0destar CAN bus test.

Listens on a gs_usb-compatible adapter (canable2) for a PING frame
(ID 0x100, data b"PING"), replies with PONG (ID 0x101, data b"PONG"),
and prints all traffic.

Usage:
    sudo DYLD_LIBRARY_PATH=/opt/local/lib python3 can_test_host.py

Requires: python-can, gs_usb, pyusb, libusb
    pip install python-can gs_usb pyusb
"""

import ctypes.util
import sys

# sudo strips DYLD_LIBRARY_PATH (SIP), so help pyusb find libusb
_orig_find = ctypes.util.find_library
def _find_library(name):
    if name in ("usb-1.0", "usb"):
        return "/opt/local/lib/libusb-1.0.dylib"
    return _orig_find(name)
ctypes.util.find_library = _find_library

import can


BITRATE = 500_000
PING_ID = 0x100
PONG_ID = 0x101


def main():
    print(f"opening gs_usb adapter at {BITRATE} bps...")

    try:
        bus = can.Bus(interface="gs_usb", channel=0, bitrate=BITRATE)
    except Exception as e:
        print(f"failed to open adapter: {e}")
        print("is the canable2 plugged in?")
        sys.exit(1)

    print("listening for CAN frames (ctrl-c to stop)...\n")

    try:
        while True:
            msg = bus.recv(timeout=1.0)
            if msg is None:
                continue

            data_str = msg.data.hex(" ")
            ascii_str = "".join(
                chr(b) if 0x20 <= b < 0x7F else "." for b in msg.data
            )
            print(f"RX  ID=0x{msg.arbitration_id:03X}  DLC={msg.dlc}  [{data_str}]  {ascii_str}")

            if (
                msg.arbitration_id == PING_ID
                and msg.dlc >= 4
                and msg.data[:4] == b"PING"
            ):
                reply = can.Message(
                    arbitration_id=PONG_ID,
                    data=b"PONG",
                    is_extended_id=False,
                )
                bus.send(reply)
                print(f"TX  ID=0x{PONG_ID:03X}  DLC=4  [50 4f 4e 47]  PONG")
                print("\nPING/PONG exchange complete.")

    except KeyboardInterrupt:
        print("\nstopped.")
    finally:
        bus.shutdown()


if __name__ == "__main__":
    main()
