import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scope import Scope
tag, tb, off = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
sc = Scope()
def c(x): sc.cmd(x); time.sleep(0.05)
c(":DISPlay:GRADing:TIME MIN"); c(f":TIMebase:MAIN:SCALe {tb}"); c(f":TIMebase:MAIN:OFFSet {off}")
c(":TRIGger:SWEep NORMal"); c(":RUN"); time.sleep(1.5); c(":STOP"); time.sleep(0.3)
def wave(src):
    c(f":WAVeform:SOURce {src}"); c(":WAVeform:MODE NORMal"); c(":WAVeform:FORMat BYTE")
    pre = sc.query(":WAVeform:PREamble?").split(",")
    xinc, xorig, yinc, yorig, yref = float(pre[4]), float(pre[5]), float(pre[7]), float(pre[8]), float(pre[9])
    raw = sc.block(":WAVeform:DATA?")
    return [(b - yorig - yref) * yinc for b in raw], xinc, xorig
h, xinc, xorig = wave("CHANnel1"); l, _, _ = wave("CHANnel2")
n = min(len(h), len(l)); d = [h[i] - l[i] for i in range(n)]
print(f"{n} samples @ {xinc*1e9:.1f} ns, window {xorig*1e6:.2f}..{(xorig+n*xinc)*1e6:.2f} us")
# level statistics
dom = [x for x in d if x > 1.0]; rec = [x for x in d if x < 0.5]
print(f"dominant diff: min {min(dom):.2f} max {max(dom):.2f} mean {sum(dom)/len(dom):.2f} V ({len(dom)} samples)")
print(f"recessive diff: min {min(rec):.2f} max {max(rec):.2f} V")
# edges: 10-90 % of 0 -> mean dominant
lo, hi = 0.1 * (sum(dom)/len(dom)), 0.9 * (sum(dom)/len(dom))
rises, falls = [], []
i = 1
while i < n:
    if d[i-1] < lo and d[i] >= lo:
        j = i
        while j < n and d[j] < hi: j += 1
        if j < n: rises.append((j - i) * xinc); i = j
    elif d[i-1] > hi and d[i] <= hi:
        j = i
        while j < n and d[j] > lo: j += 1
        if j < n: falls.append((j - i) * xinc); i = j
    i += 1
if rises: print(f"rise 10-90: {min(rises)*1e9:.0f}..{max(rises)*1e9:.0f} ns over {len(rises)} edges")
if falls: print(f"fall 90-10: {min(falls)*1e9:.0f}..{max(falls)*1e9:.0f} ns over {len(falls)} edges")
# overshoot after rising edges: max within 100 ns after crossing hi
ov = []
for i in range(1, n):
    if d[i-1] < hi <= d[i]:
        seg = d[i:i + max(1, int(100e-9 / xinc))]; ov.append(max(seg))
if ov: print(f"peak within 100 ns after rise: max {max(ov):.2f} V (mean dominant {sum(dom)/len(dom):.2f})")
step = max(1, int(float(sys.argv[4]) if len(sys.argv) > 4 else 50e-9) // 1) if False else max(1, int((float(sys.argv[4]) if len(sys.argv) > 4 else 50e-9) / xinc))
print("t(ns)    CANH  CANL  diff")
for i in range(0, n, step):
    hh = sum(h[i:i+step])/len(h[i:i+step]); ll = sum(l[i:i+step])/len(l[i:i+step])
    print(f"{(xorig + i*xinc)*1e9:7.0f}  {hh:5.2f} {ll:5.2f} {hh-ll:5.2f}")
c(":TRIGger:SWEep NORMal"); c(":RUN"); time.sleep(0.5)
sc.shot(f"scope_{tag}_single.png")
c(":DISPlay:CLEar"); c(":DISPlay:GRADing:TIME INFinite"); time.sleep(8); sc.shot(f"scope_{tag}_eye.png"); c(":DISPlay:GRADing:TIME MIN")
print("screenshots", tag)
