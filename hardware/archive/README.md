# archived l0destar hardware

**Before building, installing or relying on any of this, read the [project disclaimer](../DISCLAIMER.md).**

## Overview

KiCad PCB designs for the l0destar vehicle tracker. All boards are built around
the Nordic nRF9151 for LTE-M/GPS and share a common set of subsystems: 12V
automotive power input, INA228 voltage monitoring, ignition sensing, and an
accelerometer/IMU.

**THESE SHOULD NOT BE CONSIDERED FINISHED PRODUCTS. MOST OF THEM HAVEN'T BEEN
TESTED AT ALL. USE ENTIRELY AT YOUR OWN RISK.**

**SOME OF THESE HAVE KNOWN DEFECTS DETAILED IN THEIR README**

## Archived designs

This section has archived board designs, essentially older iterations of the
board as it was being developed. These are preserved here for posterity but are
generally considered either outright defective or at least incomplete.

### Bench test boards

Development boards designed for the nRF9151 Dev Kit. Pin headers connect to the
DK; a 3V relay gates all power rails behind the DK's 3.3V supply so nothing is
live when the DK is off.

- **[bench_test_iso9141](bench_test_iso9141/)** - K-wire (ISO 14230-1 K-line) version
  with dual L9637D transceivers and level shifters
- **[bench_test_can](bench_test_can/)** - CAN version with MikroBUS headers
  for the MIKROE-2379 CAN-FD Click board

### Prototype boards - hand-solderable

Standalone boards using the
[Makerdiary nRF9151 Connect Kit](https://makerdiary.com/products/nrf9151-connectkit).
Designed to be hand-built and installed in a vehicle. Powered via 4.2V buck into
the battery connector so USB-C can be connected for firmware updates without
removing power. Include TVS/reverse-polarity protection.

- **[l0destar v2.1](prototype2.1/)** - supports CAN and K-wire, dual buck
  converters, switchable auxiliary 3.3V/5V/12V rails, six 0-30V AIO pins,
  2200uF bulk cap. 0805 passives.
- **[l0destar v2.1 mini](prototype2.1_mini/)** - mini version with no OBD
  capability or AIO pins. Bench-tested at ~1 mA sleep current. 2200uF bulk cap.
  0805 passives.

### v2.5 boards - 4-layer, 0402 passives

Major redesign from v2.1: dramatically smaller footprint, 4-layer stackup,
default passive size reduced to 0402, relay power control removed (~370 µA
estimated quiescent), 2x 1210 ceramic 220uF buck output caps.

- **[l0destar v2.5M](l0destar_v2.5_micro/)** - micro variant, no OBD
  capability. Not tested.
- **[l0destar v2.5K](l0destar_v2.5_kline/)** - K-line variant with L9637D,
  second LT8609 for switchable 5V rail, switchable auxiliary 12V rail, L-line
  pulldown. Tested - 120 µA quiescent, all major subsystems passed.
- **[l0destar v2.5C](l0destar_v2.5_can/)** - CAN variant with MCP2518FD and
  TCAN334GDR, standby control via XSTBY. Not tested.

### v2.6 boards

Same as v2.5 with a stacked 47uF ceramic bulk cap (2220, 50V X7R) added on the
12V input for additional resilience against automotive transient spikes.

- **[l0destar v2.6M](l0destar_v2.6_micro/)** - micro variant. Not tested.
- **[l0destar v2.6K](l0destar_v2.6_kline/)** - K-line variant. Not tested.
- **[l0destar v2.6C](l0destar_v2.6_can/)** - CAN variant. Not tested.

### v3.0 - consolidated CAN + K-line

> [!CAUTION]
> v3.0, v3.1 and v3.2 share an L-line defect: a short from the external L wire
> to 12V while L_SEND is driven can destroy the pulldown MOSFET and put battery
> voltage onto an nRF9151 GPIO, destroying the module. Do not connect the L
> wire on these versions. Fixed in v3.3. See the
> [v3.2 README](l0destar_v3.2/README.md#known-defects) for the full analysis.
> The firmware will refuse to drive L\_SEND on affected boards, the odds of
> anyone actually having this problem are miniscule given how rare the K-wire
> interface is and how rare using the L wire is even on K-wire vehicles, but
> it bears mentioning
> because the failure mode is severe.

- **[l0destar v3.0](l0destar_v3.0/)** - CAN and K-line circuits on a single
  PCB, either/both/neither can be populated. Jumper pads select connector pins
  and OBD power rails (double as test points). L9637D replaced with TJA1027T
  (eliminates 5V buck and level shifter), CAN transceiver swapped to
  MAX33041EASA+, auxiliary MOSFETs replaced with load switches, OBD power rails
  switched separately from GPS. Not tested.
