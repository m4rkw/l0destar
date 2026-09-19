"""Put the DHO814 into the CAN capture setup: CH1 CANH, CH2 CANL, 10x probes,
1 V/div centred on the 2.2 V recessive level, 1 Mpt, edge trigger on CH1.
setup.py [chan34]  -- also enable CH3 (TXD) / CH4 (RXD) for the loop-delay run."""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scope import Scope
sc = Scope()
def c(x): sc.cmd(x); time.sleep(0.05)
c(":STOP")
c(":ACQuire:TYPE NORMal"); c(":ACQuire:MDEPth 1M")
for ch, off in ((1, -2.0), (2, -2.0)):
    c(f":CHANnel{ch}:DISPlay ON"); c(f":CHANnel{ch}:PROBe 10"); c(f":CHANnel{ch}:COUPling DC")
    c(f":CHANnel{ch}:BWLimit OFF"); c(f":CHANnel{ch}:SCALe 0.5"); c(f":CHANnel{ch}:OFFSet {off}")
for ch in (3, 4):
    c(f":CHANnel{ch}:DISPlay {'ON' if 'chan34' in sys.argv else 'OFF'}")
c(":MATH1:DISPlay OFF")
c(":TRIGger:MODE EDGE"); c(":TRIGger:EDGE:SOURce CHANnel1"); c(":TRIGger:EDGE:SLOPe POSitive")
c(":TRIGger:EDGE:LEVel 2.6"); c(":TRIGger:HOLDoff 8e-9"); c(":TRIGger:SWEep NORMal")
c(":TIMebase:MAIN:SCALe 50e-6"); c(":TIMebase:MAIN:OFFSet 100e-6"); c(":DISPlay:GRADing:TIME MIN")
c(":RUN")
print("srate", sc.query(":ACQuire:SRATe?"), "mdepth", sc.query(":ACQuire:MDEPth?"), "err", sc.query(":SYSTem:ERRor?"))
