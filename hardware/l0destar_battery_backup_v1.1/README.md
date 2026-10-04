# l0destar battery backup v1.1

## Overview

**NOTE: v1.1 HAS NOT YET BEEN BUILT OR TESTED, USE AT YOUR OWN RISK.** It
fixes the three problems found building v1.0, see
[Changes from v1.0](#changes-from-v10)

- This is a prototype inline battery backup module for the l0destar vehicle
  tracker. It sits in the harness between the car and the tracker and holds
  the tracker's supply at ~9V from a single CR123A lithium primary cell when
  car power is cut
- It is designed to be hand-solderable (hot air required). Every active part
  is in a leaded package: MSOP-12, SOT-23, TSOT-23, SC70, DPAK
- The cell is a **non-rechargeable** primary in a board-mounted holder.
  Nothing on the board charges it. See [Why a primary cell](#why-a-primary-cell)
- Pin-compatible with the v3.4 tracker's 2x3 Micro-Fit harness: J1 takes the
  car harness, J2 goes to the tracker, the CAN/K-line pins pass straight
  through and ignition passes through a diode (S1D1)
- 4-layer 63.1 x 36.2 mm board, JLC04161H-7628 stack-up, four M2 mounting
  holes. PCB DRC is clean and schematic ERC has only the cosmetic
  `endpoint_off_grid` warnings

## Changes from v1.0

v1.0 was built and three problems came out of it. Same board outline,
connectors and enclosure; the cell, boost and wake-pulse circuits are
unchanged apart from reference designators.

1. **Pass-through dropped ~0.5V while the tracker slept.** The LM74610 is a
   floating controller that runs its charge pump from the forward drop across
   the FET, so at the tracker's sleep current it can't keep the gate enhanced
   and the current runs through the body diode instead. v1.1 replaces
   LM74610 + IRLR2905 (N-channel) with an LTC4412HV driving an SQD50P06-15L
   P-channel FET (S2U1/S2Q1). The LTC4412 regulates the FET to a ~20 mV
   forward drop at any load and drops the charge-pump capacitor (old S1C3).
   The trade is quiescent current: ~13 µA from the car where the LM74610 had
   none
2. **The ignition wake pulse didn't work.** The car pulls its ignition wire
   to ground when the key is off, so the pulse injected on IGN was simply
   sunk by the car's ignition loads. v1.1 adds S1D1, an S1B in series between
   J1 pin 5 (IGN\_CAR) and J2 pin 5 (IGN). Car ignition still reaches the
   tracker, a diode drop lower, and the car can no longer pull the pulse
   down
3. **The cell lockout caused brownouts, so it's gone.** Its ~50 µs input
   filter was short enough that boost inrush and LTE transmit sag pulled the
   cell below 2.45V and shut the boost off, which browned out the tracker.
   A single primary cell doesn't need protecting from running flat, so v1.1
   removes the lockout comparator, AND gate and restart pacing (old S5U3,
   S5U4, S5D1, S5R36, S5R37, S5R40, S5R41, S5C5, S5C6, S5C7). The car-sense
   comparator drives EN\_BOOST directly

Other changes:

- The input electrolytics are now hybrid polymer: S2C2 is 47 µF 63V
  Panasonic EEH-ZC1J470P (was EEE-FK1J470P) and S2C4 is 82 µF 63V
  Chemi-Con HHXC630ARA820MJA0G (was 100 µF EEE-FK1J101P)
- Reference designators are renumbered by sheet: S1 is the root sheet
  (connectors, S1D1, mounting holes), S2 pass-through, S3 cell, S4 boost,
  S5 wake pulse, S6 switchover

## Disclaimer

This is a prototype, not a product. Nothing here is validated or certified,
the parts lists are examples rather than a verified BOM, and every number
below is a desk calculation from datasheets rather than a bench measurement -
repeat the testing yourself rather than taking any of it on trust.

I strongly recommend not using unbranded CR123 cells or buying them from less
reputable sources.

**Before building or installing anything from this repository, read the
[full disclaimer](../../DISCLAIMER.md).**

## Test status

| Item | Test | Result | Notes |
|---------|------|--------|-------|
| Input stage | 12V car input reverse polarity | NOT TESTED | S2Z1 conducts forward into S2F1, which should clear |
| Input stage | Pass-through drop VCAR to PPTRACKER, tracker asleep | NOT TESTED | v1.0 dropped ~0.5V here. Expected ~20 mV, the LTC4412 regulated forward drop |
| Input stage | Pass-through drop VCAR to PPTRACKER at 50 mA and 250 mA | NOT TESTED | Expected ~20 mV; S2Q1 is 15 mΩ so the regulation sets it, not R<sub>DS(on)</sub> |
| Input stage | ISO 7637-2 pulse 2a and 35V load dump ride-through | NOT TESTED | S2Z1 is the tracker's 33V TVS; S2C2 47 µF + S2C6 10 µF + S2C1 on VCAR. Check S2U1 (LTC4412HV) survives the clamp voltage |
| Switchover | Boost enables when the car rail falls below ~10.06V | NOT TESTED | S6U1 with the 6.8M/909k divider |
| Switchover | Boost disables when the car rail returns above ~11V | NOT TESTED | Depends on the cell voltage through S6R38/S6R39, see [Notes](#notes) |
| Switchover | PPTRACKER dip during handover under a forced LTE burst | NOT TESTED | Scope on J2 pin 4; S2C4 82 µF + S2C5 10 µF carry the gap |
| Switchover | Boost stays on through LTE bursts on a part-used cell | NOT TESTED | The v1.0 brownout; with the lockout gone, nothing but the car-sense comparator gates the boost |
| Boost | 9.6V output at S4C14/S4C15 | NOT TESTED | 1.202V x (1 + 909k/130k) = 9.61V; ~9V at J2 after S2D2 |
| Boost | Output current at a 2.5V cell | NOT TESTED | Burst Mode limits to ~190 mA at 2.5V in, ~300 mA at 3V in (datasheet curves) |
| Boost | Bode plot and load step | NOT TESTED | Compensation is the datasheet 2xAA to 12V example, not tuned for this operating point |
| Cell | Reversed cell is harmless | NOT TESTED | S3U1 blocks, VBAT sits at 0V, no backup |
| Cell | No current ever flows into the cell | NOT TESTED | Ammeter in series with the cell with the car present and during handover |
| Wake pulse | IGN pulse at boost start, car ignition wire connected and off | NOT TESTED | Failed on v1.0. Expected ~12V peak, above 8V for ~0.4 s, at J2 pin 5, with S1D1 blocking the car pulldown |
| Wake pulse | Car ignition reaches the tracker | NOT TESTED | J2 pin 5 should be one S1B drop below J1 pin 5 with the key on |
| Wake pulse | Sleeping tracker wakes and raises `backup power` | NOT TESTED | Needs firmware with `CONFIG_APP_BACKUP_SUPPLY=y` |
| Board | Idle draw from the cell, tracker asleep | NOT TESTED | Expected ~280 µA at a 2.9V cell, a few µA less than v1.0 without the lockout |
| Board | Parked drain from the car with the cell fitted | NOT TESTED | Expected ~15 µA (LTC4412HV ~13 µA + ~1.6 µA sense divider) on top of the tracker's own draw |
| Enclosure | Board and cell fit, lid clearance over the cell | NOT TESTED | 1.9 mm above the cell by the datasheet, see [enclosure/](enclosure/) |

## Features

 - Inline between the car harness and the tracker; CAN/K-line pass straight
   through, ignition through S1D1
 - ~15 µA quiescent draw from the car in pass-through (untested): ~13 µA
   LTC4412HV plus the ~1.6 µA sense divider
 - Same input protection parts as the tracker: 2A time-lag fuse, 33V TVS
   sized to ride out suppressed load dump
 - Reverse polarity protection
 - Ideal diode pass-through (LTC4412HV + P-channel FET) with a ~20 mV drop
   down to sleep current
 - Synchronous boost (LTC3122) with true output disconnect: the backup rail
   is 0V while the car is present, so the cell is only ever loaded during a
   cut
 - Nanopower comparator switchover (TLV3012, 2.8 µA) running straight from
   the cell: takes over below ~10.06V on the car rail
 - No cell lockout: the boost runs the cell until it can't. It's a
   consumable primary, so there is nothing to protect
 - Ignition wake pulse: a cut wakes a sleeping tracker immediately instead of
   at its next timed wake, and the tracker reports the cut
 - Estimated runtime on a 1500 mAh CR123A: ~200 days idle, ~58 days
   reporting hourly, ~19 days reporting every 15 minutes
 - 3D-printable two-part enclosure with the cell accessible under the lid

## Why a primary cell

A non-rechargeable cell rather than a rechargeable pack, for three reasons:

- No rechargeable 18650-format cell is rated for storage in a sun-parked car.
  The binding limit is storage temperature (+45 °C at best for Li-ion and LFP;
  +20 °C beyond three months for LTO) and a parked car reaches ~70 °C cabin
  air. A CR123A is rated to +60/+70 °C
- Cycle life is not worth paying for. The backup cycles a handful of times a
  year
- A primary turns silent ageing into an event. The boost regulates its output,
  so the tracker cannot see cell voltage - a rechargeable's calendar fade would
  be invisible until a real theft tested it. The only thing that consumes a
  primary is a power cut the tracker already alerts on, so *"did it get
  used?"* is answered in the server's alert log

The cell is a consumable. A `backup power` alert that runs for a long time
means the cell needs replacing, since it does not recover. Taking the cell
out is the storage mode.

### Ignition wake pulse

A sleeping tracker has three wake sources: its timer, the accelerometer and
ignition-present. A power cut touches none of them, so on its own it would be
found at the next timed wake. The module therefore fakes ignition for a
moment when it takes over.

The tracker's wake is level-triggered and the firmware latches it, so the
pulse only has to trip the interrupt. On an ignition wake the firmware reads
the rail first: in the backup band it is the module's pulse, so the unit
reports at once with the `backup power` alert and goes back to sleep.

The car holds its ignition wire at ground when the key is off, which sank
the pulse on v1.0. S1D1 sits in series between the car's ignition (J1 pin 5)
and the tracker's (J2 pin 5) so the pulse only drives the tracker's input.
The cost is one silicon diode drop on the ignition signal the tracker sees.

Limits: a second cut within the ~40 s VBOOST decay gives no pulse and falls
back to the timed wake.

## Board configuration

### Connectors

Both connectors are the tracker's Molex Micro-Fit 3.0 2x3 (43045-0600) with
the v3.4 pinout. J1 is the car harness, J2 goes to the tracker.

| Pin | J1 (car) | J2 (tracker) | Notes |
|-----|----------|--------------|-------|
| 1 | SPARE | SPARE | Unused by v3.4, not connected on this board |
| 2 | GND | GND | |
| 3 | CAN\_H / K | CAN\_H / K | Passed straight through |
| 4 | +12V | +12V (PPTRACKER) | Car through the ideal diode, or ~9V from the boost on backup |
| 5 | IGN | IGN | Through S1D1 (one diode drop); the wake pulse is injected on the J2 side |
| 6 | CAN\_L / L | CAN\_L / L | Passed straight through |

### Firmware

The tracker needs `CONFIG_APP_BACKUP_SUPPLY=y`. With it, a supply between
`CONFIG_APP_BACKUP_MIN_MV` (8500) and `CONFIG_APP_BACKUP_MAX_MV` (9700) with
the ignition off is treated as backup power rather than a flat car battery:
timed reports keep going, the 11.8V power-off and 12.0V sleep-safety rules
are bypassed, a `backup power` alert is raised once and a `car power
restored` alert when the rail comes back. Without the module a car battery in
that band is flat, so leave it off on trackers that do not have one.

### Cell

The board carries a Keystone 1051 CR123A holder and nothing else as a cell
input. Fit a CR123A from one of the three big brands (Energizer 123 /
EL123AP, Panasonic CR123A, Duracell DL123A): all are high-rate Li-MnO2 cells,
1500-1550 mAh, rated for the ~130 mA typical / ~250 mA worst-case bursts the
boost draws. Mark polarity on the silk and the lid; a reversed cell is
harmless but gives no backup.

Other primary chemistries (a 4.1V Tadiran TLM for installs above 60 °C, a
C-size Li-SOCl2 for long runtime) are **not** stuffing options on this board:
neither fits the CR123A holder, a 4.1V cell needs the comparator's supply
range and the boost input checked, and Li-SOCl2 wants a depassivation
bleed. Either is a sheet 3 change.

## Notes

 - **Nothing may ever charge the cell.** Do not add any path from VBOOST,
   PPTRACKER or VCAR to VBAT. If the LM66100 is ever replaced, the
   replacement must block reverse current
 - The LTC3122's exposed pad is PGND. Solder it
 - The tracker's flex-crack rule applies to every 1210 ceramic on this board
   (S2C5, S2C6, S4C14, S4C15): soft-termination parts only, kept away from
   board edges and mounting holes
 - S4R19 (boost FB top) is the one anti-sulfur resistor that matters: open, the
   LTC3122 runs to its ceiling, the same failure as the tracker buck's top
   resistor. S6R33 uses the same part for convenience. Everything else is
   plain thick film; use a non-sulfur foam under the lid over the cell
 - S2D2 and S5D3 must stay glass-passivated silicon (S1B), not Schottky. The
   wake pulse depends on VBOOST resting near 0V while the car is present, and
   the only thing holding it there against S2D2's reverse leakage is the 1.04M
   FB divider; a Schottky's leakage on a hot day parks VBOOST at a few volts
   and shortens the pulse
 - S6R32 is 0603 rather than 0402 for its 75V rating against load dump; it
   sits directly on the car rail
 - Car-sense return threshold: while on backup S6U1's output sits at the cell
   voltage, so IN+ = 1.242V + (VBAT - 1.242V) x 47k / 1.047M ≈ 1.30-1.33V
   for a 2.5-3.2V cell, and the rail has to come back above ~11.0-11.3V
   before the boost releases. That is below a resting car battery, but the
   threshold moves with the cell voltage - measure it before relying on it
 - The comparator, boost and wake-pulse switch all run from VBAT.
   With no cell fitted nothing on sheets 3-6 is powered, the LTC3122 has no
   VIN, and the board is a straight pass-through with an ideal diode in it
 - The `endpoint_off_grid` ERC warnings are cosmetic
 - S1J1/S1J2 pin 3 and pin 6 pass CAN/K-line straight through with no
   protection on this board; the tracker's own protection covers them
 - S2Z1's 33V standoff (36.7V minimum breakdown) is chosen to ride out a
   suppressed 35V load dump without conducting, the same reasoning as the
   tracker's input stage. A lower TVS would conduct for the whole 400 ms event
   and fail short. Do not lower it
 - Put the cell axis across the car (left-right) so road
   shock and braking act across the contacts rather than along them, and have
   the enclosure lid bear on the cell through a foam pad
 - Indicated voltages and tolerances are the minimum. Several of the
   "example" links are the tracker's parts, which may be tighter tolerance or
   higher voltage than the indicated minimum spec

## Bill of materials

| Item | Description | Specification | Example | Notes |
|------|-------------|---------------|---------|-------|
| S1J1 | Car harness connector | Molex Micro-Fit 3.0 2x03 43045-0600 | [43045-0600](https://uk.farnell.com/molex/43045-0600/conn-r-a-pcb-hdr-6pos-2row-3mm/dp/1012252) | |
| S1J2 | Tracker harness connector | Molex Micro-Fit 3.0 2x03 43045-0600 | [43045-0600](https://uk.farnell.com/molex/43045-0600/conn-r-a-pcb-hdr-6pos-2row-3mm/dp/1012252) | |
| S1D1 | Ignition isolation diode | S1B SMA, 100V 1A silicon | [S1B-13-F](https://www.ebay.co.uk/itm/178539943449) | IGN\_CAR to IGN; stops the car pulling the wake pulse down |
| S2F1 | 2A fuse, car input | 1206 2A time-lag | [0407002.WRA](https://www.ebay.co.uk/itm/178539955750) | Time-lag, same as tracker S2F1/S2F2 |
| S2Z1 | 33V TVS diode | PTVS33VS1UTR,115 SOD-123W | [PTVS33VS1UTR,115](https://www.ebay.co.uk/itm/178539916580) | Do not lower the standoff, see [Notes](#notes) |
| S2U1 | Ideal diode controller | LTC4412HV TSOT-23-6 | LTC4412HVIS6#PBF | ~13 µA Iq; replaces v1.0's LM74610 |
| S2Q1 | Ideal diode MOSFET | SQD50P06-15L DPAK, P-channel, -60V, 15 mΩ, AEC-Q101 | SQD50P06-15L_GE3 | |
| S2D2 | OR-ing diode from VBOOST | S1B SMA, 100V 1A silicon | [S1B-13-F](https://www.ebay.co.uk/itm/178539943449) | Silicon, not Schottky |
| S2C1 | 100nF capacitor | 0402 >= 50V 10% X7R | [GRM155R71H104KE14D](https://uk.farnell.com/murata/grm155r71h104ke14d/cap-0-1-f-50v-10-x7r-0402/dp/2611912) | |
| S2C2 | 47uF hybrid polymer electrolytic, VCAR | 63V SMD, 8 x 10.2 mm | EEH-ZC1J470P | |
| S2C4 | 82uF hybrid polymer electrolytic, PPTRACKER | 63V SMD, 10 x 10.5 mm | HHXC630ARA820MJA0G | |
| S2C5 | 10uF capacitor, PPTRACKER | 1210 >= 50V 10% X7R SOFT TERMINATION | [MCJCU32MLB7106KPPDT1](https://uk.farnell.com/taiyo-yuden/mcjcu32mlb7106kppdt1/capacitor-mlcc-10uf-50v-x7r-1210/dp/4666637) | Flex-crack rule |
| S2C6 | 10uF capacitor, VCAR | 1210 >= 50V 10% X7R SOFT TERMINATION | [MCJCU32MLB7106KPPDT1](https://uk.farnell.com/taiyo-yuden/mcjcu32mlb7106kppdt1/capacitor-mlcc-10uf-50v-x7r-1210/dp/4666637) | Flex-crack rule |
| S3BT1 | CR123A holder | Keystone 1051, through-hole | [1051](https://uk.farnell.com/keystone/1051/battery-holder-cr123a-through/dp/3759162) | Pin 1 is + |
| S3F1 | 2A fuse, cell | 1206 2A time-lag | [0407002.WRA](https://www.ebay.co.uk/itm/178539955750) | Not a PTC: the 2A 0ZCJ part is only rated 6V |
| S3U1 | Ideal diode | LM66100 SC70-6 | [LM66100DCKR](https://www.ebay.co.uk/itm/178539979903) | Same part as tracker S11U1 |
| S3C1 | 2.2uF capacitor | 0603 >= 25V 10% X7R | [GRM188Z71E225KE43D](https://uk.farnell.com/murata/grm188z71e225ke43d/cap-mlcc-2-2uf-x7r-25v-0603/dp/4335731) | |
| S4U5 | Boost converter | LTC3122EMSE MSOP-12-EP | [LTC3122EMSE#PBF](https://www.digikey.co.uk/en/products/detail/analog-devices-inc/LTC3122EMSE-PBF/3516527) | Solder the exposed pad, it is PGND |
| S4L1 | Inductor | XFL4020-222ME 2.2uH | [XFL4020-222MEC](https://uk.farnell.com/coilcraft/xfl4020-222mec/inductor-2-2uh-8a-20-pwr-38mhz/dp/2289216) | Same part as tracker S6L1 |
| S4C7 | 47uF input capacitor | 1210 >= 10V 20% X7R | [CL32B476MPJNNNE](https://uk.farnell.com/semco/cl32b476mpjnnne/cap-mlcc-47uf-10vdc-x7r-1210/dp/5109745) | Same part as tracker S11C1 |
| S4C8 | 47uF input capacitor | 1210 >= 10V 20% X7R | [CL32B476MPJNNNE](https://uk.farnell.com/semco/cl32b476mpjnnne/cap-mlcc-47uf-10vdc-x7r-1210/dp/5109745) | |
| S4C9 | 100nF CAP pin capacitor | 0402 >= 50V 10% X7R | [GRM155R71H104KE14D](https://uk.farnell.com/murata/grm155r71h104ke14d/cap-0-1-f-50v-10-x7r-0402/dp/2611912) | CAP to VOUT |
| S4C11 | 560pF compensation capacitor | 0402 >= 50V 5% C0G/NP0 | Any 0402 560pF C0G | |
| S4C12 | 10pF compensation capacitor | 0402 >= 50V 5% C0G/NP0 | [GRM1555C1H100JA01D](https://uk.farnell.com/murata/grm1555c1h100ja01d/cap-mlcc-10pf-c0g-np0-50v-0402/dp/4326648) | |
| S4C13 | 4.7uF VCC capacitor | 0805 >= 50V 10% X7R | [GRM21BZ71H475KE15K](https://uk.farnell.com/murata/grm21bz71h475ke15k/cap-4-7uf-50v-mlcc-0805/dp/3582887) | |
| S4C14 | 10uF output capacitor | 1210 >= 50V 10% X7R SOFT TERMINATION | [MCJCU32MLB7106KPPDT1](https://uk.farnell.com/taiyo-yuden/mcjcu32mlb7106kppdt1/capacitor-mlcc-10uf-50v-x7r-1210/dp/4666637) | 50V rating keeps ~9 µF at 9.6V |
| S4C15 | 10uF output capacitor | 1210 >= 50V 10% X7R SOFT TERMINATION | [MCJCU32MLB7106KPPDT1](https://uk.farnell.com/taiyo-yuden/mcjcu32mlb7106kppdt1/capacitor-mlcc-10uf-50v-x7r-1210/dp/4666637) | |
| S4R14 | Oscillator frequency resistor | 0402 57.6K 1% | Any 0402 57.6K 1% | 57.6K == 1MHz |
| S4R18 | Compensation resistor | 0402 200K 1% | Any 0402 200K 1% | |
| S4R19 | FB divider top | 0402 909K 1% ANTI-SULFUR AEC-Q200 | [AF0402FR-07909KL](https://www.digikey.co.uk/en/products/detail/yageo/AF0402FR-07909KL/16992580) | VOUT == 1.202V x (1 + S4R19/S4R20) == 9.61V, keep 1%, anti-sulfur required |
| S4R20 | FB divider bottom | 0402 130K 1% | Any 0402 130K 1% | Keep 1% |
| S5C17 | Wake pulse flying capacitor | 0805 >= 50V 10% X7R | [GRM21BZ71H475KE15K](https://uk.farnell.com/murata/grm21bz71h475ke15k/cap-4-7uf-50v-mlcc-0805/dp/3582887) | 50V part so it keeps ~4 µF at 12V bias |
| S5Q6 | Wake pulse switch | 2N7002 SOT-23 | [2N7002](https://uk.farnell.com/multicomp-pro/2n7002/mosfet-n-ch-60v-0-115a-sot-23/dp/4295174) | Any vendor |
| S5Q7 | Wake pulse high-side PNP | BC856B SOT-23 | [BC856BLT1G](https://www.ebay.co.uk/itm/178539823265) | |
| S5D3 | Wake pulse isolation diode | S1B SMA, 100V 1A silicon | [S1B-13-F](https://www.ebay.co.uk/itm/178539943449) | Silicon, not Schottky |
| S5R21 | Wake pulse series resistor | 0402 10K 5% | [CRCW040210K0FKED](https://uk.farnell.com/vishay/crcw040210k0fked/res-10k-1-0-063w-0402-thick-film/dp/1469669) | |
| S5R22 | Wake capacitor charge resistor | 0402 1M 1% | [ERJ2RKF1004X](https://uk.farnell.com/panasonic/erj2rkf1004x/res-1m-1-0-1w-0402-thick-film/dp/2302957) | |
| S5R24 | Q7 base drive resistor | 0402 1M 1% | [ERJ2RKF1004X](https://uk.farnell.com/panasonic/erj2rkf1004x/res-1m-1-0-1w-0402-thick-film/dp/2302957) | |
| S5R25 | Q7 base-emitter hold-off | 0402 1M 1% | [ERJ2RKF1004X](https://uk.farnell.com/panasonic/erj2rkf1004x/res-1m-1-0-1w-0402-thick-film/dp/2302957) | |
| S6U1 | Car-rail sense comparator | TLV3012 SOT-23-6 | [TLV3012AIDBVR](https://www.digikey.co.uk/en/products/detail/texas-instruments/TLV3012AIDBVR/1678203) | 1.242V reference, 2.8 µA |
| S6R32 | Car-rail divider top | 0603 6.8M 1% | Any 0603 6.8M 1% 75V | 0603 for the 75V rating |
| S6R33 | Car-rail divider bottom | 0402 909K 1% ANTI-SULFUR AEC-Q200 | [AF0402FR-07909KL](https://www.digikey.co.uk/en/products/detail/yageo/AF0402FR-07909KL/16992580) | Same part as S4R19; anti-sulfur not required here |
| S6R38 | Car-sense hysteresis | 0402 47K 5% | [ERJ2RKF4702X](https://uk.farnell.com/panasonic/erj2rkf4702x/res-47k-1-0-1w-0402-thick-film/dp/2302806) | |
| S6R39 | Car-sense hysteresis feedback | 0402 1M 1% | [ERJ2RKF1004X](https://uk.farnell.com/panasonic/erj2rkf1004x/res-1m-1-0-1w-0402-thick-film/dp/2302957) | |
| S6C1 | IN- filter capacitor | 0402 >= 50V 5% C0G/NP0 | [0402N101J500CT](https://uk.farnell.com/multicomp-pro/0402n101j500ct/cap-100pf-50v-5-c0g-np0-0402/dp/2496792) | |
| S6C2 | Comparator decoupling | 0402 >= 50V 10% X7R | [GRM155R71H104KE14D](https://uk.farnell.com/murata/grm155r71h104ke14d/cap-0-1-f-50v-10-x7r-0402/dp/2611912) | |
| - | CR123A cell | Li-MnO2 3.0V 1500-1550 mAh, branded | Energizer EL123AP / Panasonic CR123A / Duracell DL123A | Consumable; buy from a distributor |

## Parts list

Aggregated from the bill of materials above. Quantities are per board.

| Item | Quantity | Specification | Example | Notes |
|------|----------|---------------|---------|-------|
| Molex Micro-Fit 3.0 2x03 PCB connector | 2 | 43045-0600 | [43045-0600](https://uk.farnell.com/molex/43045-0600/conn-r-a-pcb-hdr-6pos-2row-3mm/dp/1012252) | |
| CR123A holder | 1 | Keystone 1051 | [1051](https://uk.farnell.com/keystone/1051/battery-holder-cr123a-through/dp/3759162) | |
| 2A fuse | 2 | 1206 2A time-lag | [0407002.WRA](https://www.ebay.co.uk/itm/178539955750) | 1x car input, 1x cell |
| 33V TVS diode | 1 | PTVS33VS1UTR,115 SOD-123W | [PTVS33VS1UTR,115](https://www.ebay.co.uk/itm/178539916580) | |
| S1B rectifier | 3 | SMA 100V 1A silicon | [S1B-13-F](https://www.ebay.co.uk/itm/178539943449) | |
| Ideal diode controller | 1 | LTC4412HV TSOT-23-6 | LTC4412HVIS6#PBF | |
| P-channel MOSFET, DPAK | 1 | SQD50P06-15L -60V 15 mΩ | SQD50P06-15L_GE3 | |
| Ideal diode | 1 | LM66100 SC70-6 | [LM66100DCKR](https://www.ebay.co.uk/itm/178539979903) | |
| Boost converter | 1 | LTC3122EMSE MSOP-12-EP | [LTC3122EMSE#PBF](https://www.digikey.co.uk/en/products/detail/analog-devices-inc/LTC3122EMSE-PBF/3516527) | |
| Nanopower comparator | 1 | TLV3012 SOT-23-6 | [TLV3012AIDBVR](https://www.digikey.co.uk/en/products/detail/texas-instruments/TLV3012AIDBVR/1678203) | |
| Inductor | 1 | XFL4020-222ME 2.2uH | [XFL4020-222MEC](https://uk.farnell.com/coilcraft/xfl4020-222mec/inductor-2-2uh-8a-20-pwr-38mhz/dp/2289216) | |
| 2N7002 MOSFET | 1 | SOT-23 | [2N7002](https://uk.farnell.com/multicomp-pro/2n7002/mosfet-n-ch-60v-0-115a-sot-23/dp/4295174) | |
| PNP transistor | 1 | BC856B SOT-23 | [BC856BLT1G](https://www.ebay.co.uk/itm/178539823265) | |
| 47uF 63V hybrid polymer electrolytic | 1 | 8 x 10.2 mm SMD | EEH-ZC1J470P | |
| 82uF 63V hybrid polymer electrolytic | 1 | 10 x 10.5 mm SMD | HHXC630ARA820MJA0G | |
| 10uF 1210 soft-termination capacitor | 4 | 1210 >= 50V 10% X7R SOFT TERMINATION | [MCJCU32MLB7106KPPDT1](https://uk.farnell.com/taiyo-yuden/mcjcu32mlb7106kppdt1/capacitor-mlcc-10uf-50v-x7r-1210/dp/4666637) | |
| 47uF 1210 capacitor | 2 | 1210 >= 10V 20% X7R | [CL32B476MPJNNNE](https://uk.farnell.com/semco/cl32b476mpjnnne/cap-mlcc-47uf-10vdc-x7r-1210/dp/5109745) | |
| 4.7uF 0805 capacitor | 2 | 0805 >= 50V 10% X7R | [GRM21BZ71H475KE15K](https://uk.farnell.com/murata/grm21bz71h475ke15k/cap-4-7uf-50v-mlcc-0805/dp/3582887) | |
| 2.2uF 0603 capacitor | 1 | 0603 >= 25V 10% X7R | [GRM188Z71E225KE43D](https://uk.farnell.com/murata/grm188z71e225ke43d/cap-mlcc-2-2uf-x7r-25v-0603/dp/4335731) | |
| 100nF 0402 capacitor | 3 | 0402 >= 50V 10% X7R | [GRM155R71H104KE14D](https://uk.farnell.com/murata/grm155r71h104ke14d/cap-0-1-f-50v-10-x7r-0402/dp/2611912) | |
| 560pF 0402 capacitor | 1 | 0402 >= 50V 5% C0G/NP0 | Any 0402 560pF C0G | |
| 100pF 0402 capacitor | 1 | 0402 >= 50V 5% C0G/NP0 | [0402N101J500CT](https://uk.farnell.com/multicomp-pro/0402n101j500ct/cap-100pf-50v-5-c0g-np0-0402/dp/2496792) | |
| 10pF 0402 capacitor | 1 | 0402 >= 50V 5% C0G/NP0 | [GRM1555C1H100JA01D](https://uk.farnell.com/murata/grm1555c1h100ja01d/cap-mlcc-10pf-c0g-np0-50v-0402/dp/4326648) | |
| 909K 0402 anti-sulfur resistor | 2 | 0402 909K 1% ANTI-SULFUR AEC-Q200 | [AF0402FR-07909KL](https://www.digikey.co.uk/en/products/detail/yageo/AF0402FR-07909KL/16992580) | S4R19 (required anti-sulfur), S6R33 |
| 1M 0402 resistor | 4 | 0402 1M 1% | [ERJ2RKF1004X](https://uk.farnell.com/panasonic/erj2rkf1004x/res-1m-1-0-1w-0402-thick-film/dp/2302957) | |
| 130K 0402 resistor | 1 | 0402 130K 1% | Any 0402 130K 1% | |
| 200K 0402 resistor | 1 | 0402 200K 1% | Any 0402 200K 1% | |
| 57.6K 0402 resistor | 1 | 0402 57.6K 1% | Any 0402 57.6K 1% | |
| 47K 0402 resistor | 1 | 0402 47K 5% | [ERJ2RKF4702X](https://uk.farnell.com/panasonic/erj2rkf4702x/res-47k-1-0-1w-0402-thick-film/dp/2302806) | |
| 10K 0402 resistor | 1 | 0402 10K 5% | [CRCW040210K0FKED](https://uk.farnell.com/vishay/crcw040210k0fked/res-10k-1-0-063w-0402-thick-film/dp/1469669) | |
| 6.8M 0603 resistor | 1 | 0603 6.8M 1% 75V | Any 0603 6.8M 1% 75V | |
| CR123A cell | 1 | Li-MnO2 3.0V, branded | Energizer EL123AP / Panasonic CR123A / Duracell DL123A | Consumable |
| M2 heat-set insert | 4 | Ø3.2 hole, 4.6 mm | - | Enclosure |
| M2 x 12 mm screw | 4 | - | - | Enclosure; 12 mm, not the tracker enclosure's 10 mm |

## Enclosure

A 3D-printable two-part enclosure is in [enclosure/](enclosure/): 68.9 x
42.0 x 31.0 mm outer, same construction as the v3.3 tracker enclosure (flat
butt joint, M2 heat-set inserts in the top, screws in from underneath). The
cell is accessible with the top removed. Print in PETG or ABS - PLA softens
around 60 °C, which a parked car reaches. Unprinted and unvalidated; see its
[README](enclosure/README.md).
