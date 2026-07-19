# l0destar hardware

## Overview

KiCad PCB designs for the l0destar vehicle tracker. All boards are built around
the Nordic nRF9151 for LTE-M/GPS and share a common set of subsystems: 12V
automotive power input, relay-based power source switching, INA228 voltage
monitoring, ignition sensing, and an accelerometer/IMU.

**THESE SHOULD NOT BE CONSIDERED FINISHED PRODUCTS. MOST OF THEM HAVEN'T BEEN
TESTED AT ALL. USE ENTIRELY AT YOUR OWN RISK.**

## PCB designs

### Bench test boards

Development boards designed for the nRF9151 Dev Kit. Pin headers connect to the
DK; a 3V relay gates all power rails behind the DK's 3.3V supply so nothing is
live when the DK is off.

- **[bench_test_iso9141](bench_test_iso9141/)** — ISO-9141 (K-line) version
  with dual L9637D transceivers and level shifters
- **[bench_test_can](bench_test_can/)** — CAN version with MikroBUS headers
  for the MIKROE-2379 CAN-FD Click board

### Prototype boards — hand-solderable

Standalone boards using the
[Makerdiary nRF9151 Connect Kit](https://makerdiary.com/products/nrf9151-connectkit).
Designed to be hand-built and installed in a vehicle. Powered via 4.2V buck into
the battery connector so USB-C can be connected for firmware updates without
removing power. Include TVS/reverse-polarity protection and a 2200uF bulk cap
for cranking.

- **[l0destar v2.1](prototype2.1/)** - supports CAN and ISO-9141
- **[l0destar v2.1 mini](prototype2.1_mini/)** - mini version with no OBD
  capability or AIO pins
