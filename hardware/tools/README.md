# Hardware tools

## board_features.py

Generates an annotated "features" image from a photo of an assembled board
and the KiCad PCB file: each listed feature gets a leader line from its
location on the photo to a label in the margin.

```
cd hardware/l0destar_v3.2
python3 ../tools/board_features.py features.yaml --check
```

That writes `images/features.png` and `images/features_check.png`. The check
image overlays every footprint outline and reference on the photo so you can
confirm the registration before trusting the callouts. Delete it afterwards or
leave it out of commits.

Dependencies: numpy, scipy, Pillow, PyYAML (all in the usual venv). It parses
the `.kicad_pcb` directly, so it does not need KiCad's Python bindings.

### How it locates the board

1. The PCB is separated from the background by colour (the background is
   estimated from the image border, so it should be plain and reasonably
   uniform). Overhanging connectors and wires are ignored because each edge is
   fitted with RANSAC.
2. Orientation is picked by projecting the mounting holes through each of the
   four candidate rotations and checking which one lands them on
   see-through spots. Override with `--rotate 0|90|180|270` if it guesses
   wrong; the per-rotation scores are printed.
3. A homography from board mm to photo px is solved from the corners, then
   refined with the detected hole centres (residual is printed, a few px is
   normal).

If detection fails, pass the corners by hand:
`--corners "x,y x,y x,y x,y"` (top-left, top-right, bottom-right, bottom-left,
photo pixels).

### Config file

```yaml
pcb: l0destar.kicad_pcb
photo: images/pcb0.png
output: images/features.png
title: l0destar v3.2          # optional
subtitle: ...                  # optional
auto_sides: [left, right, top, bottom]   # sides used when a feature has no `side`
crop_pad_mm: 3                 # margin around the board outline
crop_max_mm: {left: 5, right: 14, top: 3, bottom: 3}   # how far to follow overhangs
style:
  board_width: 2000            # px, output scale
  font_size: 26
  accent: "#ff6a00"
features:
  - refs: [S1J1]               # one or more designators, grouped into one callout
    label: "Main connector\n6-pin Molex Micro-Fit 3.0"   # first line bold
    side: left                 # left / right / top / bottom (optional)
    offset: [-16, 0]           # nudge the anchor, mm (optional)
  - at: [57.1, 90.2]           # or place the anchor by PCB coordinate
    label: "Somewhere"
```

`--init --pcb x.kicad_pcb --photo p.png` prints a starter config listing the
connectors, ICs, LEDs, test points and holes found in the PCB.
`--list --pcb x.kicad_pcb` dumps every footprint with position and rotation.

Labels on the top and bottom are staggered into rows when they do not fit in
one; if the leaders get too tangled, move some features to `left` or `right`
or shorten the text.
