# Component selection

The bill of materials is in the
[v3.4 README](https://github.com/m4rkw/l0destar/blob/master/hardware/l0destar_v3.4/README.md).
This page helps you decide what to build, explains how the bill of materials is organised, and
calls out the parts that must not be casually substituted.

!!! warning "The bill of materials is an example, not a verified BOM"
    Working boards have been built with many of the listed parts, but not necessarily from the
    same vendor, series or batch. Check every part against its current datasheet - footprint,
    tolerance, voltage rating - before you buy. The specifications in the bill of materials are
    minimums; some example links point at tighter-tolerance or higher-rated parts than the minimum
    because that is what the author buys.

## Choose the vehicle interface

Every v3.4 board has the same PCB. The vehicle diagnostic interface is optional and comes in two
flavours; you populate the parts for the one you want, bridge the matching configuration pads, and
tell the firmware which one is fitted.

| Build | Parts to fit | Pads to bridge | Firmware setting | What it gives you with firmware 0.4.x |
|---|---|---|---|---|
| No interface | Required parts only | none | `CONFIG_APP_OBD_MODE=0` | Position, speed, battery voltage, ignition state, accelerometer alerts |
| CAN-FD | Required + CAN bus parts | S5R1 and S5R3 | `CONFIG_APP_OBD_MODE=1` | The CAN-FD hardware, without vehicle data yet - see below |
| K-wire | Required + K-wire parts | S5R2 and S5R4 | `CONFIG_APP_OBD_MODE=2` | OBD-II engine data and stored fault codes |

To see what your vehicle uses, look at its OBD socket: CAN diagnostics use pins 6 (CAN high) and 14
(CAN low); the K wire is pin 7, with the optional L wire on pin 15.

- **K-wire.** The firmware opens a KWP2000 (ISO 14230) session over the K wire and can report
  engine RPM, vehicle speed, coolant and intake temperature, load, throttle, mass air flow, timing
  advance, fuel trims, fuel system status and the check-engine lamp with every record, and send
  the stored fault codes after the ignition comes on and whenever they change during a drive. The same wire carries ISO 9141-2 and
  manufacturer-specific pre-OBD protocols such as VAG KW1281, which the firmware does not speak.
  Each vehicle needs a one-off discovery run first.
- **CAN-FD.** The CAN block - an MCP2518FD CAN-FD controller and a CAN-FD transceiver - handles
  both classic CAN and CAN-FD. On a v3.1 board, with the MAX33041E transceiver that earlier boards
  fit, it passed about 60,000 frames in both modes from 125kbps up to an 8Mbps FD data phase (see
  the [CAN bench test report](https://github.com/m4rkw/l0destar/blob/master/firmware/CAN_BENCH_REPORT.md)).
  8Mbps is beyond the MAX33041E's 5Mbps rating, and there it logged 2 data-phase bit errors, both
  retried and delivered; the TCAN3414DR that v3.4 specifies has not been tested.
  The firmware powers it, puts it to sleep and has bench tests for it, but **firmware 0.4.x does
  not read any vehicle data over CAN**, and the interface has not been tried in a vehicle. Build it
  if you intend to work on CAN support.
- **No interface.** The smallest and cheapest build, and the right one if you only want a
  tracker.

Both sets of interface parts can be populated, but only one interface may be enabled at a time.

!!! danger "Never bridge the CAN and K pads together"
    The pads route the same two connector pins to one interface or the other. Connecting both at
    once gives unpredictable behaviour. The full pad table is in the
    [hardware reference](/reference/hardware.md#interface-selection-pads).

## How the bill of materials is organised

The README splits the parts into sections:

| Section | Contents |
|---|---|
| Bill of materials - required | Everything every build needs, including the Connect Kit, cables and connectors |
| Optional: buck enable divider | Three resistors that give the buck converter a defined under-voltage cut-off; not fitted by default |
| Bill of materials - CAN bus parts | Only for a CAN build |
| Optional: CAN common-mode choke | Part of the CAN build, but optional |
| Bill of materials - K-wire parts | Only for a K-wire build |
| Parts list | The same parts aggregated with quantities per board: `All` rows plus the `CAN` or `K-wire` rows for your build, and the `Optional` rows if you fit the buck enable divider |

Designators start with the schematic sheet they belong to, which helps when finding parts on the
board:

| Prefix | Subsystem |
|---|---|
| S1 | Main connector, Connect Kit headers and power header, I2C pull-ups |
| S2 | 12V power input protection |
| S3 | INA228 battery voltage monitor |
| S4 | Ignition sensing |
| S5 | Interface configuration pads |
| S6 | Buck converter |
| S7 | Switched auxiliary power rails |
| S8 | Accelerometer |
| S9 | CAN interface |
| S10 | K-wire interface |
| S11 | Over-voltage protection for the Connect Kit |
| S12 | Test points |
| S13 | Status LEDs |
| S15 | Antenna connectors, bias tee and ESD protection |

## Optional parts

**Buck enable divider (S6R4, S6R5, S6R6).** By default S6R4 is a 0402 0R jumper and S6R5 and S6R6
are not fitted, so the buck runs whenever input voltage is present and relies on the LT8609A's own
internal under-voltage lockout. Fitting the divider instead (S6R4 1M, S6R5 243K, S6R6 3.92M, all
1%) makes the buck start only above about 5.6V and shut off below about 4.3V by calculation (a v3.3 board measured 5.14V on and 4.47V off), which gives clean
behaviour in ISO 16750-2 style low-voltage and drop-out events. It costs around 9µA of sleep
current at 12V, which is why it is not fitted by default. Values and formula are in the
[hardware reference](/reference/hardware.md#buck-enable-divider).

**CAN common-mode choke (S9FL1).** An ACT1210-101-2P-TL00 in series with CAN high and CAN low,
which improves emissions and common-mode noise rejection. CAN works without it.

!!! warning "If you leave out the CAN choke, bridge its pads"
    Without S9FL1 the CAN high and CAN low lines are open circuit. Fit two 0402 0R resistors
    across its footprint to join each line through - an 0402 fits the ACT1210 pads.

**CAN termination (S9R3 and S9J1).** A 120R resistor in series with a 2-pin header. Fit a shunt on
the header only if the tracker is at the end of the CAN bus; plugged into a vehicle's OBD socket it
normally is not.

## Parts that need care

The reasoning behind most of these is in
[PROTECTION.md](https://github.com/m4rkw/l0destar/blob/master/hardware/l0destar_v3.4/PROTECTION.md).
It is desk analysis rather than a test report, but it records why each choice was made and which
"improvements" have already been considered and rejected.

### 12V input

| Part | Designators | Why it matters |
|---|---|---|
| All capacitors on the 12V rails | - | Must be rated 50V or more to survive transients. |
| 47µF 50V 2220, TDK CKG57NX7R1H476M500JH | S2C1, S2C2 | Bulk capacitance that soaks up ISO 7637-2 pulse 2a (+112V behind 2 ohms for 50µs). These are leadframe-stacked parts: the metal frame, not the ceramic, is soldered to the board, which is the flex-crack protection. An AEC-Q200 version exists as CKG57NX7R1H476M500JJ. |
| 10µF 50V 1210 soft termination, Taiyo Yuden MCJCU32MLB7106KPPDT1 | S2C3 | Keeps the pulse 2a peak in bounds even if both 47µF parts arrive at the bottom of their tolerance. It is a plain chip, so it must be a soft-termination part. |
| 2A 1206 time-lag fuse, Littelfuse 0407002.WRA | S2F1, S2F2 | One per 12V input, a fire backstop for installations without harness fuses. Must stay time-lag: a fast-acting 2A fuse is at risk from pulse 2a and hot-plug inrush. |
| 33V TVS, Nexperia PTVS33VS1UTR,115 | S2D2, S2D4 | Breaks down at 36.7V minimum, so a 35V suppressed load dump passes straight through to the 42V-rated buck instead of being clamped. **Do not fit a lower-voltage TVS**: it would try to absorb the whole 400ms load dump, far beyond its rating, and fail short. |
| Reverse-polarity P-FET, Vishay SQJ457EP-T1_BE3 | S2Q2 | On the permanent input, where it carries the pulse 2a charging current (about 45A peak against a 100A pulsed rating). Do not replace it with the smaller SQ2361. |
| Reverse-polarity P-FET, SQ2361 | S2Q1 | On the ignition input, which only feeds a sense divider. |
| 15V zener, BZX84C15 | S2D1, S2D3 | Gate protection for the two input FETs. |

Every bulk capacitor on the permanent 12V rail must be either a leadframe-stacked part or a
soft-termination chip. A plain ceramic chip that cracks from board flex usually fails short, and on
a rail fed straight from the battery that means a continuous fault current.

### Regulation and protection

| Part | Designators | Why it matters |
|---|---|---|
| LT8609AIMSE buck converter | S6U1 | 42V absolute maximum input and very low quiescent current, so it adds almost nothing to the sleep budget. |
| Coilcraft XFL4020-222ME inductor | S6L1 | 2.2µH buck inductor. |
| Output divider, S6R2 226K and S6R3 1M | S6R2, S6R3 | Sets the output to 0.782V x (1 + S6R3/S6R2) = 4.24V. Keep them 1%; anti-sulfur AEC-Q200 parts recommended. |
| Frequency resistor, 18.2K 1% | S6R1 | Sets the switching frequency to 2MHz. |
| Over-voltage protection stage | S11U2 (ATL431BQDBZR), S11Q1, S11Q2, S11D1, S11R1-S11R6, S11C1-S11C3 | Disconnects the Connect Kit's supply if the 4.2V rail rises above about 4.95V, releasing below about 4.80V. The Connect Kit's battery input and the nRF9151 both have a 5.5V absolute maximum, so this is what saves the module from a buck fault. Keep the S11R5/S11R6 divider at 1% - a 5% pair widens the trip window at both ends. S11R4 must be 0805 for current handling in fault conditions. |
| LM66100 ideal diode | S11U1 | Between the protection stage and the Connect Kit. Its CE pin tied to its output is a documented configuration, not a wiring error. |

### Sensing

| Part | Designators | Why it matters |
|---|---|---|
| ST ASM330LHHXTR accelerometer | S8U1 | Pads 10 and 11 must be unconnected, as they are on v3.4. Fed through ferrite bead S8R1, with 100pF filter capacitors S8C4 and S8C5 on the I2C lines. |
| INA228 voltage monitor | S3U1 | Measures the vehicle battery voltage over I2C. |
| I2C pull-ups, 1.8K 1% | S1R1, S1R2 | Sized to keep the bus rise time in spec. |
| 2N7002 | S4Q1, and S7Q1 and S10Q1 on K-wire builds | Any manufacturer's 2N7002 is acceptable: the gate dividers keep every vendor's gate rating in spec. |
| SiP32431DR3-T1GE3 load switch | S7U1 (GPS), S7U2 (CAN), S7U4 (K-wire) | Reverse-blocking 3.3V switches that power each auxiliary rail only when the firmware needs it. |

### Interfaces and RF

| Part | Designators | Why it matters |
|---|---|---|
| TCAN3414DR CAN transceiver | S9U1 | Fully rated for CAN-FD, with timing specified at 2, 5 and 8Mbps. The MAX33041EASA+ fitted on v3.3 has the same footprint and pinout and can be fitted instead; it is rated to 5Mbps. |
| MCP2518FD controller with a 40MHz crystal | S9U2, S9Y1 (ECS-400-18-33-JGN-TR3), S9C2, S9C3 | CAN-FD controller on SPI. |
| NUP2105L | S9D1 | CAN bus protection. |
| NXP TJA1027T/20 transceiver | S10U1 | It has no transmit dominant time-out, which is what lets it hold the K wire low for the 200ms bits of a 5-baud init. A LIN transceiver with a time-out cannot do that. |
| Diodes Inc. AL5809-90P1-7 | S10U2 | Caps the L-line pull-down at 90mA and shuts down thermally, so an L wire shorted to battery cannot destroy the pull-down transistor or the nRF9151. |
| ITS4060SSJNXUMA1 12V load switch | S7U3 | Switches the K-wire 12V rail. Its 40V absolute maximum is the lowest rating on the 12V rail. |
| 510R 1W 0508 resistors | S10R3, S10R4 | K and L line pull-ups. |
| 33V TVS, PTVS33VS1UTR,115 | S10D2, S10D4 | Protection on the K and L lines. |
| 15R 1W 0508 resistor | S15R1 | The bias-tee feed that powers an active GNSS antenna from the switched 3.3V GPS rail. |
| RF inductor, 47-68nH with SRF above 2GHz | S15L1 | Part of the bias tee; the example is a Murata LQW18AN68NJ00D. |
| TPD1E05U06DPYR ESD diodes | S15D1, S15D2 | ESD protection at the LTE and GPS antenna connectors. |
| SMA-J-P-H-RA-TH1 and U.FL-R-SMT(01) | S15J1-S15J4 | Antenna connectors. |
| Molex 43045-0600 | S1J1 | 6-pin Micro-Fit 3.0 vehicle connector. |

## Buying the parts

- Buy spares of every 0402 part.
- Work from the **Parts list** table in the README, taking the `All` rows plus the rows for your
  interface, and the `Optional` rows if you fit the buck enable divider. Treat the specification
  column as the minimum, not the part number.
- Use a distributor you trust. The links in the README are for identification and go stale.

## Enclosure hardware

If you print the enclosure from
[`hardware/enclosure/v3.3/`](https://github.com/m4rkw/l0destar/tree/master/hardware/enclosure/v3.3),
you also need:

| Item | Example |
|---|---|
| M2 heat-set threaded inserts | https://link.amazon/B0fiGzjDl |
| M2 x 10mm screws | https://link.amazon/B04Nlsa3c |
| Heat-set insert tip for C210-style irons (optional, makes straight inserts much easier) | https://link.amazon/B01C41sjy |

The enclosure is a prototype that has not been tested for strength, vibration, heat, moisture,
UV or flammability, and the material you print it in affects all of those.
