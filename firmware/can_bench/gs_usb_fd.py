"""
Minimal gs_usb (candleLight / CANable 2.0) driver on top of pyusb, with
CAN-FD, hardware timestamps and error-frame reporting.

The PyPI `gs_usb` package deliberately leaves CAN-FD unimplemented, so this
module speaks the protocol directly.  It is deliberately small: one channel,
blocking reads, no threading.  can_bench/server.py wraps it.

Protocol reference: linux/drivers/net/can/usb/gs_usb.c and the candleLight
firmware (candle-usb/candlelight_fw).
"""

import collections
import ctypes
import ctypes.util
import struct
import threading
import time
from ctypes import CFUNCTYPE, POINTER, Structure, byref, c_int, c_int32, c_long, c_uint, c_uint8, c_void_p, cast

# sudo strips DYLD_LIBRARY_PATH (SIP), so help pyusb find libusb
_orig_find = ctypes.util.find_library
def _find_library(name):
    if name in ("usb-1.0", "usb"):
        return "/opt/local/lib/libusb-1.0.dylib"
    return _orig_find(name)
ctypes.util.find_library = _find_library

import usb.core   # noqa: E402
import usb.util   # noqa: E402

VID = 0x1D50
PID = 0x606F

# --- control requests -------------------------------------------------------
BREQ_HOST_FORMAT     = 0
BREQ_BITTIMING       = 1
BREQ_MODE            = 2
BREQ_BERR            = 3
BREQ_BT_CONST        = 4
BREQ_DEVICE_CONFIG   = 5
BREQ_TIMESTAMP       = 6
BREQ_IDENTIFY        = 7
BREQ_DATA_BITTIMING  = 10
BREQ_BT_CONST_EXT    = 11
BREQ_SET_TERMINATION = 12
BREQ_GET_TERMINATION = 13
BREQ_GET_STATE       = 14

# --- feature / mode flags ---------------------------------------------------
FEAT_LISTEN_ONLY    = 1 << 0
FEAT_LOOP_BACK      = 1 << 1
FEAT_TRIPLE_SAMPLE  = 1 << 2
FEAT_ONE_SHOT       = 1 << 3
FEAT_HW_TIMESTAMP   = 1 << 4
FEAT_IDENTIFY       = 1 << 5
FEAT_USER_ID        = 1 << 6
FEAT_PAD_PKTS       = 1 << 7
FEAT_FD             = 1 << 8
FEAT_LPC_QUIRK      = 1 << 9
FEAT_BT_CONST_EXT   = 1 << 10
FEAT_TERMINATION    = 1 << 11
FEAT_BERR_REPORTING = 1 << 12
FEAT_GET_STATE      = 1 << 13

FEATURE_NAMES = {
    FEAT_LISTEN_ONLY: "LISTEN_ONLY", FEAT_LOOP_BACK: "LOOP_BACK",
    FEAT_TRIPLE_SAMPLE: "TRIPLE_SAMPLE", FEAT_ONE_SHOT: "ONE_SHOT",
    FEAT_HW_TIMESTAMP: "HW_TIMESTAMP", FEAT_IDENTIFY: "IDENTIFY",
    FEAT_USER_ID: "USER_ID", FEAT_PAD_PKTS: "PAD_PKTS_TO_MAX_PKT_SIZE",
    FEAT_FD: "FD", FEAT_LPC_QUIRK: "REQ_USB_QUIRK_LPC546XX",
    FEAT_BT_CONST_EXT: "BT_CONST_EXT", FEAT_TERMINATION: "TERMINATION",
    FEAT_BERR_REPORTING: "BERR_REPORTING", FEAT_GET_STATE: "GET_STATE",
}

MODE_NORMAL        = 0
MODE_LISTEN_ONLY   = 1 << 0
MODE_LOOP_BACK     = 1 << 1
MODE_TRIPLE_SAMPLE = 1 << 2
MODE_ONE_SHOT      = 1 << 3
MODE_HW_TIMESTAMP  = 1 << 4
MODE_PAD_PKTS      = 1 << 7
MODE_FD            = 1 << 8
MODE_BERR_REPORTING = 1 << 12

# --- frame flags ------------------------------------------------------------
CAN_EFF_FLAG = 0x80000000
CAN_RTR_FLAG = 0x40000000
CAN_ERR_FLAG = 0x20000000
CAN_SFF_MASK = 0x7FF
CAN_EFF_MASK = 0x1FFFFFFF
CAN_ERR_MASK = 0x1FFFFFFF

FLAG_OVERFLOW = 1 << 0
FLAG_FD       = 1 << 1
FLAG_BRS      = 1 << 2
FLAG_ESI      = 1 << 3

ECHO_ID_RX = 0xFFFFFFFF

# error class bits in can_id of an error frame (linux/can/error.h)
CAN_ERR_TX_TIMEOUT = 0x001
CAN_ERR_LOSTARB    = 0x002
CAN_ERR_CRTL       = 0x004
CAN_ERR_PROT       = 0x008
CAN_ERR_TRX        = 0x010
CAN_ERR_ACK        = 0x020
CAN_ERR_BUSOFF     = 0x040
CAN_ERR_BUSERROR   = 0x080
CAN_ERR_RESTARTED  = 0x100
CAN_ERR_CNT        = 0x200

# data[1] controller status
CRTL_RX_OVERFLOW = 0x01
CRTL_TX_OVERFLOW = 0x02
CRTL_RX_WARNING  = 0x04
CRTL_TX_WARNING  = 0x08
CRTL_RX_PASSIVE  = 0x10
CRTL_TX_PASSIVE  = 0x20
CRTL_ACTIVE      = 0x40

DLC2LEN = [0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64]
LEN2DLC = {v: i for i, v in enumerate(DLC2LEN)}


def len2dlc(n):
    """Smallest DLC whose payload holds n bytes."""
    for dlc, l in enumerate(DLC2LEN):
        if l >= n:
            return dlc
    raise ValueError(n)


class BitTiming:
    __slots__ = ("prop_seg", "phase_seg1", "phase_seg2", "sjw", "brp")

    def __init__(self, prop_seg, phase_seg1, phase_seg2, sjw, brp):
        self.prop_seg, self.phase_seg1, self.phase_seg2 = prop_seg, phase_seg1, phase_seg2
        self.sjw, self.brp = sjw, brp

    def pack(self):
        return struct.pack("<5I", self.prop_seg, self.phase_seg1,
                           self.phase_seg2, self.sjw, self.brp)

    def tq_total(self):
        return 1 + self.prop_seg + self.phase_seg1 + self.phase_seg2

    def bitrate(self, fclk):
        return fclk / (self.brp * self.tq_total())

    def sample_point(self):
        return (1 + self.prop_seg + self.phase_seg1) / self.tq_total()

    def as_dict(self, fclk=None):
        d = dict(prop_seg=self.prop_seg, phase_seg1=self.phase_seg1,
                 phase_seg2=self.phase_seg2, sjw=self.sjw, brp=self.brp,
                 tq_total=self.tq_total(), sample_point=round(self.sample_point(), 4))
        if fclk:
            d["bitrate"] = round(self.bitrate(fclk), 1)
        return d


class BtConst:
    FIELDS = ("feature", "fclk_can", "tseg1_min", "tseg1_max", "tseg2_min",
              "tseg2_max", "sjw_max", "brp_min", "brp_max", "brp_inc")
    EXT_FIELDS = ("dtseg1_min", "dtseg1_max", "dtseg2_min", "dtseg2_max",
                  "dsjw_max", "dbrp_min", "dbrp_max", "dbrp_inc")

    def __init__(self, raw, ext=False):
        vals = struct.unpack("<10I", bytes(raw[:40]))
        for k, v in zip(self.FIELDS, vals):
            setattr(self, k, v)
        self.ext = ext
        if ext:
            vals = struct.unpack("<8I", bytes(raw[40:72]))
            for k, v in zip(self.EXT_FIELDS, vals):
                setattr(self, k, v)

    def features(self):
        return [n for b, n in FEATURE_NAMES.items() if self.feature & b]

    def as_dict(self):
        d = {k: getattr(self, k) for k in self.FIELDS}
        d["features"] = self.features()
        if self.ext:
            d.update({k: getattr(self, k) for k in self.EXT_FIELDS})
        return d


def timing_for(fclk, bitrate, sample_point=0.8, tseg1_max=256, tseg2_max=128,
               sjw_max=128, brp_max=1024, tq_min=8, tq_max=None, sjw=None):
    """Pick a bit timing for `bitrate` on clock `fclk`.  Prefers the smallest
    BRP (most time quanta per bit) so the sample point can be placed
    precisely; `sjw` defaults to phase_seg2 (max resync, as the MCP2518FD
    side uses)."""
    if tq_max is None:
        tq_max = 1 + tseg1_max + tseg2_max
    best = None
    for brp in range(1, brp_max + 1):
        tq = fclk / (brp * bitrate)
        if abs(tq - round(tq)) > 1e-6:
            continue
        tq = int(round(tq))
        if tq < tq_min or tq > tq_max:
            continue
        seg1 = int(round(tq * sample_point)) - 1
        seg2 = tq - 1 - seg1
        if seg2 < 1 or seg2 > tseg2_max or seg1 < 2 or seg1 > tseg1_max:
            continue
        # split seg1 between prop and phase1 (device only sees the sum)
        prop = seg1 // 2
        ph1 = seg1 - prop
        s = min(seg2, sjw_max) if sjw is None else min(sjw, sjw_max)
        best = BitTiming(prop, ph1, seg2, s, brp)
        break
    if best is None:
        raise ValueError(f"no timing for {bitrate} bps on {fclk} Hz clock")
    return best


class Frame:
    __slots__ = ("can_id", "ext", "rtr", "err", "fd", "brs", "esi", "data",
                 "ts_us", "echo_id", "overflow", "host_time")

    def __init__(self, can_id=0, data=b"", ext=False, rtr=False, fd=False,
                 brs=False, esi=False, err=False, ts_us=None, echo_id=ECHO_ID_RX,
                 overflow=False, host_time=None):
        self.can_id, self.data, self.ext, self.rtr = can_id, bytes(data), ext, rtr
        self.fd, self.brs, self.esi, self.err = fd, brs, esi, err
        self.ts_us, self.echo_id, self.overflow = ts_us, echo_id, overflow
        self.host_time = host_time

    def is_rx(self):
        return self.echo_id == ECHO_ID_RX

    def as_dict(self):
        return dict(id=self.can_id, ext=self.ext, rtr=self.rtr, err=self.err,
                    fd=self.fd, brs=self.brs, esi=self.esi,
                    data=self.data.hex(), ts_us=self.ts_us, echo_id=self.echo_id,
                    overflow=self.overflow, t=self.host_time)

    def __repr__(self):
        kind = "ERR" if self.err else ("FD" if self.fd else "CC")
        return (f"<{kind} id=0x{self.can_id:X}{'x' if self.ext else ''}"
                f"{' rtr' if self.rtr else ''}{' brs' if self.brs else ''}"
                f" len={len(self.data)} {self.data.hex()} ts={self.ts_us}>")


# --- asynchronous IN endpoint reader (libusb async API via ctypes) ------------
# One synchronous bulk read per frame tops out at a few hundred frames/s on
# macOS, and a candleLight with only a handful of pool buffers then drops CAN
# frames.  Keeping several transfers queued in the host controller means the
# adapter can hand over every frame as soon as it has it.

class _Timeval(Structure):
    _fields_ = [("tv_sec", c_long), ("tv_usec", c_int32)]


class _Transfer(Structure):
    _fields_ = [("dev_handle", c_void_p), ("flags", c_uint8), ("endpoint", c_uint8),
                ("type", c_uint8), ("timeout", c_uint), ("status", c_int), ("length", c_int),
                ("actual_length", c_int), ("callback", c_void_p), ("user_data", c_void_p),
                ("buffer", c_void_p), ("num_iso_packets", c_int)]


_TRANSFER_CB = CFUNCTYPE(None, POINTER(_Transfer))
LIBUSB_TRANSFER_COMPLETED, LIBUSB_TRANSFER_CANCELLED, LIBUSB_TRANSFER_STALL = 0, 3, 4


class AsyncIn:
    def __init__(self, dev, ep_addr, n_transfers=8, size=512):
        ctx_obj = dev._ctx
        self.lib = ctx_obj.backend.lib
        self.ctx = ctx_obj.backend.ctx
        self.handle = ctx_obj.handle.handle
        lib = self.lib
        lib.libusb_alloc_transfer.restype = POINTER(_Transfer)
        lib.libusb_alloc_transfer.argtypes = [c_int]
        lib.libusb_submit_transfer.argtypes = [POINTER(_Transfer)]
        lib.libusb_cancel_transfer.argtypes = [POINTER(_Transfer)]
        lib.libusb_free_transfer.argtypes = [POINTER(_Transfer)]
        lib.libusb_handle_events_timeout_completed.argtypes = [c_void_p, POINTER(_Timeval), POINTER(c_int)]
        self.q = collections.deque()
        self.ev = threading.Event()
        self.running = True
        self.stalled = False
        self.errors = 0
        self.pending = 0
        self.hist = {}
        self.lens = {}
        self._cb = _TRANSFER_CB(self._on_done)
        self.bufs = []
        self.transfers = []
        for _ in range(n_transfers):
            buf = ctypes.create_string_buffer(size)
            t = lib.libusb_alloc_transfer(0)
            c = t.contents
            c.dev_handle = self.handle
            c.flags = 0
            c.endpoint = ep_addr
            c.type = 2                      # LIBUSB_TRANSFER_TYPE_BULK
            c.timeout = 0
            c.buffer = cast(buf, c_void_p)
            c.length = size
            c.callback = cast(self._cb, c_void_p)
            c.user_data = None
            c.num_iso_packets = 0
            self.bufs.append(buf)
            self.transfers.append(t)
        for t in self.transfers:
            if lib.libusb_submit_transfer(t) == 0:
                self.pending += 1
        self.thread = threading.Thread(target=self._events, daemon=True, name="usb-events")
        self.thread.start()

    def _on_done(self, tp):
        t = tp.contents
        self.hist[t.status] = self.hist.get(t.status, 0) + 1
        self.lens[t.actual_length] = self.lens.get(t.actual_length, 0) + 1
        if t.status == LIBUSB_TRANSFER_COMPLETED:
            if t.actual_length > 0:
                self.q.append((ctypes.string_at(t.buffer, t.actual_length), time.monotonic()))
                self.ev.set()
        elif t.status == LIBUSB_TRANSFER_CANCELLED:
            self.pending -= 1
            return
        else:
            self.errors += 1
            if t.status == LIBUSB_TRANSFER_STALL:
                self.stalled = True
        if self.running:
            if self.lib.libusb_submit_transfer(tp) != 0:
                self.pending -= 1
        else:
            self.pending -= 1

    def _events(self):
        tv = _Timeval(0, 50000)
        while self.running or self.pending > 0:
            self.lib.libusb_handle_events_timeout_completed(self.ctx, byref(tv), None)
            if not self.running and self.pending > 0:
                # keep pumping until every cancelled transfer has reported
                continue

    def pop(self, timeout_s):
        try:
            return self.q.popleft()
        except IndexError:
            pass
        self.ev.wait(timeout_s)
        self.ev.clear()
        try:
            return self.q.popleft()
        except IndexError:
            return None

    def stop(self):
        self.running = False
        for t in self.transfers:
            self.lib.libusb_cancel_transfer(t)
        self.thread.join(timeout=2.0)
        for t in self.transfers:
            self.lib.libusb_free_transfer(t)
        self.transfers = []


class GsUsbFd:
    MAX_ECHO = 10   # candleLight keeps this many TX slots in flight
    IN_STRIDE = 128  # padded size of one host-bound frame from the adapter

    def __init__(self, dev=None, channel=0):
        if dev is None:
            dev = usb.core.find(idVendor=VID, idProduct=PID)
        if dev is None:
            raise RuntimeError("gs_usb adapter not found (VID 1D50 PID 606F)")
        self.dev = dev
        self.channel = channel
        try:
            if dev.is_kernel_driver_active(0):
                dev.detach_kernel_driver(0)
        except (NotImplementedError, usb.core.USBError):
            pass
        dev.set_configuration()
        cfg = dev.get_active_configuration()
        intf = cfg[(0, 0)]
        self.intf_num = intf.bInterfaceNumber
        usb.util.claim_interface(dev, self.intf_num)
        self.ep_in = usb.util.find_descriptor(
            intf, custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_IN)
        self.ep_out = usb.util.find_descriptor(
            intf, custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_OUT)
        self.max_pkt = self.ep_in.wMaxPacketSize

        self._ctrl_out(BREQ_HOST_FORMAT, struct.pack("<I", 0x0000BEEF))
        cfgraw = self._ctrl_in(BREQ_DEVICE_CONFIG, 12)
        r1, r2, r3, icount, sw, hw = struct.unpack("<4B2I", bytes(cfgraw))
        self.sw_version, self.hw_version, self.icount = sw, hw, icount + 1
        self.bt = BtConst(self._ctrl_in(BREQ_BT_CONST, 40))
        if self.bt.feature & FEAT_BT_CONST_EXT:
            self.bt = BtConst(self._ctrl_in(BREQ_BT_CONST_EXT, 72), ext=True)
        self.fclk = self.bt.fclk_can
        self.started = False
        self.mode_flags = 0
        self.frame_len = 20
        self._next_echo = 0
        self.nominal = None
        self.data_timing = None
        self.ain = None

    # --- low level ----------------------------------------------------------
    def _ctrl_out(self, req, data):
        self.dev.ctrl_transfer(0x41, req, self.channel, self.intf_num, data, timeout=1000)

    def _ctrl_in(self, req, n):
        return self.dev.ctrl_transfer(0xC1, req, self.channel, self.intf_num, n, timeout=1000)

    def info(self):
        return dict(sw_version=self.sw_version, hw_version=self.hw_version,
                    channels=self.icount, product=self.dev.product,
                    manufacturer=self.dev.manufacturer, serial=self.dev.serial_number,
                    bus=self.dev.bus, address=self.dev.address,
                    max_packet=self.max_pkt, **self.bt.as_dict())

    def get_state(self):
        if not (self.bt.feature & FEAT_GET_STATE):
            return None
        st, rx, tx = struct.unpack("<3I", bytes(self._ctrl_in(BREQ_GET_STATE, 12)))
        names = {0: "ERROR_ACTIVE", 1: "ERROR_WARNING", 2: "ERROR_PASSIVE",
                 3: "BUS_OFF", 4: "STOPPED", 5: "SLEEPING"}
        return dict(state=names.get(st, st), rxerr=rx, txerr=tx)

    def device_timestamp(self):
        return struct.unpack("<I", bytes(self._ctrl_in(BREQ_TIMESTAMP, 4)))[0]

    def get_termination(self):
        if not (self.bt.feature & FEAT_TERMINATION):
            return None
        return struct.unpack("<I", bytes(self._ctrl_in(BREQ_GET_TERMINATION, 4)))[0]

    def set_termination(self, on):
        self._ctrl_out(BREQ_SET_TERMINATION, struct.pack("<I", 1 if on else 0))

    # --- configuration ------------------------------------------------------
    def stop(self):
        self._ctrl_out(BREQ_MODE, struct.pack("<2I", 0, 0))
        self.started = False
        if self.ain is not None:
            self.ain.stop()
            self.ain = None
        # drain anything left in the pipe
        for _ in range(64):
            try:
                self.ep_in.read(self.max_pkt * 4, timeout=20)
            except usb.core.USBTimeoutError:
                break
            except usb.core.USBError:
                break

    def start(self, nominal, data=None, fd=False, listen_only=False,
              one_shot=False, loopback=False, hw_timestamp=True, berr=True,
              triple_sample=False):
        """nominal/data: BitTiming objects."""
        if self.started:
            self.stop()
        self._ctrl_out(BREQ_BITTIMING, nominal.pack())
        self.nominal = nominal
        self.data_timing = None
        flags = 0
        if fd:
            if not (self.bt.feature & FEAT_FD):
                raise RuntimeError("adapter has no CAN-FD support")
            if data is None:
                raise ValueError("FD needs a data bit timing")
            self._ctrl_out(BREQ_DATA_BITTIMING, data.pack())
            self.data_timing = data
            flags |= MODE_FD
        if listen_only:
            flags |= MODE_LISTEN_ONLY
        if one_shot:
            flags |= MODE_ONE_SHOT
        if loopback:
            flags |= MODE_LOOP_BACK
        if triple_sample:
            flags |= MODE_TRIPLE_SAMPLE
        if hw_timestamp and (self.bt.feature & FEAT_HW_TIMESTAMP):
            flags |= MODE_HW_TIMESTAMP
        if berr and (self.bt.feature & FEAT_BERR_REPORTING):
            flags |= MODE_BERR_REPORTING
        if self.bt.feature & FEAT_PAD_PKTS:
            flags |= MODE_PAD_PKTS
        flags &= self.bt.feature | 0   # never request unsupported bits
        self.mode_flags = flags
        self.frame_len = 12 + (64 if fd else 8) + (4 if flags & MODE_HW_TIMESTAMP else 0)
        self._ctrl_out(BREQ_MODE, struct.pack("<2I", 1, flags))
        # This candleLight build pads every host-bound frame to IN_STRIDE
        # bytes (two full packets, no short packet), so a transfer must be
        # exactly one frame long or it only completes every few frames.
        self.ain = AsyncIn(self.dev, self.ep_in.bEndpointAddress, n_transfers=16,
                           size=self.IN_STRIDE)
        self.started = True
        self._next_echo = 0

    # --- frames -------------------------------------------------------------
    def _pack(self, f, echo_id):
        can_id = f.can_id & (CAN_EFF_MASK if f.ext else CAN_SFF_MASK)
        if f.ext:
            can_id |= CAN_EFF_FLAG
        if f.rtr:
            can_id |= CAN_RTR_FLAG
        flags = 0
        if f.fd:
            flags |= FLAG_FD
            if f.brs:
                flags |= FLAG_BRS
            if f.esi:
                flags |= FLAG_ESI
        data_len = 64 if (self.mode_flags & MODE_FD) else 8
        n = len(f.data)
        if n > data_len:
            raise ValueError("payload too long for mode")
        if f.fd:
            dlc = len2dlc(n)
            payload = f.data + bytes(DLC2LEN[dlc] - n)
        else:
            dlc = n if not f.rtr else (n if n <= 8 else 8)
            payload = f.data
        buf = struct.pack("<IIBBBB", echo_id, can_id, dlc, self.channel, flags, 0)
        buf += payload + bytes(data_len - len(payload))
        if self.mode_flags & MODE_HW_TIMESTAMP:
            buf += b"\0\0\0\0"
        return buf

    def _unpack(self, raw, host_time):
        echo_id, can_id, dlc, ch, flags, _ = struct.unpack_from("<IIBBBB", raw, 0)
        data_len = 64 if (self.mode_flags & MODE_FD) else 8
        data = bytes(raw[12:12 + data_len])
        ts = None
        if self.mode_flags & MODE_HW_TIMESTAMP and len(raw) >= 12 + data_len + 4:
            ts = struct.unpack_from("<I", raw, 12 + data_len)[0]
        f = Frame(host_time=host_time, ts_us=ts, echo_id=echo_id)
        f.err = bool(can_id & CAN_ERR_FLAG)
        f.ext = bool(can_id & CAN_EFF_FLAG)
        f.rtr = bool(can_id & CAN_RTR_FLAG)
        f.overflow = bool(flags & FLAG_OVERFLOW)
        if f.err:
            f.can_id = can_id & CAN_ERR_MASK
            f.data = data[:8]
        else:
            f.can_id = can_id & (CAN_EFF_MASK if f.ext else CAN_SFF_MASK)
            f.fd = bool(flags & FLAG_FD)
            f.brs = bool(flags & FLAG_BRS)
            f.esi = bool(flags & FLAG_ESI)
            n = DLC2LEN[dlc & 0xF] if f.fd else min(dlc & 0xF, 8)
            f.data = b"" if f.rtr else data[:n]
            if f.rtr:
                f.data = bytes(min(dlc & 0xF, 8))   # carry the DLC as length
        return f

    def next_echo_id(self):
        e = self._next_echo
        self._next_echo = (self._next_echo + 1) % self.MAX_ECHO
        return e

    def write(self, frame, echo_id=None, timeout=1000):
        if echo_id is None:
            echo_id = self.next_echo_id()
        self.ep_out.write(self._pack(frame, echo_id), timeout=timeout)
        return echo_id

    def read(self, timeout_ms=100):
        """One frame (RX, TX-echo or error) or None on timeout."""
        if self.ain is None:
            return None
        if self.ain.stalled:
            self.ain.stalled = False
            raise usb.core.USBError("Pipe error (IN endpoint stalled)", 32)
        item = self.ain.pop(timeout_ms / 1000.0)
        if item is None:
            return None
        raw, t = item
        if len(raw) < 12:
            return None
        return self._unpack(raw, t)

    def close(self):
        try:
            if self.started:
                self.stop()
        finally:
            usb.util.release_interface(self.dev, self.intf_num)
            usb.util.dispose_resources(self.dev)


def error_frame_describe(f):
    """Human-readable summary of an error frame."""
    cls = []
    for bit, name in ((CAN_ERR_TX_TIMEOUT, "tx-timeout"), (CAN_ERR_LOSTARB, "lost-arb"),
                      (CAN_ERR_CRTL, "ctrl"), (CAN_ERR_PROT, "prot"), (CAN_ERR_TRX, "trx"),
                      (CAN_ERR_ACK, "no-ack"), (CAN_ERR_BUSOFF, "bus-off"),
                      (CAN_ERR_BUSERROR, "bus-error"), (CAN_ERR_RESTARTED, "restarted"),
                      (CAN_ERR_CNT, "cnt")):
        if f.can_id & bit:
            cls.append(name)
    d = f.data + bytes(8 - len(f.data))
    st = []
    for bit, name in ((CRTL_RX_OVERFLOW, "rx-ovf"), (CRTL_TX_OVERFLOW, "tx-ovf"),
                      (CRTL_RX_WARNING, "rx-warn"), (CRTL_TX_WARNING, "tx-warn"),
                      (CRTL_RX_PASSIVE, "rx-passive"), (CRTL_TX_PASSIVE, "tx-passive"),
                      (CRTL_ACTIVE, "active")):
        if d[1] & bit:
            st.append(name)
    return dict(classes=cls, ctrl=st, prot=d[2], prot_loc=d[3], txerr=d[6], rxerr=d[7])
