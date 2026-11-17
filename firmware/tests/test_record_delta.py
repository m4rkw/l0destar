"""Host tests for src/record_delta.h: the drive stream's delta records, as the
firmware writes them.

The header is compiled with the host compiler and held to the specification in
server/tracker/deltarec.py: its output has to be the reference encoder's, byte
for byte, and the server's decoder has to read it back as the records it was
made from.

    uv run --python 3.12 --with pytest pytest -q firmware/tests/test_record_delta.py
"""
import ctypes as C
import importlib.util
import os
import random
import subprocess
import tempfile

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.environ.get("GNSS_SRC", os.path.join(HERE, "..", "src"))
DELTAREC = os.environ.get("DELTAREC", os.path.join(HERE, "..", "..", "server", "tracker", "deltarec.py"))

WRAPPER = r"""
#include "record_delta.h"
int enc(const char *prev, const char *cur, char *out, int size)
{ return rdelta_encode(prev, strlen(prev), cur, strlen(cur), out, (size_t)size); }
int enc_batch(const char *recs, char *out, int size)
{ return rdelta_encode_batch(recs, strlen(recs), out, (size_t)size); }
int sticky(const char *k) { return rd_sticky(k, strlen(k)); }
int opaque(const char *g) { return rd_opaque(g, strlen(g)); }
int blankable(int i) { return rd_blankable(i); }
int stack_bytes(void) { return (int)(2 * sizeof(struct rd_rec) + RD_WAS * sizeof(struct rd_was)); }
"""

spec = importlib.util.spec_from_file_location("deltarec", DELTAREC)
deltarec = importlib.util.module_from_spec(spec)
spec.loader.exec_module(deltarec)

A = ('16/11/26,20:01:23.456789+00,51.401515,-0.195071,38.88,62.00,227.45,110,14,13.86,0,2015,0,'
     'ax=819;ay=573;az=-158,gx=-18;gy=77;gz=-144,it=18.3,mt=23.0,vs=4.22,'
     'rsrp=-65;snr=19;band=20,orpm=1375;ospd=22;ocl=85,up=2015,rid=1234567')
B = ('16/11/26,20:01:23.711234+00,51.401530,-0.195090,39.10,62.00,227.50,110,14,13.86,0,2015,0,'
     'ax=812;ay=570;az=-160,gx=-20;gy=75;gz=-140,it=18.3,mt=23.0,vs=4.22,'
     'rsrp=-65;snr=19;band=20,orpm=1375;ospd=22;ocl=85,up=2015,rid=1234568')


@pytest.fixture(scope="module")
def lib():
    d = tempfile.mkdtemp()
    c = os.path.join(d, "w.c")
    so = os.path.join(d, "libw.so")
    with open(c, "w") as f:
        f.write(WRAPPER)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-shared", "-fPIC",
                    "-o", so, c, "-I", SRC], check=True)
    lib = C.CDLL(so)
    lib.enc.argtypes = [C.c_char_p, C.c_char_p, C.c_void_p, C.c_int]
    lib.enc_batch.argtypes = [C.c_char_p, C.c_void_p, C.c_int]
    lib.sticky.argtypes = [C.c_char_p]
    lib.opaque.argtypes = [C.c_char_p]
    lib.blankable.argtypes = [C.c_int]
    return lib


GUARD = b"\xA5" * 32


def run(fn, size, *args):
    """Calls fn(*args, out, size) with a guard behind the buffer: returns
    (result, text); the guard being touched is a failure."""
    buf = C.create_string_buffer(GUARD * 0 + b"\x00" * size + GUARD, size + len(GUARD))
    n = fn(*args, buf, size)
    assert buf.raw[size:] == GUARD, "wrote past the end of the buffer"
    return n, (buf.value.decode() if n > 0 else "")


def enc(lib, prev, cur, size=2048):
    n, text = run(lib.enc, size, prev.encode(), cur.encode())
    return n, text


def enc_batch(lib, lines, size=4096):
    return run(lib.enc_batch, size, "\n".join(lines).encode())


# -- one record against the one before ------------------------------------------------------------

def test_the_specification_example_comes_out_the_same(lib):
    n, text = enc(lib, A, B)
    assert text == deltarec.encode(A, B) and n == len(text) < len(B)


def test_it_is_the_reference_encoder_s_output_not_just_shorter(lib):
    for prev, cur in ((A, B), (B, A)):
        want = deltarec.encode(prev, cur)
        n, text = enc(lib, prev, cur)
        assert (text if n > 0 else cur) == want


def test_a_record_that_lost_a_sticky_reading_is_left_whole(lib):
    b = B.replace('rsrp=-65;snr=19;band=20,', '')
    assert enc(lib, A, b)[0] == 0 and deltarec.encode(A, b) == b


def test_a_record_with_nothing_to_leave_out_is_left_whole(lib):
    b = ('16/11/26,20:01:23.711234+00,51.401530,-0.195090,39.10,63.00,228.50,120,15,13.90,0,2016,0,'
         'ax=812;ay=570;az=-160,gx=-20;gy=75;gz=-140,it=18.4,mt=23.1,vs=4.23,'
         'rsrp=-66;snr=18;band=3,orpm=1400;ospd=23;ocl=86,up=2016,rid=1234568')
    assert enc(lib, A, b)[0] == 0


def test_a_delta_is_never_encoded_again_nor_encoded_against(lib):
    delta = deltarec.encode(A, B)
    assert enc(lib, A, delta)[0] == 0
    assert enc(lib, delta, B)[0] == 0


def test_something_that_is_not_a_record_is_left_alone(lib):
    for odd in ('', 'L,1200,W,main: something', '16/11/26,20:01:23.4+00,51.4,-0.1', ','):
        assert enc(lib, A, odd)[0] == 0
        assert enc(lib, odd, B)[0] == 0


def test_position_ignition_and_power_flag_are_never_blanked(lib):
    still = B.replace('51.401530', '51.401515').replace('-0.195090', '-0.195071')
    _, text = enc(lib, A, still)
    fields = text.split(',')
    assert fields[2:4] == ['51.401515', '-0.195071'] and fields[10] == '0' and fields[12] == '0'


def test_the_blankable_positions_are_the_specification_s(lib):
    assert [i for i in range(12) if lib.blankable(i)] == list(deltarec.BLANKED)


def test_a_changed_sticky_extra_is_sent_and_the_others_are_not(lib):
    b = B.replace('orpm=1375', 'orpm=1410')
    _, text = enc(lib, A, b)
    assert 'orpm=1410' in text and 'ospd' not in text and text == deltarec.encode(A, b)


def test_compound_extras_go_through_whole(lib):
    a = A + ',dbg=a:1;vs=4.22'
    b = B + ',dbg=a:1;vs=4.22,tb=xtt:8;xpi:900'
    _, text = enc(lib, a, b)
    assert 'dbg=a:1;vs=4.22' in text and 'tb=xtt:8;xpi:900' in text
    assert text == deltarec.encode(a, b)


def test_other_obd_pids_are_sticky(lib):
    a, b = A + ',o0B=95', B + ',o0B=95'
    _, text = enc(lib, a, b)
    assert 'o0B' not in text and text == deltarec.encode(a, b)


# -- awkward records, held to the reference -------------------------------------------------------

def same_as_reference(lib, prev, cur):
    n, text = enc(lib, prev, cur)
    assert (text if n > 0 else cur) == deltarec.encode(prev, cur)
    return n, text


def test_a_group_with_a_trailing_semicolon_is_kept_as_it_is(lib):
    a, b = A + ',mt=23.0;', B + ',mt=23.0;'
    same_as_reference(lib, a, b)
    same_as_reference(lib, A, B.replace('vs=4.22,', 'vs=4.22;,'))


def test_empty_groups_and_empty_pairs_go_through(lib):
    same_as_reference(lib, A + ',', B + ',')
    same_as_reference(lib, A, B + ',,')
    same_as_reference(lib, A, B.replace('it=18.3,', 'it=18.3;;mt=23.0,'))


def test_two_readings_of_a_key_in_the_record_before_count_the_last(lib):
    a = A.replace('mt=23.0', 'mt=23.0,mt=23.1')
    b = B.replace('mt=23.0', 'mt=23.1')
    n, text = same_as_reference(lib, a, b)
    assert n > 0 and 'mt=' not in text


def test_a_record_that_already_says_it_is_a_delta_is_left_alone_whatever_it_holds(lib):
    # every sticky key present, so only the marker says not to encode it again
    assert enc(lib, A, B + ',d=1')[0] == 0
    assert enc(lib, B + ',d=1', A)[0] == 0


def test_a_value_that_is_a_prefix_of_the_last_is_not_the_same(lib):
    b = B.replace('62.00', '62.0')
    n, text = same_as_reference(lib, A, b)
    assert text.split(',')[5] == '62.0'
    c = B.replace('orpm=1375', 'orpm=137')
    n, text = same_as_reference(lib, A, c)
    assert 'orpm=137' in text
    d = A.replace('orpm=1375', 'orpm=137')
    n, text = same_as_reference(lib, d, B)
    assert 'orpm=1375' in text


def test_a_delta_exactly_as_long_as_the_record_is_not_sent(lib):
    # four characters saved by a blank field is what the marker costs
    a = A.replace(',110,', ',1000,')
    b = ('16/11/26,20:01:23.711234+00,51.401530,-0.195090,39.10,63.00,228.50,1000,15,13.90,0,2016,0,'
         'ax=812;ay=570;az=-160,gx=-20;gy=75;gz=-140,it=18.4,mt=23.1,vs=4.23,'
         'rsrp=-66;snr=18;band=3,orpm=1400;ospd=23;ocl=86,up=2016,rid=1234568')
    assert deltarec.encode(a, b) == b
    assert enc(lib, a, b)[0] == 0


def test_a_record_one_field_short_is_not_one(lib):
    short = '16/11/26,20:01:23.456789+00,51.401515,-0.195071,38.88,62.00,227.45,110,14,13.86,0,2015'
    assert enc(lib, A, short)[0] == 0
    assert enc(lib, short, B)[0] == 0
    same_as_reference(lib, short + ',', B)         # an empty last field is a field, as the reference reads it


# -- the tables agree with the server's -------------------------------------------------------------

def test_the_sticky_keys_are_the_servers(lib):
    for key in deltarec.STICKY:
        assert lib.sticky(key.encode()) == 1, key
    for key in ('o0B', 'o5E', 'oFF', 'o00'):
        assert lib.sticky(key.encode()) == 1, key
    for key in ('ax', 'ay', 'az', 'gx', 'rid', 'age', 'fl', 'cl', 'mcc', 'cid', 'rat', 'dr', 'tm',
                'o0b', 'oGG', 'o0', 'o0B0', 'ITT', 'i', ''):
        assert lib.sticky(key.encode()) == 0, key
        assert not deltarec.is_sticky(key), key


def test_the_opaque_extras_are_the_servers(lib):
    for head in deltarec.OPAQUE:
        assert lib.opaque((head + 'x').encode()) == 1, head
    for group in ('ax=1', 'rsrp=-65;snr=19', 'it=18.3', 'dbgx=1', 'db=1'):
        assert lib.opaque(group.encode()) == 0, group


# -- limits -----------------------------------------------------------------------------------------

def test_a_buffer_too_small_gives_minus_one_and_writes_nothing_past_it(lib):
    for size in (1, 2, 10, 60, 100):
        n, _ = enc(lib, A, B, size=size)
        assert n == -1 or n == 0, size
    assert enc(lib, A, B, size=len(deltarec.encode(A, B)))[0] == -1       # no room for the NUL
    assert enc(lib, A, B, size=len(deltarec.encode(A, B)) + 1)[0] == len(deltarec.encode(A, B))


def test_too_many_extras_leaves_the_record_whole(lib):
    many = ''.join(',k%d=%d' % (i, i) for i in range(70))
    assert enc(lib, A + many, B + many)[0] == 0


def test_too_many_sticky_readings_leaves_the_record_whole(lib):
    many = ','.join('o%02X=%d' % (i, i) for i in range(60))
    assert enc(lib, A + ',' + many, B + ',' + many)[0] == 0


def test_the_work_space_is_small_enough_for_the_main_thread(lib):
    assert lib.stack_bytes() < 2200


# -- a datagram ------------------------------------------------------------------------------------

def test_a_batch_is_the_reference_encoders_datagram(lib):
    lines = [A, B, B.replace('.711234', '.966789').replace('rid=1234568', 'rid=1234569'),
             B.replace('.711234', '.216789').replace('rid=1234568', 'rid=1234570')]
    n, text = enc_batch(lib, lines)
    assert text.split('\n') == deltarec.encode_lines(lines) and n == len(text)
    assert not deltarec.is_marked(text.split('\n')[0])
    assert all(deltarec.is_marked(x) for x in text.split('\n')[1:])


def test_one_record_goes_as_it_is(lib):
    assert enc_batch(lib, [A]) == (len(A), A)


def test_a_batch_that_does_not_fit_gives_minus_one(lib):
    assert enc_batch(lib, [A, B], size=len(A))[0] == -1
    assert enc_batch(lib, [A, B], size=len(A) + 5)[0] == -1


# -- random streams, in both directions --------------------------------------------------------------

def _stream(rng, n):
    lines = []
    alt, hdop, sat, bat, up = 62.0, 110, 14, 13.86, 2015
    imu_t, mt, vs = 18.3, 23.0, 4.22
    rpm, spd_e, cool = 1375, 22, 85
    sig = (-65, 19, 20)
    for i in range(n):
        if rng.random() < 0.1:
            alt += rng.choice((-0.4, 0.4))
        if rng.random() < 0.2:
            hdop += rng.choice((-10, 10))
        if rng.random() < 0.4:
            sat += rng.choice((-1, 1))
        if rng.random() < 0.15:
            bat += rng.choice((-0.01, 0.01))
        if rng.random() < 0.25:
            up += 1
        if rng.random() < 0.2:
            imu_t += 0.1
        if rng.random() < 0.05:
            mt += 0.1
        if rng.random() < 0.3:
            rpm += rng.randint(-40, 40)
            spd_e += rng.choice((-1, 0, 1))
        if rng.random() < 0.01:
            cool += 1
        if rng.random() < 0.3:
            sig = (sig[0] + rng.choice((-1, 1)), sig[1], sig[2])
        extras = ',ax=%d;ay=%d;az=%d' % tuple(rng.randint(-900, 900) for _ in range(3))
        if rng.random() < 0.3:
            extras += ',gx=%d;gy=%d;gz=%d' % tuple(rng.randint(-300, 300) for _ in range(3))
        extras += ',it=%.1f,mt=%.1f,vs=%.2f' % (imu_t, mt, vs)
        if rng.random() < 0.85:
            extras += ',rsrp=%d;snr=%d;band=%d' % sig
        if rng.random() < 0.95:
            extras += ',orpm=%d;ospd=%d;ocl=%d' % (rpm, spd_e, cool)
        if rng.random() < 0.1:
            extras += ',o0B=%d' % rng.randint(90, 100)
        if rng.random() < 0.1:
            extras += ',dbg=a:%d;vs=4.22' % rng.randint(0, 3)
        extras += ',up=%d,rid=%d' % (up, 1000 + i)
        if rng.random() < 0.05:
            extras += ',age=%d' % rng.randint(1, 90)
        lines.append('16/11/26,20:01:%02d.%06d+00,%.6f,%.6f,%.2f,%.2f,%.2f,%d,%d,%.2f,0,%d,0%s' % (
            i // 4, (i % 4) * 250000, 51.4 + i * 1e-5, -0.19 - i * 1e-5,
            rng.uniform(20, 60), alt, rng.uniform(0, 360), hdop, sat, bat, up, extras))
    return lines


def canon(line):
    """What a record says, in no particular order."""
    parts = line.split(',')
    fixed = [parts[0] + ',' + parts[1]] + parts[2:13]
    pairs = []
    for group in parts[13:]:
        if group.startswith(deltarec.OPAQUE):
            pairs.append(group)
        else:
            pairs.extend(group.split(';'))
    return fixed, sorted(pairs)


@pytest.mark.parametrize('seed', range(40))
def test_random_streams_match_the_reference_and_read_back(lib, seed):
    rng = random.Random(seed)
    lines = _stream(rng, 40)
    for start in range(0, len(lines), 4):
        batch = lines[start:start + 4]
        n, text = enc_batch(lib, batch)
        assert n > 0
        want = deltarec.encode_lines(batch)
        assert text.split('\n') == want
        back = deltarec.expand_lines(text.split('\n'))
        assert [canon(x) for x in back] == [canon(x) for x in batch]


@pytest.mark.parametrize('seed', range(10))
def test_pairs_of_unrelated_records_match_the_reference(lib, seed):
    rng = random.Random(1000 + seed)
    a, b = _stream(rng, 2)[0], _stream(random.Random(2000 + seed), 5)[4]
    n, text = enc(lib, a, b)
    assert (text if n > 0 else b) == deltarec.encode(a, b)


def test_the_stream_gets_smaller(lib):
    lines = _stream(random.Random(7), 400)
    whole = sum(len(x) for x in lines)
    sent = 0
    for start in range(0, len(lines), 4):
        sent += enc_batch(lib, lines[start:start + 4])[0]
    assert sent < whole * 0.85
