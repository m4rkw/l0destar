# l0destar enclosure v3.3

**Before building, installing or relying on any of this, read the
[project disclaimer](../../../DISCLAIMER.md).**

3D-printable two-part enclosure (top and bottom) for the l0destar vehicle
tracker. Compatible with the **l0destar v3.2** and **l0destar v3.3** boards.

**THIS IS A PROTOTYPE, NOT A PRODUCT. IT HAS NOT BEEN VALIDATED FOR
IN-VEHICLE USE, THERMAL PERFORMANCE, INGRESS PROTECTION, FLAMMABILITY OR
ANYTHING ELSE. USE ENTIRELY AT YOUR OWN RISK.**

## Status

This has been ordered as a prototype, evaluated and found to be acceptable for
my personal use. Note that the ordered prototype had slightly different screw
positions (~1-2mm difference) from the enclosure here but that's a fairly
trivial difference.

The enclosure here will fit v3.2 and v3.3 boards.

## Variants

Two variants are provided. They are dimensionally identical; the only
difference is that one has the l0destar logo embossed in the top.

| Directory | Description |
|---|---|
| [`with_logo/`](with_logo/) | Logo embossed in the top |
| [`no_logo/`](no_logo/) | Plain top |

Each directory contains:

- `*.FCStd` - FreeCAD source, edit this if you want to modify the design
- `*_top.stl` / `*_bottom.stl` - mesh exports ready for slicing
- `*_top.step` / `*_bottom.step` - STEP exports for use in other CAD tools

## Required hardware

| Item | Link |
|---|---|
| M2 heat-set threaded inserts | https://link.amazon/B0fiGzjDl |
| M2 x 10 mm screws | https://link.amazon/B04Nlsa3c |

The heat-set inserts are pressed into the printed bosses using a soldering
iron. A dedicated insert tip makes this much easier and gives a straighter
result - this one fits C210-style irons:
https://link.amazon/B01C41sjy

## Assembly

1. Print the top and bottom of the variant you want.
2. Press the M2 heat-set inserts into the bosses with a soldering iron. Keep
   the iron square to the boss and let the insert sink under its own weight
   plus light pressure rather than forcing it - pushing too hard or too fast
   will melt through the boss or leave the insert tilted.
3. Fit the board and close the enclosure with the M2 x 10 mm screws. Do not
   over-tighten; the inserts will pull out of the plastic long before the
   screw strips.

## Disclaimer

This enclosure is a hobby-project prototype. It has not been tested for
mechanical strength, vibration, heat, moisture ingress, UV exposure,
flammability or compatibility with the temperature range found inside a
vehicle, and the material you print it in will materially affect all of
those. Nothing here is certified or qualified for automotive use. The links
above are examples of parts that worked for me, not a verified bill of
materials, and I have no affiliation with the sellers.

Anything built from these files is built, installed and used entirely at the
builder's own risk. See the [full disclaimer](../../../DISCLAIMER.md).

## License

Enclosure CAD and this documentation are licensed under
[CC-BY-4.0](../../../LICENSES/CC-BY-4.0.txt). See the project
[LICENSE.md](../../../LICENSE.md).
