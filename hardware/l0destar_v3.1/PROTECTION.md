# 12 V Input Protection

How the board survives what a car's 12 V system throws at it, and why each
part was chosen. These decisions have been re-litigated several times and
always land in the same place, so the reasoning is written down here.

Everything below was checked against the schematic, the PCB netlist and the
part datasheets on 16 Aug 2026. PCB DRC is clean with zero unconnected
items.

## The protection chain

Both 12 V inputs (permanent battery and ignition) get the same treatment:

```
harness fuse (external, 2 A, one per input)
    |
    +---- TVS to ground (PTVS33VS1UTR, 33 V standoff)
    |
reverse-blocking P-FET (SQ2361ES, gate zener BZX84C15)
    |
bulk capacitance (2 x 47 uF + 10 uF, all 50 V, permanent rail only)
    |
LT8609A buck, then the 4.2 V rail and the rest of the board
```

Each piece has one job. The TVS clips large spikes. The FET blocks reverse
voltage. The bulk capacitors absorb the one fast pulse the TVS cannot clip
low enough on its own. The external fuse covers the one fault the TVS cannot
survive unaided.

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

A third capacitor, S2C3 (10 uF / 50 V 1210, Murata GCJ soft termination),
was added to close the last soft spot: purchase tolerance. The 47 uF parts
are rated plus or minus 20 percent, and if both arrived at the bottom of
that band the peak would have reached about 44 V. With S2C3 fitted, even
that worst-case stack stays at about 42 V, inside the buck's limit, and the
typical case drops to about 37 V, right at TVS breakdown, meaning the diode
barely conducts and everything has margin. S2C3 is X7S rather than X7R;
that is fine in this role because its contribution is not load-bearing the
way the main bulk is, and its tighter 10 percent tolerance offsets the
slightly worse bias derating. It sits well away from board edges and
mounting holes, and its soft termination covers the flex-crack risk of a
plain chip capacitor on a battery-fed rail.

The ignition input carries no bulk capacitance and needs none: it only feeds
a sense divider, and during a fast pulse the TVS clamp plus the divider's
100 nF filter keep the sensing transistor safe.

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
they conduct forward like ordinary diodes into an unlimited source. The
external 2 A fuse exists for exactly this: fault current clears it in
milliseconds, well inside the TVS's forward surge capability. On-board
fusing was rejected because every circuit in a vehicle is separately fused
anyway, and a blown on-board fuse would mean a board swap instead of a fuse
swap. Installation without both harness fuses is not a supported
configuration.

## The 2N7002 sensing transistors

The gate voltage rating of a "2N7002" depends on who made it: ST rates
theirs at plus or minus 18 V, Nexperia and Diotec at plus or minus 30 V.
Rather than pin the BOM to one manufacturer, the OBD presence divider was
re-ratioed to 180K over 100K. Its gate now sees about 14 V at the pulse 2a
peak and about 12.5 V during a load dump, inside every vendor's rating with
margin. The ignition sense divider was already safe thanks to its 100 nF
gate filter, and the K-line transistor is driven by 3.3 V logic. Any brand
of 2N7002 is now acceptable.

If either divider is ever changed, re-check the gate voltage at 39 V (the
pulse 2a rail peak) and 35 V (load dump), and keep the 100 nF on the
ignition divider.

## The blocking FET during pulse 2a: known marginal, accepted

The current that charges the bulk capacitors during pulse 2a flows through
the blocking FET, and at the +112 V level that is a hard event: about 45 A
peak decaying over the 50 us pulse, roughly 14 mJ dissipated in the FET.
The SQ2361ES is rated for 11 A pulsed and about 8 mJ of single-pulse
avalanche energy, with an estimated junction temperature rise near the
175 degC limit. So at the maximum test level the FET is at or slightly past
its paper ratings. At the older +50 V level the same event is trivial
(16 A peak, 2 mJ, about 25 degC of rise).

This is accepted: real-world pulse 2a events rarely approach the bench
maximum, the part is AEC-Q101 qualified and 100 percent avalanche tested,
and the exposure is a compliance-lab scenario rather than a field one. Even
in the unlikely worst case the failure is contained: the FET likely fails
short, the device keeps running, and reverse-battery blocking on that input
falls back to the external fuse.

If formal ISO 7637-2 testing at maximum severity is ever planned, swap S2Q2
for a Vishay SQJ457EP (PowerPAK SO-8L, -60 V, 100 A pulsed, 25 mOhm) before
booking the lab. That takes every number from at-the-limit to 2x margin or
better with the same gate network, but it is a footprint change and so a
board revision. S2Q1 carries no bulk charging current and never needs the
swap. Nothing else on the board changes; the capacitors already protect the
rail itself.

## Settled. Do not re-open without new information

- The 33 V TVS standoff. Lowering it causes guaranteed load dump failure.
- Reverse protection exists (the P-FETs) and is wired correctly, including
  TVS orientation on both inputs (verified from the netlist).
- Two 47 uF plus one 10 uF on the permanent rail, sized for pulse 2a at the
  2011 maximum level including worst-case purchase tolerance. All fitted.
  A series resistor is not an acceptable substitute.
- The leadframe stacked capacitor package is the flex-crack mitigation, not
  a risk. Do not replace it with plain chip capacitors. There is also no
  47 uF plain ceramic at 50 V; anything above 10 uF at 50 V is a stacked
  assembly.
- External 2 A fusing is deliberate.
- No damping electrolytic is needed for hot-plug ringing.
- 2N7002 gate dividers are in spec for any manufacturer after the 180K
  re-ratio. No zeners.
- The LM66100 on the buck output has its CE pin tied to VOUT. This is a
  TI-documented configuration for reverse current blocking (it protects the
  buck if an external supply is attached to the VCC header), not a wiring
  error.

## Known accepted limitations

- ISO 16750-2 Test A (unsuppressed load dump) is out of scope.
- The ITS4060 OBD switch is an industrial part, not AEC-Q qualified, and
  sits just above its functional range during a load dump. It survives, but
  the OBD 12 V output may misbehave for the duration. Accepted.
- A severe negative transient can reboot the device (see above). Accepted.
- The blocking FET exceeds its pulsed ratings during pulse 2a at the +112 V
  maximum level (see above). Accepted unless formal compliance testing at
  that level is planned.
- The fitted CKG57N capacitor is the commercial grade part. An AEC-Q200
  version exists as the JJ suffix (CKG57NX7R1H476M500JJ) if automotive
  grading ever matters.

## Key numbers

| Item | Value |
|---|---|
| Suppressed load dump | 35 V for up to 400 ms |
| Pulse 2a design level (ISO 7637-2:2011 max) | +112 V, 2 ohm, 50 us |
| TVS breakdown (minimum) | 36.7 V |
| LT8609A input, absolute max | 42 V |
| ITS4060 supply, absolute max | 40 V (this is the binding limit) |
| SQ2361ES drain-source | 60 V |
| Rail peak, pulse 2a, all bulk fitted | about 37 V typical, about 42 V worst-case tolerance |
| CKG57N capacitance retention at 14 V bias (TDK measured) | about 86 percent |
| FET peak current during pulse 2a at +112 V | about 45 A vs 11 A rating |
| OBD sense gate, worst case | about 14 V |

## References

- [PTVS33VS1UTR series datasheet, Nexperia](https://assets.nexperia.com/documents/data-sheet/PTVSXS1UTR_SER.pdf)
- [SQ2361ES datasheet, Vishay](https://www.mouser.com/datasheet/2/427/VISH_S_A0001811243_1-2567854.pdf)
- [2N7002 datasheet, Nexperia](https://assets.nexperia.com/documents/data-sheet/2N7002.pdf)
- [ITS4060S-SJ-N datasheet, Infineon](https://www.infineon.com/assets/row/public/documents/10/49/infineon-its4060s-sj-n-datasheet-en.pdf)
- [LT8609A datasheet, ADI](https://www.mouser.com/pdfDocs/LT8609-8609A.pdf)
- [LM66100 datasheet, TI](https://www.ti.com/lit/ds/symlink/lm66100.pdf)
- [TI SNOAAA1, load dump protection](https://www.ti.com/lit/pdf/snoaaa1)
- [onsemi TND6424, automotive transient levels incl. pulse 2a 112 V](https://www.onsemi.com/download/design-notes/pdf/tnd6424-d.pdf)
- [ADI LTspice models of ISO 7637-2 and ISO 16750-2 transients](https://www.analog.com/en/resources/technical-articles/ltspice-models-of-iso-7637-2-iso-16750-2-transients.html)
- [TDK characterization sheet, CKG57NX7R1H476M500JH, incl. DC bias curve](https://product.tdk.com/system/files/dam/doc/product/capacitor/ceramic/mlcc/charasheet/ckg57nx7r1h476m500jh.pdf)
- [DC bias effects on MLCCs, osengr.org](https://www.osengr.org/Articles/DC-Bias-Effects-on-MLCCs.pdf)
- [KEMET, flex crack mitigation](https://www.digikey.com/en/ptm/k/kemet/ceramic-capacitor-flex-crack-mitigation)
