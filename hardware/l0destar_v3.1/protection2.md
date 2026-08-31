# How the v3.1 input stage stands up to ISO 16750-2 and ISO 7637-2

**Desk analysis only, nothing here has been tested on hardware.** See
[PROTECTION.md](PROTECTION.md) for the detailed reasoning and the numbers, and
the [project disclaimer](../../DISCLAIMER.md) before relying on any of it.

**Short version on 24 V vehicles: the board is not designed for them.** The
protection was sized for a 12 V system. On a 24 V vehicle the normal running
voltage (about 28 V) is fine, but the 24 V versions of the overvoltage, load
dump and motor-spin-down tests push the input above the 33 V TVS diode's
clamp point and the buck's 42 V limit. Those cases would destroy the input
stage. Each point below says so where it applies. A 24 V variant would need
at minimum a higher-voltage TVS, a higher-voltage buck and re-rated
capacitors, so it is a different board, not a component swap.

The parts referred to throughout: the two on-board 2 A fuses, the 33 V TVS
diode on each input, the reverse-blocking P-FET on each input, the bulk
capacitors (2 × 47 µF + 10 µF, 50 V) on the permanent rail, and the LT8609A
buck (rated 3 V to 42 V in) that makes the 4.2 V the module runs on.

---

## ISO 16750-2

### Supply voltage range

![Supply voltage range](images/protection/01_supply_range.png)

The 12 V rail is never 12 V. You have to work correctly across roughly 9–16 V
continuously (code A), and if you claim start-stop capability, down to 6 V
(code B) or 8 V (code C) during cranking. In the 2023 edition these are Tables
3 and 4; you pick a code and declare it. Class A performance — full function,
no degradation — is required across the declared range.

**12 V: solved.** The buck converter takes anything from about 3 V up to 42 V
and turns it into a steady 4.2 V, so the rest of the board never notices
whether the battery is at 9 V or 16 V. It keeps regulating down to about
4.5 V at the input (4.2 V out plus a little headroom), so 6 V and 8 V
cranking codes are covered with margin. The reverse-blocking FET drops only a
few tens of millivolts, so it does not eat into that headroom the way a
series diode would. Class A across the whole range.

**24 V: solved for the normal running range.** The 24 V range (roughly
16–32 V) is inside the buck's input range and below the TVS diode's 33 V
standoff, so day-to-day operation on a 24 V truck would work.

### Overvoltage, long term

![Overvoltage, long term](images/protection/02_overvoltage_long.png)

The alternator regulator fails and the rail sits at 18 V (12 V system) or 36 V
(24 V system) for an hour at high temperature. You must survive it. Whether
you must keep working depends on the agreed status class; surviving is not
optional.

**12 V: solved, and it keeps working.** 18 V is well inside the buck's 42 V
limit and well below the TVS's 33 V standoff, so the TVS just sits there
doing nothing. Every part on the 12 V rail (buck, OBD switch, sense
transistors, capacitors) is rated far above 18 V. Nothing gets warm.

**24 V: not solved.** 36 V is right at the TVS diode's breakdown point (it
starts conducting somewhere between 36.7 V and about 40 V depending on the
individual part and temperature). At high temperature the diode would be
leaking heavily or conducting outright for an hour, which it cannot survive.
The buck itself would be fine at 36 V, but the TVS would likely fail short and
blow the fuse.

### Overvoltage, transient

![Overvoltage, transient](images/protection/03_overvoltage_transient.png)

Five pulses of 18 V (or 36 V), 400 ms each, 1 ms rise and fall, one second
apart. Shorter and repeated, so the concern is your regulator's absolute
maximum rating and any thermal accumulation.

**12 V: solved.** Same as above: 18 V is nowhere near any limit, and 400 ms
five times over adds no meaningful heat. The board just keeps running.

**24 V: not solved.** Same problem as the long-term case: 36 V sits on the
TVS's breakdown edge. Five 400 ms pulses is less abuse than an hour, but a
part that conducts even for part of each pulse at 36 V would be dissipating
far more than its rating.

### Superimposed AC ripple

![Superimposed AC ripple](images/protection/04_ripple.png)

Alternator ripple and DC/DC switching noise riding on the DC. The 2023 edition
extended this to 200 kHz, which matters more than it used to — it now
overlaps the switching frequency of your own buck converter and can beat
against it. Test is a sine sweep at a few volts peak-to-peak on top of nominal.

**12 V: partly solved, not analysed.** The 100 µF-plus of bulk capacitance on
the rail is a low-impedance path that will soak up most of the ripple before
it reaches the buck, and the buck's own regulation removes what is left from
the 4.2 V output. This is the expected outcome, but nothing has been
calculated or simulated for the ripple test specifically, and the question of
ripple beating against the buck's switching frequency has not been looked at.

**24 V: same as 12 V.** The ripple sits on top of a 28 V rail rather than
13.5 V; the mechanism is the same and the capacitors are still well within
their 50 V rating.

### Slow voltage ramp down and up

![Slow voltage ramp down and up](images/protection/05_slow_ramp.png)

Battery gradually discharging to zero and then being recharged, at around
0.5 V/min. The failure mode this catches is the "zombie" state — supply sags
into the region where your regulator is out of spec but the MCU hasn't reset,
so it half-runs, writes garbage to flash, or holds the modem in an undefined
state. Brownout thresholds and reset supervisors are the fix.

**12 V: mostly solved, low priority.** There is no undervoltage lockout on
the buck (its enable pin is tied straight to the 12 V rail, so it runs until
its own internal ~3 V minimum) and no separate reset supervisor chip. As the
input falls below about 4.5 V the 4.2 V output sags with it. That matters
less than it sounds: the nRF9151 is specified from 3.0 V to 5.5 V, so the
module is still inside its supply range until the input is down to roughly
3.5 V, which is almost at the buck's own floor. The undefined region is only
the last half volt or so, and at that point the vehicle battery is already
destroyed. What has not been verified is whether anything on the Connect
Kit between the battery connector and the nRF9151 has a higher cutoff of its
own, and how much the reverse-blocking FET's on-resistance rises once its
gate drive is down to a few volts.

What the design does have is a watchdog: the firmware runs a 32 s task
watchdog backed by the nRF9151's hardware watchdog timer
(`firmware/src/watchdog.c`, `CONFIG_TASK_WDT_HW_FALLBACK=y`). If the MCU
stops running its main loop for 32 s, for any reason, the hardware resets it.
That covers the "hung" half of the zombie state: a processor that has
stalled or wandered off after a dirty brown-out gets forcibly restarted, and
if the supply is still too low it simply keeps resetting until it recovers,
which is the correct behaviour. It does not cover the other half: a
processor that is still running, so still feeding the watchdog, but at a
voltage where flash writes or the modem interface are out of spec. A
watchdog detects inactivity, not low voltage. Whether that window exists
depends on the Connect Kit's brown-out threshold versus the point at which
things start misbehaving, and that has not been measured.

If a defined threshold is ever wanted it is three 0402 resistors on the
buck's EN/UV pin: 1 M from PP12VP to EN/UV, 300 k from EN/UV to ground, and
3.9 M from PP4V2 back to EN/UV for hysteresis. With the LT8609A's EN/UV
threshold of 1.05 V typical (0.99 to 1.11 V) and 50 mV of pin hysteresis
that gives turn-on at about 4.8 V (4.5 to 5.1 V) and turn-off at about
3.5 V (3.2 to 3.8 V); the ±20 nA pin current is a few millivolts of error
and can be ignored. The module then sees a clean
off/on instead of a slow sag and the watchdog becomes the backstop. Worth
adding next time the schematic is open; not worth a study.

**24 V: same status.** The mechanism is identical; the ramp just starts
higher.

### Momentary drop in supply voltage

![Momentary drop in supply voltage](images/protection/06_momentary_drop.png)

Voltage dips to a few volts for tens of milliseconds, simulating another
circuit's fuse blowing or a heavy load switching. You must recover cleanly.

**12 V: mostly solved.** The bulk capacitors on the input hold charge, and the
reverse-blocking FET stops that charge flowing back out into the harness when
the supply dips, so the buck keeps running from the capacitors. There are also
two 47 µF capacitors on the 4.2 V output. Whether that is enough to ride
through a whole dip depends on how much the board is drawing at that moment: at
idle it should; in the middle of an LTE transmit burst it may not, in which
case the module brown-outs and reboots. A reboot on a severe dip is accepted
behaviour for a tracker (it recovers on its own), but "recovers cleanly" also
depends on the zombie-state gap above.

**24 V: same as 12 V.** The capacitors hold more energy at 28 V, so the
ride-through is if anything a little longer.

### Reset behaviour at voltage drop

![Reset behaviour at voltage drop](images/protection/07_reset_at_drop.png)

Deliberately walks the supply down in small steps to find the exact voltage
where you reset, and checks you reset properly rather than hanging.

**12 V: mostly solved.** There is no defined reset voltage on the board, so
the reset point is wherever the Connect Kit's regulator and the nRF9151's
brown-out detector give up, somewhere around 3.5 V at the input, and that
has not been measured. The test's second requirement, that the device
resets properly rather than hanging, is covered by the hardware-backed
watchdog: any state the MCU gets stuck in is cleared by a hardware reset
within 32 s. So the outcome at the reset voltage is not undefined, it is
"worst case, running again inside 32 s". The EN/UV divider described under
"Slow voltage ramp" would give a defined, testable threshold if one is
wanted.

**24 V: same status.**

### Interruption of supply

![Interruption of supply](images/protection/08_interruption.png)

Micro-interruptions from milliseconds to seconds — bad crimps, corroded
ground, connector chatter over a pothole. The 2023 edition splits this into
static (single event) and dynamic (bursts of them). This is the one that most
often bites a tracker: a 10 ms dropout that reboots the modem costs you a
30-second network reattach.

**12 V: partly solved.** For short interruptions (milliseconds to a few tens
of milliseconds) the input capacitors plus the FET's back-flow blocking, plus
the output capacitors, keep the module alive; the idle current is tiny, so at
rest the hold-up is comfortably long. During a transmit burst the hold-up is
much shorter and a reboot is possible. Interruptions of seconds are not
covered: nothing on the board can hold the module up for that long, and it
will reboot and reattach. Bursts of interruptions have not been analysed.

**24 V: same as 12 V**, with slightly longer hold-up because the input
capacitors store more energy at 28 V.

### Reverse polarity

![Reverse polarity](images/protection/09_reverse_polarity.png)

Someone connects the battery backwards, or installs your device backwards.
Typically 60 s at −13 V or −14 V. Must not be damaged. A series Schottky costs
you voltage headroom; a P-FET or ideal-diode controller is the usual answer.

**12 V: solved, with a caveat about the fuse.** Each input has a P-FET wired
as a reverse blocker, so everything downstream of it (capacitors, buck, the
whole board) simply sees no voltage when the battery is backwards. The FETs
are rated to −60 V. The one thing upstream of the FET is the TVS diode, which
under reverse voltage conducts like an ordinary diode straight across the
supply. That is a dead short, and it is what the fuses are for: the on-board
2 A fuse (and the harness fuse) clears in milliseconds, well before the TVS
overheats. So the board survives, but a reverse connection will blow a fuse,
which on the on-board part means a board swap rather than a fuse swap. The
FETs' gate zeners (15 V) keep the gate-to-source voltage safe at −14 V.

**24 V: solved, same mechanism.** −28 V is still inside the FETs' −60 V
rating, and the fuse clears the TVS short the same way.

### Ground offset / open circuit

![Ground offset / open circuit](images/protection/10_ground_open.png)

Any single connector pin disconnecting must not damage the device or cause a
hazard. Also loss of ground reference, where your ground is at a different
potential to the vehicle's.

**12 V: partly solved, not analysed as a whole.** Losing the battery or
ignition pin is just a supply interruption and is harmless. Losing the ground
pin while the OBD, CAN or K-line connections are attached means the board's
return current tries to find its way back through those signal pins instead;
that path has not been analysed. Small ground offsets (a volt or so) are
tolerated by the CAN transceiver and by the ignition sense divider. The
detailed single-pin-open analysis for every pin on every connector has not
been done.

**24 V: same status.**

### Short circuit and overload protection

![Short circuit and overload protection](images/protection/11_short_circuit.png)

Every input and output shorted to battery positive and to ground,
individually. The 2023 edition adds overload of load circuits. Nothing may
catch fire or fail permanently.

**12 V: partly solved.** The 12 V inputs shorted to ground are protected by
the fuses. The switched 12 V output on the OBD connector goes through the
ITS4060 high-side switch, which has its own current limit and thermal
shutdown, so shorting that output to ground is handled. The 3.3 V rails are
internal: they come from the Connect Kit and feed only on-board parts, so
they cannot be shorted to the harness. The externally reachable pins are the
CAN and K-line lines on the vehicle connector and the two antenna connectors.
The CAN transceiver and K-line transceiver are automotive parts rated for
shorts to battery, but that has not been checked pin by pin. The GPS SMA
centre pin carries the 3.3 V bias-tee rail and is not protected against a
short to 12 V, though an antenna coax touching battery positive is not a
realistic harness fault. "Nothing catches fire" is covered by the fuses;
"nothing fails permanently" is not guaranteed for every pin.

**24 V: partly solved, less so.** The fuses still cover the fire case. Any
pin shorted to a 28 V battery positive is more likely to damage the part on
the other end than a 14 V short would be, and the transceivers' short-to-
battery ratings need checking against 24 V figures.

### Withstand voltage and insulation resistance

Where isolation is claimed.

**12 V and 24 V: not applicable.** No isolation is claimed anywhere on the
board; the vehicle ground is the board ground.

### Load dump (4.6.4)

![Load dump (4.6.4)](images/protection/13_load_dump.png)

Battery terminal disconnects while the alternator is delivering current. This
lives in 16750-2 now, not 7637-2. Two cases: Test A, unsuppressed alternator,
pulse to ~100 V or more; Test B, centralised suppression (clamped alternator),
around 35 V for 12 V systems. Duration is 100–400 ms with source impedance of
a few ohms — huge energy, not a spike. For a modern aftermarket product you
design to Test B, but you should know which one your target vehicles are.

**12 V, Test B: solved, by deliberately not clamping it.** The design rides
the 35 V surge out rather than fighting it. The TVS diode is a 33 V part that
does not start conducting until at least 36.7 V, so at 35 V it does nothing.
The buck is rated to 42 V and the OBD switch to 40 V, so both just see a high
input for 400 ms and carry on. This is the reason the TVS must be 33 V and not
lower: a lower TVS would try to absorb the whole surge, which is tens of
joules against a part rated for a fraction of one, and it would burn out.

**12 V, Test A: not solved, deliberately.** An unsuppressed 80–100 V load
dump would exceed the buck's 42 V limit and the TVS would be destroyed trying
to clamp it. Surviving Test A needs an active surge-stopper circuit that is
not fitted. The design assumes a modern vehicle with a clamped alternator.

**24 V: not solved, either test.** The 24 V Test B level is about 58 V. That
is above the TVS's clamp point and above the buck's 42 V limit, so the TVS
would conduct for the whole 400 ms and fail, and the buck would be over its
rating. A 24 V board needs a higher-voltage TVS and a higher-voltage buck.

---

## ISO 7637-2 - fast transients coupled onto the supply lines

### Pulse 1

![Pulse 1](images/protection/14_pulse1.png)

Negative spike, down to around −75 V to −150 V, roughly 2 ms wide, 10 Ω
source. Caused by an inductive load (relay coil, solenoid, motor) being
switched off while you're connected in parallel with it. Applies to you if
you're wired anywhere near a relay-driven circuit, which a tracker usually is.

**12 V: solved, with an accepted reboot risk.** Two things happen together.
The TVS diode conducts forwards for negative voltages, so it pins the input
close to ground (about −1 V) for the 2 ms; the 10 Ω source means the current
is only around 15 A, which the TVS handles easily. Meanwhile the
reverse-blocking FET sees the input go below its output and switches off, so
the capacitors and the buck are cut off from the pulse entirely. The buck runs
from the stored charge for the 2 ms. At idle it rides through; in the middle
of a transmit burst it may brown out and reboot. That occasional reboot is
accepted.

**24 V: solved, same mechanism.** The 24 V pulse 1 is larger (down to
−300 V, 50 Ω source) but the TVS still clamps it near ground and the current
is of the same order. The FET's −60 V rating is never approached because the
TVS holds the input near ground.

### Pulse 2a

![Pulse 2a](images/protection/15_pulse2a.png)

Positive spike, up to about +112 V, 50 µs, 2 Ω source. Current in a circuit
parallel to you is suddenly interrupted and the harness inductance dumps into
your input.

**12 V: solved, and this is the case the input stage was sized for.** The
approach is to soak the pulse up rather than clamp it. The 2 Ω source can only
push a limited amount of charge in 50 µs, and the 100 µF-plus of bulk
capacitance absorbs that charge while rising only to about 24 V (about 28 V
at the worst tolerance and temperature corner). That is below the TVS's
clamp point, so the TVS never conducts, and below the 40 V limit of the most
sensitive part on the rail by about 12 V. The charging current (around 50 A
peak, decaying in microseconds) flows through the battery-side FET, which was
upgraded to a 100 A-pulsed part specifically so it takes this with 2× margin.
The time-lag fuses pass this current without blowing. See PROTECTION.md for
the full working, including why this must not be replaced with a series
resistor and why lower-value or higher-voltage-rated capacitors do not help.

**24 V: not analysed, likely fails.** The 24 V pulse 2a has the same +112 V
amplitude but starts from a 28 V baseline instead of 13.5 V, so the
capacitors are already partly charged and the peak lands higher. It has not
been calculated, but the 12 V worst case is already at 28 V, and adding 14 V
of baseline brings it uncomfortably close to the 40 V limit. Not something to
rely on without redoing the sums.

### Pulse 2b

![Pulse 2b](images/protection/16_pulse2b.png)

Positive, long — up to 10 V above nominal but lasting hundreds of
milliseconds, near-zero source impedance. A DC motor spinning down after
ignition off, acting as a generator. Relevant to a tracker because it happens
exactly when you've entered your low-power parked state.

**12 V: solved.** 10 V above nominal is about 24 V, which is a completely
ordinary voltage for this input: below the TVS, well inside every part's
rating, and the buck regulates it away. Being in the parked low-power state
makes no difference; the protection is all passive.

**24 V: not solved.** The 24 V version is 20 V above nominal, about 48 V,
held for hundreds of milliseconds from a stiff source. That is above the TVS
clamp point and the buck's 42 V limit; the TVS would conduct for the whole
event and fail.

### Pulse 3a

![Pulse 3a](images/protection/17_pulse3a.png)

Fast negative burst, −112 V to −220 V, 100 ns wide, 50 Ω, repeated for
minutes. Switching noise from commutating motors and relay contact bounce.

**12 V: solved at the input, not analysed for coupling.** These pulses are
so short and from such a high impedance that they carry almost no energy. The
TVS clamps negative pulses near ground and the bulk capacitors swallow what
gets past it without measurable voltage rise, so the power input itself is
fine. The real question for 3a/3b is whether the energy couples through the
board layout into ground, the antenna feeds or the signal lines, and that
depends on layout. The v3.1 layout has ESD diodes on both antenna connectors
and a solid ground plane, but nothing has been tested or simulated for this,
and it will not be known until a real burst test is run.

**24 V: same as 12 V.** The 24 V levels are higher (to −300 V) but the energy
is still tiny and the same reasoning applies.

### Pulse 3b

![Pulse 3b](images/protection/18_pulse3b.png)

Fast positive burst, +75 V to +150 V, same timings. Complement of 3a.

**12 V: solved at the input, not analysed for coupling.** Same as 3a: the
100 ns pulses carry almost no charge, so the bulk capacitors absorb them with
no meaningful voltage rise and the TVS is not even needed. The coupling
question is the same open item as for 3a.

**24 V: same as 12 V.**

### Pulse 4

![Pulse 4](images/protection/19_pulse4.png)

Cranking profile — supply drops to around 6 V, then recovers through a ramp
to nominal over several hundred milliseconds. Tests whether you brown out,
lose RAM, or corrupt storage during engine start. Now formally in 16750-2 but
still routinely tested under this name.

**12 V: solved.** The buck keeps producing 4.2 V from anything above about
4.5 V, so a dip to 6 V is not a brown-out at all; the module never sees it.
The reverse-blocking FET drops only millivolts, so it does not eat the
margin. The cranking profile's brief initial dip (to around 4.5 V in some
severity levels) is right at the buck's edge, and there the input and output
capacitors carry the module for the few milliseconds involved.

**24 V: solved.** The 24 V cranking profile dips to around 10–16 V, which is
comfortably inside the buck's range.

---

## Summary

| Test | 12 V | 24 V |
|---|---|---|
| Supply voltage range | Solved | Solved (normal range only) |
| Long-term overvoltage | Solved | **Fails** (36 V at TVS breakdown) |
| Transient overvoltage | Solved | **Fails** (same) |
| Superimposed ripple | Expected OK, not analysed | Same |
| Slow ramp down/up | Mostly: nRF9151 in spec down to ≈ 3.5 V in; watchdog catches hangs; optional UVLO divider | Same |
| Momentary drop | Mostly solved; reboot possible under load | Same |
| Reset at voltage drop | Mostly: watchdog guarantees recovery; threshold ≈ 3.5 V, not measured | Same |
| Interruption of supply | Short ones OK; long ones reboot | Same |
| Reverse polarity | Solved (blows a fuse) | Solved |
| Ground offset / open pin | Partly, not fully analysed | Same |
| Short circuit / overload | Fire case solved; not every pin analysed | Same, less margin |
| Withstand / insulation | Not applicable | Not applicable |
| Load dump Test B | Solved | **Fails** (58 V) |
| Load dump Test A | Out of scope, deliberately | **Fails** |
| Pulse 1 | Solved; reboot possible under load | Solved |
| Pulse 2a | Solved, with margin | Not analysed, likely fails |
| Pulse 2b | Solved | **Fails** (48 V) |
| Pulse 3a / 3b | Input solved; layout coupling untested | Same |
| Pulse 4 | Solved | Solved |

The one thing worth doing for the 12 V board is a pin-by-pin
short-to-battery check on the external connectors. The EN/UV divider on the
buck is a three-resistor addition to make next time the schematic is open;
it tidies the slow-ramp and reset-threshold rows but does not change how
the board behaves in practice. Everything marked "fails" under 24 V needs a different input
stage, not a tweak.
