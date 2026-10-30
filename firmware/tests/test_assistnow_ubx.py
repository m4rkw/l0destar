"""Host tests for the AssistNow side of src/gnss_ubx.c: the messages that tell
the receiver the time and where it is, the decoder for its acknowledgements,
the ephemeris/almanac/AssistNow counts of NAV-SAT, and the scanner that walks a
downloaded run of UBX frames.

gnss_ubx.c is built as a shared library with the host compiler and called
through ctypes.  The driver around it needs Zephyr and is checked on the board.

    uv run --python 3.12 --with pytest --with pyubx2 pytest -q firmware/tests/test_assistnow_ubx.py

pyubx2 is an independent decoder used as a cross-check; without it those tests
are skipped.
"""
import ctypes as C
import datetime
import os
import random
import struct
import subprocess
import tempfile

import pytest

SRC = os.environ.get("GNSS_SRC", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src"))


class Utc(C.Structure):
    _fields_ = [("year", C.c_uint16), ("month", C.c_uint8), ("day", C.c_uint8),
                ("hour", C.c_uint8), ("min", C.c_uint8), ("sec", C.c_uint8),
                ("ns", C.c_uint32), ("tacc_s", C.c_uint16), ("tacc_ns", C.c_uint32)]


class SatSummary(C.Structure):
    _fields_ = [("itow_ms", C.c_uint32), ("listed", C.c_uint8), ("tracked", C.c_uint8),
                ("used", C.c_uint8), ("best_cno", C.c_uint8), ("eph", C.c_uint8),
                ("alm", C.c_uint8), ("ano", C.c_uint8)]


class MgaAck(C.Structure):
    _fields_ = [("accepted", C.c_bool), ("info", C.c_uint8), ("msg_id", C.c_uint8)]


class CfgItem(C.Structure):
    _fields_ = [("key", C.c_uint32), ("val", C.c_uint32)]


def fletcher(data):
    a = b = 0
    for x in data:
        a = (a + x) & 0xFF
        b = (b + a) & 0xFF
    return bytes([a, b])


def frame(cls, id_, payload):
    body = bytes([cls, id_]) + struct.pack("<H", len(payload)) + payload
    return b"\xb5\x62" + body + fletcher(body)


@pytest.fixture(scope="module")
def lib():
    d = tempfile.mkdtemp()
    so = os.path.join(d, "libubx.so")
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC", "-o", so,
                    os.path.join(SRC, "gnss_ubx.c"), "-I", SRC], check=True)
    L = C.CDLL(so)
    L.ubx_utc_from_unix_ms.argtypes = [C.c_int64, C.POINTER(Utc)]
    L.ubx_utc_from_unix_ms.restype = C.c_bool
    L.ubx_build_mga_ini_time_utc.argtypes = [C.c_char_p, C.c_size_t, C.POINTER(Utc)]
    L.ubx_build_mga_ini_time_utc.restype = C.c_size_t
    L.ubx_build_mga_ini_pos_llh.argtypes = [C.c_char_p, C.c_size_t, C.c_int32, C.c_int32, C.c_int32, C.c_uint32]
    L.ubx_build_mga_ini_pos_llh.restype = C.c_size_t
    L.ubx_frame_scan.argtypes = [C.c_char_p, C.c_size_t]
    L.ubx_frame_scan.restype = C.c_int
    L.ubx_decode_mga_ack.argtypes = [C.c_char_p, C.c_uint16, C.POINTER(MgaAck)]
    L.ubx_decode_mga_ack.restype = C.c_bool
    L.ubx_decode_nav_sat.argtypes = [C.c_char_p, C.c_uint16, C.POINTER(SatSummary)]
    L.ubx_decode_nav_sat.restype = C.c_bool
    L.ubx_build_valset.argtypes = [C.c_char_p, C.c_size_t, C.c_uint8, C.POINTER(CfgItem), C.c_size_t]
    L.ubx_build_valset.restype = C.c_size_t
    return L


# -- time ---------------------------------------------------------------------

def to_ms(dt):
    return int(dt.replace(tzinfo=datetime.timezone.utc).timestamp() * 1000)


def test_unix_ms_matches_datetime_across_the_calendar(lib):
    rng = random.Random(7)
    lo, hi = to_ms(datetime.datetime(2020, 1, 1)), to_ms(datetime.datetime(2105, 12, 31))
    for _ in range(20000):
        ms = rng.randrange(lo, hi)
        u = Utc()
        assert lib.ubx_utc_from_unix_ms(ms, C.byref(u))
        ref = datetime.datetime.fromtimestamp(ms / 1000, tz=datetime.timezone.utc)
        assert (u.year, u.month, u.day, u.hour, u.min, u.sec) == \
            (ref.year, ref.month, ref.day, ref.hour, ref.minute, ref.second), ms
        assert u.ns == (ms % 1000) * 1_000_000


@pytest.mark.parametrize("stamp", [
    "2020-01-01T00:00:00", "2024-02-29T23:59:59", "2024-03-01T00:00:00", "2026-10-30T18:44:03",
    "2028-12-31T23:59:59", "2100-02-28T23:59:59", "2100-03-01T00:00:00", "2400-02-29T12:00:00",
])
def test_unix_ms_edges(lib, stamp):
    dt = datetime.datetime.fromisoformat(stamp)
    u = Utc()
    assert lib.ubx_utc_from_unix_ms(to_ms(dt) + 250, C.byref(u))
    assert (u.year, u.month, u.day, u.hour, u.min, u.sec) == (dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second)
    assert u.ns == 250_000_000


def test_a_clock_that_was_never_set_is_refused(lib):
    u = Utc()
    for ms in (0, 1, 86_400_000, to_ms(datetime.datetime(2019, 12, 31, 23, 59, 59)), -5):
        assert not lib.ubx_utc_from_unix_ms(ms, C.byref(u))


# -- messages to the receiver ---------------------------------------------------

def test_time_utc_frame_layout(lib):
    u = Utc(year=2026, month=10, day=30, hour=18, min=44, sec=3, ns=250_000_000, tacc_s=5, tacc_ns=0)
    out = C.create_string_buffer(64)
    n = lib.ubx_build_mga_ini_time_utc(out, 64, C.byref(u))
    assert n == 32
    # type, version, ref, leapSecs, year, month, day, hour, minute, second, (reserved),
    # ns, tAccS, (2 reserved), tAccNs
    payload = struct.pack("<BBBbHBBBBBxIH2xI", 0x10, 0, 0, -128, 2026, 10, 30, 18, 44, 3, 250_000_000, 5, 0)
    assert len(payload) == 24
    assert out.raw[:n] == frame(0x13, 0x40, payload)


def test_time_utc_frame_does_not_fit(lib):
    out = C.create_string_buffer(8)
    assert lib.ubx_build_mga_ini_time_utc(out, 31, C.byref(Utc(year=2026, month=1, day=1))) == 0


def test_pos_llh_frame_layout(lib):
    out = C.create_string_buffer(64)
    n = lib.ubx_build_mga_ini_pos_llh(out, 64, 513_012_345, -1_256_789, 4_520, 2_000_000)
    assert n == 28
    payload = struct.pack("<BB2xiiiI", 0x01, 0, 513_012_345, -1_256_789, 4_520, 2_000_000)
    assert out.raw[:n] == frame(0x13, 0x40, payload)


def test_ackaiding_is_one_byte_on_the_wire(lib):
    out = C.create_string_buffer(64)
    items = (CfgItem * 1)(CfgItem(0x10110025, 1))
    n = lib.ubx_build_valset(out, 64, 0x01, items, 1)
    payload = bytes([0, 0x01, 0, 0]) + bytes([0x25, 0x00, 0x11, 0x10, 0x01])
    assert out.raw[:n] == frame(0x06, 0x8A, payload)


def test_messages_decode_with_an_independent_parser(lib):
    pyubx2 = pytest.importorskip("pyubx2")
    u = Utc(year=2026, month=10, day=30, hour=18, min=44, sec=3, ns=250_000_000, tacc_s=5, tacc_ns=0)
    out = C.create_string_buffer(64)
    n = lib.ubx_build_mga_ini_time_utc(out, 64, C.byref(u))
    try:
        msg = pyubx2.UBXReader.parse(out.raw[:n], msgmode=pyubx2.SET)
    except Exception as e:                       # an older pyubx2 without the MGA-INI set
        pytest.skip("pyubx2 cannot parse MGA-INI: %s" % e)
    assert msg.identity.startswith("MGA-INI")
    n = lib.ubx_build_mga_ini_pos_llh(out, 64, 513_012_345, -1_256_789, 4_520, 2_000_000)
    msg = pyubx2.UBXReader.parse(out.raw[:n], msgmode=pyubx2.SET)
    assert msg.identity.startswith("MGA-INI")


# -- what comes back ----------------------------------------------------------------

@pytest.mark.parametrize("typ,info,mid", [(1, 0, 0x20), (0, 1, 0x13), (0, 5, 0x02), (0, 6, 0xFF)])
def test_mga_ack_decode(lib, typ, info, mid):
    pl = bytes([typ, 0, info, mid, 1, 2, 3, 4])
    a = MgaAck()
    assert lib.ubx_decode_mga_ack(pl, len(pl), C.byref(a))
    assert (a.accepted, a.info, a.msg_id) == (typ == 1, info, mid)


def test_short_mga_ack_is_refused(lib):
    assert not lib.ubx_decode_mga_ack(b"\x01\x00\x00", 3, C.byref(MgaAck()))


def sat_payload(svs):
    """svs: list of (cno, flags)"""
    out = struct.pack("<IBB2x", 1000, 1, len(svs))
    for i, (cno, flags) in enumerate(svs):
        out += struct.pack("<BBBbhhI", 0, i + 1, cno, 45, 100, 0, flags)
    return out


def test_nav_sat_counts_orbit_data_on_hand(lib):
    eph, alm, ano, used = 1 << 11, 1 << 12, 1 << 13, 1 << 3
    pl = sat_payload([
        (38, used | eph | alm),         # tracked, with an ephemeris
        (0, eph),                       # known, not heard, ephemeris
        (0, alm),                       # almanac only
        (0, alm | ano),                 # almanac and AssistNow
        (27, 0),                        # heard, nothing on hand
    ])
    s = SatSummary()
    assert lib.ubx_decode_nav_sat(pl, len(pl), C.byref(s))
    assert (s.listed, s.tracked, s.used, s.best_cno) == (5, 2, 1, 38)
    assert (s.eph, s.alm, s.ano) == (2, 3, 1)


def test_nav_sat_with_nothing_listed_has_no_orbits(lib):
    pl = sat_payload([])
    s = SatSummary()
    assert lib.ubx_decode_nav_sat(pl, len(pl), C.byref(s))
    assert (s.listed, s.eph, s.alm, s.ano) == (0, 0, 0, 0)


# -- walking a downloaded run of frames -----------------------------------------------

def ano(sv, gnss=0, day=30):
    return frame(0x13, 0x20, struct.pack("<BBBBBBBB64s4s", 0, 0, sv, gnss, 26, 10, day, 0, bytes(range(64)), bytes(4)))


def test_scan_complete_frame(lib):
    f = ano(7)
    assert len(f) == 84
    assert lib.ubx_frame_scan(f, len(f)) == 84
    assert lib.ubx_frame_scan(f + b"extra", len(f) + 5) == 84


def test_scan_wants_more_bytes_until_the_frame_is_whole(lib):
    f = ano(7)
    assert lib.ubx_frame_scan(b"", 0) == 0
    for cut in (1, 2, 5, 6, 40, 83):
        assert lib.ubx_frame_scan(f[:cut], cut) == 0, cut


def test_scan_rejects_what_is_not_a_frame(lib):
    f = bytearray(ano(7))
    assert lib.ubx_frame_scan(b"\x00" + bytes(f), 85) == -1
    assert lib.ubx_frame_scan(b"\xb5\x00rest", 6) == -1
    bad = bytearray(f)
    bad[40] ^= 0x01
    assert lib.ubx_frame_scan(bytes(bad), len(bad)) == -1
    huge = b"\xb5\x62\x13\x20" + struct.pack("<H", 5000) + bytes(10)
    assert lib.ubx_frame_scan(huge, len(huge)) == -1


def walk(lib, data):
    """The loop assistnow.c uses: frames out, junk skipped a byte at a time."""
    frames, off = [], 0
    while off < len(data):
        n = lib.ubx_frame_scan(data[off:], len(data) - off)
        if n > 0:
            frames.append(data[off:off + n])
            off += n
        elif n == 0:
            break                       # truncated tail
        else:
            off += 1
    return frames, off


def test_walk_finds_every_frame_through_junk_and_a_bad_one(lib):
    good = [ano(sv) for sv in range(1, 9)] + [frame(0x13, 0x00, bytes(36)), frame(0x13, 0x02, bytes(32))]
    damaged = bytearray(ano(99))
    damaged[20] ^= 0xFF
    stream = good[0] + b"\x00\xb5\x62junk" + good[1] + bytes(damaged) + b"".join(good[2:])
    frames, _ = walk(lib, stream)
    assert frames == good


def test_walk_stops_at_a_truncated_tail(lib):
    stream = ano(1) + ano(2) + ano(3)[:50]
    frames, off = walk(lib, stream)
    assert len(frames) == 2
    assert off == 168


def test_walk_survives_random_input(lib):
    rng = random.Random(3)
    for _ in range(300):
        data = bytes(rng.randrange(256) for _ in range(rng.randrange(0, 600)))
        frames, off = walk(lib, data)
        assert off <= len(data)
        for f in frames:
            assert f[:2] == b"\xb5\x62" and f[-2:] == fletcher(f[2:-2])
