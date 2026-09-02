# CAN bench tests

Host-driven test rig for the l0destar CAN interface (MCP2518FD + MAX33041E)
using a CANable 2.0 class USB adapter.  Results and conclusions from the
first run are in [`../CAN_BENCH_REPORT.md`](../CAN_BENCH_REPORT.md).

## Pieces

| file | role |
|---|---|
| `../src/can_bench.c` | firmware test agent, built with `CONFIG_APP_CAN_BENCH=y` (bench only) |
| `../can_test_host.py` | root-side entry point: runs `server.py` |
| `server.py` | owns the USB adapter, JSON-lines API on `127.0.0.1:5920` |
| `gs_usb_fd.py` | gs_usb (candleLight) driver on pyusb with CAN FD and async IN transfers |
| `client.py` | unprivileged client for the server |
| `device.py` | encodes the agent's control protocol (ID 0x7E0 -> 0x7E8) |
| `run_all.py` | the test suite; writes `results/<stamp>.json` and `.md` |
| `console_log.py` | logs the tracker's serial console (timestamped, ANSI stripped) |
| `adapter_firmware/` | adapter firmware images and the DFU tools used to flash them |

## Adapter firmware

The DSD TECH SH-C31A ships with a candleLight build that does **not**
advertise CAN FD over gs_usb (the vendor documents slcan firmware for FD).
The tests were run with the community FD fork
`tymmothy/candleLight_fw_canable_v2_fd` v1.0.1, kept here as
`adapter_firmware/canable2_gs_usb_fd_tymmothy_v1.0.1.bin`.

Flashing (adapter in DFU mode: hold BOOT while plugging in, or use
`client.CanHost().dfu_detach()` on a running candleLight):

```
cd can_bench/adapter_firmware
python3 dfu.py -b 0x08000000:canable2_gs_usb_fd_tymmothy_v1.0.1.bin fw.dfu
python3 -c "import ctypes.util,sys;o=ctypes.util.find_library;\
ctypes.util.find_library=lambda n:'/opt/local/lib/libusb-1.0.dylib' if n in ('usb-1.0','usb') else o(n);\
sys.argv=['pydfu','-u','fw.dfu'];import pydfu;pydfu.main()"
```

The stock canable.io images (`canable2_candlelight_ba6b1dd.bin`, classic
only; `canable2_slcan_fd_b158aa7.bin`, slcan with FD) are kept alongside to
restore the adapter.

Quirks of the FD fork that the host code works around:

* every host-bound frame is padded to 128 bytes with no short packet, so IN
  transfers must be exactly 128 bytes (`GsUsbFd.IN_STRIDE`);
* a frame the FDCAN cannot send is retried forever and wedges the adapter's
  USB queue — use `one_shot=True` whenever no ACK is expected, and
  `CanHost.adapter_reboot()` (DFU detach + leave) to recover;
* TX echo means "queued", not "transmitted";
* no hardware timestamps, no error-counter reporting beyond error frames;
* bit timing limited to tseg1 <= 16, tseg2 <= 8, sjw <= 4 time quanta on a
  160 MHz clock.

## Running

```
# 1. firmware
#    local.conf: CONFIG_APP_CAN_TEST=n, CONFIG_APP_CAN_BENCH=y
./build.sh && ./flash.sh && ./reset.sh

# 2. console (optional, gives the agent's printk lines in the results)
python3 can_bench/console_log.py /dev/cu.usbmodem1301 /tmp/console.log &

# 3. adapter server (needs root for USB on macOS)
sudo /Users/mark/.venv/bin/python3 /Users/mark/code/l0destar/firmware/can_test_host.py &

# 4. tests
python3 can_bench/run_all.py --console /tmp/console.log            # everything (~15 min)
python3 can_bench/run_all.py --only B,C --quick                     # subsets
```

Stop the server with ctrl-c or `touch can_stop` in its working directory.

## Interactive use

```python
import sys; sys.path.insert(0, "can_bench")
from client import CanHost
from device import Device, MODE_NORMAL_FD
h = CanHost(); h.start(bitrate=500000); h.rules(pong=False)
d = Device(h)
d.ping(); d.stats()
d.set_mode(MODE_NORMAL_FD, 500000, 2000000)
h.start(bitrate=500000, fd=True, data_bitrate=2000000)
d.echo(True); h.send(0x100, bytes(range(64)), fd=True, brs=True); h.recv()
```

The boot-time `CONFIG_APP_CAN_TEST` PING/PONG still works against the
server (it answers PING on 0x100 with PONG on 0x101 unless
`h.rules(pong=False)`).
