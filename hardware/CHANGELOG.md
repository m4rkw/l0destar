# l0destar hardware changelog

Changes to the PCB designs over time, newest first. Boards no longer in the
tree are noted where they were removed.

## 08/08/2026

- Removed excessive stitching vias and optimised placements
- Removed unnecessary ENABLE resistor
- Other minor PCB layout tweaks

## 07/08/2026

- Fixed voltage spec of the buck output caps

## 05/08/2026

- Annotated the v3.0 schematics with specific part requirements
- Updated the v3.0 parts lists with the specific part requirements
- Relaxed tolerances where they don't matter

## 05/08/2026

- Added **l0destar v3.0** — the CAN and K-line variants consolidated onto a
  single PCB. Changes from v2.6:
  - Both the CAN and ISO-9141 (K-line) circuits are on the one board; either,
    neither or both can be populated, with the external connector pins and OBD
    power rails selected by shorting jumper pads (which double as power rail
    test points to save board space)
  - The L9637D was replaced with a TJA1027T LIN transceiver, dramatically
    simplifying the K-line circuit — the separate 5V buck converter and level
    shifter are gone
  - CAN transceiver swapped for a MAX33041EASA+ for more robust transient
    protection, with a pulldown added to STBY so the transceiver defaults to
    normal mode instead of floating
  - Auxiliary power MOSFETs and several ideal diodes replaced with load
    switches to simplify the system and reduce board footprint
  - OBD power rails are switched separately from the GPS power rail to cut
    power use during engine-off telemetry wakes
  - S2R2 increased to 10M and S4R2 to 100K for lower quiescent draw and more
    resilient ignition sensing at low system voltage

## 03/08/2026

- Minor PCB layout tweaks

## 02/08/2026

- Added **l0destar v2.6 micro (v2.6M)**. Changes from v2.5 micro:
  - Added stacked 47uF ceramic bulk capacitance (2220, 50V X7R,
    CKG57NX7R1H476M500JH) on the 12V input for additional resilience against
    automotive transient spikes
- Added **lodestar v2.6 K-line (v2.6K)**. Changes from v2.5 K-line:
  - Added stacked 47uF ceramic bulk capacitance (2220, 50V X7R,
    CKG57NX7R1H476M500JH) on the 12V input for additional resilience against
    automotive transient spikes
- Added **lodestar v2.6 CAN (v2.6C)**. Changes from v2.5 CAN:
  - Added stacked 47uF ceramic bulk capacitance (2220, 50V X7R,
    CKG57NX7R1H476M500JH) on the 12V input for additional resilience against
    automotive transient spikes

## 01/08/2026

- Added **l0destar v2.5 micro (v2.5M)** — a major redesign of the mini
  prototype:
  - Dramatically smaller PCB footprint
  - Switched to a 4-layer stackup for simpler routing
  - Default passive size reduced from 0805 to 0402, bumped to 0603/0805 only
    where required by specification
  - Removed the large aluminium polymer caps; buck output cap replaced with
    2x 1210 ceramic 220uF for much lower ESR
  - Sleep-mode power consumption optimised — estimated quiescent draw with the
    accelerometer armed is ~370 µA
  - Removed the relay power control circuit; with quiescent current this low
    it's unnecessary (the 12V rail can be fed directly from ignition if
    ignition-only power is wanted)
  - Component specifications reviewed for automotive use and indicated in the
    BOM; input stage MOSFETs and TVS tuned for smaller footprint and more
    appropriate ratings
  - Component vias dogleg-routed deliberately to keep fabrication cost down
- Fixed the input TVS part on v2.5 micro: PTVS30VS1UTR → PTVS33VS1UTR
- Fixed reference designators in the v2.5 micro schematics
- Added **l0destar v2.5K** — K-line variant of v2.5 with the ISO-9141
  interface (L9637D), a second LT8609 providing a switchable 5V rail, a
  switchable auxiliary 12V rail on the k-enable signal, and L-line pulldown
- Added **l0destar v2.5C** — CAN-bus variant of v2.5 with a CAN interface
  including standby control via the XSTBY signal

## 20/07/2026

- v2.1 mini: fixed the status LED footprint and mounted 5 LEDs vertically

## 19/07/2026

- Added **l0destar v2.1** and **l0destar v2.1 mini** (developed from
  prototype v2.0):
  - v2.1: CAN + ISO-9141 interfaces, dual buck converters, switchable
    auxiliary 3.3V/5V/12V rails, six general-purpose 0-30V AIO pins
  - v2.1 mini: same board with the CAN, ISO-9141 and AIO circuitry removed
    for a smaller footprint and simpler construction; later bench-tested with
    ~1 mA sleep current
- Removed earlier prototypes that were either failed or non-viable:
  **prototype v1.0**, **v1.1 ISO-9141 handsolder**, **v1.1 CAN handsolder**
  and **prototype v2.0**

## 15/07/2026

- Added **prototype v2.0** — first fully integrated standalone design (no
  breakout modules): onboard GPS antenna bias tee with SMA/u.FL connectors,
  AIO pins, integrated CAN and ISO-9141 circuits, status LED and test points.
  Superseded by v2.1 four days later.

## 25/06/2026

- Documented known issues found while testing the v1.1 ISO-9141 handsolder
  board:
  - With AUX_SW low the +5V_AUX rail reads ~2.8V due to 12V backfeeding
    through the L9637D into the rail
  - Trace clearance too low in several places, making the board very hard to
    hand-assemble without bridging nets

## 13/06/2026

- Added **prototype v1.1 CAN handsolder** — CAN version of the hand-solderable
  tracker (MCP2518FD-based CAN interface in place of the K-line circuit)

## 12/06/2026

- Added **bench_test_iso9141** — bench development board for the nRF9151 Dev
  Kit with dual L9637D transceivers and level shifters; a 3V relay gates all
  power rails behind the DK's 3.3V supply so nothing is live when the DK is
  off
- Added **bench_test_can** — CAN version of the bench board with MikroBUS
  headers for the MIKROE-2379 CAN-FD Click
- Added **prototype v1.1 ISO-9141 handsolder** — first standalone
  hand-buildable tracker using the Makerdiary nRF9151 Connect Kit: 4.2V buck
  into the battery connector (USB-C usable without removing power), 12V
  live/ignition inputs with TVS and reverse polarity protection, relay-based
  power source switching, INA228 voltage monitoring, L9637D with 5V buck,
  switchable auxiliary rails, ASM330LHHXG1TR IMU, 2200uF bulk cap for
  cranking

## 09/06/2026

- v1.0: replaced the voltage divider that derives the ignition signal with a
  MOSFET gating the 3.3V rail. Previously the divider produced ~9V clamped by
  a 3.3V zener — a failed-open zener would have put 9V straight onto the MCU
  pin. The MOSFET inverts the signal (high when ignition is off), which is
  acceptable and matches similar devices

## 07/06/2026

- v1.0: fixed the ignition sense divider to give a solid 3.3V/0V signal — the
  nRF9151 docs warn against leaving signal pins floating between logic levels
- v1.0: added a 4.2V buck converter with an LM66100 ideal diode to power the
  Makerdiary ConnectKit via its battery connector, so the device can be
  hardwired to vehicle power while still allowing USB connection for
  programming (powering VBUS and USB simultaneously is explicitly
  disallowed)

## 30/05/2026

- Added **l0destar v1.0 prototype** — initial schematics and board design
