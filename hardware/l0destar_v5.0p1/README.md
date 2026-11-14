# l0destar v5.0p1

## Overview

**NOTE: THIS HAS NOT BEEN TESTED, USE AT YOUR OWN RISK**

- This is a prototype l0destar vehicle tracker PCB designed to be
  hand-solderable (hot air required)
- It makes use of the [Makerdiary nRF9151 Connect Kit](https://makerdiary.com/products/nrf9151-connectkit) to provide the LTE and GPS
functions
- Double-sided 4-layer board, 76.8 x 41.8 mm, with components on both sides
- Includes the battery backup from
  [l0destar_battery_backup_v1.1](../l0destar_battery_backup_v1.1/README.md) on
  the same board: a CR123A primary cell and boost hold the tracker's supply
  when car power is cut, so there is no separate inline module
- Component vias are dogleg-routed deliberately in order to keep the PCB costs
as low as possible
- The RF trace width is calculated using a standard 4-layer stackup at JLCPCB,
  different fabrication processes may require adjustment

## Disclaimer

This is a prototype, not a product. Nothing here is validated or certified,
the parts lists are examples rather than a verified BOM, and any test results
recorded below are my own unverified bench observations - repeat the testing
yourself rather than taking them on trust.

**Before building or installing anything from this repository, read the
[full disclaimer](../../DISCLAIMER.md).**

## New features

- Battery backup integrated: CR123A cell, LTC3122 boost and TLV3012 switchover
- Two low-side auxiliary outputs (AIO1/AIO2) on the main connector
- TCA6408A I2C GPIO expander

## Test status

| Item | Test | Result | Notes |
|---------|------|--------|-------|
| Input stage | 12V input reverse polarity | NOT TESTED | |
| Input stage | 12V ignition input reverse polarity | NOT TESTED | |
| Input stage | S2Q2 stays off while VBOOST is up, no backup current into a cut car supply | NOT TESTED | |
| Input stage | Car current still passes through the S2Q2 body diode when PP12VP is below the car rail | NOT TESTED | |
| INA238 | Voltage reading function | NOT TESTED | |
| Ignition presence | Ignition sense 3.3v signal | NOT TESTED | |
| LT8609 | 4.2V output | NOT TESTED | |
| LT8609 | UVLO thresholds | NOT TESTED | |
| MCU OVP | 4.2V passes through at nominal voltage | NOT TESTED | S11Q1 on, S12TP2 ≈ the buck output at S6C5 |
| MCU OVP | Cuts off when the buck output (S6C5) is driven above ~5V | NOT TESTED | Trip ≈ 4.95V, release ≈ 4.80V (calculated, see below) |
| MCU OVP | Scope trigger on VBAT rail, should never peak above abs max (~5.5V) during overvoltage condition | NOT TESTED | |
| GPS auxiliary 3.3V rail | Switches on enable signal | NOT TESTED | |
| CAN auxiliary 3.3V rail | Switches on CAN-enable signal | NOT TESTED | |
| K-line auxiliary 3.3V rail | Switches on K-enable signal | NOT TESTED | |
| K-line auxiliary 12V rail | Switches on K-enable signal | NOT TESTED | |
| Backup | Boost enables when the car rail falls below ~10.06V | NOT TESTED | |
| Backup | Boost disables when the car rail returns above ~11V | NOT TESTED | Depends on the cell voltage, see below |
| Backup | 9.6V at S19C14/S19C15 | NOT TESTED | 1.202V x (1 + 909k/130k) |
| Backup | Boost load step and Bode | NOT TESTED | Compensation is the datasheet 2xAA example, not tuned |
| Backup | No brownout at handover or during LTE TX bursts on the cell | NOT TESTED | |
| Backup | No current ever flows into the cell | NOT TESTED | |
| Backup | BACKUP\_PWR high on backup, low with the car present or no cell fitted | NOT TESTED | |
| Backup | Sleeping tracker wakes on BACKUP\_PWR | NOT TESTED | |
| Backup | Parked drain from the car with the cell fitted | NOT TESTED | |
| AIO | AIO1/AIO2 sink a relay coil, off while the MCU pin is high-Z | NOT TESTED | |
| GPIO expander | Responds at I2C 0x20 | NOT TESTED | |
| Accelerometer | Operates while awake | NOT TESTED | |
| Accelerometer | Wake on motion | NOT TESTED | |
| GPS antenna bias tee | Obtains GPS signal | NOT TESTED | |
| K-wire | K wire connectivity | NOT TESTED | |
| CAN | Connectivity | NOT TESTED | |
| CAN standby via XSTBY signal | Low standby current | NOT TESTED | |
| Board | Quiescent current, GNSS backup supply off | | |
| Board | Quiescent current, GNSS backup supply on | | Reduces the TTFF from 17s to around 3s |

Note: the GNSS backup supply can be configured to enable on the first drive of a
day and then disable after a specific time (eg midnight) in order to conserve
power.

## Features

 - 12V live and 12V ignition inputs with TVS and reverse polarity protection
 - Enhanced input protection sized for pulse 2a at the ISO 7637-2:2011 maximum
 - Onboard 2A fusing
 - Ignition presence sensing
 - INA238 voltage reading
 - High efficiency buck converter
 - Auxiliary 3.3V rail for the GPS antenna bias tee
 - Battery backup: CR123A primary cell, LTC3122 synchronous boost (9.6V, true
   output disconnect) and TLV3012 comparator switchover below ~10.06V on the
   car rail, diode-ORed onto the 12V rail
 - Cell protected by a 2A fuse and an LM66100 ideal diode so nothing can charge it
 - BACKUP\_PWR signal to the MCU to wake and report a power cut
 - Two low-side auxiliary outputs (AIO1/AIO2, ~100mA each) with 33V TVS
 - TCA6408A 8-bit I2C GPIO expander
 - ASM330LHHXTR 6-axis IMU gyro/accelerometer
 - USB-C can be connected and disconnected for programming without any power
   disruption
 - Optional CAN interface
 - Optional K-wire (ISO 14230-1 K-line) interface - the wire carries KWP2000 (ISO 14230-4), the older
   ISO 9141-2, and manufacturer-specific pre-OBD protocols such as VAG KW1281
 - Auxiliary rail sensing/fault detection
 - MCU over-voltage protection on the 4.2V rail with fault-injection test point
 - I2C bus filtering against LTE TX bursts
 - Dedicated U-blox GPS module supporting BeiDou, Galileo, GLONASS, GPS / QZSS
 - MX25R6435FZNIL0 64Mbit (8MB) SPI flash for data buffering / config

## Changes from v4.0p3

- Battery backup module integrated (sheets 18-20): S18 cell, S19 boost, S20
  switchover. VBOOST is diode-ORed onto PP12VP by S2D5
- S2Q2 gate network added (S2Q3, S2R3, S2R4) so the backup cannot feed a cut car
  supply, and S2R2 changed from 100K to 2.2M
- BACKUP\_PWR added, replacing the standalone module's ignition wake pulse
- AIO1/AIO2 low-side outputs added (S21)
- TCA6408A GPIO expander added (S22)
- Main connector changed from 6-pin to 8-pin Micro-Fit (43045-0800)
- Board grown from 66.3 x 38.0 mm to 76.8 x 41.8 mm and made double-sided

## Main connector (S1J1)

| Pin | Signal | Pin | Signal |
|-----|--------|-----|--------|
| 1 | AIO1 | 5 | AIO2 |
| 2 | GND | 6 | PP12V |
| 3 | K-wire K | 7 | PP12VI (ignition) |
| 4 | CAN H | 8 | CAN L |

## Power supply

The Connect Kit can be powered through VBUS which would be much simpler as it
can take 5V, but then you have to disconnect the power before connecting a USB
cable. Same for the Nordic dev board - they both explicitly say not to connect
powered USB and VBUS at the same time. Because the intention is to install this
in a vehicle for testing the pragmatic decision was taken to power it with a
4.2V buck feeding the battery connector. With this power connection we can
connect USB-C at any time to update the firmware without needing to disconnect
power.

All of the board's 3.3V rails (PP3V3, and the switched CAN, K-line and GNSS
rails derived from it) are fed from the Connect Kit's VDD\_GPIO output on header
pin 40, which is a 400 mA regulator (TPS63901), not a dedicated 3V3 rail.
Firmware must set VDD\_GPIO to 3.3V (not 1.8V) before anything on the board
works, and the total 3.3V load must stay inside that 400 mA.

## Battery backup

When the car rail drops below ~10.06V the TLV3012 enables the LTC3122, which
boosts the CR123A to 9.6V and feeds PP12VP through S2D5; the tracker sees ~9V.
While the car is present the boost output is off and the cell is unloaded.
BACKUP\_PWR goes high so the firmware can wake and report the cut. The cell is
a non-rechargeable consumable: a primary survives a sun-parked car where a
rechargeable does not, and a power cut the tracker already alerts on is the only
thing that uses it. Use genuine branded cells from a reputable source.

## Notes

 - All caps on the 12V rails must be >= 50V in order to handle transients
 - MCP2518FD with PP3V3\_CAN off - abs max on all I/O is VDD + 0.3V. Firmware
   must drive CAN\_SCK/SDI/CS low or tri-state them before dropping
   CAN\_ENABLE, or the nRF backfeeds the dead rail through the clamp diodes.
   Additionally, CAN\_CS and CAN\_INT have their pull-ups (S9R1, S9R2) on the
   switched rail, so both float when the domain is off - enable the nRF
   internal pulldown on those pins
 - K-line side, now its own domain - K\_ENABLE gates PP3V3\_K and PP12V\_K
   together. Both K\_TX and K\_SLEEP land on the TJA1027T, which is supplied
   from PP12V\_K (VBAT, pin 7), so driving either high with the rail down
   backfeeds it through the transceiver's ESD clamps. K\_TX and K\_RX also
   carry 10K pull-ups (S10R2, S10R1) to PP3V3\_K, which float when that rail
   is down. Firmware must take the TJA1027T to sleep (K\_SLEEP low) and then
   park K\_TX/K\_RX/K\_SLEEP before dropping K\_ENABLE
 - K\_SLEEP has a 100K pulldown (S10R6), so the transceiver stays asleep while
   the nRF is in reset or the pin is parked
 - The OVP stage latches off while the unprotected rail stays above the
   release threshold. If the buck is genuinely faulted it will stay off; to
   clear a transient trip the input power has to drop far enough for the buck
   output to fall below ~4.8V
 - What the trip point has to protect: the 4.2V rail lands on the Connect Kit's
   battery connector (J2, MX1.25-2P), which goes to the **BAT pin of its
   BQ25180 charger** - not to the nRF9151 directly. On the BQ25180 only the IN
   pin is 25V tolerant; BAT and SYS are rated -0.3V to 5.5V absolute maximum.
   With no USB attached the BATFET conducts BAT to SYS, and SYS feeds nRF9151
   VDD through ferrite L4, so an over-voltage reaches the SiP as well - and the
   nRF9151 VDD absolute maximum is also 5.5V (VDD\_GPIO is a separate rail at
   3.9V). So 5.5V is the ceiling on both counts
 - S11R5/S11R6 set the trip point: V\_trip ≈ 2.5V × (1 + S11R5/S11R6) shifted
   slightly by S11R3. Nominal trip is ≈4.95V; worst case over the ATL431B's
   0.5% reference, 1% on S11R5/S11R6 and 5% on S11R3 the window is ≈4.87V to
   5.02V, so it stays clear of the buck's nominal 4.24V plus ripple at the
   bottom and keeps ~0.48V of margin to the 5.5V ceiling at the top. Keep the
   divider at 1% - a 5% pair widens that window at both ends
 - Indicated voltages and tolerances are the minimum, I generally always buy the
   tightest tolerances and highest voltages possible of everything as it's just
   simpler for managing inventory. Because of this, several of the "example"
   links will be to the parts I bought which might be tighter tolerance or
   higher voltage rating than the indicated minimum spec

 - **Nothing may ever charge the cell.** Do not add any path from VBOOST,
   PP12VP or PP12V to the cell. If S18U1 is replaced the replacement must block
   reverse current
 - S2D5 must stay silicon (S1B), not Schottky. S2Q2 conducts both ways once its
   gate is low: S2R4 ties the gate to VBOOST so it is off while the boost runs
   (its body diode still passes car current), and S2Q3 clamps the gate to PP12VP
   in the few ms before the boost is up. S2R3 isolates S2Q3's gate from car
   transients
 - The LTC3122's exposed pad is PGND, solder it. S19C14/S19C15 are 1210
   ceramics, soft termination only, kept away from board edges and mounting holes
 - S19R19 (boost FB top) is anti-sulfur for the same reason as the buck's top
   resistor: if it fails open the boost runs to its ceiling
 - Boost compensation is the datasheet 2xAA -> 12V example, not tuned. Bench
   Bode and load-step testing is required
 - BACKUP\_PWR runs at cell voltage (2.5-3.3V), so set VDD\_GPIO to 3.3V, not
   1.8V. It is a plain MCU input with SENSE wake; S20R41 (2.2M) holds it low
   with no cell fitted and S20R40 (10K) limits current into the pin if the
   Connect Kit I/O rail is down
 - Switchover releases at roughly 11V on the car rail (11.0V at a 2.5V cell,
   11.3V at 3.3V, calculated from S20R38/S20R39), so it moves with the cell
   voltage - measure it before relying on it
 - With no cell fitted nothing on sheets 19-20 is powered and the board runs
   from the car as normal. The S20R32/S20R33 sense divider adds ~1.6 uA at 12V
 - S20R32 is 0603 for its 75V rating against load dump, it sits directly on
   the car rail
 - AIO1/AIO2 are open-drain outputs only (the MCU cannot read them back), ~100mA
   each (2N7002). Wire relay coil+ to a fused vehicle 12V supply and coil- to
   AIOn, and fit a flyback diode at the relay. The 33V TVS standoff must not be
   lowered, see PROTECTION.md
 - TCA6408A is at I2C 0x20 (ADDR to GND), no clash with the ASM330LHH or INA238.
   P0-P4 read the switched-rail state nets (PP3V3\_CAN\_ST, PP3V3\_K\_ST,
   PP12V\_K\_ST, PP3V3\_GNSS\_ST, PP3V3\_GNSS\_BCKP\_ST) and P5-P7 are
   unconnected. EXP\_INT is routed to the Connect Kit header (S1J3 pad 27)
 - The SPI flash (S17U1) is on the permanent PP3V3 rail, it is not switched

## Bill of materials - required

| Item | Description | Specification | Example | Notes |
|------|-------------|---------------|---------|-------|
| MCU | MCU and GSM/GPS 40pin board | nRF9151 Connect Kit | [Makerdiary nRF9151 Connect Kit](https://makerdiary.com/products/nrf9151-connectkit) | |
| S1J1 | Molex Micro-fit 3.0 2x04 PCB connector | 43045-0800 | [43045-0800](https://uk.farnell.com/molex/43045-0800/connector-header-8pos-2row-3mm/dp/1012254) | AIO1 on pin 1, AIO2 on pin 5, see Main connector |
| S1J2 | Makerdiary header 1 | 20-pin 2.54mm header | [20-pin pcb header](https://www.aliexpress.com/item/1005003610333849.html) | |
| S1J3 | Makerdiary header 2 | 20-pin 2.54mm header | [20-pin pcb header](https://www.aliexpress.com/item/1005003610333849.html) | |
| S1R1 | I2C pull-up resistor | 0402 1.8K 1% | [MCMR04X1801FTL](https://uk.farnell.com/multicomp-pro/mcmr04x1801ftl/res-1k8-1-0-0625w-0402-ceramic/dp/2072709) | |
| S1R2 | I2C pull-up resistor | 0402 1.8K 1% | [MCMR04X1801FTL](https://uk.farnell.com/multicomp-pro/mcmr04x1801ftl/res-1k8-1-0-0625w-0402-ceramic/dp/2072709) | |
| S1R3 | 100K pulldown resistor | 0402 100K 5% | [MCPWR02FTEP1003A](https://uk.farnell.com/multicomp-pro/mcpwr02ftep1003a/res-100k-1-thick-film-0402/dp/4538624) | |
| S1R4 | 100K pulldown resistor | 0402 100K 5% | [MCPWR02FTEP1003A](https://uk.farnell.com/multicomp-pro/mcpwr02ftep1003a/res-100k-1-thick-film-0402/dp/4538624) | |
| S1R5 | 100K pulldown resistor | 0402 100K 5% | [MCPWR02FTEP1003A](https://uk.farnell.com/multicomp-pro/mcpwr02ftep1003a/res-100k-1-thick-film-0402/dp/4538624) | |
| S1C1 | 100nF capacitor | 0402 >= 50V 10% X7R | [GRM155R71H104KE14D](https://uk.farnell.com/murata/grm155r71h104ke14d/cap-0-1-f-50v-10-x7r-0402/dp/2611912) | |
| S2F1 | 2A fuse | 1206 2A slow blow | [0407002.WRA](https://www.ebay.co.uk/itm/178539955750) | |
| S2F2 | 2A fuse | 1206 2A slow blow | [0407002.WRA](https://www.ebay.co.uk/itm/178539955750) | |
| S2Q1 | Reverse-polarity MOSFET | SQ2361 SOT-23 | [SQ2361CES-T1_BE3](https://www.ebay.co.uk/itm/178539853850) | |
| S2Q2 | Reverse-polarity MOSFET | SQJ457EP-T1_BE3 | [SQJ457EP-T1-GE3](https://www.ebay.co.uk/itm/178539865485) | |
| S2D1 | 15V Zener diode | BZX84C15 | [BZX84C15](https://uk.farnell.com/multicomp-pro/bzx84c15/zener-diode-0-3w-15v-sot-23/dp/2675186) | |
| S2D2 | 400W TVS diode | PTVS33VS1UTR,115 SOD-123W | [PTVS33VS1UTR,115](https://www.ebay.co.uk/itm/178539916580) | |
| S2D3 | 15V Zener diode | BZX84C15 | [BZX84C15](https://uk.farnell.com/multicomp-pro/bzx84c15/zener-diode-0-3w-15v-sot-23/dp/2675186) | |
| S2D4 | 400W TVS diode | PTVS33VS1UTR,115 SOD-123W | [PTVS33VS1UTR,115](https://www.ebay.co.uk/itm/178539916580) | |
| S2R1 | Pulldown resistor | 0402 1M 5% | [ERJ2RKF1004X](https://uk.farnell.com/panasonic/erj2rkf1004x/res-1m-1-0-1w-0402-thick-film/dp/2302957) | |
| S2R2 | S2Q2 gate pulldown resistor | 0402 2.2M 1% | ERJ2RKF2204X (Panasonic) | Was 100K in v4.0p3, keeps the gate network to ~4 uA on backup |
| S2D5 | Backup OR-ing diode | S1B SMA 100V 1A silicon | [S1B-13-F](https://www.ebay.co.uk/itm/178539943449) | Silicon, not Schottky. ORs VBOOST onto PP12VP, keeps car transients off the boost output |
| S2Q3 | Reverse-current blocker MOSFET | SQ2361 SOT-23 | [SQ2361CES-T1_BE3](https://www.ebay.co.uk/itm/178539853850) | Clamps the S2Q2 gate to PP12VP before the boost is up |
| S2R3 | S2Q3 gate series resistor | 0402 100K 5% | [MCPWR02FTEP1003A](https://uk.farnell.com/multicomp-pro/mcpwr02ftep1003a/res-100k-1-thick-film-0402/dp/4538624) | |
| S2R4 | S2Q2 gate to VBOOST resistor | 0402 100K 5% | [MCPWR02FTEP1003A](https://uk.farnell.com/multicomp-pro/mcpwr02ftep1003a/res-100k-1-thick-film-0402/dp/4538624) | Holds S2Q2 off while the boost runs |
| S2C1 | 47uF input capacitor | 2220 >= 50V 20% X7R | [CKG57NX7R1H476M500JH](https://uk.farnell.com/tdk/ckg57nx7r1h476m500jh/cap-stacked-47uf-50v-mlcc-2220/dp/3816888) | |
| S2C2 | 47uF input capacitor | 2220 >= 50V 20% X7R | [CKG57NX7R1H476M500JH](https://uk.farnell.com/tdk/ckg57nx7r1h476m500jh/cap-stacked-47uf-50v-mlcc-2220/dp/3816888) | |
| S2C3 | 10uF input capacitor | 1210 >= 50V 10% X7R SOFT TERMINATION | [MCJCU32MLB7106KPPDT1](https://uk.farnell.com/taiyo-yuden/mcjcu32mlb7106kppdt1/capacitor-mlcc-10uf-50v-x7r-1210/dp/4666637) | |
| S3U1 | INA238 voltage read IC | INA238 10-VSSOP | [INA238AIDGSR](https://www.digikey.co.uk/en/products/detail/texas-instruments/INA238AIDGSR/15857213) | |
| S3C1 | 2.2uF capacitor | 0603 >= 25V 10% X7R | [GRM188Z71E225KE43D](https://uk.farnell.com/murata/grm188z71e225ke43d/cap-mlcc-2-2uf-x7r-25v-0603/dp/4335731) | |
| S3C2 | 100nF capacitor | 0402 >= 50V 10% X7R | [GRM155R71H104KE14D](https://uk.farnell.com/murata/grm155r71h104ke14d/cap-0-1-f-50v-10-x7r-0402/dp/2611912) | |
| S4Q1 | Ignition sense MOSFET | 2N7002 SOT-23 | [2N7002](https://uk.farnell.com/multicomp-pro/2n7002/mosfet-n-ch-60v-0-115a-sot-23/dp/4295174) | |
| S4R1 | Ignition sense resistor | 0402 180K 5% | [MCWR04X1803FTL](https://uk.farnell.com/multicomp-pro/mcwr04x1803ftl/res-180k-1-0-0625w-thick-film/dp/2447116) | |
| S4R2 | Ignition sense resistor | 0402 56K 5% | [MCMR04X5602FTL](https://uk.farnell.com/multicomp-pro/mcmr04x5602ftl/res-56k-1-0-0625w-0402-ceramic/dp/2073131) | |
| S4R3 | Ignition sense resistor | 0402 56K 5% | [MCMR04X5602FTL](https://uk.farnell.com/multicomp-pro/mcmr04x5602ftl/res-56k-1-0-0625w-0402-ceramic/dp/2073131) | |
| S4C1 | 100nF capacitor | 0402 >= 50V 10% X7R | [GRM155R71H104KE14D](https://uk.farnell.com/murata/grm155r71h104ke14d/cap-0-1-f-50v-10-x7r-0402/dp/2611912) | |
| S6U1 | Buck converter | LT8609AIMSE MSOP-EP-10 | [LT8609AIMSE#PBF](https://www.ebay.co.uk/itm/178544379670) | |
| S6L1 | Inductor | XFL4020-222ME | [XFL4020-222MEC](https://uk.farnell.com/coilcraft/xfl4020-222mec/inductor-2-2uh-8a-20-pwr-38mhz/dp/2289216) | |
| S6C1 | 100nF capacitor | 0402 >= 50V 10% X7R | [GRM155R71H104KE14D](https://uk.farnell.com/murata/grm155r71h104ke14d/cap-0-1-f-50v-10-x7r-0402/dp/2611912) | |
| S6C2 | 2.2uF capacitor | 0603 >= 25V 10% X7R | [GRM188Z71E225KE43D](https://uk.farnell.com/murata/grm188z71e225ke43d/cap-mlcc-2-2uf-x7r-25v-0603/dp/4335731) | |
| S6C3 | 4.7uF capacitor | 0805 >= 50V 10% X7R | [GRM21BZ71H475KE15K](https://uk.farnell.com/murata/grm21bz71h475ke15k/cap-4-7uf-50v-mlcc-0805/dp/3582887) | |
| S6C4 | 10pF capacitor | 0402 >= 50V 5% C0G/NP0 | [GRM1555C1H100JA01D](https://uk.farnell.com/murata/grm1555c1h100ja01d/cap-mlcc-10pf-c0g-np0-50v-0402/dp/4326648) | |
| S6C5 | 22uF capacitor | 0805 >= 10V 20% X7R | [GMC21X7R226M10NT](https://www.digikey.co.uk/en/products/detail/cal-chip-electronics-inc/GMC21X7R226M10NT/22461324) | |
| S6C8 | 100nF capacitor | 0402 >= 50V 10% X7R | [GRM155R71H104KE14D](https://uk.farnell.com/murata/grm155r71h104ke14d/cap-0-1-f-50v-10-x7r-0402/dp/2611912) | |
| S6R1 | Oscillator frequency resistor | 0402 18.2K 1% | [RC0402FR-0718K2L](https://uk.farnell.com/yageo/rc0402fr-0718k2l/res-18k2-1-0-0625w-0402-thick/dp/3495542) | 18.2K == 2MHz |
| S6R2 | Output voltage divider resistor | 0402 226K 1% ANTI-SULFUR AEC-Q200 | [MCMR04X2263FTL](https://uk.farnell.com/multicomp-pro/mcmr04x2263ftl/res-226k-1-0-0625w-0402-ceramic/dp/2072796) | VOUT == 0.782V x (1 + S6R3/S6R2) == 4.24V, sets output voltage, keep 1%, anti-sulfur AEC-Q200 recommended |
| S6R3 | Output voltage divider resistor | 0402 1M 1% ANTI-SULFUR AEC-Q200 | [AF0402FR-071ML](https://uk.farnell.com/yageo/af0402fr-071ml/res-1m-1-0-063w-thick-film-0402/dp/4148383) | Sets output voltage, keep 1%, anti-sulfur AEC-Q200 recommended |
| S6R4 | Buck enable jumper | 0402 0R | Any 0402 0R jumper | Ties EN to VIN. Replace with 1M if fitting the optional enable divider - see note below |
| S6R5 | Enable voltage divider resistor | DNP | - | **Optional** - see note below |
| S6R6 | Enable voltage divider resistor | DNP | - | **Optional** - see note below |
| S7U1 | Reverse-blocking load switch | Active high 3.3v load switch with reverse blocking | [SiP32431DR3-T1GE3](https://www.ebay.co.uk/itm/178539837620) | |
| S7C1 | 2.2uF capacitor | 0603 >= 25V 10% X7R | [GRM188Z71E225KE43D](https://uk.farnell.com/murata/grm188z71e225ke43d/cap-mlcc-2-2uf-x7r-25v-0603/dp/4335731) | |
| S7C2 | 100nF capacitor | 0402 >= 50V 10% X7R | [GRM155R71H104KE14D](https://uk.farnell.com/murata/grm155r71h104ke14d/cap-0-1-f-50v-10-x7r-0402/dp/2611912) | |
| S7R10 | 100K resistor | 0402 100K 5% | [MCPWR02FTEP1003A](https://uk.farnell.com/multicomp-pro/mcpwr02ftep1003a/res-100k-1-thick-film-0402/dp/4538624) | |
| S7R11 | 1M resistor | 0402 1M 5% | [ERJ2RKF1004X](https://uk.farnell.com/panasonic/erj2rkf1004x/res-1m-1-0-1w-0402-thick-film/dp/2302957) | |
| S8U1 | ASM330LHHXTR accelerometer | ASM330LHHXTR | [ASM330LHHXTR](https://estore.st.com/en/products/mems-and-sensors/inemo-inertial-modules/asm330lhhx.html) | |
| S8C1 | 100nF capacitor | 0402 >= 50V 10% X7R | [GRM155R71H104KE14D](https://uk.farnell.com/murata/grm155r71h104ke14d/cap-0-1-f-50v-10-x7r-0402/dp/2611912) | |
| S8C2 | 10uF capacitor | 0603 >= 10V 20% X7R | [C1608X7R1A106M080AT](https://www.digikey.co.uk/en/products/detail/tdk/C1608X7R1A106M080AT/25595431?s=N4IgTCBcDaIMIEYBsAGAHADQOwCUEEEEUkBZdFfAFRAF0BfIA) | |
| S8C3 | 100nF capacitor | 0402 >= 50V 10% X7R | [GRM155R71H104KE14D](https://uk.farnell.com/murata/grm155r71h104ke14d/cap-0-1-f-50v-10-x7r-0402/dp/2611912) | |
| S8C4 | 100pF SDA filter capacitor | 0402 >= 50V 5% C0G / NP0 | [0402N101J500CT](https://uk.farnell.com/multicomp-pro/0402n101j500ct/cap-100pf-50v-5-c0g-np0-0402/dp/2496792) | |
| S8C5 | 100pF SCL filter capacitor | 0402 >= 50V 5% C0G / NP0 | [0402N101J500CT](https://uk.farnell.com/multicomp-pro/0402n101j500ct/cap-100pf-50v-5-c0g-np0-0402/dp/2496792) | |
| S8R1 | Accelerometer ferrite bead | Ferrite 600R 300mA | [BLM15AG601SH1D](https://uk.farnell.com/murata/blm15ag601sh1d/ferrite-bead-0-6ohm-0-3a-0402/dp/2470354) | |
| S11U1 | Ideal diode | LM66100 | [LM66100](https://www.ebay.co.uk/itm/178539979903) | |
| S11U2 | Shunt voltage reference | ATL431BQDBZR SOT-23 | [ATL431BQDBZR](https://www.ebay.co.uk/itm/178539382502) | 2.5V, 0.5%, low Iq |
| S11Q1 | OVP series MOSFET | SQ2361 SOT-23 | [SQ2361CES-T1_BE3](https://www.ebay.co.uk/itm/178539853850) | |
| S11Q2 | OVP gate drive PNP | BC856BLT1G SOT-23 | [BC856BLT1G](https://www.ebay.co.uk/itm/178539823265) | |
| S11D1 | 15V Zener diode (Vgs clamp) | BZX84C15 | [BZX84C15](https://uk.farnell.com/multicomp-pro/bzx84c15/zener-diode-0-3w-15v-sot-23/dp/2675186) | |
| S11R1 | PNP base pull-up | 0402 10K 1% | [CRCW040210K0FKED](https://uk.farnell.com/vishay/crcw040210k0fked/res-10k-1-0-063w-0402-thick-film/dp/1469669) | |
| S11R2 | MOSFET gate pulldown | 0402 1M 1% | [ERJ2RKF1004X](https://uk.farnell.com/panasonic/erj2rkf1004x/res-1m-1-0-1w-0402-thick-film/dp/2302957) | |
| S11R3 | Hysteresis resistor | 0603 33M 5% | [MCHVR03JTHX3305](https://uk.farnell.com/multicomp-pro/mchvr03jthx3305/res-33m-5-0-1w-0603-thick-film/dp/2825824) | |
| S11R4 | ATL431 cathode / PNP base resistor | 0805 15K 1% ANTI-SULFUR AEC-Q200 | [MCMR08X1502FTL](https://uk.farnell.com/multicomp-pro/mcmr08x1502ftl/res-15k-1-0-125w-0805-ceramic/dp/2073646) | anti-sulfur AEC-Q200 recommended, must be 0805 for current handling in fault conditions |
| S11R5 | OVP divider (top) | 0402 1M 1% ANTI-SULFUR AEC-Q200 | [AF0402FR-071ML](https://uk.farnell.com/yageo/af0402fr-071ml/res-1m-1-0-063w-thick-film-0402/dp/4148383) | Sets trip point, keep 1%, anti-sulfur AEC-Q200 recommended |
| S11R6 | OVP divider (bottom) | 0402 1.05M 1% | [RC0402FR-071M05L](https://uk.farnell.com/yageo/rc0402fr-071m05l/res-1m05-1-0-063w-thick-film-0402/dp/3951647) | Sets trip point, keep 1% |
| S11C1 | 47uF capacitor | 1210 >= 10V 20% X7R | [CL32B476MPJNNNE](https://uk.farnell.com/semco/cl32b476mpjnnne/cap-mlcc-47uf-10vdc-x7r-1210/dp/5109745) | Was S6C6 in v3.1 |
| S11C2 | 47uF capacitor | 1210 >= 10V 20% X7R | [CL32B476MPJNNNE](https://uk.farnell.com/semco/cl32b476mpjnnne/cap-mlcc-47uf-10vdc-x7r-1210/dp/5109745) | Was S6C7 in v3.1 |
| S11C3 | 3.3pF reference filter capacitor | 0402 >= 50V ±0.25pF C0G / NP0 | [CGA2B2C0G1H3R3C050BA](https://uk.farnell.com/tdk/cga2b2c0g1h3r3c050ba/cap-3-3pf-50v-c0g-np0-0402/dp/2210780) | |
| S13R1 | LED resistor | 0402 1K 5% | [CRCW04021K00FKED](https://uk.farnell.com/vishay/crcw04021k00fked/res-1k-1-0-063w-0402-thick-film/dp/1469662) | |
| S13R2 | LED resistor | 0402 1K 5% | [CRCW04021K00FKED](https://uk.farnell.com/vishay/crcw04021k00fked/res-1k-1-0-063w-0402-thick-film/dp/1469662) | |
| S13R3 | LED resistor | 0402 1K 5% | [CRCW04021K00FKED](https://uk.farnell.com/vishay/crcw04021k00fked/res-1k-1-0-063w-0402-thick-film/dp/1469662) | |
| S13D1 | Status LED | 0603 | [150060RS86000](https://uk.farnell.com/wurth-elektronik/150060rs86000/led-red-800mcd-624nm/dp/3408608) | |
| S13D2 | Status LED | 0603 | [150060RS86000](https://uk.farnell.com/wurth-elektronik/150060rs86000/led-red-800mcd-624nm/dp/3408608) | |
| S13D3 | Status LED | 0603 | [150060RS86000](https://uk.farnell.com/wurth-elektronik/150060rs86000/led-red-800mcd-624nm/dp/3408608) | |
| S15J2 | SMA PCB connector | 50 ohm | [SMA-J-P-H-RA-TH1](https://www.ebay.co.uk/itm/178540028662) | |
| S15J3 | u.FL PCB connector | 50 ohm | [U.FL-R-SMT(01)](https://www.ebay.co.uk/itm/178540053901) | |
| S15J4 | SMA PCB connector | 50 ohm | [SMA-J-P-H-RA-TH1](https://www.ebay.co.uk/itm/178540028662) | |
| S15FB1 | Ferrite bead | 600 ohm | [BLM15PX601SZ1D](https://uk.farnell.com/murata/blm15px601sz1d/ferrite-bead-0-9a-0-23ohm-0402/dp/3678458) | |
| S15L1 | RF inductor | 0603 47-68 nH, SRF > 2GHz | [LQW18AN68NJ00D](https://www.ebay.co.uk/itm/178539565608) | |
| S15C1 | 47pF RF capacitor | 0402 >= 50V 5% C0G / NP0 | [0402N470J500CT](https://uk.farnell.com/multicomp-pro/0402n470j500ct/cap-47pf-50v-5-c0g-np0-0402/dp/2496804) | |
| S15C2 | 100pF RF capacitor | 0402 >= 50V 5% C0G / NP0 | [0402N101J500CT](https://uk.farnell.com/multicomp-pro/0402n101j500ct/cap-100pf-50v-5-c0g-np0-0402/dp/2496792) | |
| S15C3 | 10nF RF capacitor | 0402 >= 50V 5% C0G / NP0 | [GRM1555C1H103JE01D](https://uk.farnell.com/murata/grm1555c1h103je01d/cap-mlcc-0-01uf-5-c0g-50vdc-0402/dp/4803046RL) | |
| S15C4 | 2.2uF capacitor | 0603 >= 25V 10% X7R | [GRM188Z71E225KE43D](https://uk.farnell.com/murata/grm188z71e225ke43d/cap-mlcc-2-2uf-x7r-25v-0603/dp/4335731) | |
| S15R1 | Bias tee feed resistor | 0508 15R 5% 1W | [3430A2F15RTDF](https://uk.farnell.com/cgs-te-connectivity/3430a2f15rtdf/res-15r-1w-thick-film-0508-wide/dp/4206818) | |
| S16U1 | Reverse-blocking load switch | Active high 3.3v load switch with reverse blocking | [SiP32431DR3-T1GE3](https://www.ebay.co.uk/itm/178539837620) | |
| S16U2 | U-blox MAX-M10S GPS Module | U-blox MAX-M10S GPS Module | [MAX-M10S-00B](https://www.digikey.co.uk/en/products/detail/u-blox/MAX-M10S-00B/15712906) | |
| S16C1 | 2.2uF capacitor | 0603 >= 25V 10% X7R | [GRM188Z71E225KE43D](https://uk.farnell.com/murata/grm188z71e225ke43d/cap-mlcc-2-2uf-x7r-25v-0603/dp/4335731) | |
| S16C2 | 100nF capacitor | 0402 >= 50V 10% X7R | [GRM155R71H104KE14D](https://uk.farnell.com/murata/grm155r71h104ke14d/cap-0-1-f-50v-10-x7r-0402/dp/2611912) | |
| S16C3 | 2.2uF capacitor | 0603 >= 25V 10% X7R | [GRM188Z71E225KE43D](https://uk.farnell.com/murata/grm188z71e225ke43d/cap-mlcc-2-2uf-x7r-25v-0603/dp/4335731) | |
| S16C4 | 100nF capacitor | 0402 >= 50V 10% X7R | [GRM155R71H104KE14D](https://uk.farnell.com/murata/grm155r71h104ke14d/cap-0-1-f-50v-10-x7r-0402/dp/2611912) | |
| S16C5 | 100nF capacitor | 0402 >= 50V 10% X7R | [GRM155R71H104KE14D](https://uk.farnell.com/murata/grm155r71h104ke14d/cap-0-1-f-50v-10-x7r-0402/dp/2611912) | |
| S16R1 | 100K resistor | 0402 100K 5% | [MCPWR02FTEP1003A](https://uk.farnell.com/multicomp-pro/mcpwr02ftep1003a/res-100k-1-thick-film-0402/dp/4538624) | |
| S16R2 | 1K resistor | 0402 1K 5% | [CRCW04021K00FKED](https://uk.farnell.com/vishay/crcw04021k00fked/res-1k-1-0-063w-0402-thick-film/dp/1469662) | |
| S16R3 | 1K resistor | 0402 1K 5% | [CRCW04021K00FKED](https://uk.farnell.com/vishay/crcw04021k00fked/res-1k-1-0-063w-0402-thick-film/dp/1469662) | |
| S16R4 | 100K resistor | 0402 100K 5% | [MCPWR02FTEP1003A](https://uk.farnell.com/multicomp-pro/mcpwr02ftep1003a/res-100k-1-thick-film-0402/dp/4538624) | |
| S16R5 | 1M resistor | 0402 1M 5% | [ERJ2RKF1004X](https://uk.farnell.com/panasonic/erj2rkf1004x/res-1m-1-0-1w-0402-thick-film/dp/2302957) | |
| S17U1 | SPI flash | MX25R6435FZNIL0 64Mbit WSON-8 6x5mm | MX25R6435FZNIL0 (Macronix) | Verify stock/P/N before ordering; footprint is WSON-8-1EP 6x5mm |
| S17C1 | 100nF capacitor | 0402 >= 50V 10% X7R | [GRM155R71H104KE14D](https://uk.farnell.com/murata/grm155r71h104ke14d/cap-0-1-f-50v-10-x7r-0402/dp/2611912) | Flash decoupling |
| S17R1 | Flash pull-up resistor | 0402 10K 5% | [CRCW040210K0FKED](https://uk.farnell.com/vishay/crcw040210k0fked/res-10k-1-0-063w-0402-thick-film/dp/1469669) | |
| S17R2 | Flash pull-up resistor | 0402 10K 5% | [CRCW040210K0FKED](https://uk.farnell.com/vishay/crcw040210k0fked/res-10k-1-0-063w-0402-thick-film/dp/1469669) | |
| S17R3 | Flash pull-up resistor | 0402 10K 5% | [CRCW040210K0FKED](https://uk.farnell.com/vishay/crcw040210k0fked/res-10k-1-0-063w-0402-thick-film/dp/1469669) | |
| S18BT1 | CR123A cell holder | Keystone 1050 CR123A SMT holder | 1050 (Keystone) | Non-rechargeable primary cell. Verify P/N and stock before ordering |
| S18F1 | 2A fuse, cell | 1206 2A slow blow | [0407002.WRA](https://www.ebay.co.uk/itm/178539955750) | Not a PTC: the 2A 0ZCJ part is only rated 6V |
| S18U1 | Ideal diode | LM66100 | [LM66100](https://www.ebay.co.uk/itm/178539979903) | Nothing may ever charge the cell |
| S18C1 | 2.2uF capacitor | 0603 >= 25V 10% X7R | [GRM188Z71E225KE43D](https://uk.farnell.com/murata/grm188z71e225ke43d/cap-mlcc-2-2uf-x7r-25v-0603/dp/4335731) | |
| S19U5 | Boost converter | LTC3122EMSE MSOP-12-EP | [LTC3122EMSE#PBF](https://www.digikey.co.uk/en/products/detail/analog-devices-inc/LTC3122EMSE-PBF/3516527) | Solder the exposed pad, it is PGND |
| S19L1 | Inductor | XFL4020-222ME | [XFL4020-222MEC](https://uk.farnell.com/coilcraft/xfl4020-222mec/inductor-2-2uh-8a-20-pwr-38mhz/dp/2289216) | |
| S19C7 | 47uF input capacitor | 1210 >= 10V 20% X7R | [CL32B476MPJNNNE](https://uk.farnell.com/semco/cl32b476mpjnnne/cap-mlcc-47uf-10vdc-x7r-1210/dp/5109745) | |
| S19C8 | 47uF input capacitor | 1210 >= 10V 20% X7R | [CL32B476MPJNNNE](https://uk.farnell.com/semco/cl32b476mpjnnne/cap-mlcc-47uf-10vdc-x7r-1210/dp/5109745) | |
| S19C9 | 100nF CAP pin capacitor | 0402 >= 50V 10% X7R | [GRM155R71H104KE14D](https://uk.farnell.com/murata/grm155r71h104ke14d/cap-0-1-f-50v-10-x7r-0402/dp/2611912) | CAP to VOUT |
| S19C11 | 560pF compensation capacitor | 0402 >= 50V 5% C0G/NP0 | Any 0402 560pF C0G | |
| S19C12 | 10pF compensation capacitor | 0402 >= 50V 5% C0G/NP0 | [GRM1555C1H100JA01D](https://uk.farnell.com/murata/grm1555c1h100ja01d/cap-mlcc-10pf-c0g-np0-50v-0402/dp/4326648) | |
| S19C13 | 4.7uF VCC capacitor | 0805 >= 50V 10% X7R | [GRM21BZ71H475KE15K](https://uk.farnell.com/murata/grm21bz71h475ke15k/cap-4-7uf-50v-mlcc-0805/dp/3582887) | |
| S19C14 | 10uF output capacitor | 1210 >= 50V 10% X7R SOFT TERMINATION | [MCJCU32MLB7106KPPDT1](https://uk.farnell.com/taiyo-yuden/mcjcu32mlb7106kppdt1/capacitor-mlcc-10uf-50v-x7r-1210/dp/4666637) | 50V rating keeps ~9 uF at 9.6V |
| S19C15 | 10uF output capacitor | 1210 >= 50V 10% X7R SOFT TERMINATION | [MCJCU32MLB7106KPPDT1](https://uk.farnell.com/taiyo-yuden/mcjcu32mlb7106kppdt1/capacitor-mlcc-10uf-50v-x7r-1210/dp/4666637) | |
| S19R14 | Oscillator frequency resistor | 0402 57.6K 1% | Any 0402 57.6K 1% | 57.6K == 1MHz |
| S19R18 | Compensation resistor | 0402 200K 1% | Any 0402 200K 1% | |
| S19R19 | FB divider top | 0402 909K 1% ANTI-SULFUR AEC-Q200 | [AF0402FR-07909KL](https://www.digikey.co.uk/en/products/detail/yageo/AF0402FR-07909KL/16992580) | VOUT == 1.202V x (1 + S19R19/S19R20) == 9.61V, keep 1%, anti-sulfur required |
| S19R20 | FB divider bottom | 0402 130K 1% | Any 0402 130K 1% | Keep 1% |
| S20U1 | Car-rail sense comparator | TLV3012 SOT-23-6 | [TLV3012AIDBVR](https://www.digikey.co.uk/en/products/detail/texas-instruments/TLV3012AIDBVR/1678203) | 1.242V reference, 2.8 uA |
| S20R32 | Car-rail divider top | 0603 6.8M 1% | Any 0603 6.8M 1% 75V | 0603 for the 75V rating |
| S20R33 | Car-rail divider bottom | 0402 909K 1% ANTI-SULFUR AEC-Q200 | [AF0402FR-07909KL](https://www.digikey.co.uk/en/products/detail/yageo/AF0402FR-07909KL/16992580) | Same part as S19R19; anti-sulfur not required here |
| S20R38 | Car-sense hysteresis resistor | 0402 47K 5% | [ERJ2RKF4702X](https://uk.farnell.com/panasonic/erj2rkf4702x/res-47k-1-0-1w-0402-thick-film/dp/2302806) | |
| S20R39 | Car-sense hysteresis feedback resistor | 0402 1M 1% | [ERJ2RKF1004X](https://uk.farnell.com/panasonic/erj2rkf1004x/res-1m-1-0-1w-0402-thick-film/dp/2302957) | |
| S20R40 | BACKUP\_PWR series resistor | 0402 10K 5% | [CRCW040210K0FKED](https://uk.farnell.com/vishay/crcw040210k0fked/res-10k-1-0-063w-0402-thick-film/dp/1469669) | |
| S20R41 | BACKUP\_PWR pulldown resistor | 0402 2.2M 1% | ERJ2RKF2204X (Panasonic) | |
| S20C1 | IN- filter capacitor | 0402 >= 50V 5% C0G / NP0 | [0402N101J500CT](https://uk.farnell.com/multicomp-pro/0402n101j500ct/cap-100pf-50v-5-c0g-np0-0402/dp/2496792) | |
| S20C2 | Comparator decoupling capacitor | 0402 >= 50V 10% X7R | [GRM155R71H104KE14D](https://uk.farnell.com/murata/grm155r71h104ke14d/cap-0-1-f-50v-10-x7r-0402/dp/2611912) | |
| S21D1 | AIO TVS diode | PTVS33VS1UTR,115 SOD-123W | [PTVS33VS1UTR,115](https://www.ebay.co.uk/itm/178539916580) | Do not lower the standoff, see PROTECTION.md |
| S21D2 | AIO TVS diode | PTVS33VS1UTR,115 SOD-123W | [PTVS33VS1UTR,115](https://www.ebay.co.uk/itm/178539916580) | |
| S21Q1 | AIO low-side MOSFET | 2N7002 SOT-23 | [2N7002](https://uk.farnell.com/multicomp-pro/2n7002/mosfet-n-ch-60v-0-115a-sot-23/dp/4295174) | |
| S21Q2 | AIO low-side MOSFET | 2N7002 SOT-23 | [2N7002](https://uk.farnell.com/multicomp-pro/2n7002/mosfet-n-ch-60v-0-115a-sot-23/dp/4295174) | |
| S21R1 | AIO gate resistor | 0402 1K 5% | [CRCW04021K00FKED](https://uk.farnell.com/vishay/crcw04021k00fked/res-1k-1-0-063w-0402-thick-film/dp/1469662) | |
| S21R2 | AIO gate pulldown resistor | 0402 100K 5% | [MCPWR02FTEP1003A](https://uk.farnell.com/multicomp-pro/mcpwr02ftep1003a/res-100k-1-thick-film-0402/dp/4538624) | |
| S21R3 | AIO gate resistor | 0402 1K 5% | [CRCW04021K00FKED](https://uk.farnell.com/vishay/crcw04021k00fked/res-1k-1-0-063w-0402-thick-film/dp/1469662) | |
| S21R4 | AIO gate pulldown resistor | 0402 100K 5% | [MCPWR02FTEP1003A](https://uk.farnell.com/multicomp-pro/mcpwr02ftep1003a/res-100k-1-thick-film-0402/dp/4538624) | |
| S22U1 | GPIO expander | TCA6408APWR TSSOP-16 | TCA6408APWR (Texas Instruments) | I2C 0x20. Verify stock/P/N before ordering |
| S22R1 | ~INT pull-up resistor | 0402 10K 5% | [CRCW040210K0FKED](https://uk.farnell.com/vishay/crcw040210k0fked/res-10k-1-0-063w-0402-thick-film/dp/1469669) | |
| S22C1 | 100nF capacitor | 0402 >= 50V 10% X7R | [GRM155R71H104KE14D](https://uk.farnell.com/murata/grm155r71h104ke14d/cap-0-1-f-50v-10-x7r-0402/dp/2611912) | |
| - | u.FL cable - LTE (Connect Kit to S15J3) | 50 ohm 35mm | [U.FL-2LPHF6-04N1TV-A-35](https://uk.farnell.com/hirose-hrs/u-fl-2lphf6-04n1tv-a-35/cbl-assy-u-fl-r-a-plug-r-a-plug/dp/4294251) | |
| - | JST power cable | Ultra-thin MX1.25 51146 | [10PCS MX1.25 51146 Cable](https://www.ebay.co.uk/itm/178550497683) | |

### Optional: buck enable divider (S6R4/S6R5/S6R6)

S6R4/S6R5/S6R6 form a 3-resistor divider on the LT8609 EN/UV pin that gives
the buck a defined under-voltage lockout with hysteresis: it won't start until
the supply reaches ~5.6V and will shut off if the supply falls to ~4.3V, then
needs ~5.6V again to restart. This is aimed at ISO 16750-2 low-voltage and
drop-out tests. The divider draws around ~11 µA continuously from the 12V
input, on top of the ~35.5 µA the rest of the board draws asleep, so it is
DNP by default.

**Default build (no divider):** fit S6R4 as a `0402 0R` jumper and leave S6R5
and S6R6 unpopulated. EN is tied directly to VIN and the buck runs whenever
the input is present, relying on the LT8609's own internal UVLO.

**With the divider:** fit the three parts below in place of the jumper.

| Item | Description | Specification | Example | Notes |
|------|-------------|---------------|---------|-------|
| S6R4 | Enable voltage divider resistor | 0402 1M 1% ANTI-SULFUR AEC-Q200 | [AF0402FR-071ML](https://uk.farnell.com/yageo/af0402fr-071ml/res-1m-1-0-063w-thick-film-0402/dp/4148383) | Replaces the 0R jumper. Sets enable/disable voltage, keep 1%, anti-sulfur AEC-Q200 recommended |
| S6R5 | Enable voltage divider resistor | 0402 243K 1% ANTI-SULFUR AEC-Q200 | [MCMR04X2433FTL](https://uk.farnell.com/multicomp-pro/mcmr04x2433ftl/res-243k-1-0-0625w-0402-ceramic/dp/2072824) | Sets enable/disable voltage, keep 1%, anti-sulfur AEC-Q200 recommended |
| S6R6 | Enable voltage divider resistor | 0402 3.92M 1% | [CRCW04023M92FKED](https://uk.farnell.com/vishay/crcw04023m92fked/res-3m92-1-0-063w-0402-thick-film/dp/2141113) | |

With EN threshold 1.05V rising / 1.00V falling and VOUT = 4.24V:
K = 1 + S6R4/S6R5 + S6R4/S6R6, VIN\_on = K x 1.05 ≈ 5.6V,
VIN\_off = K x 1.00 - S6R4 x VOUT / S6R6 ≈ 4.3V.

## Bill of materials - CAN bus parts

Can be omitted if CAN is not required.

| Item | Description | Specification | Example | Notes |
|------|-------------|---------------|---------|-------|
| S7U2 | Reverse-blocking load switch | Active high 3.3v load switch with reverse blocking | [SiP32431DR3-T1GE3](https://www.ebay.co.uk/itm/178539837620) | |
| S7R3 | 100K resistor | 0402 100K 5% | [MCPWR02FTEP1003A](https://uk.farnell.com/multicomp-pro/mcpwr02ftep1003a/res-100k-1-thick-film-0402/dp/4538624) | |
| S7R4 | 1M resistor | 0402 1M 5% | [ERJ2RKF1004X](https://uk.farnell.com/panasonic/erj2rkf1004x/res-1m-1-0-1w-0402-thick-film/dp/2302957) | |
| S7C3 | 2.2uF capacitor | 0603 >= 25V 10% X7R | [GRM188Z71E225KE43D](https://uk.farnell.com/murata/grm188z71e225ke43d/cap-mlcc-2-2uf-x7r-25v-0603/dp/4335731) | |
| S7C4 | 100nF capacitor | 0402 >= 50V 10% X7R | [GRM155R71H104KE14D](https://uk.farnell.com/murata/grm155r71h104ke14d/cap-0-1-f-50v-10-x7r-0402/dp/2611912) | |
| S9U1 | TCAN3414DR CAN transceiver | TCAN3414DR | [TCAN3414DR](https://www.ebay.co.uk/itm/178539591356) | |
| S9U2 | MCP2518FD CAN controller | MCP2518FD | [MCP2518FDT-H/SL](https://www.ebay.co.uk/itm/178539603676) | |
| S9D1 | CAN protector | NUP2105L | [NUP2105L](https://www.ebay.co.uk/itm/178539794622) | |
| S9FL1 | CAN common-mode choke | ACT1210-101-2P-TL00 | [ACT1210-101-2P-TL00](https://www.digikey.com/en/products/detail/tdk-corporation/ACT1210-101-2P-TL00/4918053) | **Optional** - see note below |
| S9C1 | 100nF capacitor | 0402 >= 50V 10% X7R | [GRM155R71H104KE14D](https://uk.farnell.com/murata/grm155r71h104ke14d/cap-0-1-f-50v-10-x7r-0402/dp/2611912) | |
| S9C2 | 27pF capacitor | 0402 >= 10V 5% C0G / NP0 | [GCM1555C1H270FA16D](https://uk.farnell.com/murata/gcm1555c1h270fa16d/cap-aec-q200-27pf-50v-mlcc-0402/dp/3581175) | |
| S9C3 | 27pF capacitor | 0402 >= 10V 5% C0G / NP0 | [GCM1555C1H270FA16D](https://uk.farnell.com/murata/gcm1555c1h270fa16d/cap-aec-q200-27pf-50v-mlcc-0402/dp/3581175) | |
| S9C4 | 100nF capacitor | 0402 >= 50V 10% X7R | [GRM155R71H104KE14D](https://uk.farnell.com/murata/grm155r71h104ke14d/cap-0-1-f-50v-10-x7r-0402/dp/2611912) | |
| S9C5 | 10uF capacitor | 0603 >= 10V 20% X7R | [C1608X7R1A106M080AT](https://www.digikey.co.uk/en/products/detail/tdk/C1608X7R1A106M080AT/25595431?s=N4IgTCBcDaIMIEYBsAGAHADQOwCUEEEEUkBZdFfAFRAF0BfIA) | |
| S9Y1 | 40MHz crystal | ECS-400-18-33-JGN-TR3 | [ECS-400-18-33-JGN-TR3](https://www.ebay.co.uk/itm/178539630480) | |
| S9R1 | 10K 0402 resistor | 0402 10K 5% | [CRCW040210K0FKED](https://uk.farnell.com/vishay/crcw040210k0fked/res-10k-1-0-063w-0402-thick-film/dp/1469669) | |
| S9R2 | 10K 0402 resistor | 0402 10K 5% | [CRCW040210K0FKED](https://uk.farnell.com/vishay/crcw040210k0fked/res-10k-1-0-063w-0402-thick-film/dp/1469669) | |
| S9R3 | 120R 0805 resistor | 0805 120R 5% 0.5W | [MCPAS05W2J0121T5E](https://uk.farnell.com/multicomp-pro/mcpas05w2j0121t5e/res-120r-5-0-5w-thick-film-0805/dp/4069078) | |
| S9R4 | 10K 0402 resistor | 0402 10K 5% | [CRCW040210K0FKED](https://uk.farnell.com/vishay/crcw040210k0fked/res-10k-1-0-063w-0402-thick-film/dp/1469669) | |
| S9R5 | 1K series damping resistor | 0402 1K 5% | [CRCW04021K00FKED](https://uk.farnell.com/vishay/crcw04021k00fked/res-1k-1-0-063w-0402-thick-film/dp/1469662) | |
| S9R6 | 220R series damping resistor | 0402 220R 5% | [MC00625W04021220R](https://uk.farnell.com/multicomp/mc00625w04021220r/res-220r-1-0-0625w-0402-thick/dp/1358024) | |
| S9R7 | 220R series damping resistor | 0402 220R 5% | [MC00625W04021220R](https://uk.farnell.com/multicomp/mc00625w04021220r/res-220r-1-0-0625w-0402-thick/dp/1358024) | |
| S9J1 | 2-pin 2.54mm header | 2-pin 2.54mm header | [68001-402HLF](https://uk.farnell.com/amphenol-communications-solutions/68001-402hlf/conn-header-2pos-1row-2-54mm-th/dp/3881905) | CAN termination jumper, in series with S9R3 - fit a shunt only if the tracker is at the end of the bus |

### Optional: CAN common-mode choke (S9FL1)

S9FL1 sits in series with CANH and CANL between the transceiver and the
connector. It improves radiated emissions and common-mode noise rejection,
but CAN works without it, so it is optional.

**If S9FL1 is omitted its two signal paths must be bridged, otherwise CANH
and CANL are left open circuit.** Fit `2 x 0402 0R` resistors across the
S9FL1 pads to jump them - an 0402 fits the ACT1210 footprint pads (checked
against the layout).

## Bill of materials - K-wire parts

Can be omitted if K-wire is not required

| Item | Description | Specification | Example | Notes |
|------|-------------|---------------|---------|-------|
| S7Q1 | MOSFET | 2N7002 SOT-23 | [2N7002](https://uk.farnell.com/multicomp-pro/2n7002/mosfet-n-ch-60v-0-115a-sot-23/dp/4295174) | |
| S7U3 | 12V load switch | Active high 12v load switch | [ITS4060SSJNXUMA1](https://www.ebay.co.uk/itm/178539828483) | |
| S7U4 | Reverse-blocking load switch | Active high 3.3v load switch with reverse blocking | [SiP32431DR3-T1GE3](https://www.ebay.co.uk/itm/178539837620) | |
| S7C5 | 1nF capacitor | 0402 >= 50V 10% X7R | [0402B102K500CT](https://uk.farnell.com/multicomp-pro/0402b102k500ct/cap-1000pf-50v-10-x7r-0402/dp/2496767) | |
| S7C6 | 220nF capacitor | 0603 >= 50V 10% X7R | [GRM188R71H224KAC4D](https://uk.farnell.com/murata/grm188r71h224kac4d/cap-0-22-f-50v-10-x7r-0603/dp/2688525) | |
| S7C7 | 1nF capacitor | 0402 >= 50V 10% X7R | [0402B102K500CT](https://uk.farnell.com/multicomp-pro/0402b102k500ct/cap-1000pf-50v-10-x7r-0402/dp/2496767) | |
| S7C8 | 2.2uF capacitor | 0603 >= 25V 10% X7R | [GRM188Z71E225KE43D](https://uk.farnell.com/murata/grm188z71e225ke43d/cap-mlcc-2-2uf-x7r-25v-0603/dp/4335731) | |
| S7C9 | 100nF capacitor | 0402 >= 50V 10% X7R | [GRM155R71H104KE14D](https://uk.farnell.com/murata/grm155r71h104ke14d/cap-0-1-f-50v-10-x7r-0402/dp/2611912) | |
| S7R5 | 180K resistor | 0402 180K 5% | [MCWR04X1803FTL](https://uk.farnell.com/multicomp-pro/mcwr04x1803ftl/res-180k-1-0-0625w-thick-film/dp/2447116) | |
| S7R6 | 100K resistor | 0402 100K 5% | [MCPWR02FTEP1003A](https://uk.farnell.com/multicomp-pro/mcpwr02ftep1003a/res-100k-1-thick-film-0402/dp/4538624) | |
| S7R7 | 100K resistor | 0402 100K 5% | [MCPWR02FTEP1003A](https://uk.farnell.com/multicomp-pro/mcpwr02ftep1003a/res-100k-1-thick-film-0402/dp/4538624) | |
| S7R8 | 100K resistor | 0402 100K 5% | [MCPWR02FTEP1003A](https://uk.farnell.com/multicomp-pro/mcpwr02ftep1003a/res-100k-1-thick-film-0402/dp/4538624) | |
| S7R9 | 1M resistor | 0402 1M 5% | [ERJ2RKF1004X](https://uk.farnell.com/panasonic/erj2rkf1004x/res-1m-1-0-1w-0402-thick-film/dp/2302957) | |
| S10U1 | Line transceiver | TJA1027T\_20,118 | [TJA1027T\_20,118](https://uk.farnell.com/nxp/tja1027t-20-118/lin-transceiver-20kbaud-18v-soic/dp/2400570) | |
| S10D1 | 1N4148W diode | 1N4148W | [1N4148W-E3-08](https://www.ebay.co.uk/itm/178539887745) | |
| S10D2 | 400W TVS diode | PTVS33VS1UTR,115 SOD-123W | [PTVS33VS1UTR,115](https://www.ebay.co.uk/itm/178539916580) | |
| S10R1 | 10K 0402 resistor | 0402 10K 5% | [CRCW040210K0FKED](https://uk.farnell.com/vishay/crcw040210k0fked/res-10k-1-0-063w-0402-thick-film/dp/1469669) | |
| S10R2 | 10K 0402 resistor | 0402 10K 5% | [CRCW040210K0FKED](https://uk.farnell.com/vishay/crcw040210k0fked/res-10k-1-0-063w-0402-thick-film/dp/1469669) | |
| S10R3 | 510R 0508 resistor | 0508 510R 5% 1W | [3430A2F510RTDF](https://uk.farnell.com/cgs-te-connectivity/3430a2f510rtdf/res-510r-1w-thick-film-0508-wide/dp/4206859) | |
| S10R6 | 100K resistor | 0402 100K 5% | [MCPWR02FTEP1003A](https://uk.farnell.com/multicomp-pro/mcpwr02ftep1003a/res-100k-1-thick-film-0402/dp/4538624) | |
| S10C1 | 100nF capacitor | 0402 >= 50V 10% X7R | [GRM155R71H104KE14D](https://uk.farnell.com/murata/grm155r71h104ke14d/cap-0-1-f-50v-10-x7r-0402/dp/2611912) | |

## Parts list

Aggregated from the bills of materials above. The `All` rows are the required
build; add the `CAN` rows and/or the `K-wire` rows on top of those depending
on which interfaces are fitted, and the `Optional` rows only if you fit the
buck enable divider. Quantities are per board.

| Build | Item | Quantity | Specification | Example | Notes |
|-------|------|----------|---------------|---------|-------|
| All | MCU and GSM/GPS 40pin board | 1 | nRF9151 Connect Kit | [Makerdiary nRF9151 Connect Kit](https://makerdiary.com/products/nrf9151-connectkit) | |
| All | Molex Micro-fit 3.0 2x04 PCB connector | 1 | 43045-0800 | [43045-0800](https://uk.farnell.com/molex/43045-0800/connector-header-8pos-2row-3mm/dp/1012254) | |
| All | 20-pin 2.54mm header | 2 | 20-pin 2.54mm header | [20-pin pcb header](https://www.aliexpress.com/item/1005003610333849.html) | |
| All | 0R 0402 jumper | 1 | 0402 0R | Any 0402 0R jumper | S6R4, the buck enable jumper |
| All | 15R 0508 resistor | 1 | 0508 15R 5% 1W | [3430A2F15RTDF](https://uk.farnell.com/cgs-te-connectivity/3430a2f15rtdf/res-15r-1w-thick-film-0508-wide/dp/4206818) | |
| All | 1K 0402 resistor | 7 | 0402 1K 5% | [CRCW04021K00FKED](https://uk.farnell.com/vishay/crcw04021k00fked/res-1k-1-0-063w-0402-thick-film/dp/1469662) | |
| All | Accelerometer ferrite bead | 1 | Ferrite 600R 300mA | [BLM15AG601SH1D](https://uk.farnell.com/murata/blm15ag601sh1d/ferrite-bead-0-6ohm-0-3a-0402/dp/2470354) | |
| All | 1.8K 0402 resistor | 2 | 0402 1.8K 1% | [MCMR04X1801FTL](https://uk.farnell.com/multicomp-pro/mcmr04x1801ftl/res-1k8-1-0-0625w-0402-ceramic/dp/2072709) | |
| All | 10K 0402 resistor | 5 | 0402 10K 5% | [CRCW040210K0FKED](https://uk.farnell.com/vishay/crcw040210k0fked/res-10k-1-0-063w-0402-thick-film/dp/1469669) | Flash pull-ups S17R1-S17R3 |
| All | 10K 0402 resistor | 1 | 0402 10K 1% | [CRCW040210K0FKED](https://uk.farnell.com/vishay/crcw040210k0fked/res-10k-1-0-063w-0402-thick-film/dp/1469669) | |
| All | 15K 0805 anti-sulfur resistor | 1 | 0805 15K 1% ANTI-SULFUR AEC-Q200 | [MCMR08X1502FTL](https://uk.farnell.com/multicomp-pro/mcmr08x1502ftl/res-15k-1-0-125w-0805-ceramic/dp/2073646) | anti-sulfur AEC-Q200 recommended, must be 0805 for current handling in fault conditions |
| All | 18.2K 0402 resistor | 1 | 0402 18.2K 1% | [RC0402FR-0718K2L](https://uk.farnell.com/yageo/rc0402fr-0718k2l/res-18k2-1-0-0625w-0402-thick/dp/3495542) | 18.2K == 2MHz |
| All | 56K 0402 resistor | 2 | 0402 56K 5% | [MCMR04X5602FTL](https://uk.farnell.com/multicomp-pro/mcmr04x5602ftl/res-56k-1-0-0625w-0402-ceramic/dp/2073131) | |
| All | 100K 0402 resistor | 10 | 0402 100K 5% | [MCPWR02FTEP1003A](https://uk.farnell.com/multicomp-pro/mcpwr02ftep1003a/res-100k-1-thick-film-0402/dp/4538624) | |
| All | 180K 0402 resistor | 1 | 0402 180K 5% | [MCWR04X1803FTL](https://uk.farnell.com/multicomp-pro/mcwr04x1803ftl/res-180k-1-0-0625w-thick-film/dp/2447116) | |
| All | 226K 0402 anti-sulfur resistor | 1 | 0402 226K 1% ANTI-SULFUR AEC-Q200 | [MCMR04X2263FTL](https://uk.farnell.com/multicomp-pro/mcmr04x2263ftl/res-226k-1-0-0625w-0402-ceramic/dp/2072796) | VOUT == 0.782V x (1 + S6R3/S6R2) == 4.24V, sets output voltage, keep 1%, anti-sulfur AEC-Q200 recommended |
| All | 1M 0402 resistor 1% | 2 | 0402 1M 1% | [ERJ2RKF1004X](https://uk.farnell.com/panasonic/erj2rkf1004x/res-1m-1-0-1w-0402-thick-film/dp/2302957) | |
| All | 1M 0402 resistor 5% | 3 | 0402 1M 5% | [ERJ2RKF1004X](https://uk.farnell.com/panasonic/erj2rkf1004x/res-1m-1-0-1w-0402-thick-film/dp/2302957) | |
| All | 1M 0402 anti-sulfur resistor | 2 | 0402 1M 1% ANTI-SULFUR AEC-Q200 | [AF0402FR-071ML](https://uk.farnell.com/yageo/af0402fr-071ml/res-1m-1-0-063w-thick-film-0402/dp/4148383) | Regulator output / trip point, keep 1%, anti-sulfur AEC-Q200 recommended |
| All | 1.05M 0402 resistor | 1 | 0402 1.05M 1% | [RC0402FR-071M05L](https://uk.farnell.com/yageo/rc0402fr-071m05l/res-1m05-1-0-063w-thick-film-0402/dp/3951647) | Sets trip point, keep 1% |
| All | 33M 0603 resistor | 1 | 0603 33M 5% | [MCHVR03JTHX3305](https://uk.farnell.com/multicomp-pro/mchvr03jthx3305/res-33m-5-0-1w-0603-thick-film/dp/2825824) | |
| All | 3.3pF 0402 capacitor | 1 | 0402 >= 50V ±0.25pF C0G / NP0 | [CGA2B2C0G1H3R3C050BA](https://uk.farnell.com/tdk/cga2b2c0g1h3r3c050ba/cap-3-3pf-50v-c0g-np0-0402/dp/2210780) | |
| All | 10pF capacitor | 2 | 0402 >= 50V 5% C0G/NP0 | [GRM1555C1H100JA01D](https://uk.farnell.com/murata/grm1555c1h100ja01d/cap-mlcc-10pf-c0g-np0-50v-0402/dp/4326648) | |
| All | 47pF RF capacitor | 1 | 0402 >= 50V 5% C0G / NP0 | [0402N470J500CT](https://uk.farnell.com/multicomp-pro/0402n470j500ct/cap-47pf-50v-5-c0g-np0-0402/dp/2496804) | |
| All | 100pF RF capacitor | 4 | 0402 >= 50V 5% C0G / NP0 | [0402N101J500CT](https://uk.farnell.com/multicomp-pro/0402n101j500ct/cap-100pf-50v-5-c0g-np0-0402/dp/2496792) | |
| All | 10nF RF capacitor | 1 | 0402 >= 50V 5% C0G / NP0 | [GRM1555C1H103JE01D](https://uk.farnell.com/murata/grm1555c1h103je01d/cap-mlcc-0-01uf-5-c0g-50vdc-0402/dp/4803046RL) | |
| All | 100nF 50V 0402 capacitor | 15 | 0402 >= 50V 10% X7R | [GRM155R71H104KE14D](https://uk.farnell.com/murata/grm155r71h104ke14d/cap-0-1-f-50v-10-x7r-0402/dp/2611912) | |
| All | 2.2uF 0603 capacitor | 7 | 0603 >= 25V 10% X7R | [GRM188Z71E225KE43D](https://uk.farnell.com/murata/grm188z71e225ke43d/cap-mlcc-2-2uf-x7r-25v-0603/dp/4335731) | |
| All | 4.7uF 0805 capacitor | 2 | 0805 >= 50V 10% X7R | [GRM21BZ71H475KE15K](https://uk.farnell.com/murata/grm21bz71h475ke15k/cap-4-7uf-50v-mlcc-0805/dp/3582887) | |
| All | 10uF 0603 capacitor | 1 | 0603 >= 10V 20% X7R | [C1608X7R1A106M080AT](https://www.digikey.co.uk/en/products/detail/tdk/C1608X7R1A106M080AT/25595431?s=N4IgTCBcDaIMIEYBsAGAHADQOwCUEEEEUkBZdFfAFRAF0BfIA) | |
| All | 10uF input capacitor | 3 | 1210 >= 50V 10% X7R SOFT TERMINATION | [MCJCU32MLB7106KPPDT1](https://uk.farnell.com/taiyo-yuden/mcjcu32mlb7106kppdt1/capacitor-mlcc-10uf-50v-x7r-1210/dp/4666637) | |
| All | 22uF 0805 capacitor | 1 | 0805 >= 10V 20% X7R | [GMC21X7R226M10NT](https://www.digikey.co.uk/en/products/detail/cal-chip-electronics-inc/GMC21X7R226M10NT/22461324) | |
| All | 47uF 1210 capacitor | 4 | 1210 >= 10V 20% X7R | [CL32B476MPJNNNE](https://uk.farnell.com/semco/cl32b476mpjnnne/cap-mlcc-47uf-10vdc-x7r-1210/dp/5109745) | |
| All | 47uF 2220 input capacitor | 2 | 2220 >= 50V 20% X7R | [CKG57NX7R1H476M500JH](https://uk.farnell.com/tdk/ckg57nx7r1h476m500jh/cap-stacked-47uf-50v-mlcc-2220/dp/3816888) | |
| All | Inductor | 2 | XFL4020-222ME | [XFL4020-222MEC](https://uk.farnell.com/coilcraft/xfl4020-222mec/inductor-2-2uh-8a-20-pwr-38mhz/dp/2289216) | |
| All | RF inductor | 1 | 0603 47-68 nH, SRF > 2GHz | [LQW18AN68NJ00D](https://www.ebay.co.uk/itm/178539565608) | |
| All | Ferrite bead | 1 | 600 ohm | [BLM15PX601SZ1D](https://uk.farnell.com/murata/blm15px601sz1d/ferrite-bead-0-9a-0-23ohm-0402/dp/3678458) | |
| All | 2A fuse | 3 | 1206 2A slow blow | [0407002.WRA](https://www.ebay.co.uk/itm/178539955750) | Time-lag, one per input |
| All | SQ2361 MOSFET | 3 | SQ2361 SOT-23 | [SQ2361CES-T1_BE3](https://www.ebay.co.uk/itm/178539853850) | |
| All | PNP transistor | 1 | BC856BLT1G SOT-23 | [BC856BLT1G](https://www.ebay.co.uk/itm/178539823265) | |
| All | Shunt voltage reference | 1 | ATL431BQDBZR SOT-23 | [ATL431BQDBZR](https://www.ebay.co.uk/itm/178539382502) | 2.5V, 0.5%, low Iq |
| All | Reverse-polarity MOSFET | 1 | SQJ457EP-T1_BE3 | [SQJ457EP-T1-GE3](https://www.ebay.co.uk/itm/178539865485) | Battery input |
| All | MOSFET | 3 | 2N7002 SOT-23 | [2N7002](https://uk.farnell.com/multicomp-pro/2n7002/mosfet-n-ch-60v-0-115a-sot-23/dp/4295174) | |
| All | 15V Zener diode | 3 | BZX84C15 | [BZX84C15](https://uk.farnell.com/multicomp-pro/bzx84c15/zener-diode-0-3w-15v-sot-23/dp/2675186) | |
| All | 400W TVS diode | 4 | PTVS33VS1UTR,115 SOD-123W | [PTVS33VS1UTR,115](https://www.ebay.co.uk/itm/178539916580) | |
| All | Ideal diode | 2 | LM66100 | [LM66100](https://www.ebay.co.uk/itm/178539979903) | |
| All | INA238 voltage read IC | 1 | INA238 10-VSSOP | [INA238AIDGSR](https://www.digikey.co.uk/en/products/detail/texas-instruments/INA238AIDGSR/15857213) | |
| All | Buck converter | 1 | LT8609AIMSE MSOP-EP-10 | [LT8609AIMSE#PBF](https://www.ebay.co.uk/itm/178544379670) | |
| All | Reverse-blocking load switch | 2 | Active high 3.3v load switch with reverse blocking | [SiP32431DR3-T1GE3](https://www.ebay.co.uk/itm/178539837620) | |
| All | ASM330LHHXTR accelerometer | 1 | ASM330LHHXTR | [ASM330LHHXTR](https://estore.st.com/en/products/mems-and-sensors/inemo-inertial-modules/asm330lhhx.html) | |
| All | Status LED | 3 | 0603 | [150060RS86000](https://uk.farnell.com/wurth-elektronik/150060rs86000/led-red-800mcd-624nm/dp/3408608) | |
| All | u.FL PCB connector | 1 | 50 ohm | [U.FL-R-SMT(01)](https://www.ebay.co.uk/itm/178540053901) | |
| All | SMA PCB connector | 2 | 50 ohm | [SMA-J-P-H-RA-TH1](https://www.ebay.co.uk/itm/178540028662) | |
| All | 130K 0402 resistor | 1 | 0402 130K 1% | Any 0402 130K 1% | |
| All | 2.2M 0402 resistor | 2 | 0402 2.2M 1% | ERJ2RKF2204X (Panasonic) | |
| All | 200K 0402 resistor | 1 | 0402 200K 1% | Any 0402 200K 1% | |
| All | 47K 0402 resistor | 1 | 0402 47K 5% | [ERJ2RKF4702X](https://uk.farnell.com/panasonic/erj2rkf4702x/res-47k-1-0-1w-0402-thick-film/dp/2302806) | |
| All | 57.6K 0402 resistor | 1 | 0402 57.6K 1% | Any 0402 57.6K 1% | |
| All | 909K 0402 anti-sulfur resistor | 2 | 0402 909K 1% ANTI-SULFUR AEC-Q200 | [AF0402FR-07909KL](https://www.digikey.co.uk/en/products/detail/yageo/AF0402FR-07909KL/16992580) | |
| All | 560pF 0402 capacitor | 1 | 0402 >= 50V 5% C0G/NP0 | Any 0402 560pF C0G | |
| All | 6.8M 0603 resistor | 1 | 0603 6.8M 1% | Any 0603 6.8M 1% 75V | |
| All | CR123A cell holder | 1 | Keystone 1050 CR123A SMT holder | 1050 (Keystone) | |
| All | Boost converter | 1 | LTC3122EMSE MSOP-12-EP | [LTC3122EMSE#PBF](https://www.digikey.co.uk/en/products/detail/analog-devices-inc/LTC3122EMSE-PBF/3516527) | |
| All | Silicon rectifier diode | 1 | S1B SMA 100V 1A silicon | [S1B-13-F](https://www.ebay.co.uk/itm/178539943449) | |
| All | GPIO expander | 1 | TCA6408APWR TSSOP-16 | TCA6408APWR (Texas Instruments) | |
| All | Car-rail sense comparator | 1 | TLV3012 SOT-23-6 | [TLV3012AIDBVR](https://www.digikey.co.uk/en/products/detail/texas-instruments/TLV3012AIDBVR/1678203) | |
| All | u.FL cable | 1 | 50 ohm 35mm | [U.FL-2LPHF6-04N1TV-A-35](https://uk.farnell.com/hirose-hrs/u-fl-2lphf6-04n1tv-a-35/cbl-assy-u-fl-r-a-plug-r-a-plug/dp/4294251) | LTE. The GPS antenna goes straight to SMA S15J2, no cable |
| All | U-blox MAX-M10S GPS Module | 1 | U-blox MAX-M10S GPS Module | [MAX-M10S-00B](https://www.digikey.co.uk/en/products/detail/u-blox/MAX-M10S-00B/15712906) | |
| All | SPI flash | 1 | MX25R6435FZNIL0 64Mbit WSON-8 6x5mm | MX25R6435FZNIL0 (Macronix) | Verify stock/P/N before ordering |
| All | JST power cable | 1 | Ultra-thin MX1.25 51146 | [10PCS MX1.25 51146 Cable](https://www.ebay.co.uk/itm/178550497683) | |
| CAN | 120R 0805 resistor | 1 | 0805 120R 5% 0.5W | [MCPAS05W2J0121T5E](https://uk.farnell.com/multicomp-pro/mcpas05w2j0121t5e/res-120r-5-0-5w-thick-film-0805/dp/4069078) | |
| CAN | 220R 0402 resistor | 2 | 0402 220R 5% | [MC00625W04021220R](https://uk.farnell.com/multicomp/mc00625w04021220r/res-220r-1-0-0625w-0402-thick/dp/1358024) | |
| CAN | 1K 0402 resistor | 1 | 0402 1K 5% | [CRCW04021K00FKED](https://uk.farnell.com/vishay/crcw04021k00fked/res-1k-1-0-063w-0402-thick-film/dp/1469662) | |
| CAN | 10K 0402 resistor | 3 | 0402 10K 5% | [CRCW040210K0FKED](https://uk.farnell.com/vishay/crcw040210k0fked/res-10k-1-0-063w-0402-thick-film/dp/1469669) | |
| CAN | 100K 0402 resistor | 1 | 0402 100K 5% | [MCPWR02FTEP1003A](https://uk.farnell.com/multicomp-pro/mcpwr02ftep1003a/res-100k-1-thick-film-0402/dp/4538624) | |
| CAN | 1M 0402 resistor | 1 | 0402 1M 5% | [ERJ2RKF1004X](https://uk.farnell.com/panasonic/erj2rkf1004x/res-1m-1-0-1w-0402-thick-film/dp/2302957) | |
| CAN | 27pF capacitor | 2 | 0402 >= 10V 5% C0G / NP0 | [GCM1555C1H270FA16D](https://uk.farnell.com/murata/gcm1555c1h270fa16d/cap-aec-q200-27pf-50v-mlcc-0402/dp/3581175) | |
| CAN | 100nF 50V 0402 capacitor | 3 | 0402 >= 50V 10% X7R | [GRM155R71H104KE14D](https://uk.farnell.com/murata/grm155r71h104ke14d/cap-0-1-f-50v-10-x7r-0402/dp/2611912) | |
| CAN | 2.2uF 0603 capacitor | 1 | 0603 >= 25V 10% X7R | [GRM188Z71E225KE43D](https://uk.farnell.com/murata/grm188z71e225ke43d/cap-mlcc-2-2uf-x7r-25v-0603/dp/4335731) | |
| CAN | 10uF 0603 capacitor | 1 | 0603 >= 10V 20% X7R | [C1608X7R1A106M080AT](https://www.digikey.co.uk/en/products/detail/tdk/C1608X7R1A106M080AT/25595431?s=N4IgTCBcDaIMIEYBsAGAHADQOwCUEEEEUkBZdFfAFRAF0BfIA) | |
| CAN | 40MHz crystal | 1 | ECS-400-18-33-JGN-TR3 | [ECS-400-18-33-JGN-TR3](https://www.ebay.co.uk/itm/178539630480) | |
| CAN | CAN protector | 1 | NUP2105L | [NUP2105L](https://www.ebay.co.uk/itm/178539794622) | |
| CAN | CAN common-mode choke | 1 | ACT1210-101-2P-TL00 | [ACT1210-101-2P-TL00](https://www.digikey.com/en/products/detail/tdk-corporation/ACT1210-101-2P-TL00/4918053) | Optional; if omitted fit 2 x 0402 0R |
| CAN | TCAN3414DR CAN transceiver | 1 | TCAN3414DR | [TCAN3414DR](https://www.ebay.co.uk/itm/178539591356) | |
| CAN | MCP2518FD CAN controller | 1 | MCP2518FDT-H/SL | [MCP2518FDT-H/SL](https://www.ebay.co.uk/itm/178539603676) | |
| CAN | Reverse-blocking load switch | 1 | Active high 3.3v load switch with reverse blocking | [SiP32431DR3-T1GE3](https://www.ebay.co.uk/itm/178539837620) | |
| CAN | 2-pin 2.54mm header | 1 | 2-pin 2.54mm header | [68001-402HLF](https://uk.farnell.com/amphenol-communications-solutions/68001-402hlf/conn-header-2pos-1row-2-54mm-th/dp/3881905) | |
| K-wire | 510R 0508 resistor | 1 | 0508 510R 5% 1W | [3430A2F510RTDF](https://uk.farnell.com/cgs-te-connectivity/3430a2f510rtdf/res-510r-1w-thick-film-0508-wide/dp/4206859) | |
| K-wire | 10K 0402 resistor | 2 | 0402 10K 5% | [CRCW040210K0FKED](https://uk.farnell.com/vishay/crcw040210k0fked/res-10k-1-0-063w-0402-thick-film/dp/1469669) | |
| K-wire | 100K 0402 resistor | 4 | 0402 100K 5% | [MCPWR02FTEP1003A](https://uk.farnell.com/multicomp-pro/mcpwr02ftep1003a/res-100k-1-thick-film-0402/dp/4538624) | |
| K-wire | 180K 0402 resistor | 1 | 0402 180K 5% | [MCWR04X1803FTL](https://uk.farnell.com/multicomp-pro/mcwr04x1803ftl/res-180k-1-0-0625w-thick-film/dp/2447116) | |
| K-wire | 1M 0402 resistor | 1 | 0402 1M 5% | [ERJ2RKF1004X](https://uk.farnell.com/panasonic/erj2rkf1004x/res-1m-1-0-1w-0402-thick-film/dp/2302957) | |
| K-wire | 1nF capacitor | 2 | 0402 >= 50V 10% X7R | [0402B102K500CT](https://uk.farnell.com/multicomp-pro/0402b102k500ct/cap-1000pf-50v-10-x7r-0402/dp/2496767) | |
| K-wire | 100nF 50V 0402 capacitor | 2 | 0402 >= 50V 10% X7R | [GRM155R71H104KE14D](https://uk.farnell.com/murata/grm155r71h104ke14d/cap-0-1-f-50v-10-x7r-0402/dp/2611912) | |
| K-wire | 220nF capacitor | 1 | 0603 >= 50V 10% X7R | [GRM188R71H224KAC4D](https://uk.farnell.com/murata/grm188r71h224kac4d/cap-0-22-f-50v-10-x7r-0603/dp/2688525) | |
| K-wire | 2.2uF 0603 capacitor | 1 | 0603 >= 25V 10% X7R | [GRM188Z71E225KE43D](https://uk.farnell.com/murata/grm188z71e225ke43d/cap-mlcc-2-2uf-x7r-25v-0603/dp/4335731) | |
| K-wire | MOSFET | 1 | 2N7002 SOT-23 | [2N7002](https://uk.farnell.com/multicomp-pro/2n7002/mosfet-n-ch-60v-0-115a-sot-23/dp/4295174) | |
| K-wire | 1N4148W diode | 1 | 1N4148W | [1N4148W-E3-08](https://www.ebay.co.uk/itm/178539887745) | |
| K-wire | 400W TVS diode | 1 | PTVS33VS1UTR,115 SOD-123W | [PTVS33VS1UTR,115](https://www.ebay.co.uk/itm/178539916580) | |
| K-wire | Line transceiver | 1 | TJA1027T\_20,118 | [TJA1027T\_20,118](https://uk.farnell.com/nxp/tja1027t-20-118/lin-transceiver-20kbaud-18v-soic/dp/2400570) | |
| K-wire | 12V load switch | 1 | Active high 12v load switch | [ITS4060SSJNXUMA1](https://www.ebay.co.uk/itm/178539828483) | |
| K-wire | Reverse-blocking load switch | 1 | Active high 3.3v load switch with reverse blocking | [SiP32431DR3-T1GE3](https://www.ebay.co.uk/itm/178539837620) | |
| Optional | 1M 0402 anti-sulfur resistor | 1 | 0402 1M 1% ANTI-SULFUR AEC-Q200 | [AF0402FR-071ML](https://uk.farnell.com/yageo/af0402fr-071ml/res-1m-1-0-063w-thick-film-0402/dp/4148383) | Buck enable divider: S6R4, in place of the 0R jumper |
| Optional | 243K 0402 anti-sulfur resistor | 1 | 0402 243K 1% ANTI-SULFUR AEC-Q200 | [MCMR04X2433FTL](https://uk.farnell.com/multicomp-pro/mcmr04x2433ftl/res-243k-1-0-0625w-0402-ceramic/dp/2072824) | Buck enable divider: S6R5 |
| Optional | 3.92M 1% resistor | 1 | 0402 3.92M 1% | [CRCW04023M92FKED](https://uk.farnell.com/vishay/crcw04023m92fked/res-3m92-1-0-063w-0402-thick-film/dp/2141113) | Buck enable divider: S6R6 |

