"""
Root-side CAN adapter server.

Owns the gs_usb adapter (which needs root on macOS) and exposes it to
unprivileged test scripts over a JSON-lines TCP socket on localhost.  One IO
thread does all USB traffic: it writes queued TX frames (never more than the
adapter's echo-slot count in flight), reads RX / TX-echo / error frames, and
runs the "reply rules" (PING->PONG for the firmware's boot-time test, and the
echo rule used for round-trip tests) with no socket round-trip in the path.

Protocol: one JSON object per line, {"cmd": ..., ...} -> {"ok": true, ...}
See can_bench/client.py for the Python wrapper and the command list.
"""

import collections
import json
import os
import socket
import sys
import threading
import time
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gs_usb_fd as g   # noqa: E402

HOST = "127.0.0.1"
PORT = 5920
STOP_FILE = "can_stop"

RX_BUF_MAX = 400_000


class Server:
    def __init__(self):
        self.adapter = g.GsUsbFd()
        self.lock = threading.Lock()          # protects everything below
        self.rx = collections.deque(maxlen=RX_BUF_MAX)
        self.errs = collections.deque(maxlen=2000)
        self.tx_queue = collections.deque()   # (frame, token)
        self.inflight = {}                    # echo_id -> (token, host_t)
        self.echoes = {}                      # token -> dict(ts_us, t)
        self.counters = self._fresh_counters()
        self.rules = dict(pong=True, echo=False, echo_id_offset=1, echo_fd_as_is=True)
        self.ts_last = None
        self.ts_hi = 0
        self.tx_event = threading.Event()
        self.rx_event = threading.Event()
        self.running = True
        self.inq = collections.deque()
        self.inq_event = threading.Event()
        self.n_readers = 1   # the adapter object queues IN transfers asynchronously; one consumer is enough
        self.readers = [threading.Thread(target=self._reader, daemon=True, name=f"usb-reader-{i}")
                        for i in range(self.n_readers)]
        for t in self.readers:
            t.start()
        self.io = threading.Thread(target=self._io_loop, daemon=True, name="io")
        self.io.start()
        self.state_at_start = None

    @staticmethod
    def _fresh_counters():
        return dict(rx=0, rx_dropped=0, tx_echo=0, tx_timeouts=0, err_frames=0,
                    overflow_flags=0, echo_replies=0, echo_dropped=0,
                    pong_replies=0, bus_off=0, err_passive=0, err_warning=0,
                    no_ack=0, lost_arb=0, bus_error=0, io_errors=0)

    # --- timestamps (32-bit us on the adapter, unwrapped here) --------------
    def _unwrap(self, ts):
        if ts is None:
            return None
        if self.ts_last is not None and ts < self.ts_last and (self.ts_last - ts) > (1 << 31):
            self.ts_hi += 1 << 32
        self.ts_last = ts
        return ts + self.ts_hi

    # --- USB reader threads ---------------------------------------------------
    def _reader(self):
        """Several of these keep more than one IN transfer outstanding, so the
        adapter is never left holding a frame while Python turns around; a
        single synchronous read per frame tops out at a few hundred frames/s
        on macOS and the adapter's small pool then overflows."""
        while self.running:
            a = self.adapter
            if a is None or not a.started:
                time.sleep(0.01)
                continue
            try:
                f = a.read(timeout_ms=50)
            except Exception as e:  # noqa: BLE001
                with self.lock:
                    self.counters["io_errors"] += 1
                    n = self.counters["io_errors"]
                if n <= 3 or n % 500 == 0:
                    print(f"io error #{n}: {e}", flush=True)
                if "Pipe" in str(e) and a is not None:
                    try:
                        a.dev.clear_halt(a.ep_in.bEndpointAddress)
                    except Exception:  # noqa: BLE001
                        pass
                time.sleep(0.05)
                continue
            if f is None:
                continue
            self.inq.append(f)
            self.inq_event.set()

    # --- IO thread ------------------------------------------------------------
    def _io_loop(self):
        while self.running:
            a = self.adapter
            if a is None or not a.started:
                time.sleep(0.01)
                continue
            try:
                # pump TX while echo slots are free
                while True:
                    with self.lock:
                        if not self.tx_queue or len(self.inflight) >= a.MAX_ECHO:
                            break
                        if self.tx_queue[0][2] > time.monotonic():
                            break                       # paced frame not due yet
                        frame, token, _ = self.tx_queue.popleft()
                        eid = a.next_echo_id()
                        while eid in self.inflight:
                            eid = a.next_echo_id()
                        self.inflight[eid] = (token, time.monotonic())
                    try:
                        a.write(frame, echo_id=eid, timeout=200)
                    except Exception as e:  # noqa: BLE001
                        with self.lock:
                            self.inflight.pop(eid, None)
                            self.counters["io_errors"] += 1
                            self.echoes[token] = dict(error=str(e))
                        self.tx_event.set()
                try:
                    f = self.inq.popleft()
                except IndexError:
                    f = None
                    self.inq_event.wait(0.001)
                    self.inq_event.clear()
                if f is None:
                    # expire stale in-flight slots (no echo => TX never completed)
                    now = time.monotonic()
                    with self.lock:
                        stale = [e for e, (tok, t0) in self.inflight.items() if now - t0 > 3.0]
                        for e in stale:
                            tok, _ = self.inflight.pop(e)
                            self.counters["tx_timeouts"] += 1
                            self.echoes[tok] = dict(timeout=True)
                    if stale:
                        self.tx_event.set()
                    continue
                self._handle(f)
            except Exception as e:  # noqa: BLE001
                with self.lock:
                    self.counters["io_errors"] += 1
                    n = self.counters["io_errors"]
                if n <= 3 or n % 500 == 0:
                    print(f"io error #{n}: {e}", flush=True)
                if "Pipe" in str(e) and a is not None:
                    # endpoint stalled — clear the halt and carry on
                    try:
                        a.dev.clear_halt(a.ep_in.bEndpointAddress)
                        a.dev.clear_halt(a.ep_out.bEndpointAddress)
                    except Exception:  # noqa: BLE001
                        pass
                time.sleep(0.05)

    def _handle(self, f):
        f.ts_us = self._unwrap(f.ts_us)
        if f.err:
            with self.lock:
                self.counters["err_frames"] += 1
                d = g.error_frame_describe(f)
                if f.can_id & g.CAN_ERR_BUSOFF:
                    self.counters["bus_off"] += 1
                if f.can_id & g.CAN_ERR_ACK:
                    self.counters["no_ack"] += 1
                if f.can_id & g.CAN_ERR_LOSTARB:
                    self.counters["lost_arb"] += 1
                if f.can_id & g.CAN_ERR_BUSERROR:
                    self.counters["bus_error"] += 1
                if f.can_id & g.CAN_ERR_CRTL:
                    if d["ctrl"] and any(s.endswith("passive") for s in d["ctrl"]):
                        self.counters["err_passive"] += 1
                    elif d["ctrl"] and any(s.endswith("warn") for s in d["ctrl"]):
                        self.counters["err_warning"] += 1
                rec = f.as_dict()
                rec["desc"] = d
                self.errs.append(rec)
            return
        if not f.is_rx():
            # TX echo
            with self.lock:
                self.counters["tx_echo"] += 1
                ent = self.inflight.pop(f.echo_id, None)
                if ent is not None:
                    tok, t0 = ent
                    self.echoes[tok] = dict(ts_us=f.ts_us, t=f.host_time, t0=t0)
            self.tx_event.set()
            return
        with self.lock:
            self.counters["rx"] += 1
            if f.overflow:
                self.counters["overflow_flags"] += 1
            if len(self.rx) == self.rx.maxlen:
                self.counters["rx_dropped"] += 1
            self.rx.append(f)
            pong = self.rules["pong"]
            echo = self.rules["echo"]
        self.rx_event.set()
        if pong and f.can_id == 0x100 and f.data[:4] == b"PING" and not f.ext:
            self._queue_tx(g.Frame(0x101, b"PONG"), token=None)
            with self.lock:
                self.counters["pong_replies"] += 1
            print(f"RX  ID=0x{f.can_id:03X}  [{f.data.hex(' ')}]  -> TX PONG", flush=True)
        elif echo and not f.rtr and f.can_id < 0x700:
            with self.lock:
                room = len(self.tx_queue) < 64
            if room:
                r = g.Frame(f.can_id + self.rules["echo_id_offset"], f.data, ext=f.ext,
                            fd=f.fd, brs=f.brs)
                self._queue_tx(r, token=None)
                with self.lock:
                    self.counters["echo_replies"] += 1
            else:
                with self.lock:
                    self.counters["echo_dropped"] += 1

    def _queue_tx(self, frame, token, not_before=0.0):
        with self.lock:
            self.tx_queue.append((frame, token, not_before))

    # --- commands -----------------------------------------------------------
    def cmd_info(self, req):
        return self.adapter.info()

    def cmd_start(self, req):
        a = self.adapter
        bt = a.bt
        br = int(req.get("bitrate", 500000))
        sp = float(req.get("sample_point", 0.8))
        if "timing" in req:
            t = req["timing"]
            nominal = g.BitTiming(t["prop_seg"], t["phase_seg1"], t["phase_seg2"], t["sjw"], t["brp"])
        else:
            nominal = g.timing_for(a.fclk, br, sp, bt.tseg1_max, bt.tseg2_max, bt.sjw_max,
                                   bt.brp_max, sjw=req.get("sjw"))
        data = None
        fd = bool(req.get("fd", False))
        if fd:
            if "data_timing" in req:
                t = req["data_timing"]
                data = g.BitTiming(t["prop_seg"], t["phase_seg1"], t["phase_seg2"], t["sjw"], t["brp"])
            else:
                dbr = int(req.get("data_bitrate", 2000000))
                dsp = float(req.get("data_sample_point", 0.75))
                data = g.timing_for(a.fclk, dbr, dsp, bt.dtseg1_max, bt.dtseg2_max, bt.dsjw_max,
                                    bt.dbrp_max, tq_min=5, sjw=req.get("data_sjw"))
        with self.lock:
            self.tx_queue.clear()
            self.inflight.clear()
            self.rx.clear()
        a.start(nominal, data=data, fd=fd,
                listen_only=bool(req.get("listen_only", False)),
                one_shot=bool(req.get("one_shot", False)),
                loopback=bool(req.get("loopback", False)),
                triple_sample=bool(req.get("triple_sample", False)))
        self.ts_last, self.ts_hi = None, 0
        out = dict(nominal=nominal.as_dict(a.fclk), mode_flags=a.mode_flags,
                   frame_len=a.frame_len)
        if data:
            out["data"] = data.as_dict(a.fclk)
        return out

    def cmd_stop(self, req):
        self.adapter.stop()
        with self.lock:
            self.tx_queue.clear()
            self.inflight.clear()
        return {}

    def cmd_state(self, req):
        st = self.adapter.get_state()
        with self.lock:
            c = dict(self.counters)
            c["rx_buffered"] = len(self.rx)
            c["tx_queued"] = len(self.tx_queue)
            c["inflight"] = len(self.inflight)
        ain = self.adapter.ain
        return dict(adapter=st, counters=c, started=self.adapter.started,
                    termination=self.adapter.get_termination(),
                    ain=(None if ain is None else dict(status_hist=ain.hist, len_hist=ain.lens,
                                                       pending=ain.pending, queued=len(ain.q),
                                                       errors=ain.errors)))

    def cmd_reset_counters(self, req):
        with self.lock:
            self.counters = self._fresh_counters()
            self.errs.clear()
        return {}

    def cmd_rules(self, req):
        with self.lock:
            for k in ("pong", "echo", "echo_id_offset"):
                if k in req:
                    self.rules[k] = req[k]
            return dict(self.rules)

    @staticmethod
    def _frame_from(d):
        return g.Frame(int(d["id"]), bytes.fromhex(d.get("data", "")),
                       ext=bool(d.get("ext", False)), rtr=bool(d.get("rtr", False)),
                       fd=bool(d.get("fd", False)), brs=bool(d.get("brs", False)))

    def cmd_send(self, req):
        """Queue frames; wait for their TX echoes if wait (default true)."""
        frames = [self._frame_from(d) for d in req["frames"]]
        timeout = float(req.get("timeout", 2.0))
        tokens = []
        with self.lock:
            base = time.monotonic_ns()
            for i, f in enumerate(frames):
                tok = (base, i)
                self.tx_queue.append((f, tok, 0.0))
                tokens.append(tok)
        if not req.get("wait", True):
            return dict(queued=len(frames))
        deadline = time.monotonic() + timeout
        results = [None] * len(tokens)
        while True:
            with self.lock:
                for i, tok in enumerate(tokens):
                    if results[i] is None and tok in self.echoes:
                        results[i] = self.echoes.pop(tok)
            if all(r is not None for r in results):
                break
            if time.monotonic() > deadline:
                with self.lock:
                    # abandon: strip from queue, leave in-flight to expire
                    self.tx_queue = collections.deque(
                        e for e in self.tx_queue if e[1] not in tokens)
                    for i, tok in enumerate(tokens):
                        if results[i] is None:
                            results[i] = dict(timeout=True)
                            self.echoes.pop(tok, None)
                break
            self.tx_event.wait(0.01)
            self.tx_event.clear()
        return dict(results=results,
                    sent=sum(1 for r in results if "ts_us" in r),
                    timeouts=sum(1 for r in results if r.get("timeout")),
                    errors=sum(1 for r in results if r.get("error")))

    def cmd_burst(self, req):
        """Send `count` frames back-to-back from inside the server.
        Payload = 4-byte LE sequence number + fill pattern.  Returns echo
        timestamps of first/last, count confirmed, and per-frame echo ts if
        asked."""
        can_id = int(req.get("id", 0x200))
        count = int(req.get("count", 100))
        length = int(req.get("len", 8))
        ext = bool(req.get("ext", False))
        fd = bool(req.get("fd", False))
        brs = bool(req.get("brs", False))
        fill = int(req.get("fill", 0xA5)) & 0xFF
        seq0 = int(req.get("seq0", 0))
        gap_us = float(req.get("gap_us", 0))
        want_ts = bool(req.get("timestamps", False))
        timeout = float(req.get("timeout", 30.0))
        tokens = []
        t_start = time.monotonic()
        with self.lock:
            base = time.monotonic_ns()
            for i in range(count):
                seq = seq0 + i
                if length >= 4:
                    data = seq.to_bytes(4, "little") + bytes([fill]) * (length - 4)
                else:
                    data = bytes([fill]) * length
                f = g.Frame(can_id, data, ext=ext, fd=fd, brs=brs)
                tok = (base, i)
                tokens.append(tok)
                # pacing is enforced by the IO thread via the release time
                self.tx_queue.append((f, tok, t_start + i * gap_us / 1e6 if gap_us > 0 else 0.0))
        if req.get("nowait"):
            return dict(queued=count)
        deadline = time.monotonic() + timeout
        done = {}
        while len(done) < len(tokens):
            with self.lock:
                for tok in tokens:
                    if tok not in done and tok in self.echoes:
                        done[tok] = self.echoes.pop(tok)
            if len(done) >= len(tokens):
                break
            if time.monotonic() > deadline:
                with self.lock:
                    self.tx_queue = collections.deque(
                        e for e in self.tx_queue if e[1] not in tokens)
                break
            self.tx_event.wait(0.01)
            self.tx_event.clear()
        t_end = time.monotonic()
        ok = [done[t] for t in tokens if t in done and "ts_us" in done[t]]
        out = dict(requested=count, confirmed=len(ok),
                   timeouts=sum(1 for t in tokens if t in done and done[t].get("timeout")),
                   unconfirmed=count - len(done), wall_s=round(t_end - t_start, 4))
        if ok and ok[0].get("ts_us") is not None and ok[-1].get("ts_us") is not None:
            out["first_ts_us"] = ok[0]["ts_us"]
            out["last_ts_us"] = ok[-1]["ts_us"]
            span = ok[-1]["ts_us"] - ok[0]["ts_us"]
            out["span_us"] = span
            if len(ok) > 1 and span > 0:
                out["frames_per_s"] = round((len(ok) - 1) * 1e6 / span, 1)
        if want_ts:
            out["ts_us"] = [r.get("ts_us") for r in ok]
        return out

    def cmd_recv(self, req):
        """Pop up to `max` buffered RX frames; wait up to `timeout` s for at
        least `min` to be available."""
        mx = int(req.get("max", 10000))
        mn = int(req.get("min", 1))
        timeout = float(req.get("timeout", 1.0))
        deadline = time.monotonic() + timeout
        while True:
            with self.lock:
                if len(self.rx) >= mn or time.monotonic() > deadline:
                    out = []
                    while self.rx and len(out) < mx:
                        out.append(self.rx.popleft().as_dict())
                    remaining = len(self.rx)
                    break
            self.rx_event.wait(0.005)
            self.rx_event.clear()
        return dict(frames=out, remaining=remaining)

    def cmd_drain(self, req):
        with self.lock:
            n = len(self.rx)
            self.rx.clear()
        return dict(discarded=n)

    def cmd_errors(self, req):
        with self.lock:
            out = list(self.errs)
            if req.get("clear", True):
                self.errs.clear()
        return dict(errors=out)

    def cmd_timestamp(self, req):
        return dict(ts_us=self.adapter.device_timestamp(), t=time.monotonic())

    def cmd_termination(self, req):
        if "on" in req:
            self.adapter.set_termination(bool(req["on"]))
        return dict(termination=self.adapter.get_termination())

    def cmd_suspend(self, req):
        """Stop the bus and release the USB interface.  Needed while pyocd
        runs (its probe scan opens every USB device and wedges an adapter
        whose interface another process holds)."""
        if self.adapter is not None:
            try:
                self.adapter.close()
            except Exception:  # noqa: BLE001
                traceback.print_exc()
            self.adapter = None
        return {}

    def cmd_resume(self, req):
        if self.adapter is None:
            self.adapter = g.GsUsbFd()
            with self.lock:
                self.tx_queue.clear()
                self.inflight.clear()
        return self.adapter.info()

    def cmd_usbreset(self, req):
        """Port-reset the adapter (re-enumerates it) and reopen.  Unjams a
        candleLight whose from-host queue is stuck on an unsendable frame."""
        a = self.adapter
        self.adapter = None
        try:
            if a is not None:
                try:
                    a.stop()
                except Exception:  # noqa: BLE001
                    pass
                a.dev.reset()
                try:
                    a.close()
                except Exception:  # noqa: BLE001
                    pass
        finally:
            time.sleep(float(req.get("wait", 2.0)))
            self.adapter = g.GsUsbFd()
            with self.lock:
                self.tx_queue.clear()
                self.inflight.clear()
                self.rx.clear()
        return self.adapter.info()

    def cmd_dfu_detach(self, req):
        """Reboot the adapter MCU: DFU_DETACH on its DFU runtime interface
        makes candleLight jump to the STM32 system bootloader.  The caller
        then runs `pydfu.py -x` (as any user) to leave DFU, and `resume`."""
        a = self.adapter
        self.adapter = None
        if a is None:
            a = g.GsUsbFd()
        try:
            a.stop()
        except Exception:  # noqa: BLE001
            pass
        cfg = a.dev.get_active_configuration()
        dfu_if = None
        for intf in cfg:
            if intf.bInterfaceClass == 0xFE and intf.bInterfaceSubClass == 0x01:
                dfu_if = intf.bInterfaceNumber
        if dfu_if is None:
            self.adapter = a
            raise RuntimeError("no DFU runtime interface on adapter")
        try:
            a.dev.ctrl_transfer(0x21, 0, 1000, dfu_if, None, timeout=1000)   # DFU_DETACH
        except Exception as e:  # noqa: BLE001
            print(f"detach request: {e} (device may already be rebooting)", flush=True)
        try:
            a.close()
        except Exception:  # noqa: BLE001
            pass
        return dict(dfu_interface=dfu_if)

    def cmd_debug(self, req):
        import sys
        frames = sys._current_frames()
        out = {}
        for th in threading.enumerate():
            fr = frames.get(th.ident)
            out[th.name] = traceback.format_stack(fr) if fr else None
        return dict(threads=out)

    def cmd_quit(self, req):
        self.running = False
        return {}

    def dispatch(self, req):
        cmd = req.get("cmd")
        fn = getattr(self, "cmd_" + str(cmd), None)
        if fn is None:
            return dict(ok=False, error=f"unknown cmd {cmd!r}")
        if self.adapter is None and cmd not in ("resume", "debug", "quit", "state"):
            return dict(ok=False, error="adapter suspended — call resume first")
        t0 = time.monotonic()
        self.busy = (cmd, t0)
        try:
            out = fn(req) or {}
            out["ok"] = True
            return out
        except Exception as e:  # noqa: BLE001
            traceback.print_exc()
            return dict(ok=False, error=f"{type(e).__name__}: {e}")
        finally:
            self.busy = None
            dt = time.monotonic() - t0
            if dt > 5.0:
                print(f"slow command {cmd}: {dt:.1f}s", flush=True)

    def monitor(self):
        """Dump every thread's stack if a command has been stuck > 20 s."""
        import sys
        warned = None
        while self.running:
            time.sleep(1.0)
            b = getattr(self, "busy", None)
            if b and time.monotonic() - b[1] > 20 and warned != b:
                warned = b
                print(f"command {b[0]} stuck for {time.monotonic() - b[1]:.0f}s:", flush=True)
                for th in threading.enumerate():
                    fr = sys._current_frames().get(th.ident)
                    if fr:
                        print(f"--- thread {th.name}", flush=True)
                        print("".join(traceback.format_stack(fr)), flush=True)


def serve():
    srv = Server()
    srv.busy = None
    threading.Thread(target=srv.monitor, daemon=True, name="monitor").start()
    print("adapter: " + json.dumps(srv.adapter.info()), flush=True)
    st = srv.adapter.get_state()
    print(f"adapter state: {st}", flush=True)
    ls = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind((HOST, PORT))
    ls.listen(2)
    ls.settimeout(0.5)
    print(f"listening on {HOST}:{PORT}  (touch {STOP_FILE} or ctrl-c to stop)", flush=True)
    try:
        while srv.running:
            if os.path.exists(STOP_FILE):
                os.remove(STOP_FILE)
                print(f"\n{STOP_FILE} found, exiting.", flush=True)
                break
            try:
                conn, _ = ls.accept()
            except socket.timeout:
                continue
            conn.settimeout(None)
            try:
                with conn, conn.makefile("rwb", buffering=0) as fh:
                    for line in fh:
                        line = line.strip()
                        if not line:
                            continue
                        try:
                            req = json.loads(line)
                        except json.JSONDecodeError as e:
                            resp = dict(ok=False, error=f"bad json: {e}")
                        else:
                            resp = srv.dispatch(req)
                        fh.write((json.dumps(resp) + "\n").encode())
                        if not srv.running:
                            break
            except (BrokenPipeError, ConnectionResetError, OSError) as e:
                print(f"client went away: {e}", flush=True)
    except KeyboardInterrupt:
        print("\nstopped.", flush=True)
    finally:
        srv.running = False
        try:
            srv.adapter.close()
        except Exception:  # noqa: BLE001
            pass
        ls.close()


if __name__ == "__main__":
    serve()
