# Assembly: read this first

This section walks through building a l0destar carrier board by hand, from ordering the PCB to
a powered bench test. Read this page, and the project's full
[disclaimer](https://github.com/m4rkw/l0destar/blob/master/DISCLAIMER.md), before you order
anything. If you intend to fit the finished tracker to a vehicle, also read
[Deployment: read this first](/deployment/read-this-first.html).

!!! danger "Fuse both 12V inputs"
    A board wired to a vehicle battery can start a fire if it is built, wired or fused wrong.
    It is essential to fit an external 2A fuse on the permanent 12V feed **and** on the ignition
    feed. The 2A fuses on the board are a fire backstop for installations made without them, not
    a substitute for them.

## This is a prototype, not a product

- l0destar is a personal project in the prototyping and design phase. Treat every board, and
  the firmware, as unvalidated and unsafe by default.
- Nothing has been through automotive qualification, EMC testing, or formal ISO 7637-2 or
  ISO 16750 compliance testing. The protection analysis in
  [PROTECTION.md](https://github.com/m4rkw/l0destar/blob/master/hardware/l0destar_v3.4/PROTECTION.md)
  is desk work backed by datasheets - estimates and reasoning, not a test report - and the
  reasoning may be wrong.
- The design is not validated for safety-critical or security-critical use. It may fail
  silently, and it must not be anyone's only theft-recovery, safety or emergency measure.

## Which board these pages describe

These pages describe the **l0destar v3.4** board.

- **v3.4 is designed but has not yet been built or tested.** Every row of the test table in the
  [v3.4 README](https://github.com/m4rkw/l0destar/blob/master/hardware/l0destar_v3.4/README.md)
  reads NOT TESTED.
- **v3.3 is the most recent board that has been built and bench tested.** v3.4 changes the
  accelerometer footprint (see below), makes the buck converter's enable divider an optional
  part that is not fitted by default, adds test points, enlarges the holes for the Connect Kit power
  lead and specifies a TCAN3414DR CAN transceiver in place of the MAX33041E (same footprint and
  pinout).
- Earlier v3.x boards are close enough that most of the assembly guide still applies, but the
  test points and the designators in the over-voltage protection stage differ.

## Test results are the author's own, and unverified

The measurements and pass/fail results quoted throughout these docs are the author's own
observations, on the author's boards, under bench conditions, with the author's instruments.
They are not independently verified. Repeat the testing yourself rather than taking them on
trust.

## Parts lists are examples, not a verified BOM

The bills of materials are suggestions. Working boards have been built with many of the listed
parts, but not necessarily from the same vendor, series or batch, and distributor links go
stale. Check every part against its current datasheet, satisfy yourself about footprint,
tolerance and voltage rating, and buy from a distributor you trust. Distributor and manufacturer
links are for identification only - they are not endorsements.

## If you build one, you are responsible for it

That means your own due diligence on part selection, assembly, fusing, wiring and installation.

- A device wired to a vehicle battery can start a fire if it is built or fused wrong.
- Anything connected to OBD or CAN can interfere with systems you want working while the
  vehicle is moving, or cause irreparable damage to very expensive computers in the car.
- 24V vehicles are not supported.

## Known defects on older boards

If you are working with a board older than v3.4, two defects matter.

### The L wire on every board before v3.3

Every board before v3.3 switches the K-wire interface's L-line pull-down transistor (a 2N7002)
straight onto the vehicle's L wire, with nothing limiting the current. An L wire shorted to
battery looks exactly like a healthy idle one - both sit at 12-16V - and driving the pull-down
into that short makes the transistor dissipate roughly 1-12W in a SOT-23. It fails within the
first 200ms address bit of a 5-baud init. Roughly half of those failures involve the gate, and
a drain-gate short puts battery voltage directly onto the nRF9151 GPIO that drives it, which is
past the module's absolute maximum and can destroy it.

!!! danger "Never connect the vehicle's L wire to a board before v3.3"
    The firmware leaves L-line driving disabled by default on those boards
    (`CONFIG_APP_L_SEND_ENABLED`), but the only safe hardware position is not to connect the L
    wire at all. v3.3 fixed the defect: an AL5809-90 limits the pull-down to 90mA and shuts down
    thermally, a 47K resistor limits fault current into the GPIO, and a new L_SENSE input lets
    the firmware detect a short to battery before it tries to drive the line.

### The accelerometer on v3.0 to v3.3

On every board from v3.0 to v3.3 the ASM330LHHXTR's pins 10 and 11, which the datasheet marks
NC, were tied to the ground pour through a symbol error. With those pins grounded the part never
enters its low-power state, so the board draws far more while asleep: a v3.2 board measured
~140.8µA before the rework and ~35.5µA at 12V after it. v3.4 fixes the footprint. An existing
board can be reworked - see
[reworking the accelerometer](/assembly/assembly-guide.html#reworking-the-accelerometer-on-v30-to-v33).

## Hazards while you build

- **Fumes.** Solder and flux fumes are toxic and can cause lasting harm. Use fume extraction and
  ventilate the room; do not skimp on this.
- **Lead.** The guide recommends leaded solder paste. Wash your hands after handling it and keep
  food and drink away from the bench.
- **Heat.** Hot air stations and soldering irons run at several hundred degrees. Work on a
  heat-resistant surface, keep flammable material away, and switch the tools off when you leave
  the bench.
- **Isopropyl alcohol** is highly flammable. Keep it away from the iron and the hot air, and let
  a cleaned board dry completely before you apply power.
- **Static.** The Connect Kit and the ICs are static sensitive. Handle boards by their edges and
  use ESD precautions.
- **First power.** Power a new board for the first time from a current-limited supply, as
  described on the [board test](/assembly/board-test.html) page, and never connect the Connect Kit
  until the board has passed the checks there.

## Regulatory approval

Nothing here is CE, UKCA or FCC marked, and no conformity assessment has been carried out.
Running an LTE radio may require type approval where you live. The licences permit commercial
manufacture and sale, but anyone placing a product on the market becomes the manufacturer of
radio equipment and takes on the resulting obligations. The nRF9151's modular approval does not
transfer to a finished product.

## No warranty, no liability

No claim is made that any of this is suitable for installation in a vehicle or for any other
purpose, and no warranty is offered. All responsibility for the safety, legality and fitness for
purpose of anything built from this project rests with the person building, operating and
installing it. The full terms are in the
[disclaimer](https://github.com/m4rkw/l0destar/blob/master/DISCLAIMER.md) and
[LICENSE.md](https://github.com/m4rkw/l0destar/blob/master/LICENSE.md).
