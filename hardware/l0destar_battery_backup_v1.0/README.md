# l0destar battery backup v1.0

## Overview

**NOTE: THIS HAS NOT YET BEEN BUILT OR TESTED, USE AT YOUR OWN RISK**

- This is a prototype inline battery backup module for the l0destar vehicle
  tracker. It sits in the harness between the car and the tracker and holds
  the tracker's supply at ~9V from a single CR123A lithium primary cell when
  car power is cut
- It is designed to be hand-solderable (hot air required). Every active part
  is in a leaded package: MSOP-12, SOT-23, SC70, VSSOP-8, DPAK
- The cell is a **non-rechargeable** primary in a board-mounted holder.
  Nothing on the board charges it. See [Why a primary cell](#why-a-primary-cell)
- Pin-compatible with the v3.4 tracker's 2x3 Micro-Fit harness: J1 takes the
  car harness, J2 goes to the tracker, and the CAN/K-line and ignition pins pass
  straight through
- 4-layer 63.1 x 36.2 mm board, JLC04161H-7628 stack-up, four M2 mounting
  holes. Schematic ERC and PCB DRC are clean apart from the expected items
  listed under [Notes](#notes)

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
| Input stage | 12V car input reverse polarity | NOT TESTED | S1U1/S1Q1 ideal diode blocks; S1Z1 conducts forward into S1F1, which should clear |
| Input stage | Pass-through drop VCAR to PPTRACKER at 50 mA and 250 mA | NOT TESTED | Expected tens of mV through S1Q1 (27 mΩ), with brief body-diode drops while the LM74610 charge pump recharges |
| Input stage | ISO 7637-2 pulse 2a and 35V load dump ride-through | NOT TESTED | S1Z1 is the tracker's 33V TVS; S1C2 47 µF + S1C6 10 µF + S1C1 on VCAR |
| Switchover | Boost enables when the car rail falls below ~10.06V | NOT TESTED | S5U1 with the 6.8M/909k divider |
| Switchover | Boost disables when the car rail returns above ~11V | NOT TESTED | Depends on the cell voltage through S5R38/S5R39, see [Notes](#notes) |
| Switchover | PPTRACKER dip during handover under a forced LTE burst | NOT TESTED | Scope on J2 pin 4; S1C4 100 µF + S1C5 10 µF carry the gap |
| Boost | 9.6V output at S4C14/S4C15 | NOT TESTED | 1.202V x (1 + 909k/130k) = 9.61V; ~9V at J2 after S1D2 |
| Boost | Output current at a 2.5V cell | NOT TESTED | Burst Mode limits to ~190 mA at 2.5V in, ~300 mA at 3V in (datasheet curves) |
| Boost | Bode plot and load step | NOT TESTED | Compensation is the datasheet 2xAA to 12V example, not tuned for this operating point |
| Cell lockout | Boost held off below 2.45V, released above 2.52V | NOT TESTED | S5U4 with 1M/1M divider and 33M feedback; test on a bench supply in place of the cell |
| Cell lockout | Re-enable paced ~0.3 s | NOT TESTED | S5R41/S5C7; S5D1 makes the disable immediate |
| Cell | Reversed cell is harmless | NOT TESTED | S3U1 blocks, VBAT sits at 0V, no backup |
| Cell | No current ever flows into the cell | NOT TESTED | Ammeter in series with the cell with the car present and during handover |
| Wake pulse | IGN pulse at boost start | NOT TESTED | Expected ~12V peak, above 8V for ~0.4 s, at J2 pin 5 |
| Wake pulse | Sleeping tracker wakes and raises `backup power` | NOT TESTED | Needs firmware with `CONFIG_APP_BACKUP_SUPPLY=y` |
| Board | Idle draw from the cell, tracker asleep | NOT TESTED | Expected ~280 µA at a 2.9V cell |
| Board | Parked drain from the car with the cell fitted | NOT TESTED | Expected ~1.6 µA from the 7.7M sense divider on top of the tracker's own draw |
| Enclosure | Board and cell fit, lid clearance over the cell | NOT TESTED | 1.9 mm above the cell by the datasheet, see [enclosure/](enclosure/) |

## Features

 - Inline between the car harness and the tracker; CAN/K-line and ignition
   pass straight through
 - ~5-10uA quiescent current in pass-through operation (untested)
 - Same input protection parts as the tracker: 2A time-lag fuse, 33V TVS
   sized to ride out suppressed load dump
 - Reverse polarity protection
 - Synchronous boost (LTC3122) with true output disconnect: the backup rail
   is 0V while the car is present, so the cell is only ever loaded during a
   cut
 - Nanopower comparator switchover (2 x TLV3012, 2.8 µA each) running
   straight from the cell: takes over below ~10.06V on the car rail
 - Cell undervoltage lockout at 2.45V with a paced restart, so a tired cell
   is stopped a few percent above its rated end voltage rather than flattened
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

Limits: a second cut within the ~40 s VBOOST decay, or a cut with the key on
and the ignition wire loaded by the car, gives no pulse; both fall back to
the timed wake.

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
| 5 | IGN | IGN | Passed straight through; the wake pulse is injected here |
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
neither fits the CR123A holder, a 4.1V cell exceeds the AND gate's 3.6V
supply limit and needs a different lockout divider, and Li-SOCl2 wants a
depassivation bleed. Either is a sheet 2 change.

## Notes

 - **Nothing may ever charge the cell.** Do not add any path from VBOOST,
   PPTRACKER or VCAR to VBAT. If the LM66100 is ever replaced, the
   replacement must block reverse current
 - The LTC3122's exposed pad is PGND. Solder it
 - The tracker's flex-crack rule applies to every 1210 ceramic on this board
   (S1C5, S1C6, S4C14, S4C15): soft-termination parts only, kept away from
   board edges and mounting holes
 - S4R19 (boost FB top) is the one anti-sulfur resistor that matters: open, the
   LTC3122 runs to its ceiling, the same failure as the tracker buck's top
   resistor. S5R33 uses the same part for convenience. Everything else is
   plain thick film; use a non-sulfur foam under the lid over the cell
 - S1D2 and S4D3 must stay glass-passivated silicon (S1B), not Schottky. The
   wake pulse depends on VBOOST resting near 0V while the car is present, and
   the only thing holding it there against S1D2's reverse leakage is the 1.04M
   FB divider; a Schottky's leakage on a hot day parks VBOOST at a few volts
   and shortens the pulse
 - S5R32 is 0603 rather than 0402 for its 75V rating against load dump; it
   sits directly on the car rail
 - Car-sense return threshold: while on backup S5U1's output sits at the cell
   voltage, so IN+ = 1.242V + (VBAT - 1.242V) x 47k / 1.047M ≈ 1.30-1.33V
   for a 2.5-3.2V cell, and the rail has to come back above ~11.0-11.3V
   before the boost releases. That is below a resting car battery, but the
   threshold moves with the cell voltage - measure it before relying on it
 - The comparators, AND gate, boost and wake-pulse switch all run from VBAT.
   With no cell fitted nothing on sheets 2-4 is powered, the LTC3122 has no
   VIN, and the board is a straight pass-through with an ideal diode in it
 - The two expected ERC errors are J1/J2 pin 1 (SPARE, intentionally
   unconnected) and the PWR\_FLAG on VBAT. The `endpoint_off_grid` warnings
   are cosmetic
 - S1J1/S1J2 pin 3 and pin 6 pass CAN/K-line straight through with no
   protection on this board; the tracker's own protection covers them
 - S1Z1's 33V standoff (36.7V minimum breakdown) is chosen to ride out a
   suppressed 35V load dump without conducting, the same reasoning as the
   tracker's input stage. A lower TVS would conduct for the whole 400 ms event
   and fail short. Do not lower it
 - Put the cell axis across the car (left-right) so road
   shock and braking act across the contacts rather than along them, and have
   the enclosure lid bear on the cell through a foam pad
 - IRLR2905TRPBF was on backorder at DigiKey when the links below were
   checked. Any logic-level N-channel DPAK with Vds >= 55V and Rds(on) below
   ~50 mΩ will do in its place; the LM74610 gate drive is ~5-6V
 - Indicated voltages and tolerances are the minimum. Several of the
   "example" links are the tracker's parts, which may be tighter tolerance or
   higher voltage than the indicated minimum spec

## Bill of materials

| Item | Description | Specification | Example | Notes |
|------|-------------|---------------|---------|-------|
| S1J1 | Car harness connector | Molex Micro-Fit 3.0 2x03 43045-0600 | [43045-0600](https://uk.farnell.com/molex/43045-0600/conn-r-a-pcb-hdr-6pos-2row-3mm/dp/1012252) | |
| S1J2 | Tracker harness connector | Molex Micro-Fit 3.0 2x03 43045-0600 | [43045-0600](https://uk.farnell.com/molex/43045-0600/conn-r-a-pcb-hdr-6pos-2row-3mm/dp/1012252) | |
| S1F1 | 2A fuse, car input | 1206 2A time-lag | [0407002.WRA](https://www.digikey.co.uk/en/products/detail/littelfuse-inc/0407002-WRA/14640147) | Time-lag, same as tracker S2F1/S2F2 |
| S1Z1 | 33V TVS diode | PTVS33VS1UTR,115 SOD-123W | [PTVS33VS1UTR,115](https://uk.farnell.com/nexperia/ptvs33vs1utr-115/tvs-diode-aecq101-unidir-33v-400w/dp/3440137) | Do not lower the standoff, see [Notes](#notes) |
| S1U1 | Smart diode controller | LM74610-Q1 VSSOP-8 | [LM74610QDGKRQ1](https://www.digikey.co.uk/en/products/detail/texas-instruments/LM74610QDGKRQ1/5702219) | Zero Iq, floating |
| S1Q1 | Ideal diode MOSFET | IRLR2905 DPAK, 55V, 27 mΩ | [IRLR2905TRPBF](https://www.digikey.co.uk/en/products/detail/infineon-technologies/IRLR2905TRPBF/811417) | Backordered at DigiKey when checked, see Notes |
| S1D2 | OR-ing diode from VBOOST | S1B SMA, 100V 1A silicon | [S1B-13-F](https://www.digikey.co.uk/en/products/detail/diodes-incorporated/S1B-13-F/725026) | Silicon, not Schottky |
| S1C1 | 100nF capacitor | 0402 >= 50V 10% X7R | [GRM155R71H104KE14D](https://uk.farnell.com/murata/grm155r71h104ke14d/cap-0-1-f-50v-10-x7r-0402/dp/2611912) | |
| S1C2 | 47uF electrolytic, VCAR | 63V SMD, 105 °C | [EEE-FK1J470P](https://www.digikey.co.uk/en/products/detail/panasonic-electronic-components/EEE-FK1J470P/765992) | |
| S1C3 | 2.2uF charge-pump capacitor | 0603 >= 25V 10% X7R | [GRM188Z71E225KE43D](https://uk.farnell.com/murata/grm188z71e225ke43d/cap-mlcc-2-2uf-x7r-25v-0603/dp/4335731) | |
| S1C4 | 100uF electrolytic, PPTRACKER | 63V SMD, 105 °C | [EEE-FK1J101P](https://www.digikey.co.uk/en/products/detail/panasonic-electronic-components/EEE-FK1J101P/765988) | |
| S1C5 | 10uF capacitor, PPTRACKER | 1210 >= 50V 10% X7R SOFT TERMINATION | [MCJCU32MLB7106KPPDT1](https://uk.farnell.com/taiyo-yuden/mcjcu32mlb7106kppdt1/capacitor-mlcc-10uf-50v-x7r-1210/dp/4666637) | Flex-crack rule |
| S1C6 | 10uF capacitor, VCAR | 1210 >= 50V 10% X7R SOFT TERMINATION | [MCJCU32MLB7106KPPDT1](https://uk.farnell.com/taiyo-yuden/mcjcu32mlb7106kppdt1/capacitor-mlcc-10uf-50v-x7r-1210/dp/4666637) | Flex-crack rule |
| S3BT1 | CR123A holder | Keystone 1051, through-hole | [1051](https://uk.farnell.com/keystone/1051/battery-holder-cr123a-through/dp/3759162) | Pin 1 is + |
| S3F1 | 2A fuse, cell | 1206 2A time-lag | [0407002.WRA](https://www.digikey.co.uk/en/products/detail/littelfuse-inc/0407002-WRA/14640147) | Not a PTC: the 2A 0ZCJ part is only rated 6V |
| S3U1 | Ideal diode | LM66100 SC70-6 | [LM66100DCKR](https://www.digikey.co.uk/en/products/detail/texas-instruments/LM66100DCKR/10273183) | Same part as tracker S11U1 |
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
| S4C17 | Wake pulse flying capacitor | 0805 >= 50V 10% X7R | [GRM21BZ71H475KE15K](https://uk.farnell.com/murata/grm21bz71h475ke15k/cap-4-7uf-50v-mlcc-0805/dp/3582887) | 50V part so it keeps ~4 µF at 12V bias |
| S4Q6 | Wake pulse switch | 2N7002 SOT-23 | [2N7002](https://uk.farnell.com/multicomp-pro/2n7002/mosfet-n-ch-60v-0-115a-sot-23/dp/4295174) | Any vendor |
| S4Q7 | Wake pulse high-side PNP | BC856B SOT-23 | [BC856BLT1G](https://uk.farnell.com/onsemi/bc856blt1g/transistor-pnp-sot-23/dp/1459043) | |
| S4D3 | Wake pulse isolation diode | S1B SMA, 100V 1A silicon | [S1B-13-F](https://www.digikey.co.uk/en/products/detail/diodes-incorporated/S1B-13-F/725026) | Silicon, not Schottky |
| S4R21 | Wake pulse series resistor | 0402 10K 5% | [CRCW040210K0FKED](https://uk.farnell.com/vishay/crcw040210k0fked/res-10k-1-0-063w-0402-thick-film/dp/1469669) | |
| S4R22 | Wake capacitor charge resistor | 0402 1M 1% | [ERJ2RKF1004X](https://uk.farnell.com/panasonic/erj2rkf1004x/res-1m-1-0-1w-0402-thick-film/dp/2302957) | |
| S4R24 | Q7 base drive resistor | 0402 1M 1% | [ERJ2RKF1004X](https://uk.farnell.com/panasonic/erj2rkf1004x/res-1m-1-0-1w-0402-thick-film/dp/2302957) | |
| S4R25 | Q7 base-emitter hold-off | 0402 1M 1% | [ERJ2RKF1004X](https://uk.farnell.com/panasonic/erj2rkf1004x/res-1m-1-0-1w-0402-thick-film/dp/2302957) | |
| S5U1 | Car-rail sense comparator | TLV3012 SOT-23-6 | [TLV3012AIDBVR](https://www.digikey.co.uk/en/products/detail/texas-instruments/TLV3012AIDBVR/1678203) | 1.242V reference, 2.8 µA |
| S5U4 | Cell lockout comparator | TLV3012 SOT-23-6 | [TLV3012AIDBVR](https://www.digikey.co.uk/en/products/detail/texas-instruments/TLV3012AIDBVR/1678203) | |
| S5U3 | AND gate | SN74AUP1G08 SOT-23-5 | [SN74AUP1G08DBVR](https://www.digikey.co.uk/en/products/detail/texas-instruments/SN74AUP1G08DBVR/864078) | Schmitt inputs; do not substitute an LVC part |
| S5D1 | Pacing discharge diode | PMEG2010ER SOD-123W | [PMEG2010ER,115](https://uk.farnell.com/nexperia/pmeg2010er-115/diode-rect-sch-20v-1a-sod123w/dp/1907681) | |
| S5R32 | Car-rail divider top | 0603 6.8M 1% | Any 0603 6.8M 1% 75V | 0603 for the 75V rating |
| S5R33 | Car-rail divider bottom | 0402 909K 1% ANTI-SULFUR AEC-Q200 | [AF0402FR-07909KL](https://www.digikey.co.uk/en/products/detail/yageo/AF0402FR-07909KL/16992580) | Same part as S4R19; anti-sulfur not required here |
| S5R38 | Car-sense hysteresis | 0402 47K 5% | [ERJ2RKF4702X](https://uk.farnell.com/panasonic/erj2rkf4702x/res-47k-1-0-1w-0402-thick-film/dp/2302806) | |
| S5R39 | Car-sense hysteresis feedback | 0402 1M 1% | [ERJ2RKF1004X](https://uk.farnell.com/panasonic/erj2rkf1004x/res-1m-1-0-1w-0402-thick-film/dp/2302957) | |
| S5R36 | Cell lockout divider top | 0402 1M 1% | [ERJ2RKF1004X](https://uk.farnell.com/panasonic/erj2rkf1004x/res-1m-1-0-1w-0402-thick-film/dp/2302957) | Keep 1%, sets 2.45V/2.52V with S5R37/S5R40 |
| S5R37 | Cell lockout divider bottom | 0402 1M 1% | [ERJ2RKF1004X](https://uk.farnell.com/panasonic/erj2rkf1004x/res-1m-1-0-1w-0402-thick-film/dp/2302957) | Keep 1% |
| S5R40 | Cell lockout hysteresis | 0603 33M 5% | [MCHVR03JTHX3305](https://uk.farnell.com/multicomp-pro/mchvr03jthx3305/res-33m-5-0-1w-0603-thick-film/dp/2825824) | Same part as tracker S11R3 |
| S5R41 | Pacing resistor | 0402 1M 1% | [ERJ2RKF1004X](https://uk.farnell.com/panasonic/erj2rkf1004x/res-1m-1-0-1w-0402-thick-film/dp/2302957) | With S5C7: ~0.3 s |
| S5C1 | IN- filter capacitor | 0402 >= 50V 5% C0G/NP0 | [0402N101J500CT](https://uk.farnell.com/multicomp-pro/0402n101j500ct/cap-100pf-50v-5-c0g-np0-0402/dp/2496792) | |
| S5C6 | IN+ filter capacitor | 0402 >= 50V 5% C0G/NP0 | [0402N101J500CT](https://uk.farnell.com/multicomp-pro/0402n101j500ct/cap-100pf-50v-5-c0g-np0-0402/dp/2496792) | |
| S5C2 | Comparator decoupling | 0402 >= 50V 10% X7R | [GRM155R71H104KE14D](https://uk.farnell.com/murata/grm155r71h104ke14d/cap-0-1-f-50v-10-x7r-0402/dp/2611912) | Place between S5U1 and S5U4 |
| S5C5 | AND gate decoupling | 0402 >= 50V 10% X7R | [GRM155R71H104KE14D](https://uk.farnell.com/murata/grm155r71h104ke14d/cap-0-1-f-50v-10-x7r-0402/dp/2611912) | |
| S5C7 | Pacing capacitor | 0603 220nF >= 50V 10% X7R | Any 0603 220nF 50V X7R | |
| - | CR123A cell | Li-MnO2 3.0V 1500-1550 mAh, branded | Energizer EL123AP / Panasonic CR123A / Duracell DL123A | Consumable; buy from a distributor |

## Parts list

Aggregated from the bill of materials above. Quantities are per board.

| Item | Quantity | Specification | Example | Notes |
|------|----------|---------------|---------|-------|
| Molex Micro-Fit 3.0 2x03 PCB connector | 2 | 43045-0600 | [43045-0600](https://uk.farnell.com/molex/43045-0600/conn-r-a-pcb-hdr-6pos-2row-3mm/dp/1012252) | |
| CR123A holder | 1 | Keystone 1051 | [1051](https://uk.farnell.com/keystone/1051/battery-holder-cr123a-through/dp/3759162) | |
| 2A fuse | 2 | 1206 2A time-lag | [0407002.WRA](https://www.digikey.co.uk/en/products/detail/littelfuse-inc/0407002-WRA/14640147) | 1x car input, 1x cell |
| 33V TVS diode | 1 | PTVS33VS1UTR,115 SOD-123W | [PTVS33VS1UTR,115](https://uk.farnell.com/nexperia/ptvs33vs1utr-115/tvs-diode-aecq101-unidir-33v-400w/dp/3440137) | |
| S1B rectifier | 2 | SMA 100V 1A silicon | [S1B-13-F](https://www.digikey.co.uk/en/products/detail/diodes-incorporated/S1B-13-F/725026) | |
| PMEG2010ER Schottky | 1 | SOD-123W | [PMEG2010ER,115](https://uk.farnell.com/nexperia/pmeg2010er-115/diode-rect-sch-20v-1a-sod123w/dp/1907681) | |
| Smart diode controller | 1 | LM74610-Q1 VSSOP-8 | [LM74610QDGKRQ1](https://www.digikey.co.uk/en/products/detail/texas-instruments/LM74610QDGKRQ1/5702219) | |
| N-channel MOSFET, DPAK | 1 | IRLR2905 55V 27 mΩ | [IRLR2905TRPBF](https://www.digikey.co.uk/en/products/detail/infineon-technologies/IRLR2905TRPBF/811417) | See Notes for substitutes |
| Ideal diode | 1 | LM66100 SC70-6 | [LM66100DCKR](https://www.digikey.co.uk/en/products/detail/texas-instruments/LM66100DCKR/10273183) | |
| Boost converter | 1 | LTC3122EMSE MSOP-12-EP | [LTC3122EMSE#PBF](https://www.digikey.co.uk/en/products/detail/analog-devices-inc/LTC3122EMSE-PBF/3516527) | |
| Nanopower comparator | 2 | TLV3012 SOT-23-6 | [TLV3012AIDBVR](https://www.digikey.co.uk/en/products/detail/texas-instruments/TLV3012AIDBVR/1678203) | |
| AND gate | 1 | SN74AUP1G08 SOT-23-5 | [SN74AUP1G08DBVR](https://www.digikey.co.uk/en/products/detail/texas-instruments/SN74AUP1G08DBVR/864078) | |
| Inductor | 1 | XFL4020-222ME 2.2uH | [XFL4020-222MEC](https://uk.farnell.com/coilcraft/xfl4020-222mec/inductor-2-2uh-8a-20-pwr-38mhz/dp/2289216) | |
| 2N7002 MOSFET | 1 | SOT-23 | [2N7002](https://uk.farnell.com/multicomp-pro/2n7002/mosfet-n-ch-60v-0-115a-sot-23/dp/4295174) | |
| PNP transistor | 1 | BC856B SOT-23 | [BC856BLT1G](https://uk.farnell.com/onsemi/bc856blt1g/transistor-pnp-sot-23/dp/1459043) | |
| 47uF 63V electrolytic | 1 | SMD 105 °C | [EEE-FK1J470P](https://www.digikey.co.uk/en/products/detail/panasonic-electronic-components/EEE-FK1J470P/765992) | |
| 100uF 63V electrolytic | 1 | SMD 105 °C | [EEE-FK1J101P](https://www.digikey.co.uk/en/products/detail/panasonic-electronic-components/EEE-FK1J101P/765988) | |
| 10uF 1210 soft-termination capacitor | 4 | 1210 >= 50V 10% X7R SOFT TERMINATION | [MCJCU32MLB7106KPPDT1](https://uk.farnell.com/taiyo-yuden/mcjcu32mlb7106kppdt1/capacitor-mlcc-10uf-50v-x7r-1210/dp/4666637) | |
| 47uF 1210 capacitor | 2 | 1210 >= 10V 20% X7R | [CL32B476MPJNNNE](https://uk.farnell.com/semco/cl32b476mpjnnne/cap-mlcc-47uf-10vdc-x7r-1210/dp/5109745) | |
| 4.7uF 0805 capacitor | 2 | 0805 >= 50V 10% X7R | [GRM21BZ71H475KE15K](https://uk.farnell.com/murata/grm21bz71h475ke15k/cap-4-7uf-50v-mlcc-0805/dp/3582887) | |
| 2.2uF 0603 capacitor | 2 | 0603 >= 25V 10% X7R | [GRM188Z71E225KE43D](https://uk.farnell.com/murata/grm188z71e225ke43d/cap-mlcc-2-2uf-x7r-25v-0603/dp/4335731) | |
| 220nF 0603 capacitor | 1 | 0603 >= 50V 10% X7R | Any 0603 220nF 50V X7R | |
| 100nF 0402 capacitor | 4 | 0402 >= 50V 10% X7R | [GRM155R71H104KE14D](https://uk.farnell.com/murata/grm155r71h104ke14d/cap-0-1-f-50v-10-x7r-0402/dp/2611912) | |
| 560pF 0402 capacitor | 1 | 0402 >= 50V 5% C0G/NP0 | Any 0402 560pF C0G | |
| 100pF 0402 capacitor | 2 | 0402 >= 50V 5% C0G/NP0 | [0402N101J500CT](https://uk.farnell.com/multicomp-pro/0402n101j500ct/cap-100pf-50v-5-c0g-np0-0402/dp/2496792) | |
| 10pF 0402 capacitor | 1 | 0402 >= 50V 5% C0G/NP0 | [GRM1555C1H100JA01D](https://uk.farnell.com/murata/grm1555c1h100ja01d/cap-mlcc-10pf-c0g-np0-50v-0402/dp/4326648) | |
| 909K 0402 anti-sulfur resistor | 2 | 0402 909K 1% ANTI-SULFUR AEC-Q200 | [AF0402FR-07909KL](https://www.digikey.co.uk/en/products/detail/yageo/AF0402FR-07909KL/16992580) | S4R19 (required anti-sulfur), S5R33 |
| 1M 0402 resistor | 7 | 0402 1M 1% | [ERJ2RKF1004X](https://uk.farnell.com/panasonic/erj2rkf1004x/res-1m-1-0-1w-0402-thick-film/dp/2302957) | |
| 130K 0402 resistor | 1 | 0402 130K 1% | Any 0402 130K 1% | |
| 200K 0402 resistor | 1 | 0402 200K 1% | Any 0402 200K 1% | |
| 57.6K 0402 resistor | 1 | 0402 57.6K 1% | Any 0402 57.6K 1% | |
| 47K 0402 resistor | 1 | 0402 47K 5% | [ERJ2RKF4702X](https://uk.farnell.com/panasonic/erj2rkf4702x/res-47k-1-0-1w-0402-thick-film/dp/2302806) | |
| 10K 0402 resistor | 1 | 0402 10K 5% | [CRCW040210K0FKED](https://uk.farnell.com/vishay/crcw040210k0fked/res-10k-1-0-063w-0402-thick-film/dp/1469669) | |
| 6.8M 0603 resistor | 1 | 0603 6.8M 1% 75V | Any 0603 6.8M 1% 75V | |
| 33M 0603 resistor | 1 | 0603 33M 5% | [MCHVR03JTHX3305](https://uk.farnell.com/multicomp-pro/mchvr03jthx3305/res-33m-5-0-1w-0603-thick-film/dp/2825824) | |
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
