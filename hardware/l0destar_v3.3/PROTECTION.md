# 12 V Input Protection

**This document is desk analysis, not a test report.** The figures are estimates
derived from datasheets and manufacturer characterisation data, none of it has
been verified on hardware, and the interpretation may be wrong. Before relying
on any of it, read the [project disclaimer](../../DISCLAIMER.md).

How the board survives what a car's 12 V system throws at it, and why each
part was chosen. These decisions have been re-litigated several times and
always land in the same place, so the reasoning is written down here.

Everything below was checked against the schematic, the PCB netlist and the
part datasheets on 23 Aug 2026, except "The L line pull-down", added and
checked on 28 Aug 2026. PCB DRC is clean with zero unconnected items.

## The protection chain

Both 12 V inputs (permanent battery and ignition) get the same treatment:

```
harness fuse (external, 2 A, one per input)
    |
on-board fuse (S2F1 / S2F2, 2 A time-lag, one per input)
    |
    +---- TVS to ground (PTVS33VS1UTR, 33 V standoff)
    |
reverse-blocking P-FET (battery: S2Q2 SQJ457EP; ignition: S2Q1 SQ2361ES;
                        gate zener BZX84C15 on both)
    |
bulk capacitance (2 x 47 uF + 10 uF, all 50 V, permanent rail only)
    |
LT8609A buck, then the 4.2 V rail and the rest of the board
```

Each piece has one job. The TVS clips large spikes. The FET blocks reverse
voltage. The bulk capacitors absorb the one fast pulse the TVS cannot clip
low enough on its own. The fuses cover the one fault the TVS cannot survive
unaided.

## Load dump: why the TVS is 33 V and must not be lowered

When an alternator loses its battery connection, the vehicle's central
suppression limits the surge to 35 V, held for up to 400 ms. The board's job
is to ride this out, not to clamp it. The 33 V TVS breaks down at 36.7 V
minimum, so at 35 V it just leaks microamps and the buck (rated 42 V) sees
the event directly and shrugs it off.

A lower TVS, say 24 V, feels safer but is a trap. It would conduct hard for
the entire 400 ms, absorbing somewhere between 12 and 53 joules. The part is
rated for about 0.4 J. It burns, fails short, and takes the input with it.
This exact "improvement" has been proposed and rejected several times.

The 53.3 V clamping figure in the datasheet applies to a 10/1000 us surge at
7.5 A. It has nothing to do with load dump and quoting it against load dump
has caused repeated false alarms.

Unsuppressed load dump (ISO 16750-2 Test A, 79 to 101 V) is deliberately not
covered. Surviving it needs an active surge stopper, and a hardwired tracker
on a modern vehicle with working central suppression does not need one.

## Fast pulses: why there is so much bulk capacitance

The fast transients are defined by ISO 7637-2. The design target is the 2011
edition's maximum test levels, and one pulse dominates everything: pulse 2a,
which is +112 V behind 2 ohms for 50 us. Every other pulse is either too
brief, too weak, or negative (where the FETs and TVS handle it).

The strategy for pulse 2a is to soak it up rather than clamp it. With enough
capacitance on the rail, the 50 us pulse cannot charge the rail past
dangerous levels before it ends. With a single 47 uF the rail would reach
about 56 V, well past every rating on the net. With both capacitors fitted
(about 84 uF effective, counting the buck's input capacitor) the peak is
about 39 V. The binding limit is not the buck at 42 V but the ITS4060 OBD
power switch at 40 V, and 39 V clears it. Both capacitors are now in the
schematic and on the board.

A series resistor instead of the second capacitor was considered and
rejected: it protects the buck but leaves the input node clamping at 76 V,
which everything else on the rail would then see. Capacitance suppresses the
event; a resistor only relocates it.

The capacitance figures are backed by measured data, not the generic X7R
heuristic: TDK's characterization sheet for the 47 uF part shows about 14
percent loss at 14 V bias, so each contributes roughly 40 uF effective.

A third capacitor, S2C3 (10 uF / 50 V 1210, Taiyo Yuden MCJ soft termination,
MCJCU32MLB7106KPPDT1), was added to close the last soft spot: purchase
tolerance. The 47 uF parts are rated plus or minus 20 percent, and if both
arrived at the bottom of that band the peak would have reached about 44 V.
With S2C3 fitted, even that worst-case stack stays at about 42 V, inside the
buck's limit, and the typical case drops to about 37 V, right at TVS
breakdown, meaning the diode barely conducts and everything has margin. S2C3
is X7R like the main bulk, but rated to plus or minus 10 percent rather than
20, and it is that tighter tolerance which makes it useful against the
purchase-tolerance case. Its termination is covered in the next section.

The on-board fuses do not interfere with any of this. A 2 A 407 Series part
has a nominal melting I2t of 0.870 A2Sec, against roughly 0.03 to 0.1 A2Sec
for the whole pulse 2a charging event, so there is an order of magnitude of
margin. Its 0.1 ohm sits in series with the pulse's 2 ohm source, which
slightly reduces the peak rather than adding to it.

The ignition input carries no bulk capacitance and needs none: it only feeds
a sense divider, and during a fast pulse the TVS clamp plus the divider's
100 nF filter keep the sensing transistor safe.

## Flex cracking: why the three bulk capacitors are terminated differently

A ceramic capacitor that cracks from board flex usually fails as a low
resistance short, not an open. On PP12VP that is the worst available failure:
the rail is fed straight from the battery, so a shorted cap draws current
continuously rather than causing a reset. Every bulk capacitor on this net
therefore has to carry a flex-crack mitigation. Which mitigation depends on
the package, and the two are not interchangeable.

**S2C1 and S2C2 (TDK CKG57N, 2220) are leadframe stacked parts.** The MLCC
chips sit on a metal J-lead frame and it is the frame, not the ceramic, that
solders to the board. The compliant legs take up board bending before it
reaches the dielectric. This is mechanical decoupling and it is the strongest
mitigation available - better than soft termination, not a substitute for it.
Soft termination on these would be redundant.

**S2C3 (Taiyo Yuden MCJ, 1210) is a plain monolithic chip.** Its body solders
rigidly to the pads, so any flex goes straight into the ceramic. The fix for
that geometry is soft termination: a conductive resin layer between the
internal electrode and the solder termination that absorbs the strain. The
fitted part is AEC-Q200 qualified, which the commercial-grade CKG57N is not.

The split is also partly forced. There is no 47 uF plain ceramic at 50 V -
anything above 10 uF at 50 V is a stacked assembly - so S2C1 and S2C2 were
always going to be leadframe parts. At 10 uF / 50 V / 1210 a plain chip is
available, which is why specifying the soft-termination variant there was a
real choice rather than an automatic one.

The rule for any future change: **every bulk capacitor on PP12VP must be
either a leadframe stacked part or a soft-termination chip.** A standard
plain chip capacitor is not acceptable on this net at any value. Keep them
away from board edges, mounting holes and depanelisation points, where flex
is worst.

## Negative transients and reverse battery

The P-FETs block reverse voltage for everything downstream, and they cost
nothing at rest (the gate network draws no current in normal operation).

During a large negative pulse (ISO 7637-2 pulse 1, up to minus 150 V for
2 ms) the TVS clamps the input near ground and the FET disconnects once the
rail sags. The buck loses its input for the rest of the pulse and the 4.2 V
rail runs on its output capacitors. At idle the module rides through; under
heavy load (an LTE transmit burst) it may brown out and reboot. A rare
reset on a severe transient is accepted behaviour for a tracker. It
recovers on its own when the input returns.

The TVS diodes sit upstream of the FETs, so under sustained reverse battery
they conduct forward like ordinary diodes into an unlimited source. Fusing
exists for exactly this: fault current clears in milliseconds, well inside
the TVS's forward surge capability.

## Why there are now two fuses per input

The external harness fuse (2 A, one per input) is still the expected
installation and is still meant to be the one that clears in ordinary
service. Every circuit in a vehicle is separately fused anyway, and a blown
on-board fuse means a board swap instead of a fuse swap.

S2F1 and S2F2 were added on top of that for a different reason: not every
unit gets installed the way the manual says. An unfused feed that goes
reverse or shorts is a vehicle fire, and against that a fuse that costs a
board swap is plainly the better outcome. The on-board fuses are a fire
backstop for the unsupported install, not a replacement for harness fusing.
Installation without both harness fuses is still not a supported
configuration; the on-board parts exist so that an unsupported one fails
safe rather than dangerously.

The part is a Littelfuse 0407002.WRA, 2 A, 1206, time-lag. Time-lag matters
here: the 407 Series is a high-I2t design specifically intended to survive
inrush and surge without nuisance opening, which is what lets it sit in the
pulse 2a charging path without being consumed by it. A fast-acting fuse of
the same rating would not be a safe substitute.

## Open items on the on-board fuses

Three things about S2F1 and S2F2 have not been worked through yet. None of
them block the current build, but all three should be closed before the
fuses are relied on as a safety claim.

**Interrupting rating versus the unfused install.** The 2 A part is rated to
interrupt 50 A at 63 VDC. The fault it exists for is limited only by battery
internal resistance and harness wiring. With 0.1 ohm of fuse and roughly
0.1 ohm of 20 AWG feed the available current is already around 60 A, and
shorter or thicker wire pushes it higher. Past its interrupting rating a
fuse can rupture or arc instead of clearing cleanly, which defeats the
purpose. Check this against the worst harness the product will realistically
see.

**Continuous rating at automotive ambient.** Littelfuse recommends no more
than 80 percent of rating continuously (1.6 A), and the temperature
re-rating curve takes more off again in a hot vehicle, likely landing near
1.1 to 1.3 A usable. PP12V_K, the ITS4060 OBD 12 V output, sits downstream
of S2F2 along with the entire board. Confirm the worst-case sum of board
load and anything the OBD port sources against that derated figure.

**Selectivity.** The external harness fuse and the on-board fuse are both
2 A, and nothing guarantees the external one opens first in an ordinary
overload, so a routine fault may still cost a board swap. If that matters,
the on-board parts want to be rated above the external ones: high enough to
let the harness fuse win the normal case, low enough to remain a credible
fire backstop.

## The 2N7002 sensing transistors

The gate voltage rating of a "2N7002" depends on who made it: ST rates
theirs at plus or minus 18 V, Nexperia and Diotec at plus or minus 30 V.
Rather than pin the BOM to one manufacturer, the OBD presence divider was
re-ratioed to 180K over 100K. Its gate now sees about 14 V at the pulse 2a
peak and about 12.5 V during a load dump, inside every vendor's rating with
margin. The ignition sense divider is 180K over 56K with a 100 nF gate
filter, which puts its gate at about 9 V at the same pulse 2a peak. The
K-line transistor is driven by 3.3 V logic. Any brand of 2N7002 is
acceptable.

If either divider is ever changed, re-check the gate voltage at 39 V (the
pulse 2a rail peak) and 35 V (load dump), and keep the 100 nF on the
ignition divider.

## The L line pull-down: current-limited in v3.3

Up to v3.2 the L line was pulled down by a bare 2N7002 (S10Q1) with
its gate wired straight to the L_SEND GPIO. If the external L wire was shorted
to battery while the firmware drove a 5-baud init, the FET was switched hard
into that short. A 2N7002 at Vgs 3.3 V saturates somewhere between about
100 mA and 1 A depending on brand and threshold spread, so it dissipated
roughly 1 to 12 W in a SOT-23 and failed inside the first address bit. Roughly
half of those failures take the gate with them, and a drain-gate short puts
battery voltage directly onto L_SEND, past the nRF9151 absolute maximum. That
is a module kill, not just a dead transistor.

v3.3 breaks that chain in three places:

- S10U2, an AL5809-90 constant-current regulator, is now in series between the
  L pin and the FET drain, so fault current is capped at 90 mA instead of
  whatever the FET happens to pass. The regulator absorbs the fault voltage
  instead: at 12 to 16 V and 90 mA that is about 1.1 to 1.4 W, which no small
  SMD package sustains continuously, so the part's own thermal shutdown is the
  intended endpoint rather than an accident. The fold-back time, and what the
  L line does while it folds back, are bench items - they are not derived
  anywhere in this document.
- S10R7, 4.7K, is new in the gate lead between L_SEND and S10Q1. In v3.2 the
  gate sat directly on the GPIO. If the FET ever does fail drain-gate, the pin
  now sees about 2 mA once its clamp conducts rather than an unlimited 12 V
  source. This is not a substitute for the current limit; it removes the
  specific failure mode that cost modules rather than boards.
- The FET is no longer the sacrificial element. At 90 mA and Vgs 3.3 V a
  2N7002 drops about 0.7 V and dissipates under 100 mW. The 3.3 V logic note
  in "The 2N7002 sensing transistors" above still applies unchanged.

S10D5 and S10R8 add the L_SENSE readback, taken off the regulator output
rather than the L pin. The diode is oriented cathode to the L side so an
external 12 V is blocked rather than injected, and the 47K limits the pin to
about 250 uA if that diode ever fails short.

## The blocking FET during pulse 2a: resolved by the SQJ457EP

The current that charges the bulk capacitors during pulse 2a flows through
the blocking FET, and at the +112 V level that is a hard event: about 45 A
peak decaying over the 50 us pulse.

This used to be the board's weakest accepted risk. The original SQ2361ES is
rated for 11 A pulsed and about 8 mJ of single-pulse avalanche energy, with
an estimated junction temperature rise near the 175 degC limit, so at the
maximum test level it was at or slightly past its paper ratings.

That is no longer the case. The battery input now carries S2Q2, a Vishay
SQJ457EP-T1_BE3 (PowerPAK SO-8L, -60 V, 100 A pulsed, 25 mOhm). Peak current
sits at well under half the pulsed rating, the roughly 5x lower Rds(on) cuts
the energy dissipated in the device to a few millijoules, and the package has
the die area and thermal path to absorb it. It is also AEC-Q101 automotive
qualified. Every number moved from
at-the-limit to 2x margin or better, on the same gate network. Formal
ISO 7637-2 testing at maximum severity no longer needs a board revision
first.

S2Q1, the ignition-side FET, is still an SQ2361ES in SOT-23. It carries no
bulk charging current and never needed the swap.

## Settled. Do not re-open without new information

- The 33 V TVS standoff. Lowering it causes guaranteed load dump failure.
- Reverse protection exists (the P-FETs) and is wired correctly, including
  TVS orientation on both inputs (verified from the netlist).
- Two 47 uF plus one 10 uF on the permanent rail, sized for pulse 2a at the
  2011 maximum level including worst-case purchase tolerance. All fitted.
  A series resistor is not an acceptable substitute.
- The leadframe stacked capacitor package is the flex-crack mitigation, not
  a risk. Every bulk capacitor on PP12VP must be either leadframe stacked
  (S2C1, S2C2) or soft-terminated (S2C3); a standard plain chip capacitor is
  not acceptable on this net. There is also no 47 uF plain ceramic at 50 V;
  anything above 10 uF at 50 V is a stacked assembly. See "Flex cracking"
  above.
- Fusing is doubled deliberately. External harness fuses remain the
  supported installation; the on-board 2 A time-lag fuses are a fire
  backstop for units that arrive without them. Neither replaces the other.
- The on-board fuses must stay time-lag. A fast-acting 2 A part would be
  at risk from pulse 2a and from hot-plug inrush.
- The battery blocking FET is the SQJ457EP in PowerPAK SO-8L. Do not revert
  it to the SQ2361ES to recover the smaller footprint.
- The AL5809-90 (S10U2) stays in series with the L line pull-down, and S10R7
  stays in the S10Q1 gate lead. Removing either restores the v3.2 defect: the
  board looks and behaves identically until the first short to battery.
- No damping electrolytic is needed for hot-plug ringing.
- 2N7002 gate dividers are in spec for any manufacturer after the 180K
  re-ratio. No zeners.
- The LM66100 (S11U1) sits between PP4V2_OVP_PROTECTED and PP4V2, after
  the OVP MOSFET. Its CE pin is tied to its own output (PP4V2). This is a
  TI-documented configuration for reverse current blocking — it prevents
  downstream capacitors from back-driving the rail when the OVP trips, and
  blocks reverse current if an external supply is attached to the VCC
  header. Not a wiring error.

## Known accepted limitations

- ISO 16750-2 Test A (unsuppressed load dump) is out of scope.
- The ITS4060 OBD switch is an industrial part, not AEC-Q qualified, and
  sits just above its functional range during a load dump. It survives, but
  the OBD 12 V output may misbehave for the duration. Accepted.
- A severe negative transient can reboot the device (see above). Accepted.
- The fitted CKG57N capacitor is the commercial grade part. An AEC-Q200
  version exists as the JJ suffix (CKG57NX7R1H476M500JJ) if automotive
  grading ever matters.
- The 407 Series fuse is not AEC-Q qualified either, though its -55 to
  +150 degC range covers the environment.

## Key numbers

| Item | Value |
|---|---|
| Suppressed load dump | 35 V for up to 400 ms |
| Pulse 2a design level (ISO 7637-2:2011 max) | +112 V, 2 ohm, 50 us |
| TVS breakdown (minimum) | 36.7 V |
| LT8609A input, absolute max | 42 V |
| ITS4060 supply, absolute max | 40 V (this is the binding limit) |
| Battery blocking FET (S2Q2) | SQJ457EP, -60 V, 100 A pulsed, 25 mOhm |
| Ignition blocking FET (S2Q1) | SQ2361ES, -60 V, 11 A pulsed |
| Rail peak, pulse 2a, all bulk fitted | about 37 V typical, about 42 V worst-case tolerance |
| CKG57N capacitance retention at 14 V bias (TDK measured) | about 86 percent |
| FET peak current during pulse 2a at +112 V | about 45 A vs 100 A pulsed rating |
| On-board fuse (S2F1, S2F2) | 0407002.WRA, 2 A time-lag, 63 V, 50 A interrupting, 0.100 ohm |
| On-board fuse nominal melting I2t | 0.870 A2Sec vs about 0.03 to 0.1 A2Sec for pulse 2a |
| OBD sense gate, worst case | about 14 V |
| L line fault current, external short to battery | 90 mA, set by the AL5809-90 |

## References

- [PTVS33VS1UTR series datasheet, Nexperia](https://assets.nexperia.com/documents/data-sheet/PTVSXS1UTR_SER.pdf)
- [SQ2361ES datasheet, Vishay](https://www.mouser.com/datasheet/2/427/VISH_S_A0001811243_1-2567854.pdf)
- [SQJ457EP datasheet, Vishay](https://www.vishay.com/docs/76628/sqj457ep.pdf)
- [2N7002 datasheet, Nexperia](https://assets.nexperia.com/documents/data-sheet/2N7002.pdf)
- [AL5809 datasheet, Diodes Incorporated](https://www.diodes.com/assets/Datasheets/AL5809.pdf)
- [ITS4060S-SJ-N datasheet, Infineon](https://www.infineon.com/assets/row/public/documents/10/49/infineon-its4060s-sj-n-datasheet-en.pdf)
- [LT8609A datasheet, ADI](https://www.mouser.com/pdfDocs/LT8609-8609A.pdf)
- [LM66100 datasheet, TI](https://www.ti.com/lit/ds/symlink/lm66100.pdf)
- [Littelfuse 407 Series, 1206 time-lag fuse datasheet](https://www.farnell.com/datasheets/3157884.pdf)
- [TI SNOAAA1, load dump protection](https://www.ti.com/lit/pdf/snoaaa1)
- [onsemi TND6424, automotive transient levels incl. pulse 2a 112 V](https://www.onsemi.com/download/design-notes/pdf/tnd6424-d.pdf)
- [ADI LTspice models of ISO 7637-2 and ISO 16750-2 transients](https://www.analog.com/en/resources/technical-articles/ltspice-models-of-iso-7637-2-iso-16750-2-transients.html)
- [TDK characterization sheet, CKG57NX7R1H476M500JH, incl. DC bias curve](https://product.tdk.com/system/files/dam/doc/product/capacitor/ceramic/mlcc/charasheet/ckg57nx7r1h476m500jh.pdf)
- [DC bias effects on MLCCs, osengr.org](https://www.osengr.org/Articles/DC-Bias-Effects-on-MLCCs.pdf)
- [KEMET, flex crack mitigation](https://www.digikey.com/en/ptm/k/kemet/ceramic-capacitor-flex-crack-mitigation)
