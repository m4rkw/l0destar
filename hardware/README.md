# l0destar hardware

**Before building, installing or relying on any of this, read the [project disclaimer](../DISCLAIMER.md).**

## Overview

KiCad PCB designs for the l0destar vehicle tracker. All boards are built around
the Nordic nRF9151 for LTE-M/GPS and share a common set of subsystems: 12V
automotive power input, INA228 voltage monitoring, ignition sensing, and an
accelerometer/IMU.

**THESE SHOULD NOT BE CONSIDERED FINISHED PRODUCTS. SOME OF THEM HAVEN'T BEEN
TESTED AT ALL. USE ENTIRELY AT YOUR OWN RISK.**

**SOME OF THESE HAVE KNOWN DEFECTS DETAILED IN THEIR README**

Prototypes here are currently being tested and evaluated, there are typically a
few in the pipeline at any given time. Older boards no longer being tested are
moved into the [archive](archive/).

## PCB designs

### v3.1 - input resilience, hardened antenna feed, aux rail fault detection

- **[l0destar v3.1](l0destar_v3.1/)** - v3.0 with more robust input protection
  (sized for ISO 7637-2 pulse 2a), auxiliary power rail sensing/fault
  detection, and a hardened antenna feed with 15R series resistance and ESD
  protection. Same footprint as v3.0. Tested - 120 µA quiescent, all major
  subsystems passed.

### v3.2 - MCU over-voltage protection

- **[l0destar v3.2](l0destar_v3.2/)** - v3.1 plus hardware over-voltage
  protection that disconnects the module supply within microseconds of a
  regulator fault (self-recovering, no firmware dependency, <3 µA), and I2C
  bus hardening against LTE TX bursts (stronger pull-ups, 100pF filter caps
  on SDA/SCL, series jumper/ferrite on the accelerometer supply).

  Tested successfully: <a href="https://www.youtube.com/watch?v=6-iVvQPaaeg">OVP scope test</a>

### v3.3 - L-line defect fix

- **[l0destar v3.3](l0destar_v3.3/)** - fixes the L-line short-to-battery
  defect present in v3.0-v3.2 (pulldown current limited to 90 mA via an
  AL5809-90, gate-fault current into the nRF limited) and adds L-line fault
  sensing so a 12V short can be detected. Buck converter tuned to shut off
  cleanly below ~3.5V and restart at 4.6V to avoid flapping. Not tested;
  quiescent estimated ~132 µA.
