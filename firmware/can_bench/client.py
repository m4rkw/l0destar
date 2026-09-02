"""
Unprivileged client for can_bench/server.py (the root-side adapter server).

    from can_bench.client import CanHost
    h = CanHost()
    h.start(bitrate=500000)
    h.send(0x123, b"\x01\x02")
    for f in h.recv(timeout=1.0): print(f)

Commands map 1:1 onto Server.cmd_* in server.py.
"""

import json
import socket
import time

HOST = "127.0.0.1"
PORT = 5920


class CanHostError(RuntimeError):
    pass


class CanHost:
    def __init__(self, host=HOST, port=PORT, connect_timeout=5.0):
        deadline = time.monotonic() + connect_timeout
        while True:
            try:
                self.sock = socket.create_connection((host, port), timeout=2.0)
                break
            except OSError:
                if time.monotonic() > deadline:
                    raise CanHostError("cannot connect to CAN host server — is "
                                       "`sudo .../python3 can_test_host.py` running?")
                time.sleep(0.2)
        self.sock.settimeout(120.0)
        self.fh = self.sock.makefile("rwb", buffering=0)

    def call(self, cmd, **kw):
        kw["cmd"] = cmd
        self.fh.write((json.dumps(kw) + "\n").encode())
        line = self.fh.readline()
        if not line:
            raise CanHostError("server closed connection")
        resp = json.loads(line)
        if not resp.get("ok"):
            raise CanHostError(resp.get("error", "unknown error"))
        return resp

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    # --- convenience ---------------------------------------------------------
    def info(self):
        return self.call("info")

    def start(self, bitrate=500000, sample_point=0.8, fd=False, data_bitrate=2000000,
              data_sample_point=0.75, listen_only=False, one_shot=False, loopback=False,
              timing=None, data_timing=None, sjw=None, data_sjw=None, triple_sample=False):
        kw = dict(bitrate=bitrate, sample_point=sample_point, fd=fd,
                  data_bitrate=data_bitrate, data_sample_point=data_sample_point,
                  listen_only=listen_only, one_shot=one_shot, loopback=loopback,
                  triple_sample=triple_sample)
        if timing:
            kw["timing"] = timing
        if data_timing:
            kw["data_timing"] = data_timing
        if sjw is not None:
            kw["sjw"] = sjw
        if data_sjw is not None:
            kw["data_sjw"] = data_sjw
        return self.call("start", **kw)

    def stop(self):
        return self.call("stop")

    def state(self):
        return self.call("state")

    def counters(self):
        return self.state()["counters"]

    def reset_counters(self):
        return self.call("reset_counters")

    def rules(self, **kw):
        return self.call("rules", **kw)

    def send(self, can_id, data=b"", ext=False, rtr=False, fd=False, brs=False,
             wait=True, timeout=2.0):
        r = self.send_many([dict(id=can_id, data=bytes(data).hex(), ext=ext, rtr=rtr,
                                 fd=fd, brs=brs)], wait=wait, timeout=timeout)
        return r["results"][0] if wait else r

    def send_many(self, frames, wait=True, timeout=5.0):
        return self.call("send", frames=frames, wait=wait, timeout=timeout)

    def burst(self, can_id=0x200, count=100, length=8, ext=False, fd=False, brs=False,
              fill=0xA5, seq0=0, gap_us=0, timestamps=False, timeout=30.0, nowait=False):
        return self.call("burst", id=can_id, count=count, len=length, ext=ext, fd=fd,
                         brs=brs, fill=fill, seq0=seq0, gap_us=gap_us,
                         timestamps=timestamps, timeout=timeout, nowait=nowait)

    def recv(self, timeout=1.0, min_frames=1, max_frames=10000):
        r = self.call("recv", timeout=timeout, min=min_frames, max=max_frames)
        for f in r["frames"]:
            f["data"] = bytes.fromhex(f["data"])
        return r["frames"]

    def recv_all(self, quiet_s=0.3, max_total=1_000_000, hard_timeout=60.0):
        """Keep receiving until the bus has been quiet for quiet_s."""
        out = []
        t_end = time.monotonic() + hard_timeout
        while time.monotonic() < t_end and len(out) < max_total:
            fr = self.recv(timeout=quiet_s, min_frames=1)
            if not fr:
                break
            out.extend(fr)
        return out

    def drain(self):
        return self.call("drain")["discarded"]

    def errors(self, clear=True):
        return self.call("errors", clear=clear)["errors"]

    def timestamp(self):
        return self.call("timestamp")

    def termination(self, on=None):
        if on is None:
            return self.call("termination")["termination"]
        return self.call("termination", on=on)["termination"]

    def usbreset(self, wait=2.0):
        return self.call("usbreset", wait=wait)

    def dfu_detach(self):
        return self.call("dfu_detach")

    def adapter_reboot(self, wait=3.0):
        """Full MCU reset of a jammed candleLight: DFU detach, leave DFU."""
        import subprocess, sys, os, time as _t
        here = os.path.dirname(os.path.abspath(__file__))
        self.dfu_detach()
        _t.sleep(wait)
        script = ("import ctypes.util,sys;o=ctypes.util.find_library;"
                  "ctypes.util.find_library=lambda n:'/opt/local/lib/libusb-1.0.dylib' if n in ('usb-1.0','usb') else o(n);"
                  "sys.argv=['pydfu','-x'];sys.path.insert(0,%r);import pydfu;pydfu.main()"
                  % os.path.join(here, "adapter_firmware"))
        r = subprocess.run([sys.executable, "-c", script], capture_output=True, text=True, timeout=60)
        _t.sleep(wait)
        return dict(dfu_leave_rc=r.returncode, out=(r.stdout + r.stderr)[-300:], resume=self.resume())

    def suspend(self):
        return self.call("suspend")

    def resume(self):
        return self.call("resume")

    def quit(self):
        try:
            self.call("quit")
        except CanHostError:
            pass
