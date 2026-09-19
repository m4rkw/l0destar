"""Ad-hoc screenshots: shots.py <tag> <timebase_s> <offset_s> [persist_s]  (trigger as already set)"""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scope import Scope
tag, tb, off = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
persist = float(sys.argv[4]) if len(sys.argv) > 4 else 0
sc = Scope()
def c(x): sc.cmd(x); time.sleep(0.05)
c(":DISPlay:GRADing:TIME MIN"); c(f":TIMebase:MAIN:SCALe {tb}"); c(f":TIMebase:MAIN:OFFSet {off}"); c(":TRIGger:SWEep NORMal"); c(":RUN")
if persist:
    c(":DISPlay:CLEar"); c(":DISPlay:GRADing:TIME INFinite"); time.sleep(persist)
else:
    time.sleep(1.5); c(":STOP"); time.sleep(0.3)
print(tag, sc.shot(f"scope_{tag}.png"), "bytes")
c(":DISPlay:GRADing:TIME MIN"); c(":RUN")
