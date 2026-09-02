import os, sys, time, statistics
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scope import Scope
sc = Scope()
def c(x): sc.cmd(x); time.sleep(0.05)
for ch in (3, 4):
    c(f":CHANnel{ch}:DISPlay ON"); c(f":CHANnel{ch}:PROBe 10"); c(f":CHANnel{ch}:COUPling DC")
    c(f":CHANnel{ch}:SCALe 1"); c(f":CHANnel{ch}:OFFSet 0"); c(f":CHANnel{ch}:BWLimit OFF")
c(":TRIGger:MODE EDGE"); c(":TRIGger:EDGE:SOURce CHANnel3"); c(":TRIGger:EDGE:SLOPe NEGative"); c(":TRIGger:EDGE:LEVel 1.65")
c(":TRIGger:HOLDoff 8e-9"); c(":TRIGger:SWEep NORMal"); c(":DISPlay:GRADing:TIME MIN")
c(":TIMebase:MAIN:SCALe 50e-9"); c(":TIMebase:MAIN:OFFSet 100e-9"); c(":RUN"); time.sleep(1.5)
sc.shot("scope_loopdelay_fall.png")
c(":TRIGger:EDGE:SLOPe POSitive"); time.sleep(1.5); sc.shot("scope_loopdelay_rise.png")
c(":TIMebase:MAIN:SCALe 20e-6"); c(":TIMebase:MAIN:OFFSet 60e-6"); time.sleep(1.5); c(":STOP"); time.sleep(0.3)
print("error:", sc.query(":SYSTem:ERRor?"))
def raw(src):
    c(f":WAVeform:SOURce {src}"); c(":WAVeform:MODE RAW"); c(":WAVeform:FORMat BYTE")
    pre = sc.query(":WAVeform:PREamble?").split(","); xinc, yinc, yorig, yref = float(pre[4]), float(pre[7]), float(pre[8]), float(pre[9])
    total = int(float(sc.query(":ACQuire:MDEPth?"))); out = bytearray(); s0 = 1
    while s0 <= total:
        s1 = min(total, s0 + 125000 - 1); c(f":WAVeform:STARt {s0}"); c(f":WAVeform:STOP {s1}")
        blk = sc.block(":WAVeform:DATA?", timeout=60)
        if not blk: break
        out += blk; s0 = s1 + 1
    return [(b - yorig - yref) * yinc for b in out], xinc
h, xinc = raw("CHANnel1"); l, _ = raw("CHANnel2"); txd, _ = raw("CHANnel3"); rxd, _ = raw("CHANnel4")
n = min(len(h), len(l), len(txd), len(rxd)); d = [h[i] - l[i] for i in range(n)]
print(f"{n} samples @ {xinc*1e9:.2f} ns")
def crossings(v, thr, falling):
    out = []
    for i in range(1, n):
        if falling and v[i-1] >= thr > v[i]: out.append(i)
        elif not falling and v[i-1] < thr <= v[i]: out.append(i)
    return out
win = int(400e-9 / xinc)
for falling, name in ((True, "TXD fall (recessive->dominant)"), (False, "TXD rise (dominant->recessive)")):
    tx = crossings(txd, 1.65, falling); rx = crossings(rxd, 1.65, falling)
    bus = crossings(d, 0.9, not falling)   # dominant = high on the difference
    d_tr, d_tb, d_br = [], [], []
    for t in tx:
        r = next((x for x in rx if t < x <= t + win), None); b = next((x for x in bus if t < x <= t + win), None)
        if r: d_tr.append((r - t) * xinc * 1e9)
        if b: d_tb.append((b - t) * xinc * 1e9)
        if r and b: d_br.append((r - b) * xinc * 1e9)
    for lab, vals in (("TXD -> RXD loop delay", d_tr), ("TXD -> bus (diff crosses 0.9 V)", d_tb), ("bus -> RXD", d_br)):
        if vals: print(f"  {name}: {lab:34s} min {min(vals):5.1f}  mean {statistics.mean(vals):5.1f}  max {max(vals):5.1f} ns  (n={len(vals)})")
print("TXD levels: low %.2f high %.2f V; RXD: low %.2f high %.2f V" % (min(txd), max(txd), min(rxd), max(rxd)))
c(":RUN")
