# CAN Bus Test

**Before building, installing or relying on any of this, read the [project disclaimer](../DISCLAIMER.md).**

Verifies CAN TX/RX on v2.6C boards via a PING/PONG exchange between the
MCP2518FD and a host-side canable2 USB adapter.

## Hardware setup

- l0destar v2.6C board (MCP2518FD + MAX33041EASA+ transceiver)
- canable2 USB-CAN adapter (gs_usb firmware, VID 1D50 PID 606F)
- Two-wire CAN bus between them (CANH, CANL)
- 120R termination on both ends (PCB-side resistor + DSD TECH jumper)
- Bus speed: 500 kbps

## Host dependencies

```
pip install python-can gs_usb pyusb
port install libusb   # or brew install libusb
```

## Running the test

1. Enable the test in `local.conf`:
   ```
   CONFIG_APP_CAN_TEST=y
   ```

2. Build and flash:
   ```
   ./build.sh && ./flash.sh
   ```

3. Start the host responder (needs sudo for USB access under SIP):
   ```
   sudo python3 -u can_test_host.py
   ```

4. Reset the board (pyocd or power cycle). Serial output shows:
   ```
   *** CAN BUS TEST ***
   TX -> ID=0x100 [PING]
   RX <- ID=0x101 DLC=4
   PASS: CAN bus test passed
   ```

   Host output shows:
   ```
   RX  ID=0x100  DLC=4  [50 49 4e 47]  PING
   TX  ID=0x101  DLC=4  [50 4f 4e 47]  PONG
   ```

5. Comment out `CONFIG_APP_CAN_TEST=y` when done.

## Troubleshooting

**Host: "No such device"** -- macOS kernel driver may be claiming the adapter.
Run this before the host script:
```python
sudo python3 -c "
import ctypes.util
ctypes.util.find_library = lambda name: '/opt/local/lib/libusb-1.0.dylib' if 'usb' in name else None
import usb.core
dev = usb.core.find(idVendor=0x1d50, idProduct=0x606f)
if dev and dev.is_kernel_driver_active(0): dev.detach_kernel_driver(0)
dev.reset()
"
```

**TX aborted / timeout** -- check termination (need 120R on both ends) and
that CANH/CANL aren't swapped.

## Protocol

The firmware sends a standard CAN 2.0 frame (ID 0x100, DLC 4, data "PING")
and waits up to 5 seconds for a reply (ID 0x101, DLC 4, data "PONG") from
the host script. The test halts after completion.
