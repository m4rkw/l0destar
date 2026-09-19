# l0destar battery backup v1.0 enclosure

3D-printable two-part enclosure (top and bottom) for the l0destar battery
backup board (revision F). Same construction as the
[l0destar v3.3 tracker enclosure](../../../code/l0destar/hardware/enclosure/v3.3/):
flat butt joint, M2 heat-set inserts in the top, screws in from underneath.

**THIS IS AN UNPRINTED DESIGN. IT HAS NOT BEEN VALIDATED FOR FIT, THERMAL
PERFORMANCE, INGRESS PROTECTION, FLAMMABILITY OR ANYTHING ELSE. USE ENTIRELY
AT YOUR OWN RISK.**

## Files

| File | Description |
|---|---|
| `enclosure.py` | Generator. Run in FreeCAD (`exec(open('enclosure.py').read())`) to rebuild the model from the parameters at the top of the file |
| `enclosure.FCStd` | FreeCAD document: `Enclosure_Top`, `Enclosure_Bottom`, `Mock_Board` |
| `enclosure_top.stl` / `enclosure_bottom.stl` | Mesh exports ready for slicing |
| `enclosure_top.step` / `enclosure_bottom.step` | STEP exports for other CAD tools |
| `board.step` | Board with 3D models (`kicad-cli pcb export step --subst-models`), used by the generator as the mock and for clearance checks |

## Dimensions

- Outer 68.9 × 42.0 × 31.0 mm (v3.3: 72.5 × 43.4 × 28.5). Same wall (2.4),
  floor and roof (2.5), R5 outer / R1.4 inner corners, R2 top and R1 bottom
  edge fillets, 0.5 mm clearance around the 63.1 × 36.2 mm board.
- PCB sits on 4 mm standoffs. Split at 12 mm above the floor.
- 20 mm headroom above the PCB top (v3.3 has 17.9). The Keystone 1051 is
  18.1 mm tall (H ref) with a CR123A fitted per the datasheet (K75p27), so
  there is 1.9 mm above the cell. The KiCad 3D model only shows the 15.2 mm
  holder body.
- Four bosses at the PCB's M2 holes (H1..H4), R2.75 with R2.5 tips that land
  on the PCB, tied into the wall where they meet it, with 0.5 mm clearance
  notches in the bottom wall. Standoffs are R2.75 with an R1.15 through hole
  and an R2.25 × 1.7 screw-head recess underneath.
- Two Micro-Fit 3.0 cutouts, 14.2 × 13 mm, straddling the split: car harness
  (J1) on the left, tracker harness (J2) on the right.

Two bosses had to work around neighbouring parts (both 0.3 mm clear):

- **H2** is 2.4 mm from the corner of C4's square base plate, so its tip is
  flat-sided for the first 1.9 mm and its insert starts above the plate.
- **H3** is 2.8 mm from the cell holder body, so the boss has a flat on that
  side.

## Required hardware

| Item | Notes |
|---|---|
| 4 × M2 heat-set threaded inserts | Ø3.2 hole, 4.6 mm deep (same as v3.3) |
| 4 × M2 × 12 mm screws | 12 mm, not the v3.3's 10 mm: H2's insert sits 1.9 mm higher. Holes have 2.3 mm of relief past the insert so a 12 mm screw never bottoms out |

## Assembly

As for the v3.3 enclosure: press the inserts into the four top bosses with a
soldering iron, fit the board, close with the screws, do not over-tighten.
The cell is accessible with the top removed.
