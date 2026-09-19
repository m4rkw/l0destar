# Assembly: read this first

This section walks through building a l0destar carrier board by hand, from ordering the PCB to
a powered bench test. Read this page, and the project's full
[disclaimer](https://github.com/m4rkw/l0destar/blob/master/DISCLAIMER.md), before you order
anything. If you intend to fit the finished tracker to a vehicle, also read
[Deployment: read this first](../deployment/read-this-first.md).

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
- **v3.3 is the most recent board that has been built and bench tested.** v3.4 leaves the
  accelerometer's two NC pads unconnected (they were grounded on v3.0-v3.3, which kept the part
  out of its low-power state), makes the buck converter's enable divider an optional
  part that is not fitted by default, adds test points, enlarges the holes for the Connect Kit power
  lead and specifies a TCAN3414DR CAN transceiver in place of the MAX33041E (same footprint and
  pinout).

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
  described on the [board test](board-test.md) page, and never connect the Connect Kit
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
