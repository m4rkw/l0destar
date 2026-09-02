"""continuous device->host traffic for scope work: traffic.py classic|fd2m|fd5m [host]"""
import os, sys, time
sys.path.insert(0, "/Users/mark/code/l0destar/firmware/can_bench")
from client import CanHost
from device import Device, MODE_NORMAL_20, MODE_NORMAL_FD
mode = sys.argv[1] if len(sys.argv) > 1 else "classic"
host_tx = len(sys.argv) > 2 and sys.argv[2] == "host"
h = CanHost(); h.rules(pong=False); d = Device(h)
h.start(bitrate=500000)
if mode == "classic":
    d.set_mode(MODE_NORMAL_20, 500000, 2000000); h.start(bitrate=500000); fd = False; dr = None
else:
    dr = 2000000 if mode == "fd2m" else 5000000
    d.set_mode(MODE_NORMAL_FD, 500000, dr); h.start(bitrate=500000, fd=True, data_bitrate=dr); fd = True
d.ping(); d.set_fast_spi(True)
print("running", mode, "host tx" if host_tx else "device tx", flush=True)
while True:
    if host_tx:
        h.burst(can_id=0x200, count=500, length=64 if fd else 8, fd=fd, brs=fd, fill=0x55, timeout=30)
        d.clear()
    else:
        d.burst(count=500, length=64 if fd else 8, fd=fd, brs=fd, can_id=0x300, fill=0x55, timeout=30)
    h.drain()
