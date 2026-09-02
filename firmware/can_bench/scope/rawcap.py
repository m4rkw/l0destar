import os, sys, time, statistics
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scope import Scope
tag, bit_ns = sys.argv[1], float(sys.argv[2])
holdoff = sys.argv[3] if len(sys.argv) > 3 else "8e-9"
offset = sys.argv[4] if len(sys.argv) > 4 else "100e-6"
sc = Scope()
def c(x): sc.cmd(x); time.sleep(0.05)
c(":TRIGger:MODE EDGE"); c(":TRIGger:EDGE:SOURce CHANnel1"); c(":TRIGger:EDGE:SLOPe POSitive"); c(":TRIGger:EDGE:LEVel 2.9")
c(f":TRIGger:HOLDoff {holdoff}"); c(":TRIGger:SWEep NORMal"); c(":DISPlay:GRADing:TIME MIN")
c(":TIMebase:MAIN:SCALe 50e-6"); c(f":TIMebase:MAIN:OFFSet {offset}")
c(":RUN"); time.sleep(1.5); c(":STOP"); time.sleep(0.3); sc.shot(f"scope_{tag}_frame.png")
c(":RUN"); time.sleep(1.5); c(":STOP"); time.sleep(0.3)
def raw(src):
    c(f":WAVeform:SOURce {src}"); c(":WAVeform:MODE RAW"); c(":WAVeform:FORMat BYTE")
    pre = sc.query(":WAVeform:PREamble?").split(",")
    npts = int(float(pre[2])); xinc, xorig, yinc, yorig, yref = float(pre[4]), float(pre[5]), float(pre[7]), float(pre[8]), float(pre[9])
    # the record is much longer than the screen; read 250k points starting
    # 60 us before the trigger (memory centre = screen centre = trigger + offset)
    total = int(float(sc.query(":ACQuire:MDEPth?")))      # whole record, find the frame in software
    out = bytearray(); s0 = 1; chunk = 125000
    while s0 <= total:
        s1 = min(total, s0 + chunk - 1)
        c(f":WAVeform:STARt {s0}"); c(f":WAVeform:STOP {s1}")
        blk = sc.block(":WAVeform:DATA?", timeout=60)
        if not blk: break
        out += blk; s0 = s1 + 1
    v = [(b - yorig - yref) * yinc for b in out]
    return v, xinc
h, xinc = raw("CHANnel1"); l, _ = raw("CHANnel2")
n = min(len(h), len(l))
act = [i for i in range(0, n, 50) if h[i] > 2.7]
if act:
    a0, a1 = max(0, act[0] - 1000), min(n, act[-1] + 1000); h, l = h[a0:a1], l[a0:a1]; n = a1 - a0
    print(f"  active region {a0}..{a1} ({(a1-a0)*xinc*1e6:.0f} us) of {len(act)*50} samples record")
d = [h[i] - l[i] for i in range(n)]
print(f"{n} samples @ {xinc*1e9:.2f} ns = {n*xinc*1e6:.0f} us")
# segment into dominant / recessive runs using hysteresis on the difference
runs = []; state = d[0] > 0.9; i0 = 0
for i in range(1, n):
    s = d[i] > 1.2 if not state else d[i] > 0.6
    if s != state:
        runs.append((state, i0, i)); state = s; i0 = i
runs.append((state, i0, n))
bit = bit_ns * 1e-9 / xinc
short = [r for r in runs if 0.8 * bit <= (r[2] - r[1]) <= 1.25 * bit]   # single bits of the data phase
dom1 = [r for r in short if r[0]]; rec1 = [r for r in short if not r[0]]
print(f"runs: {len(runs)} total, single-bit runs at {bit_ns:.0f} ns: {len(dom1)} dominant, {len(rec1)} recessive")
def at(r, frac): return d[r[1] + int((r[2] - r[1]) * frac)]
def stats(name, vals):
    if vals: print(f"  {name:38s} min {min(vals):6.2f}  mean {statistics.mean(vals):6.2f}  max {max(vals):6.2f} V")
stats("dominant diff at 75 % of single bit", [at(r, 0.75) for r in dom1])
stats("dominant diff at 50 %", [at(r, 0.5) for r in dom1])
stats("recessive diff at 75 % of single bit", [at(r, 0.75) for r in rec1])
stats("recessive diff at 50 %", [at(r, 0.5) for r in rec1])
stats("dominant peak (overshoot) in bit", [max(d[r[1]:r[2]]) for r in dom1])
stats("recessive trough (undershoot) in bit", [min(d[r[1]:r[2]]) for r in rec1])
# settling: time from bit start until |d - level| < 0.15 V for the rest of the bit
def settle(r, level):
    seg = d[r[1]:r[2]]
    for k in range(len(seg)):
        if all(abs(x - level) < 0.15 for x in seg[k:]): return k * xinc * 1e9
    return None
sd = [s for s in (settle(r, at(r, 0.75)) for r in dom1) if s is not None]
sr = [s for s in (settle(r, 0.0) for r in rec1) if s is not None]
if sd: print(f"  dominant settles within 150 mV: mean {statistics.mean(sd):.0f} ns, max {max(sd):.0f} ns")
if sr: print(f"  recessive settles within 150 mV: mean {statistics.mean(sr):.0f} ns, max {max(sr):.0f} ns")
# bit width accuracy
w = [(r[2] - r[1]) * xinc * 1e9 for r in short]
print(f"  single-bit width: min {min(w):.0f} mean {statistics.mean(w):.0f} max {max(w):.0f} ns (nominal {bit_ns:.0f})")
# edge times 10-90 across data-phase edges
ed = []
for r in dom1:
    lvl = at(r, 0.75); i = r[1]
    while i > 0 and d[i] > 0.1 * lvl: i -= 1
    j = i
    while j < n and d[j] < 0.9 * lvl: j += 1
    ed.append((j - i) * xinc * 1e9)
if ed: print(f"  rise 10-90 %: min {min(ed):.1f} mean {statistics.mean(ed):.1f} max {max(ed):.1f} ns")
c(f":TIMebase:MAIN:SCALe {bit_ns/2*1e-9}"); c(":TIMebase:MAIN:OFFSet 0"); c(":TRIGger:HOLDoff 8e-9"); c(":RUN"); c(":DISPlay:CLEar"); c(":DISPlay:GRADing:TIME INFinite"); time.sleep(8)
sc.shot(f"scope_{tag}_eye.png"); c(":DISPlay:GRADing:TIME MIN"); print("  eye screenshot", tag)
