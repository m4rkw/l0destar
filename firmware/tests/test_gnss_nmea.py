"""Host tests for the NMEA fallback of the external GNSS driver: src/gnss_nmea.c
and the part of src/gnss_ubx.c's stream parser that keeps NMEA sentences.

Both are built as a shared library with the host compiler and driven through
ctypes, through a small shim that does what the driver's drain() does: bytes
into the parser, sentences from the parser into the decoder.  The real driver
needs Zephyr, so what is not here (mode switching, publishing) is checked on
the board.

    uv run --python 3.12 --with pytest --with pynmea2 pytest -q firmware/tests/test_gnss_nmea.py

pynmea2 is an independent decoder used as a cross-check; without it those
tests are skipped.
"""
import ctypes as C
import os
import random
import subprocess
import tempfile
import textwrap

import pytest

SRC = os.environ.get("GNSS_SRC", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src"))

SHIM = textwrap.dedent(r"""
    #include <string.h>
    #include "gnss_ubx.h"
    #include "gnss_nmea.h"

    static struct ubx_parser P;
    static struct nmea_decoder D;
    static long long R[32];
    static char TIME[40], LAT[16], LON[16];

    enum { RMCS, EPOCHS, FIXES, SATS_UPD, TRACKED, BEST, LAT_E7, LON_E7, SPEED_MMS, HEAD_E5,
           HEIGHT_MM, HMSL_MM, NUM_SV, FIX_TYPE, HDOP_E2, YEAR, UBX_FRAMES, NMEA_SEEN, BAD_CKS,
           LAST_FIX_VALID };

    void shim_reset(void)
    {
        memset(&P, 0, sizeof(P));
        memset(R, 0, sizeof(R));
        TIME[0] = LAT[0] = LON[0] = 0;
        ubx_parser_reset(&P);
        nmea_reset(&D);
    }

    void shim_feed(const unsigned char *b, size_t n)
    {
        for (size_t i = 0; i < n; i++) {
            enum ubx_event ev = ubx_parse_byte(&P, b[i]);

            if (ev == UBX_EV_FRAME) {
                R[UBX_FRAMES]++;
            }
            if (ev != UBX_EV_NMEA) {
                continue;
            }
            R[NMEA_SEEN]++;

            unsigned f = nmea_feed(&D, (const char *)P.payload, P.len);

            if (f & NMEA_F_RMC) {
                R[RMCS]++;
            }
            if (f & NMEA_F_SATS) {
                R[SATS_UPD]++;
                R[TRACKED] = D.sats.tracked;
                R[BEST] = D.sats.best_cno;
            }
            if (f & NMEA_F_EPOCH) {
                struct ubx_pvt v;
                uint16_t h;

                R[EPOCHS]++;
                R[LAST_FIX_VALID] = 0;
                if (nmea_epoch_pvt(&D, &v, &h)) {
                    R[FIXES]++;
                    R[LAST_FIX_VALID] = 1;
                    R[LAT_E7] = v.lat_e7;
                    R[LON_E7] = v.lon_e7;
                    R[SPEED_MMS] = v.gspeed_mms;
                    R[HEAD_E5] = v.head_mot_e5;
                    R[HEIGHT_MM] = v.height_mm;
                    R[HMSL_MM] = v.hmsl_mm;
                    R[NUM_SV] = v.num_sv;
                    R[FIX_TYPE] = v.fix_type;
                    R[HDOP_E2] = h;
                    R[YEAR] = v.year;
                    ubx_fmt_time(&v, TIME, sizeof(TIME));
                    ubx_fmt_deg6(v.lat_e7, LAT, sizeof(LAT));
                    ubx_fmt_deg6(v.lon_e7, LON, sizeof(LON));
                }
            }
        }
        R[BAD_CKS] = P.bad_cks;
    }

    long long shim_get(int i) { return R[i]; }
    const char *shim_time(void) { return TIME; }
    const char *shim_lat(void) { return LAT; }
    const char *shim_lon(void) { return LON; }
""")

FUZZ = textwrap.dedent(r"""
    #include <stdio.h>
    #include <stdlib.h>
    #include <string.h>
    #include "gnss_ubx.h"
    #include "gnss_nmea.h"

    static uint32_t rng = 12345;
    static uint32_t next(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

    static const char *seeds[] = {
        "GNRMC,123519.00,A,5130.12345,N,00012.56789,W,022.4,084.4,301026,,,A,V",
        "GNGGA,123519.00,5130.12345,N,00012.56789,W,1,09,0.84,45.2,M,47.0,M,,",
        "GPGSV,2,1,07,02,40,083,41,05,18,304,30,07,22,201,,09,,,38,1",
        "GNRMC,,V,,,,,,,,,,N,V", "GNGGA,,,,,,0,00,99.99,,,,,,", "GNGSA,A,3,02,05,07,,,,,,,,,,1.2,0.8,0.9,1",
    };

    static void sentence(char *out, const char *body)
    {
        unsigned cs = 0;
        for (const char *c = body; *c; c++) cs ^= (unsigned char)*c;
        snprintf(out, 200, "$%s*%02X\r\n", body, cs);
    }

    int main(void)
    {
        static struct ubx_parser p;
        static struct nmea_decoder d;
        unsigned long events = 0;

        ubx_parser_reset(&p);
        nmea_reset(&d);
        for (int iter = 0; iter < 300000; iter++) {
            char buf[256], body[128];
            size_t n;

            if (next() % 3 == 0) {                 /* noise */
                n = next() % 200;
                for (size_t i = 0; i < n; i++) buf[i] = (char)next();
            } else {                               /* a mutated sentence, checksum made right */
                strcpy(body, seeds[next() % (sizeof(seeds) / sizeof(seeds[0]))]);
                size_t bl = strlen(body);
                for (unsigned m = next() % 6; m; m--) {
                    unsigned k = next() % 4;
                    size_t at = next() % (bl ? bl : 1);
                    if (k == 0 && bl) body[at] = (char)(0x20 + next() % 0x5f);
                    else if (k == 1 && bl) { memmove(body + at, body + at + 1, bl - at); bl--; }
                    else if (k == 2 && bl < 120) { memmove(body + at + 1, body + at, bl - at + 1); body[at] = ",.-0123456789ANSEW"[next() % 18]; bl++; }
                    else if (k == 3) { size_t cut = next() % (bl + 1); body[cut] = 0; bl = cut; }
                }
                body[bl] = 0;
                sentence(buf, body);
                n = strlen(buf);
            }
            for (size_t i = 0; i < n; i++) {
                enum ubx_event ev = ubx_parse_byte(&p, (uint8_t)buf[i]);
                if (ev == UBX_EV_NMEA) {
                    unsigned f = nmea_feed(&d, (const char *)p.payload, p.len);
                    struct ubx_pvt v;
                    uint16_t h;
                    if (f & NMEA_F_EPOCH) nmea_epoch_pvt(&d, &v, &h);
                    events++;
                }
            }
        }
        printf("ok %lu sentences\n", events);
        return 0;
    }
""")


@pytest.fixture(scope="module")
def build():
    d = tempfile.mkdtemp()
    with open(os.path.join(d, "shim.c"), "w") as f:
        f.write(SHIM)
    with open(os.path.join(d, "fuzz.c"), "w") as f:
        f.write(FUZZ)
    srcs = [os.path.join(SRC, "gnss_nmea.c"), os.path.join(SRC, "gnss_ubx.c")]
    warn = ["-std=c11", "-Wall", "-Wextra", "-Werror"]
    lib = os.path.join(d, "libnmea.so")
    subprocess.run(["cc", *warn, "-shared", "-fPIC", "-o", lib, os.path.join(d, "shim.c"), *srcs,
                    "-I", SRC], check=True)
    return d, lib


@pytest.fixture()
def lib(build):
    L = C.CDLL(build[1])
    L.shim_feed.argtypes = [C.c_char_p, C.c_size_t]
    L.shim_get.argtypes = [C.c_int]
    L.shim_get.restype = C.c_longlong
    for name in ("shim_time", "shim_lat", "shim_lon"):
        getattr(L, name).restype = C.c_char_p
    L.shim_reset()
    return L


NAMES = ("RMCS EPOCHS FIXES SATS_UPD TRACKED BEST LAT_E7 LON_E7 SPEED_MMS HEAD_E5 HEIGHT_MM HMSL_MM "
         "NUM_SV FIX_TYPE HDOP_E2 YEAR UBX_FRAMES NMEA_SEEN BAD_CKS LAST_FIX_VALID").split()
IDX = {n: i for i, n in enumerate(NAMES)}


def get(lib, name):
    return lib.shim_get(IDX[name])


def nmea(body, ck=None):
    cs = 0
    for c in body.encode():
        cs ^= c
    return f"${body}*{cs if ck is None else ck:02X}\r\n".encode()


def feed(lib, *sentences):
    for s in sentences:
        lib.shim_feed(s, len(s))


# What the board's receiver sent on the bench, 2026-10-30 (no fix, two GPS
# satellites heard at 31 and 38 dBHz), as captured.
REAL_BURST = [
    b"$GNRMC,,V,,,,,,,,,,N,V*37\r\n",
    b"$GNVTG,,,,,,,,,N*2E\r\n",
    b"$GNGGA,,,,,,0,00,99.99,,,,,,*56\r\n",
    b"$GNGSA,A,1,,,,,,,,,,,,,99.99,99.99,99.99,1*33\r\n",
    b"$GNGSA,A,1,,,,,,,,,,,,,99.99,99.99,99.99,3*31\r\n",
    b"$GNGSA,A,1,,,,,,,,,,,,,99.99,99.99,99.99,4*36\r\n",
    b"$GNGSA,A,1,,,,,,,,,,,,,99.99,99.99,99.99,5*37\r\n",
    b"$GPGSV,1,1,02,07,,,31,09,,,38,1*61\r\n",
    b"$GAGSV,1,1,00,0*74\r\n",
    b"$GBGSV,1,1,00,0*77\r\n",
    b"$GQGSV,1,1,00,0*64\r\n",
    b"$GNGLL,,,,,,V,N*7A\r\n",
]

RMC = "GNRMC,123519.00,A,5130.12345,N,00012.56789,W,022.4,084.4,301026,,,A,V"
GGA = "GNGGA,123519.00,5130.12345,N,00012.56789,W,1,09,0.84,45.2,M,47.0,M,,"


def test_real_receiver_burst_has_no_fix_and_counts_satellites(lib):
    for s in REAL_BURST:
        feed(lib, s)
    assert get(lib, "NMEA_SEEN") == len(REAL_BURST)
    assert get(lib, "RMCS") == 1
    assert get(lib, "EPOCHS") == 1                    # empty RMC and GGA pair as an epoch...
    assert get(lib, "FIXES") == 0                     # ...with no fix in it
    assert get(lib, "SATS_UPD") == 0                  # final only once the next epoch begins
    for s in REAL_BURST:
        feed(lib, s)
    assert get(lib, "SATS_UPD") == 1
    assert get(lib, "TRACKED") == 2
    assert get(lib, "BEST") == 38
    assert get(lib, "BAD_CKS") == 0


def test_fix_epoch_values(lib):
    feed(lib, nmea(RMC), nmea(GGA))
    assert get(lib, "FIXES") == 1
    assert get(lib, "LAT_E7") == 515020575            # 51 deg 30.12345 min
    assert get(lib, "LON_E7") == -2094648             # 0 deg 12.56789 min west
    assert lib.shim_lat() == b"51.502058"
    assert lib.shim_lon() == b"-0.209465"
    assert get(lib, "SPEED_MMS") == 11523             # 22.4 kn = 41.485 km/h
    assert get(lib, "HEAD_E5") == 8440000
    assert get(lib, "HMSL_MM") == 45200
    assert get(lib, "HEIGHT_MM") == 92200             # + 47.0 m geoid separation: ellipsoid height
    assert get(lib, "NUM_SV") == 9
    assert get(lib, "FIX_TYPE") == 3
    assert get(lib, "HDOP_E2") == 84
    assert get(lib, "YEAR") == 2026
    assert lib.shim_time() == b"30/10/26,12:35:19.000000+00"


def test_southern_eastern_hemispheres_and_fractional_seconds(lib):
    feed(lib,
         nmea("GNRMC,010203.456789,A,3351.50000,S,15112.00000,E,000.0,,010199,,,A,V"),
         nmea("GNGGA,010203.456789,3351.50000,S,15112.00000,E,1,05,1.50,-12.5,M,0.0,M,,"))
    assert get(lib, "LAT_E7") == -338583333
    assert get(lib, "LON_E7") == 1512000000
    assert get(lib, "HMSL_MM") == -12500
    assert get(lib, "HEIGHT_MM") == -12500
    assert get(lib, "YEAR") == 2099                   # the century is this one
    assert lib.shim_time() == b"01/01/99,01:02:03.456789+00"


def test_no_fix_when_status_is_void_or_quality_zero_or_no_satellites(lib):
    feed(lib, nmea(RMC.replace(",A,5130", ",V,5130")), nmea(GGA))
    assert get(lib, "EPOCHS") == 1 and get(lib, "FIXES") == 0
    feed(lib, nmea(RMC), nmea(GGA.replace(",W,1,09,", ",W,0,09,")))
    assert get(lib, "EPOCHS") == 2 and get(lib, "FIXES") == 0
    feed(lib, nmea(RMC), nmea(GGA.replace(",W,1,09,", ",W,1,00,")))
    assert get(lib, "EPOCHS") == 3 and get(lib, "FIXES") == 0
    feed(lib, nmea(RMC), nmea(GGA))
    assert get(lib, "FIXES") == 1


def test_mode_indicator_n_is_no_fix(lib):
    feed(lib, nmea(RMC.replace(",A,V", ",N,V")), nmea(GGA))
    assert get(lib, "FIXES") == 0


def test_gga_before_rmc_pairs_too(lib):
    feed(lib, nmea(GGA), nmea(RMC))
    assert get(lib, "EPOCHS") == 1 and get(lib, "FIXES") == 1


def test_epochs_pair_only_by_time(lib):
    feed(lib, nmea(RMC), nmea(GGA.replace("123519.00", "123520.00")))
    assert get(lib, "EPOCHS") == 0                    # a GGA from another second is not this RMC's
    feed(lib, nmea(RMC.replace("123519.00", "123520.00")))
    assert get(lib, "EPOCHS") == 1                    # but it pairs with the RMC that matches


def test_lost_gga_does_not_pair_old_rmc_with_next_gga(lib):
    feed(lib, nmea(RMC))                              # its GGA never arrives
    feed(lib, nmea(RMC.replace("123519.00", "123520.00")), nmea(GGA.replace("123519.00", "123520.00")))
    assert get(lib, "EPOCHS") == 1 and get(lib, "FIXES") == 1
    assert lib.shim_time() == b"30/10/26,12:35:20.000000+00"


def test_corrupt_checksum_is_dropped_before_the_decoder(lib):
    feed(lib, nmea(RMC, ck=0), nmea(GGA))
    assert get(lib, "BAD_CKS") == 0                   # NMEA failures are not counted as UBX ones
    assert get(lib, "RMCS") == 0 and get(lib, "FIXES") == 0


def test_course_is_carried_when_the_receiver_leaves_it_empty(lib):
    feed(lib, nmea(RMC), nmea(GGA))
    assert get(lib, "HEAD_E5") == 8440000
    feed(lib, nmea(RMC.replace(",084.4,", ",,").replace("123519.00", "123520.00")),
         nmea(GGA.replace("123519.00", "123520.00")))
    assert get(lib, "FIXES") == 2
    assert get(lib, "HEAD_E5") == 8440000


def test_course_unknown_is_zero_until_first_given(lib):
    feed(lib, nmea(RMC.replace(",084.4,", ",,")), nmea(GGA))
    assert get(lib, "FIXES") == 1 and get(lib, "HEAD_E5") == 0


def test_satellites_over_several_talkers(lib):
    burst = [nmea(RMC), nmea(GGA),
             nmea("GPGSV,2,1,05,02,40,083,41,05,18,304,30,07,22,201,,09,10,11,25,1"),
             nmea("GPGSV,2,2,05,13,,,19,1"),
             nmea("GLGSV,1,1,02,70,50,100,44,71,,,,1"),
             nmea("GAGSV,1,1,00,0")]
    feed(lib, *burst)
    assert get(lib, "SATS_UPD") == 0
    feed(lib, nmea(RMC.replace("123519.00", "123520.00")))
    assert get(lib, "SATS_UPD") == 1
    assert get(lib, "TRACKED") == 5                   # 41, 30, 25, 19 and 44 dBHz; 07 and 71 are not heard
    assert get(lib, "BEST") == 44


@pytest.mark.parametrize("lat,hemi,ok", [
    ("9000.00000", "N", True), ("9000.00001", "N", False), ("5160.00000", "N", False),
    ("5159.99999", "N", True), ("0000.00000", "N", True), ("", "N", False), ("5130.1", "X", False),
    ("51a0.1", "N", False), ("5130.1.2", "N", False), ("-5130.1", "N", False),
])
def test_latitude_limits(lib, lat, hemi, ok):
    rmc = f"GNRMC,123519.00,A,{lat},{hemi},00012.56789,W,022.4,084.4,301026,,,A,V"
    feed(lib, nmea(rmc), nmea(GGA))
    assert get(lib, "FIXES") == (1 if ok else 0)


def test_bad_time_and_date_do_not_stop_a_fix_but_leave_the_time_empty(lib):
    feed(lib, nmea(RMC.replace("301026", "")), nmea(GGA))
    assert get(lib, "FIXES") == 1
    assert lib.shim_time() == b""


def test_ubx_frames_still_parse_between_sentences(lib):
    poll = bytes.fromhex("b5620103000004 0d".replace(" ", ""))      # NAV-STATUS poll: a valid UBX frame
    feed(lib, nmea(RMC), poll, nmea(GGA), poll)
    assert get(lib, "UBX_FRAMES") == 2
    assert get(lib, "FIXES") == 1


def test_overlong_and_foreign_sentences_are_ignored(lib):
    feed(lib, nmea("GNRMC," + "1," * 80))
    feed(lib, nmea("PUBX,00,081350.00,4717.113210,N,00833.915187,E,546.589,G3,2.1,2.0,0.007,77.52,0.007,,0.92,1.19,0.77,9,0,0"))
    feed(lib, nmea("GNTXT,01,01,02,u-blox AG - www.u-blox.com"))
    assert get(lib, "EPOCHS") == 0 and get(lib, "FIXES") == 0


def test_fuzz_under_sanitizers(build):
    d = build[0]
    exe = os.path.join(d, "fuzz")
    r = subprocess.run(["cc", "-std=c11", "-g", "-O1", "-fsanitize=address,undefined",
                        "-fno-sanitize-recover=undefined", "-o", exe, os.path.join(d, "fuzz.c"),
                        os.path.join(SRC, "gnss_nmea.c"), os.path.join(SRC, "gnss_ubx.c"), "-I", SRC],
                       capture_output=True, text=True)
    if r.returncode != 0:
        pytest.skip("compiler has no sanitizers: " + r.stderr[-200:])
    run = subprocess.run([exe], capture_output=True, text=True, timeout=300)
    assert run.returncode == 0, run.stderr[-1500:]
    assert run.stdout.startswith("ok")


# -- cross-check against an independent decoder -----------------------------------

try:
    import pynmea2
except ImportError:                                   # only the cross-check needs it
    pynmea2 = None


def _cases():
    rnd = random.Random(7)
    out = []
    for _ in range(60):
        lat = rnd.uniform(-89.9, 89.9)
        lon = rnd.uniform(-179.9, 179.9)
        knots = rnd.uniform(0, 120)
        course = rnd.uniform(0, 359.9)
        alt = rnd.uniform(-50, 3000)
        sep = rnd.uniform(-60, 60)
        hdop = rnd.uniform(0.5, 9.9)
        sats = rnd.randint(4, 24)
        out.append((lat, lon, knots, course, alt, sep, hdop, sats,
                    rnd.randint(0, 23), rnd.randint(0, 59), rnd.randint(0, 59), rnd.randint(0, 99)))
    return out


def _fmt_angle(deg, width):
    a = abs(deg)
    d = int(a)
    m = (a - d) * 60
    return f"{d:0{width}d}{m:09.6f}"


@pytest.mark.skipif(pynmea2 is None, reason="pynmea2 not installed (uv run --with pynmea2)")
@pytest.mark.parametrize("case", _cases())
def test_matches_pynmea2(lib, case):
    lat, lon, kn, crs, alt, sep, hdop, sats, hh, mm, ss, cs = case
    utc = f"{hh:02d}{mm:02d}{ss:02d}.{cs:02d}"
    ns = "N" if lat >= 0 else "S"
    ew = "E" if lon >= 0 else "W"
    rmc = f"GNRMC,{utc},A,{_fmt_angle(lat, 2)},{ns},{_fmt_angle(lon, 3)},{ew},{kn:.3f},{crs:.2f},150826,,,A,V"
    gga = (f"GNGGA,{utc},{_fmt_angle(lat, 2)},{ns},{_fmt_angle(lon, 3)},{ew},1,{sats:02d},{hdop:.2f},"
           f"{alt:.1f},M,{sep:.1f},M,,")
    feed(lib, nmea(rmc), nmea(gga))
    assert get(lib, "FIXES") == 1

    r = pynmea2.parse(nmea(rmc).decode().strip())
    g = pynmea2.parse(nmea(gga).decode().strip())
    assert get(lib, "LAT_E7") / 1e7 == pytest.approx(r.latitude, abs=2e-7)
    assert get(lib, "LON_E7") / 1e7 == pytest.approx(r.longitude, abs=2e-7)
    assert get(lib, "SPEED_MMS") / 1000 * 3.6 / 1.852 == pytest.approx(r.spd_over_grnd, abs=0.004)
    assert get(lib, "HEAD_E5") / 1e5 == pytest.approx(r.true_course, abs=0.006)
    assert get(lib, "HMSL_MM") / 1000 == pytest.approx(float(g.altitude), abs=0.001)
    assert get(lib, "HEIGHT_MM") / 1000 == pytest.approx(float(g.altitude) + float(g.geo_sep), abs=0.002)
    assert get(lib, "HDOP_E2") / 100 == pytest.approx(float(g.horizontal_dil), abs=0.0051)
    assert get(lib, "NUM_SV") == int(g.num_sats)
    assert lib.shim_time() == (f"15/08/26,{hh:02d}:{mm:02d}:{ss:02d}.{cs:02d}0000+00").encode()
