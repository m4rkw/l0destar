#!/usr/bin/env python3
"""
l0destar CAN bench test suite.

Prerequisites
  * board flashed with CONFIG_APP_CAN_BENCH=y (src/can_bench.c agent)
  * adapter server running:  sudo .../python3 can_test_host.py
  * optional: can_bench/console_log.py capturing the serial console

    python3 can_bench/run_all.py [--console console.log] [--out can_bench/results]
                                 [--only B,C] [--quick]

Writes <out>/<timestamp>.json and .md with every measurement.
"""

import argparse
import json
import os
import random
import statistics
import struct
import sys
import time
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from client import CanHost, CanHostError          # noqa: E402
from device import (Device, DeviceError, MODE_NORMAL_FD, MODE_NORMAL_20, MODE_LISTEN,  # noqa: E402
                    MODE_RESTRICT, CFG_ONE_SHOT, CFG_SMALL_RX, CFG_NO_TDC, CTRL_RSP, CTRL_REQ)

DLC2LEN = [0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64]


class Suite:
    def __init__(self, args):
        self.args = args
        self.h = CanHost()
        self.h.rules(pong=False)
        self.d = Device(self.h)
        self.results = []
        self.t0 = time.time()
        self.console_pos = 0
        if args.console and os.path.exists(args.console):
            self.console_pos = os.path.getsize(args.console)

    # --- bookkeeping -----------------------------------------------------------
    def console_lines(self):
        """New console lines since last call (bench: lines only)."""
        if not self.args.console or not os.path.exists(self.args.console):
            return []
        with open(self.args.console, "rb") as f:
            f.seek(self.console_pos)
            data = f.read()
            self.console_pos = f.tell()
        return [l for l in data.decode("ascii", "replace").splitlines() if "bench:" in l]

    def record(self, tid, name, status, metrics=None, notes=None):
        r = dict(id=tid, name=name, status=status, metrics=metrics or {}, notes=notes or [],
                 console=self.console_lines(), t=round(time.time() - self.t0, 1))
        self.results.append(r)
        flag = {"PASS": "PASS", "FAIL": "FAIL", "INFO": "info", "WARN": "WARN", "SKIP": "skip",
                "ERROR": "ERR "}[status]
        print(f"[{flag}] {tid} {name}", flush=True)
        for k, v in (metrics or {}).items():
            print(f"       {k}: {v}", flush=True)
        for n in notes or []:
            print(f"       - {n}", flush=True)
        return r

    def run(self, tid, name, fn):
        try:
            fn(tid, name)
        except Exception as e:  # noqa: BLE001
            traceback.print_exc()
            self.record(tid, name, "ERROR", notes=[f"{type(e).__name__}: {e}"])
            self.recover()

    def recover(self):
        """Get host and device back to classic 500 kbps, Normal 2.0, whatever
        bit rate or mode a failed test left the device in."""
        for attempt in range(3):
            found = None
            for br in (500000, 125000, 250000, 1000000):
                try:
                    self.h.start(bitrate=br)
                    self.d.retries = 1
                    self.d.ping(timeout=0.4)
                    found = br
                    break
                except (CanHostError, DeviceError):
                    continue
                finally:
                    self.d.retries = 3
            if found is None:
                try:
                    self.h.adapter_reboot()
                except Exception:  # noqa: BLE001
                    pass
                continue
            try:
                self.d.set_mode(MODE_NORMAL_20, 500000, 2000000, 0)
                self.h.start(bitrate=500000)
                self.d.ping()
                self.d.echo(False)
                self.d.filter(None)
                return True
            except (CanHostError, DeviceError):
                continue
        return False

    # --- helpers ----------------------------------------------------------------
    def classic(self, **kw):
        self.h.start(bitrate=500000, **kw)

    def dev(self, mode=MODE_NORMAL_20, nom=500000, dat=2000000, flags=0):
        self.d.set_mode(mode, nom, dat, flags)

    def collect(self, quiet=0.2, hard=10.0):
        fr = list(self.d.stray)
        self.d.stray = []
        fr += self.h.recv_all(quiet_s=quiet, hard_timeout=hard)
        return [f for f in fr if f["id"] != CTRL_RSP]

    def echo_frames(self, frames, timeout=0.5):
        """Send frames one at a time with device echo on; return per-frame
        (ok, rtt_ms, note)."""
        out = []
        for fr in frames:
            self.h.drain()
            t0 = time.monotonic()
            self.h.send(fr["id"], fr["data"], ext=fr.get("ext", False), rtr=fr.get("rtr", False),
                        fd=fr.get("fd", False), brs=fr.get("brs", False), timeout=timeout)
            want_id = fr["id"] + 1
            got = None
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline and got is None:
                for f in self.h.recv(timeout=min(0.1, deadline - time.monotonic())):
                    if f["id"] == want_id and f["ext"] == fr.get("ext", False):
                        got = (f, time.monotonic())
                        break
            if got is None:
                out.append((False, None, "no echo"))
                continue
            f, t1 = got
            exp_data = fr["data"]
            if fr.get("fd"):
                dlc = next(i for i, l in enumerate(DLC2LEN) if l >= len(exp_data))
                exp_data = exp_data + bytes(DLC2LEN[dlc] - len(exp_data))
            ok = (f["data"] == exp_data and f["fd"] == fr.get("fd", False)
                  and f["brs"] == fr.get("brs", False))
            note = "" if ok else f"got fd={f['fd']} brs={f['brs']} data={f['data'].hex()}"
            out.append((ok, (t1 - t0) * 1000, note))
        return out

    def host_timing_points(self, target=500000, span=0.07):
        """(brp, tq, bitrate, offset) combos the adapter can do near target."""
        pts = []
        for brp in range(1, 65):
            for tq in range(8, 26):
                br = 160e6 / (brp * tq)
                off = br / target - 1
                if abs(off) <= span:
                    pts.append((brp, tq, br, off))
        pts.sort(key=lambda p: p[3])
        # thin out: keep at most one point per 0.4 % bucket
        seen, out = set(), []
        for p in pts:
            b = round(p[3] / 0.004)
            if b not in seen:
                seen.add(b)
                out.append(p)
        return out

    # =========================================================================
    # A. identification / SPI / interrupt
    # =========================================================================
    def test_A1(self, tid, name):
        self.classic()
        self.dev()
        p = self.d.ping()
        s = self.d.stats()
        info = self.h.info()
        m = dict(device_mode=p["opmod_name"], fast_spi=p["fast_spi"], int_pin_idle=s["int_pin"],
                 tec=s["tec"], rec=s["rec"], adapter=info["product"],
                 adapter_features=",".join(info["features"]), adapter_clock_hz=info["fclk_can"])
        st = "PASS" if p["opmod"] == MODE_NORMAL_20 and s["int_pin"] == 1 else "FAIL"
        self.record(tid, name, st, m)

    def test_A2(self, tid, name):
        self.classic()
        self.dev()
        rows = {}
        for fast in (False, True):
            self.d.set_fast_spi(fast)
            r = self.d.spibench(n=200)
            rows["fast" if fast else "slow"] = r
        self.d.set_fast_spi(False)
        m = {}
        for k, r in rows.items():
            m[f"{k}_ram_sweep_errors_of_10240"] = r["ram_err"]
            m[f"{k}_rw_errors_of_200"] = r["rw_err"]
            m[f"{k}_write16_us"] = r["write16_us"]
            m[f"{k}_read16_us"] = r["read16_us"]
            m[f"{k}_spi_kbit_s"] = r["spi_kbps"]
        st = "PASS" if all(r["ram_err"] == 0 and r["rw_err"] == 0 for r in rows.values()) else "FAIL"
        self.record(tid, name, st, m, ["18 bytes on the wire per timed 16-byte transaction"])

    def test_A3(self, tid, name):
        self.classic()
        self.dev()
        self.d.clear()
        self.h.burst(can_id=0x200, count=30, length=8, gap_us=3000)
        time.sleep(0.3)
        s = self.d.stats()
        m = dict(rx=s["rx"], int_low_whenever_fifo_nonempty=s["int_always_low_when_rx"],
                 int_idle_now=s["int_pin"])
        st = "PASS" if s["rx"] == 30 and s["int_always_low_when_rx"] and s["int_pin"] == 1 else "FAIL"
        self.record(tid, name, st, m)

    # =========================================================================
    # B. classic CAN 2.0 @ 500 kbps
    # =========================================================================
    def frames_B1(self):
        rnd = random.Random(1)
        frames = []
        pats = {"00": lambda n: bytes(n), "ff": lambda n: b"\xff" * n, "55": lambda n: b"\x55" * n,
                "aa": lambda n: b"\xaa" * n, "rnd": lambda n: bytes(rnd.getrandbits(8) for _ in range(n))}
        for n in range(0, 9):
            for pn, pf in pats.items():
                frames.append(dict(id=0x123, data=pf(n), tag=f"std dlc{n} {pn}"))
        for sid in (0x000, 0x001, 0x3FF, 0x7EE):
            frames.append(dict(id=sid, data=pats["rnd"](8), tag=f"std id 0x{sid:03x}"))
        for eid in (0x00000000, 0x00000800, 0x1ABCDEF0, 0x1FFFFFFE):
            frames.append(dict(id=eid, data=pats["rnd"](8), ext=True, tag=f"ext id 0x{eid:08x}"))
        return frames

    def test_B1(self, tid, name):
        self.classic()
        self.dev()
        self.d.echo(True)
        frames = self.frames_B1()
        res = self.echo_frames(frames)
        self.d.echo(False)
        bad = [(f["tag"], n) for f, (ok, _, n) in zip(frames, res) if not ok]
        rtts = [r for ok, r, _ in res if ok]
        s = self.d.stats()
        m = dict(frames=len(frames), ok=len(frames) - len(bad), failed=len(bad),
                 rtt_ms_median=round(statistics.median(rtts), 2) if rtts else None,
                 tec=s["tec"], rec=s["rec"], bdiag1=s["bdiag1"])
        self.record(tid, name, "PASS" if not bad else "FAIL", m, [f"{t}: {n}" for t, n in bad[:10]])

    def test_B1r(self, tid, name):
        """RTR frames: device must receive and count them (echo excluded)."""
        self.classic()
        self.dev()
        self.d.clear()
        self.h.drain()
        for i in range(5):
            self.h.send(0x321, bytes(4), rtr=True)
        time.sleep(0.2)
        s = self.d.stats()
        m = dict(rtr_sent=5, device_rx=s["rx"], device_rx_rtr=s["rx_rtr"])
        self.record(tid, name, "PASS" if s["rx_rtr"] == 5 else "FAIL", m)

    def test_B2(self, tid, name):
        self.classic()
        self.dev()
        m = {}
        st = "PASS"
        for fast in (False, True):
            self.d.set_fast_spi(fast)
            self.d.echo(True)
            frames = [dict(id=0x400, data=struct.pack("<I", i) + b"\x5a" * 4) for i in range(200)]
            res = self.echo_frames(frames, timeout=0.5)
            self.d.echo(False)
            rtts = sorted(r for ok, r, _ in res if ok)
            lost = sum(1 for ok, _, _ in res if not ok)
            k = "fast_spi" if fast else "slow_spi"
            if rtts:
                m[f"{k}_rtt_ms_min/median/p95/max"] = (
                    f"{rtts[0]:.2f} / {statistics.median(rtts):.2f} / "
                    f"{rtts[int(len(rtts) * 0.95) - 1]:.2f} / {rtts[-1]:.2f}")
            m[f"{k}_lost"] = lost
            if lost:
                st = "FAIL"
        self.d.set_fast_spi(False)
        self.record(tid, name, st, m, ["RTT includes USB + adapter latency on both legs; "
                                       "device turnaround = RX read + TX write over SPI"])

    def test_B3(self, tid, name):
        """Host->device sustained rate: find the fastest pace with no loss."""
        self.classic()
        self.dev()
        m = {}
        notes = []
        for fast in (False, True):
            self.d.set_fast_spi(fast)
            k = "fast" if fast else "slow"
            best = None
            for gap in (5000, 3000, 2000, 1500, 1200, 1000, 800, 600, 500, 400, 300, 250, 0):
                self.d.clear()
                self.h.drain()
                n = 300
                b = self.h.burst(can_id=0x200, count=n, length=8, gap_us=gap)
                time.sleep(0.4)
                s = self.d.stats()
                rate = (s["rx"] - 1) * 1e6 / s["span_us"] if s["rx"] > 1 and s["span_us"] else 0
                ok = s["rx"] == n and s["seq_missing"] == 0 and s["rxovf"] == 0
                notes.append(f"{k} gap={gap}us: rx={s['rx']}/{n} miss={s['seq_missing']} "
                             f"ovf={s['rxovf']} rate={rate:.0f}/s")
                if ok:
                    best = (gap, rate)
                else:
                    break
            m[f"{k}_spi_max_lossless_rate_f_s"] = round(best[1]) if best else 0
            m[f"{k}_spi_min_gap_us"] = best[0] if best else None
        self.d.set_fast_spi(False)
        self.record(tid, name, "INFO", m, notes)

    def test_B3b(self, tid, name):
        """RX FIFO absorbs a full-rate burst up to its depth (64 classic)."""
        self.classic()
        self.dev()
        m = {}
        st = "PASS"
        for n in (16, 32, 60, 100):
            self.d.clear()
            self.h.drain()
            self.h.burst(can_id=0x200, count=n, length=8)
            time.sleep(0.5)
            s = self.d.stats()
            m[f"burst{n}_rx/miss/ovf"] = f"{s['rx']}/{s['seq_missing']}/{s['rxovf']}"
            if n <= 32 and (s["rx"] != n or s["rxovf"]):
                st = "FAIL"
        self.record(tid, name, st, m, ["RX FIFO is 32 deep (MCP2518FD FSIZE max): bursts up to 32 must be "
                                       "lossless with the slow SPI; 60 and 100 are expected to overflow"])

    def test_B4(self, tid, name):
        self.classic()
        self.dev()
        m = {}
        st = "PASS"
        for fast in (False, True):
            self.d.set_fast_spi(fast)
            for length in (8, 0):
                self.h.drain()
                n = 500
                b = self.d.burst(count=n, length=length, can_id=0x300)
                fr = self.collect()
                seqs = [int.from_bytes(f["data"][:4], "little") for f in fr if f["id"] == 0x300 and len(f["data"]) >= 4]
                got = len([f for f in fr if f["id"] == 0x300])
                k = f"{'fast' if fast else 'slow'}_spi_dlc{length}"
                m[f"{k}_rate_f_s"] = b["frames_per_s"]
                m[f"{k}_host_got"] = f"{got}/{n}"
                m[f"{k}_tec"] = b["tec"]
                m[f"{k}_tx_flags"] = b["tx_fifo_flags"]
                if got != n or (length == 8 and sorted(seqs) != list(range(n))):
                    st = "FAIL"
        self.d.set_fast_spi(False)
        self.record(tid, name, st, m)

    def test_B5(self, tid, name):
        """Both directions at once: arbitration and no loss."""
        self.classic()
        self.dev()
        self.d.clear()
        self.h.drain()
        n_host, n_dev = 200, 200
        # queue the host stream (paced 3 ms) without waiting so the device
        # burst overlaps it; both ends transmit at once for ~0.6 s
        self.h.burst(can_id=0x200, count=n_host, length=8, gap_us=3000, nowait=True)
        b = self.d.burst(count=n_dev, length=8, can_id=0x300)
        time.sleep(1.0)
        fr = self.collect()
        got = len([f for f in fr if f["id"] == 0x300])
        s = self.d.stats()
        m = dict(host_frames=n_host, device_rx=s["rx"], device_miss=s["seq_missing"],
                 device_ovf=s["rxovf"], device_tx=b["sent"], host_got=got,
                 device_lost_arbitration_flag=("TXLARB" in b["tx_fifo_flags"]),
                 tec=s["tec"], rec=s["rec"], bdiag1=s["bdiag1"])
        st = "PASS" if s["rx"] == n_host and s["seq_missing"] == 0 and got == n_dev and s["tec"] == 0 else "FAIL"
        self.record(tid, name, st, m)

    def test_B6(self, tid, name):
        s = self.d.stats()
        m = dict(tec=s["tec"], rec=s["rec"], trec=s["trec"], bdiag1=s["bdiag1"],
                 nrerr=s["nrerr"], nterr=s["nterr"], drerr=s["drerr"], dterr=s["dterr"])
        st = "PASS" if s["tec"] == 0 and s["rec"] == 0 and not s["trec"] else "FAIL"
        self.record(tid, name, st, m)

    # =========================================================================
    # C. bit rates / timing margin
    # =========================================================================
    def test_C1(self, tid, name):
        m = {}
        st = "PASS"
        cur = 500000
        for br in (125000, 250000, 500000, 1000000):
            self.h.start(bitrate=cur)          # talk to the device at its current rate
            self.dev(MODE_NORMAL_20, br)
            cur = br
            self.h.start(bitrate=br)
            try:
                self.d.ping()
                self.d.echo(True)
                res = self.echo_frames([dict(id=0x123, data=bytes(range(i, i + 8))) for i in range(20)])
                self.d.echo(False)
                ok = sum(1 for r in res if r[0])
                self.h.drain()
                b = self.d.burst(count=100, length=8, can_id=0x300)
                got = len([f for f in self.collect() if f["id"] == 0x300])
                s = self.d.stats()
                m[f"{br // 1000}k"] = f"echo {ok}/20, burst {got}/100 @{b['frames_per_s']}/s, TEC {s['tec']} REC {s['rec']} {s['bdiag1']}"
                if ok != 20 or got != 100 or s["tec"] or s["rec"]:
                    st = "FAIL"
            except (DeviceError, CanHostError) as e:
                m[f"{br // 1000}k"] = f"FAILED: {e}"
                st = "FAIL"
        self.recover()
        self.record(tid, name, st, m)

    def test_C2(self, tid, name):
        """Host sample point varied at 500 kbps (device fixed at 80 %)."""
        self.classic()
        self.dev()
        m = {}
        st = "PASS"
        for seg2 in (8, 6, 5, 4, 3, 2, 1):
            tq = 20
            seg1 = tq - 1 - seg2
            sp = (1 + seg1) / tq
            self.h.start(timing=dict(prop_seg=seg1 // 2, phase_seg1=seg1 - seg1 // 2, phase_seg2=seg2,
                                     sjw=min(4, seg2), brp=16))
            self.d.retries = 1
            oks = 0
            for _ in range(20):
                try:
                    self.d.ping(timeout=0.3)
                    oks += 1
                except DeviceError:
                    pass
            self.d.retries = 3
            m[f"host_sp_{sp * 100:.0f}%"] = f"{oks}/20 pings"
            if oks < 20:
                st = "WARN"
        self.h.start(bitrate=500000)
        self.record(tid, name, st, m, ["device sample point 80 % (62+1 of 80 tq), SJW 16 tq"])

    def test_C3(self, tid, name):
        """Host bit rate deliberately offset from 500 kbps: where does the link break?"""
        self.classic()
        self.dev()
        pts = self.host_timing_points()
        m = {}
        notes = []
        self.d.retries = 1
        worst_ok = [0, 0]
        for brp, tq, br, off in pts:
            seg2 = max(1, min(8, round(tq * 0.2)))
            seg1 = tq - 1 - seg2
            self.h.start(timing=dict(prop_seg=seg1 // 2, phase_seg1=seg1 - seg1 // 2, phase_seg2=seg2,
                                     sjw=min(4, seg2), brp=brp))
            oks = 0
            for _ in range(10):
                try:
                    self.d.ping(timeout=0.25)
                    oks += 1
                except DeviceError:
                    pass
            notes.append(f"{off * 100:+.2f}% ({br / 1000:.1f} kbps, brp {brp} x {tq} tq): {oks}/10")
            if oks == 10:
                worst_ok[0] = min(worst_ok[0], off)
                worst_ok[1] = max(worst_ok[1], off)
        self.d.retries = 3
        self.h.start(bitrate=500000)
        self.d.clear()
        m["lossless_window"] = f"{worst_ok[0] * 100:+.2f}% .. {worst_ok[1] * 100:+.2f}%"
        self.record(tid, name, "INFO", m, notes)

    # =========================================================================
    # D. CAN FD
    # =========================================================================
    def test_D1(self, tid, name):
        self.dev(MODE_NORMAL_FD, 500000, 2000000)
        self.h.start(bitrate=500000, fd=True, data_bitrate=2000000)
        self.d.ping()
        self.d.echo(True)
        rnd = random.Random(2)
        frames = []
        for n in (0, 1, 7, 8, 12, 16, 20, 24, 32, 48, 64):
            for pn, pf in (("rnd", lambda k: bytes(rnd.getrandbits(8) for _ in range(k))),
                           ("55", lambda k: b"\x55" * k), ("00", lambda k: bytes(k))):
                frames.append(dict(id=0x500, data=pf(n), fd=True, brs=True, tag=f"fd len{n} {pn}"))
        for eid in (0x1ABCDEF0, 0x1FFFFFFE):
            frames.append(dict(id=eid, data=bytes(range(64)), fd=True, brs=True, ext=True, tag=f"fd ext 0x{eid:08x}"))
        frames.append(dict(id=0x501, data=bytes(range(8)), tag="classic in FD mode"))
        res = self.echo_frames(frames)
        self.d.echo(False)
        bad = [(f["tag"], n) for f, (ok, _, n) in zip(frames, res) if not ok]
        s = self.d.stats()
        m = dict(frames=len(frames), ok=len(frames) - len(bad), failed=len(bad), tec=s["tec"], rec=s["rec"],
                 bdiag1=s["bdiag1"], device_rx_fd=s["rx_fd"], device_rx_brs=s["rx_brs"])
        self.record(tid, name, "PASS" if not bad else "FAIL", m, [f"{t}: {n}" for t, n in bad[:10]])

    def test_D2(self, tid, name):
        m = {}
        st = "PASS"
        for dr in (1000000, 2000000, 4000000, 5000000, 8000000):
            self.h.start(bitrate=500000)
            self.dev(MODE_NORMAL_FD, 500000, dr)
            self.h.start(bitrate=500000, fd=True, data_bitrate=dr)
            try:
                self.d.ping()
                self.d.clear()
                self.d.echo(True)
                frames = [dict(id=0x500, data=bytes((i * 7 + k) & 0xFF for k in range(64)), fd=True, brs=True) for i in range(20)]
                res = self.echo_frames(frames)
                self.d.echo(False)
                ok = sum(1 for r in res if r[0])
                s = self.d.stats()
                m[f"data_{dr // 1000000}M"] = (f"echo {ok}/20, TEC {s['tec']} REC {s['rec']} {s['bdiag1']} "
                                              f"drerr {s['drerr']} dterr {s['dterr']}")
                if ok != 20:
                    st = "FAIL"
            except (DeviceError, CanHostError) as e:
                m[f"data_{dr // 1000000}M"] = f"FAILED: {e}"
                st = "FAIL"
                self.h.adapter_reboot()
        self.recover()
        self.record(tid, name, st, m, ["nominal 500 kbps; device TDC auto, host TDC on above 2.5 Mbps"])

    def test_D3(self, tid, name):
        m = {}
        st = "PASS"
        for dr in (2000000, 5000000):
            self.h.start(bitrate=500000)
            self.dev(MODE_NORMAL_FD, 500000, dr)
            self.h.start(bitrate=500000, fd=True, data_bitrate=dr)
            self.d.ping()
            for fast in (False, True):
                self.d.set_fast_spi(fast)
                self.h.drain()
                n = 200
                b = self.d.burst(count=n, length=64, fd=True, brs=True, can_id=0x300)
                fr = [f for f in self.collect() if f["id"] == 0x300]
                good = sum(1 for f in fr if f["fd"] and f["brs"] and len(f["data"]) == 64 and f["data"][4:] == b"\x5a" * 60)
                k = f"{dr // 1000000}M_{'fast' if fast else 'slow'}_spi"
                m[f"{k}_dev_to_host"] = f"{good}/{n} good @{b['frames_per_s']}/s TEC {b['tec']} {b['tx_fifo_flags']}"
                if good != n:
                    st = "FAIL"
                # host -> device paced 64-byte frames
                self.d.clear()
                self.h.drain()
                self.h.burst(can_id=0x200, count=100, length=64, fd=True, brs=True, gap_us=4000)
                time.sleep(0.5)
                s = self.d.stats()
                m[f"{k}_host_to_dev_paced4ms"] = f"rx {s['rx']}/100 miss {s['seq_missing']} ovf {s['rxovf']} bytes {s['rx_bytes']}"
                if s["rx"] != 100:
                    st = "FAIL"
            self.d.set_fast_spi(False)
        self.recover()
        self.record(tid, name, st, m)

    def test_D4(self, tid, name):
        """Host->device FD sustained rate (64-byte frames), like B3."""
        self.h.start(bitrate=500000)
        self.dev(MODE_NORMAL_FD, 500000, 2000000)
        self.h.start(bitrate=500000, fd=True, data_bitrate=2000000)
        m = {}
        notes = []
        for fast in (False, True):
            self.d.set_fast_spi(fast)
            k = "fast" if fast else "slow"
            best = None
            for gap in (8000, 5000, 4000, 3000, 2500, 2000, 1500, 1000, 500, 0):
                self.d.clear()
                self.h.drain()
                n = 100
                self.h.burst(can_id=0x200, count=n, length=64, fd=True, brs=True, gap_us=gap)
                time.sleep(0.5)
                s = self.d.stats()
                rate = (s["rx"] - 1) * 1e6 / s["span_us"] if s["rx"] > 1 and s["span_us"] else 0
                ok = s["rx"] == n and s["seq_missing"] == 0 and s["rxovf"] == 0
                notes.append(f"{k} gap={gap}us: rx={s['rx']}/{n} miss={s['seq_missing']} ovf={s['rxovf']} rate={rate:.0f}/s")
                if ok:
                    best = (gap, rate)
                else:
                    break
            m[f"{k}_spi_max_lossless_rate_64B_f_s"] = round(best[1]) if best else 0
        self.d.set_fast_spi(False)
        self.recover()
        self.record(tid, name, "INFO", m, notes)

    def test_D5(self, tid, name):
        """Classic-mode device on an FD bus: it must error-flag FD frames (a
        deployment hazard), and an FD-mode device must accept classic frames."""
        self.h.start(bitrate=500000)
        self.dev(MODE_NORMAL_20, 500000, 2000000)
        self.h.start(bitrate=500000, fd=True, data_bitrate=2000000, one_shot=True)
        self.d.clear()
        self.h.drain()
        self.h.burst(can_id=0x200, count=10, length=16, fd=True, brs=True, gap_us=5000)
        time.sleep(0.5)
        errs = self.h.errors()
        s = self.d.stats()
        m = dict(fd_frames_sent_to_classic_device=10, device_rx=s["rx"], device_rec=s["rec"],
                 device_bdiag1=s["bdiag1"], device_nrerr=s["nrerr"], host_error_frames=len(errs))
        self.recover()
        st = "INFO"
        self.record(tid, name, st, m, ["expected: device receives none, counts form errors and "
                                       "actively destroys the FD frames (host sees error frames)"])

    def test_D6(self, tid, name):
        """Device external loopback FD through the MAX33041E at several data rates,
        host stopped so nothing else drives the bus."""
        m = {}
        st = "PASS"
        for dr in (1000000, 2000000, 4000000, 5000000, 8000000):
            self.h.start(bitrate=500000)
            self.dev(MODE_NORMAL_20, 500000, dr)
            row = {}
            for internal in (True, False):
                self.d.looptest(internal=internal, fd=True, brs=True, length=64, count=16, delay_ms=800)
                self.h.stop()
                time.sleep(2.5)
                self.h.start(bitrate=500000)
                try:
                    r = self.d.looptest_report(timeout=5)
                    row["int" if internal else "ext"] = (f"{r['received']}/{r['sent']} ok, mismatch {r['mismatch']}, "
                                                        f"TEC {r['tec']} {r['bdiag1']} {r['tx_fifo_flags']}")
                    if r["received"] != 16 or r["mismatch"]:
                        st = "FAIL"
                except DeviceError as e:
                    row["int" if internal else "ext"] = f"no report: {e}"
                    st = "FAIL"
            m[f"data_{dr // 1000000}M"] = row
        self.recover()
        self.record(tid, name, st, m, ["internal = digital core only; external = through transceiver and bus"])

    def test_D7(self, tid, name):
        """TDC disabled at 5 Mbps: does the device still transmit cleanly?"""
        self.h.start(bitrate=500000)
        self.dev(MODE_NORMAL_FD, 500000, 5000000, CFG_NO_TDC)
        self.h.start(bitrate=500000, fd=True, data_bitrate=5000000)
        self.d.ping()
        self.h.drain()
        b = self.d.burst(count=50, length=64, fd=True, brs=True, can_id=0x300)
        got = len([f for f in self.collect() if f["id"] == 0x300 and len(f["data"]) == 64])
        s = self.d.stats()
        m = dict(host_got=f"{got}/50", tec=b["tec"], tx_flags=b["tx_fifo_flags"], bdiag1=s["bdiag1"], dterr=s["dterr"])
        self.recover()
        self.record(tid, name, "INFO", m, ["TDC (transmitter delay compensation) is normally required above ~2 Mbps"])

    # =========================================================================
    # E. error handling / modes / power
    # =========================================================================
    def test_E1(self, tid, name):
        """No partner: one-shot TX must abort cleanly; auto-retry TX must go
        error-passive and deliver the frame once the partner returns."""
        self.classic()
        # part 1: one-shot, host stopped, device sends 10 frames once each
        self.dev(MODE_NORMAL_20, 500000, 2000000, CFG_ONE_SHOT)
        self.d.clear()
        self.h.send(CTRL_REQ, bytes([6]) + struct.pack("<HBBHB", 10, 8, 0, 0x300, 0x5A), wait=True)
        self.h.stop()
        time.sleep(1.5)
        self.classic()
        time.sleep(0.3)
        self.h.drain()
        s1 = self.d.stats()           # burst report itself was one-shot and lost; flags/TEC persist
        # part 2: auto-retry, host stopped, device queues 1 frame, host returns after 1 s
        self.dev(MODE_NORMAL_20, 500000, 2000000, 0)
        self.d.clear()
        self.h.send(CTRL_REQ, bytes([14]) + struct.pack("<BBHB", 8, 0, 0x310, 1), wait=True)
        self.h.stop()
        time.sleep(1.0)
        self.classic()
        time.sleep(0.5)
        fr = self.collect()
        got = len([f for f in fr if f["id"] == 0x310])
        s2 = self.d.stats()
        # recovery: TEC decays with successful traffic
        self.h.drain()
        self.d.burst(count=100, length=8, can_id=0x300)
        self.collect()
        s3 = self.d.stats()
        m = dict(one_shot_tec=s1["tec"], one_shot_aborted_frames=s1["tx_abort"], one_shot_trec=s1["trec"],
                 one_shot_tx_flags=s1["tx_fifo_flags"],
                 one_shot_bdiag1=s1["bdiag1"], one_shot_c1int=s1["c1int"],
                 retry_frame_delivered_when_partner_returned=got, retry_tec=s2["tec"],
                 retry_trec=s2["trec"], retry_bdiag1=s2["bdiag1"],
                 tec_after_100_good_frames=s3["tec"], trec_after=s3["trec"])
        ok = (s1["tec"] > 0 or s1["tx_abort"]) and got == 1 and s3["tec"] < max(s2["tec"], 1)
        self.recover()
        self.record(tid, name, "PASS" if ok else "WARN", m)

    def test_E2(self, tid, name):
        """Bus-off: same ID, different data from both ends at once."""
        self.classic()
        self.dev()
        self.d.clear()
        self.h.reset_counters()
        self.h.errors()
        self.h.drain()
        self.h.burst(can_id=0x300, count=400, length=8, fill=0x00, gap_us=0, nowait=True)
        try:
            b = self.d.burst(count=400, length=8, can_id=0x300, fill=0xFF, timeout=20)
        except DeviceError as e:
            b = dict(error=str(e))
        time.sleep(2.0)
        errs = self.h.errors()
        hc = self.h.counters()
        try:
            s = self.d.stats()
        except DeviceError:
            s = None
        self.h.drain()
        pingok = True
        try:
            self.d.ping()
        except DeviceError:
            pingok = False
        m = dict(device_burst=b, host_error_frames=len(errs), host_bus_off_events=hc["bus_off"],
                 host_err_passive_events=hc["err_passive"], host_lost_arb=hc["lost_arb"],
                 device_tec=s["tec"] if s else None, device_rec=s["rec"] if s else None,
                 device_trec=s["trec"] if s else None, device_bdiag1=s["bdiag1"] if s else None,
                 device_c1int=s["c1int"] if s else None, device_responds_after=pingok)
        self.recover()
        self.record(tid, name, "PASS" if pingok else "FAIL", m,
                    ["a controller that hits bus-off must recover on its own (128 x 11 recessive bits)"])

    def test_E3(self, tid, name):
        """Listen-only: the device must not ACK.  On a two-node bench bus the
        host then sees ACK errors and no frame completes, which is the correct
        behaviour of a purely passive node."""
        self.classic()
        self.dev()
        self.d.clear()
        self.dev(MODE_LISTEN, 500000, 2000000)
        self.h.start(bitrate=500000, one_shot=True)
        self.h.reset_counters()
        self.h.errors()
        self.h.burst(can_id=0x200, count=20, length=8, gap_us=5000)
        time.sleep(0.5)
        errs = self.h.errors()
        acks = sum(1 for e in errs if "no-ack" in e["desc"]["classes"])
        self.classic()
        self.dev(MODE_NORMAL_20, 500000, 2000000)
        s = self.d.stats()
        m = dict(host_frames=20, host_error_frames=len(errs), host_no_ack_errors=acks,
                 device_rx_counted=s["rx"], device_rec=s["rec"], device_tec=s["tec"], device_bdiag1=s["bdiag1"])
        self.record(tid, name, "PASS" if (acks > 0 or len(errs) > 0) else "WARN", m,
                    ["device rx should be 0: without any ACKing node the host destroys its own frames"])

    def test_E4(self, tid, name):
        self.classic()
        self.dev()
        self.d.filter(sid=0x123, mask=0x7FF)
        self.d.clear()
        self.h.drain()
        self.h.burst(can_id=0x123, count=20, length=8, gap_us=3000)
        self.h.burst(can_id=0x124, count=20, length=8, gap_us=3000, seq0=20)
        self.h.burst(can_id=0x122, count=20, length=8, gap_us=3000, seq0=40)
        time.sleep(0.3)
        s = self.d.stats()
        self.d.filter(None)
        self.d.clear()
        self.h.drain()
        self.h.burst(can_id=0x124, count=20, length=8, gap_us=3000)
        time.sleep(0.3)
        s2 = self.d.stats()
        m = dict(with_filter_0x123_rx=s["rx"], after_restore_rx_0x124=s2["rx"])
        self.record(tid, name, "PASS" if s["rx"] == 20 and s2["rx"] == 20 else "FAIL", m)

    def test_E5(self, tid, name):
        """RX overflow with a 4-deep FIFO is detected and survivable."""
        self.classic()
        self.dev(MODE_NORMAL_20, 500000, 2000000, CFG_SMALL_RX)
        self.d.clear()
        self.h.drain()
        self.h.burst(can_id=0x200, count=50, length=8)
        time.sleep(0.5)
        s = self.d.stats()
        self.dev()
        self.d.clear()
        self.h.drain()
        self.h.burst(can_id=0x200, count=20, length=8, gap_us=3000)
        time.sleep(0.3)
        s2 = self.d.stats()
        m = dict(small_fifo_rx=s["rx"], small_fifo_missing=s["seq_missing"], overflow_events=s["rxovf"],
                 c1int=s["c1int"], after_restore_rx=f"{s2['rx']}/20")
        self.record(tid, name, "PASS" if s["rxovf"] > 0 and s2["rx"] == 20 else "FAIL", m)

    def test_E6(self, tid, name):
        """Sleep / wake on bus activity, with the transceiver in standby (production
        configuration) and with it awake."""
        self.classic()
        self.dev()
        m = {}
        for xstby in (True, False):
            self.d.sleep(ms=2000, xstby=xstby)
            time.sleep(0.5)
            self.h.start(bitrate=500000, one_shot=True)
            self.h.burst(can_id=0x200, count=5, length=8, gap_us=100000)
            self.h.errors()
            self.classic()
            r = self.d.sleep_report(timeout=6)
            k = "xstby_standby" if xstby else "xcvr_awake"
            m[k] = dict(wakif=r["wakif"], int_low_seen=r["int_seen_low"], int_low_at_ms=r["int_low_at_ms"],
                        osc_ready_us=r["osc_ready_us"], mode_after_wake=r["opmod_after_wake_name"],
                        int_before=r["int_before"], int_at_end=r["int_at_end"],
                        osc_before=hex(r["osc_before"]), c1int=r["c1int"], reconfig_failed=r["reconfig_failed"])
            time.sleep(0.2)
            self.d.ping()
        self.record(tid, name, "INFO", m, ["host sends 5 frames (one-shot, 100 ms apart) 0.5 s into a 2 s sleep"])

    def test_E7(self, tid, name):
        self.classic()
        self.dev()
        rows = []
        st = "PASS"
        for i in range(5):
            off = 300 if i < 3 else 1500
            self.d.railcycle(off_ms=off)
            time.sleep(off / 1000 + 0.3)
            r = self.d.railcycle_report(timeout=6)
            rows.append(f"off {off} ms: rail before/end-of-off/back={r['rail_before']}/{r['rail_off']}/{r['rail_back']} "
                        f"sense low after {r['rail_sense_low_after_ms']} ms, "
                        f"power_on={r['power_on_err']} ({r['power_on_ms']} ms) reconfig={r['reconfig_err']} "
                        f"osc=0x{r['osc']:04x} mode={r['opmod_name']}")
            if r["power_on_err"] or r["reconfig_err"] or r["rail_back"] != 1:
                st = "FAIL"
            self.d.ping()
            self.d.echo(True)
            res = self.echo_frames([dict(id=0x123, data=bytes(range(8)))])
            self.d.echo(False)
            if not res[0][0]:
                st = "FAIL"
                rows[-1] += " ECHO FAILED"
        self.record(tid, name, st, {"cycles": 5}, rows)

    def test_E8(self, tid, name):
        self.classic()
        self.d.reboot()
        time.sleep(6.0)
        ok = False
        for _ in range(10):
            try:
                self.d.ping()
                ok = True
                break
            except DeviceError:
                time.sleep(1.0)
        self.record(tid, name, "PASS" if ok else "FAIL", dict(agent_back_after_reboot=ok),
                    self.console_lines()[-3:])

    def test_E9(self, tid, name):
        """Soak: mixed traffic for a while, zero loss and clean error counters."""
        dur = 20 if self.args.quick else 120
        self.classic()
        self.dev()
        self.d.clear()
        self.h.reset_counters()
        self.h.errors()
        self.h.drain()
        self.d.echo(True)
        t_end = time.monotonic() + dur
        sent = 0
        echoed = 0
        seq = 0
        while time.monotonic() < t_end:
            self.h.burst(can_id=0x200, count=100, length=8, gap_us=5000, seq0=seq)
            seq += 100
            sent += 100
            fr = self.h.recv(timeout=0.3, max_frames=100000)
            echoed += len([f for f in fr if f["id"] == 0x201])
        time.sleep(0.5)
        echoed += len([f for f in self.h.recv(timeout=0.3, max_frames=100000) if f["id"] == 0x201])
        self.d.echo(False)
        s = self.d.stats()
        hc = self.h.counters()
        m = dict(seconds=dur, host_sent=sent, device_rx=s["rx"], device_missing=s["seq_missing"],
                 device_ovf=s["rxovf"], device_echoed=s["echo_tx"], echo_dropped=s["echo_drop"],
                 host_got_echoes=echoed, tec=s["tec"], rec=s["rec"], bdiag1=s["bdiag1"],
                 host_error_frames=hc["err_frames"])
        st = "PASS" if s["rx"] == sent and s["seq_missing"] == 0 and echoed == sent and s["tec"] == 0 and s["rec"] == 0 else "FAIL"
        self.record(tid, name, st, m)

    # =========================================================================
    def all_tests(self):
        return [
            ("A1", "Identification and idle state", self.test_A1),
            ("A2", "SPI link: MCP2518FD RAM sweep and transaction timing (slow/fast bit-bang)", self.test_A2),
            ("A3", "CAN_INT line follows RX FIFO", self.test_A3),
            ("B1", "Classic 500 kbps frame integrity: DLC 0-8, std/ext IDs, data patterns (echo)", self.test_B1),
            ("B1r", "Classic 500 kbps remote (RTR) frames received", self.test_B1r),
            ("B2", "Classic 500 kbps round-trip latency (200 echoes, slow/fast SPI)", self.test_B2),
            ("B3", "Classic 500 kbps host->device sustained rate limit", self.test_B3),
            ("B3b", "Classic 500 kbps RX FIFO burst absorption", self.test_B3b),
            ("B4", "Classic 500 kbps device->host bursts (500 frames, DLC 8/0, slow/fast SPI)", self.test_B4),
            ("B5", "Classic 500 kbps simultaneous bidirectional traffic (arbitration)", self.test_B5),
            ("B6", "Error counters clean after classic tests", self.test_B6),
            ("C1", "Bit rates 125k / 250k / 500k / 1M", self.test_C1),
            ("C2", "Host sample-point sweep at 500 kbps", self.test_C2),
            ("C3", "Host bit-rate offset tolerance at 500 kbps", self.test_C3),
            ("D1", "CAN FD frame integrity 500k/2M: lengths 0-64, BRS, ext IDs (echo)", self.test_D1),
            ("D2", "CAN FD data rates 1M / 2M / 4M / 5M / 8M (echo)", self.test_D2),
            ("D3", "CAN FD throughput both directions at 2M and 5M", self.test_D3),
            ("D4", "CAN FD host->device sustained rate limit (64-byte frames)", self.test_D4),
            ("D5", "Classic-mode device facing FD frames (misconfiguration hazard)", self.test_D5),
            ("D6", "Device FD loopback through the transceiver at 1M-8M (host stopped)", self.test_D6),
            ("D7", "FD 5M with TDC disabled", self.test_D7),
            ("E1", "No partner: one-shot abort, error-passive and recovery", self.test_E1),
            ("E2", "Bus-off provocation and recovery", self.test_E2),
            ("E3", "Listen-only mode is truly passive", self.test_E3),
            ("E4", "Acceptance filter", self.test_E4),
            ("E5", "RX FIFO overflow detection", self.test_E5),
            ("E6", "Sleep and wake-on-bus (transceiver standby vs awake)", self.test_E6),
            ("E7", "CAN rail power cycle x5 and re-init", self.test_E7),
            ("E8", "Cold reboot re-init", self.test_E8),
            ("E9", "Soak: mixed traffic with echo", self.test_E9),
        ]

    def main(self):
        only = set(self.args.only.split(",")) if self.args.only else None
        tests = self.all_tests()
        self.console_lines()
        for tid, name, fn in tests:
            if only and not any(tid == o or tid.startswith(o) for o in only):
                continue
            self.run(tid, name, fn)
        self.recover()
        self.write()

    def write(self):
        os.makedirs(self.args.out, exist_ok=True)
        stamp = time.strftime("%Y%m%d-%H%M%S")
        base = os.path.join(self.args.out, stamp)
        with open(base + ".json", "w") as f:
            json.dump(dict(started=time.ctime(self.t0), adapter=self.h.info(), results=self.results), f, indent=1, default=str)
        with open(base + ".md", "w") as f:
            f.write(f"# CAN bench results {stamp}\n\n")
            f.write("| id | test | result | key metrics |\n|---|---|---|---|\n")
            for r in self.results:
                ms = "; ".join(f"{k}={v}" for k, v in list(r["metrics"].items())[:6])
                f.write(f"| {r['id']} | {r['name']} | {r['status']} | {ms} |\n")
            f.write("\n")
            for r in self.results:
                f.write(f"## {r['id']} {r['name']} — {r['status']}\n\n")
                for k, v in r["metrics"].items():
                    f.write(f"- {k}: `{v}`\n")
                for n in r["notes"]:
                    f.write(f"- {n}\n")
                if r["console"]:
                    f.write("\n```\n" + "\n".join(r["console"][-12:]) + "\n```\n")
                f.write("\n")
        print(f"\nresults: {base}.json / .md", flush=True)
        return base


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--console", default=None, help="console_log.py output file")
    ap.add_argument("--out", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "results"))
    ap.add_argument("--only", default=None, help="comma list of test id prefixes")
    ap.add_argument("--quick", action="store_true")
    Suite(ap.parse_args()).main()
