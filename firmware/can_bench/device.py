"""
Host-side wrapper for the firmware's CAN bench agent (src/can_bench.c).

The agent listens for 8-byte control frames on CTRL_REQ (0x7E0) and replies
on CTRL_RSP (0x7E8) with chunks [op, idx | 0x80-if-last, 6 payload bytes].
This module encodes the commands, reassembles the replies and decodes the
reply payloads into dicts.
"""

import struct
import time

CTRL_REQ = 0x7E0
CTRL_RSP = 0x7E8

OP_PING, OP_STATS, OP_CLEAR, OP_SETMODE, OP_ECHO, OP_BURST, OP_SLEEP, \
    OP_RAILCYCLE, OP_SPIBENCH, OP_FILTER, OP_LOOPTEST, OP_SETFAST, OP_REBOOT, \
    OP_TXONE = range(1, 15)

# MCP2518FD operating modes
MODE_NORMAL_FD = 0
MODE_SLEEP = 1
MODE_INT_LOOP = 2
MODE_LISTEN = 3
MODE_CONFIG = 4
MODE_EXT_LOOP = 5
MODE_NORMAL_20 = 6
MODE_RESTRICT = 7
MODE_NAMES = {0: "Normal FD", 1: "Sleep", 2: "Internal loopback", 3: "Listen-only",
              4: "Configuration", 5: "External loopback", 6: "Normal CAN 2.0",
              7: "Restricted"}

NOM_RATES = {125000: 0, 250000: 1, 500000: 2, 1000000: 3}
DAT_RATES = {1000000: 0, 2000000: 1, 4000000: 2, 5000000: 3, 8000000: 4}

# cfg.flags bits
CFG_ONE_SHOT = 1 << 0
CFG_SMALL_RX = 1 << 1
CFG_NO_TDC = 1 << 2

# C1TREC status bits (byte "trec" in stats)
TREC = {0x01: "EWARN", 0x02: "RXWARN", 0x04: "TXWARN", 0x08: "RXBP", 0x10: "TXBP", 0x20: "TXBO"}
# C1INT bits 8..15
C1INT_HI = {0x01: "ECCIF", 0x02: "SPICRCIF", 0x04: "TXATIF", 0x08: "RXOVIF", 0x10: "SERRIF",
            0x20: "CERRIF", 0x40: "WAKIF", 0x80: "IVMIF"}
# C1BDIAG1 bits 16..31
BDIAG1 = {0x0001: "NBIT0ERR", 0x0002: "NBIT1ERR", 0x0004: "NACKERR", 0x0008: "NFORMERR",
          0x0010: "NSTUFERR", 0x0020: "NCRCERR", 0x0080: "TXBOERR", 0x0100: "DBIT0ERR",
          0x0200: "DBIT1ERR", 0x0800: "DFORMERR", 0x1000: "DSTUFERR", 0x2000: "DCRCERR",
          0x4000: "ESI", 0x8000: "DLCMM"}
FIFO_TX_FLAGS = {1: "TXERR", 2: "TXLARB", 4: "TXABT"}


def bits(value, table):
    return [n for b, n in table.items() if value & b]


class DeviceError(RuntimeError):
    pass


class Device:
    def __init__(self, host):
        self.h = host

    # --- transport -----------------------------------------------------------
    def _send(self, op, args=b"", wait=True):
        payload = bytes([op]) + bytes(args)
        payload = payload + bytes(8 - len(payload))
        return self.h.send(CTRL_REQ, payload, wait=wait, timeout=2.0)

    def _collect(self, op, timeout):
        """Gather reply chunks for op from the host RX buffer; returns the
        reassembled payload.  Non-control frames seen meanwhile are kept in
        self.stray for the caller."""
        chunks = {}
        deadline = time.monotonic() + timeout
        self.stray = []
        while time.monotonic() < deadline:
            for f in self.h.recv(timeout=min(0.2, max(0.01, deadline - time.monotonic()))):
                if f["id"] == CTRL_RSP and not f["ext"] and len(f["data"]) == 8 and f["data"][0] == op:
                    idx = f["data"][1] & 0x7F
                    chunks[idx] = f["data"][2:8]
                    if f["data"][1] & 0x80:
                        if all(i in chunks for i in range(idx + 1)):
                            return b"".join(chunks[i] for i in range(idx + 1))
                elif f["id"] == CTRL_RSP:
                    pass
                else:
                    self.stray.append(f)
        raise DeviceError(f"no reply to op 0x{op:02x} (got chunks {sorted(chunks)})")

    retries = 3

    def cmd(self, op, args=b"", timeout=2.0):
        last = None
        for _ in range(self.retries):
            self.h.drain()
            self._send(op, args)
            try:
                return self._collect(op, timeout)
            except DeviceError as e:
                last = e
        raise last

    # --- commands --------------------------------------------------------------
    def ping(self, timeout=1.0):
        p = self.cmd(OP_PING, timeout=timeout)
        return dict(ver=p[0], opmod=p[1], opmod_name=MODE_NAMES.get(p[1]), fast_spi=bool(p[2]),
                    cfg_mode=p[3], cfg_nom=p[4], cfg_dat=p[5])

    def stats(self):
        p = self.cmd(OP_STATS)
        tec, rec, trec, rxovf, c1int, intp = p[0:6]
        rx = struct.unpack_from("<I", p, 6)[0]
        miss, bad = struct.unpack_from("<HH", p, 10)
        bd0 = struct.unpack_from("<I", p, 14)[0]
        bd1hi, efmsg, echo_tx, echo_drop, tx_fail = struct.unpack_from("<5H", p, 18)
        rx_fd, rx_brs = p[28], p[29]
        span_us = struct.unpack_from("<I", p, 30)[0]
        rx_ext, rx_rtr = p[34], p[35]
        rx_bytes = struct.unpack_from("<I", p, 36)[0]
        opmod = p[40]
        fast = bool(p[41] & 1)
        fifo = (p[41] >> 1) & 7
        gap_min, gap_max = struct.unpack_from("<HH", p, 42)
        gaps10 = p[46]
        loop_max_ms = p[47]
        tx_abort = struct.unpack_from("<H", p, 48)[0] if len(p) >= 50 else None
        return dict(
            gap_min_us=gap_min, gap_max_us=gap_max, gaps_over_10ms=gaps10, loop_max_ms=loop_max_ms,
            tx_abort=tx_abort,
            tec=tec, rec=rec, trec=bits(trec, TREC), rxovf=rxovf,
            c1int=bits(c1int, C1INT_HI),
            int_pin=(None if intp & 0x80 else intp & 1),
            int_seen_low=bool(intp & 2), int_always_low_when_rx=bool(intp & 4),
            rx=rx, seq_missing=miss, seq_bad=bad,
            nrerr=bd0 & 0xFF, nterr=(bd0 >> 8) & 0xFF, drerr=(bd0 >> 16) & 0xFF,
            dterr=(bd0 >> 24) & 0xFF,
            bdiag1=bits(bd1hi, BDIAG1), efmsgcnt=efmsg,
            echo_tx=echo_tx, echo_drop=echo_drop, tx_fail=tx_fail,
            rx_fd=rx_fd, rx_brs=rx_brs, rx_ext=rx_ext, rx_rtr=rx_rtr,
            span_us=span_us, rx_bytes=rx_bytes, opmod=opmod,
            opmod_name=MODE_NAMES.get(opmod), fast_spi=fast,
            tx_fifo_flags=bits(fifo, FIFO_TX_FLAGS))

    def clear(self):
        return self.cmd(OP_CLEAR)

    def set_mode(self, mode=MODE_NORMAL_20, nominal=500000, data=2000000, flags=0):
        """Ack comes on the old configuration; the device reconfigures ~20 ms
        after acking."""
        # No retries: the device reconfigures right after acking, so a re-sent
        # command would go out on the old bit rate.  A lost ack is tolerated;
        # callers verify with ping() once the host side is reconfigured.
        self.h.drain()
        self._send(OP_SETMODE, bytes([mode, NOM_RATES[nominal], DAT_RATES[data], flags]))
        try:
            p = self._collect(OP_SETMODE, 0.6)
        except DeviceError:
            p = bytes(6)
        time.sleep(0.2)
        return dict(prev_mode=p[1], prev_nom=p[2], prev_dat=p[3], prev_flags=p[4])

    def echo(self, on):
        p = self.cmd(OP_ECHO, bytes([1 if on else 0]))
        return bool(p[0])

    def burst(self, count=100, length=8, can_id=0x300, fd=False, brs=False, ext=False,
              rtr=False, fill=0x5A, timeout=None):
        flags = (1 if fd else 0) | (2 if brs else 0) | (4 if ext else 0) | (8 if rtr else 0)
        args = struct.pack("<HBBHB", count, length, flags, can_id, fill)
        if timeout is None:
            timeout = 12 + count * 0.02
        p = self.cmd(OP_BURST, args, timeout=timeout)
        sent, failed, dur = struct.unpack_from("<HHI", p, 0)
        return dict(sent=sent, failed=failed, dur_us=dur, tec=p[8], rec=p[9],
                    tx_fifo_flags=bits(p[10], FIFO_TX_FLAGS), drained=bool(p[11]),
                    frames_per_s=(round((sent - 1) * 1e6 / dur, 1) if sent > 1 and dur else None))

    def sleep(self, ms=1000, xstby=True):
        """Ack, then the device sleeps for ms.  Call sleep_report() afterwards."""
        p = self.cmd(OP_SLEEP, struct.pack("<HB", ms, 0 if xstby else 1))
        return p[0] == 1

    def sleep_report(self, timeout):
        p = self._collect(OP_SLEEP, timeout)
        int_low_at, wake_us = struct.unpack_from("<HH", p, 2)
        return dict(wakif=bool(p[0]), int_seen_low=bool(p[1]),
                    int_low_at_ms=(None if int_low_at == 0xFFFF else int_low_at),
                    osc_ready_us=wake_us, opmod_after_wake=p[6],
                    opmod_after_wake_name=MODE_NAMES.get(p[6]),
                    int_before=(None if p[7] & 0x80 else p[7] & 1), int_at_end=(p[7] >> 1) & 1,
                    osc_before=struct.unpack_from("<H", p, 8)[0], c1int=bits(p[10], C1INT_HI),
                    reconfig_failed=bool(p[11]))

    def railcycle(self, off_ms=500):
        p = self.cmd(OP_RAILCYCLE, struct.pack("<H", off_ms))
        return p[0] == 1

    def railcycle_report(self, timeout):
        p = self._collect(OP_RAILCYCLE, timeout)
        def s8(b):
            return b - 256 if b > 127 else b
        low_at = struct.unpack_from("<H", p, 7)[0]
        return dict(power_on_err=s8(p[0]), reconfig_err=s8(p[1]), rail_before=s8(p[2]),
                    rail_off=s8(p[3]), rail_back=s8(p[4]),
                    power_on_ms=struct.unpack_from("<H", p, 5)[0],
                    rail_sense_low_after_ms=(None if low_at == 0xFFFF else low_at),
                    osc=struct.unpack_from("<H", p, 9)[0], opmod=p[11],
                    opmod_name=MODE_NAMES.get(p[11]))

    def spibench(self, n=200):
        p = self.cmd(OP_SPIBENCH, struct.pack("<H", n), timeout=60)
        ram_err, rw_err, wr_us, rd_us, kbps = struct.unpack_from("<5H", p, 0)
        return dict(ram_err=ram_err, rw_err=rw_err, write16_us=wr_us, read16_us=rd_us,
                    spi_kbps=kbps, fast_spi=bool(p[10]))

    def filter(self, sid=None, mask=0x7FF):
        if sid is None:
            p = self.cmd(OP_FILTER, struct.pack("<HHB", 0, 0, 1))
        else:
            p = self.cmd(OP_FILTER, struct.pack("<HHB", sid, mask, 0))
        return dict(accept_all=bool(p[1]))

    def looptest(self, internal=True, fd=False, brs=False, ext=False, length=8, count=8,
                 delay_ms=0):
        """delay_ms > 0: the device acks and runs the test delay_ms later;
        call looptest_report() for the result (after putting the host back
        in a mode that can ACK)."""
        flags = (1 if fd else 0) | (2 if brs else 0) | (4 if ext else 0)
        args = bytes([MODE_INT_LOOP if internal else MODE_EXT_LOOP, flags, length, count]) + \
            struct.pack("<H", delay_ms)
        if delay_ms:
            p = self.cmd(OP_LOOPTEST, args, timeout=2)
            return p[0] == 1
        p = self.cmd(OP_LOOPTEST, args, timeout=10)
        return self._loop_decode(p)

    def looptest_report(self, timeout=10):
        return self._loop_decode(self._collect(OP_LOOPTEST, timeout))

    @staticmethod
    def _loop_decode(p):
        return dict(sent=p[0], received=p[1], mismatch=p[2], tec=p[3], rec=p[4],
                    tx_fifo_flags=bits(p[5], FIFO_TX_FLAGS),
                    bdiag1=bits(struct.unpack_from("<H", p, 6)[0], BDIAG1),
                    dur_us=struct.unpack_from("<I", p, 8)[0], config_failed=bool(p[11]))

    def set_fast_spi(self, on):
        p = self.cmd(OP_SETFAST, bytes([1 if on else 0]))
        return bool(p[0])

    def txone(self, length=8, can_id=0x310, fd=False, brs=False, ext=False, rtr=False, fill=0):
        flags = (1 if fd else 0) | (2 if brs else 0) | (4 if ext else 0) | (8 if rtr else 0)
        p = self.cmd(OP_TXONE, struct.pack("<BBHB", length, flags, can_id, fill))
        err = p[0] - 256 if p[0] > 127 else p[0]
        return err

    def reboot(self):
        try:
            self.cmd(OP_REBOOT, timeout=1.0)
        except DeviceError:
            pass
