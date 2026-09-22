"""
l0destar battery backup v1.0 enclosure generator.

Same construction as the l0destar v3.3 tracker enclosure: two-part
clamshell (top/bottom), flat butt joint, M2 heat-set inserts in the top
bosses, screws in from underneath through counterbored standoffs.

Run inside FreeCAD (GUI or freecadcmd):
    exec(open('/path/to/enclosure.py').read())

Coordinates: board-local, origin at the PCB's bottom-left corner as seen in
the KiCad layout (x right, y up), z=0 at the enclosure floor (top of the
bottom plate). PCB underside sits on 4 mm standoffs.
"""
import os, math
import FreeCAD, Part
from FreeCAD import Vector as V

HERE = os.path.dirname(os.path.abspath(__file__)) if '__file__' in globals() else os.getcwd()

# ---------------------------------------------------------------- board ----
# From l0destar.kicad_pcb (Edge.Cuts rect 19.52,73.01 -> 82.62,109.2).
# local = (X_kicad - 19.52, 109.2 - Y_kicad)
PCB_L, PCB_W, PCB_T = 63.1, 36.19, 1.6
HOLES = {                      # M2 mounting holes, board-local (x, y)
    'H1': (12.02, 33.63),
    'H2': (48.45, 22.88),
    'H3': (2.62, 2.52),
    'H4': (60.53, 2.68),
}
# Micro-Fit 3.0 2x3 right-angle headers, both at the same y, one per side.
MOLEX_Y0, MOLEX_Y1 = 20.25, 33.40        # body extent in y (13.15 wide)
MOLEX_H = 8.83                           # body height above PCB top

# ------------------------------------------------------- enclosure params ----
CLEAR   = 0.5      # PCB edge to inner wall
WALL    = 2.4
FLOOR   = 2.5
ROOF    = 2.5
R_OUT   = 5.0      # outer vertical corner radius
R_IN    = 1.4      # cavity vertical corner radius
FIL_TOP = 2.0      # roof edge fillet
FIL_BOT = 1.0      # floor edge fillet
STANDOFF_H = 4.0   # floor top -> PCB underside
PCB_Z   = STANDOFF_H
PCB_TOP = PCB_Z + PCB_T                  # 5.6
SPLIT_Z = 12.0                           # butt joint height
HEADROOM = 20.0                          # PCB top -> roof underside; Keystone 1051 + CR123A is 18.1 (datasheet H ref)
CAVITY_TOP = PCB_TOP + HEADROOM          # 25.6 -> round up
CAVITY_TOP = 26.0

# screw stack (M2 x 10, heat-set insert in the top boss)
STANDOFF_R  = 2.75
THRU_R      = 1.15
CBORE_R     = 2.25
CBORE_D     = 1.7
BOSS_R      = 2.75     # top boss (R3.25 in v3.3; smaller here to clear J1 and the cell holder)
BOSS_TIP_R  = 2.5      # last 0.6 mm that lands on the PCB
BOSS_TIP_H  = 0.6
BOSS_CLR    = 0.5      # notch in the bottom wall around bosses that graze it
INSERT_R    = 1.6      # M2 heat-set insert hole
INSERT_D    = 4.6
RELIEF_D    = 2.3      # plain hole past the insert so an M2 x 12 never bottoms out
MIN_WALL    = 1.0      # thinnest wall the printer can be relied on for
# Per-hole tweaks (all dims board-local):
#  H2: C4's 10.3 mm chamfered base plate (1.6 tall) is 2.43 mm from the hole at its
#      nearest corner (47.27,25.01), up-left of the hole. A chord flat across the tip
#      would leave only 0.68 mm between the screw hole and the flat, so instead the
#      tip ring skips the 90..145 degree sector (CCW from +x): the cut faces are radial,
#      so the ring keeps its full 1.35 mm wall right up to them. The tip zone is 1.9 mm
#      tall and the heat-set insert starts above the plate. Needs the M2 x 12 (engages
#      3.7 mm).
#  H3: the Keystone 1051 body starts at x=5.43. The boss gets a flat on that side, set
#      by MIN_WALL past the insert hole (x=5.22, 0.21 mm clear of the holder) rather
#      than by clearance, which would leave 0.91 mm.
BOSSES = {
    'H1': {},
    'H2': {'tip_h': 1.9, 'tip_cut': (90, 145), 'insert_z0': PCB_TOP + 1.9},
    'H3': {'xmax': HOLES['H3'][0] + INSERT_R + MIN_WALL},
    'H4': {},
}

# connector cutouts (through the side walls, straddling the split)
CUT_Z0, CUT_Z1 = PCB_TOP - 1.1, PCB_TOP + MOLEX_H + 3.1   # 4.5 .. 17.5
CUT_MARGIN = 0.5

EPS = 0.01
BIG = 500

# --------------------------------------------------------------- helpers ----
def rrect(x0, y0, x1, y1, r, z0, h):
    """Rounded-corner box."""
    b = Part.makeBox(x1 - x0, y1 - y0, h, V(x0, y0, z0))
    vert = [e for e in b.Edges if e.BoundBox.XLength < 1e-6 and e.BoundBox.YLength < 1e-6]
    return b.makeFillet(r, vert)

def edges_at_z(shape, z):
    return [e for e in shape.Edges
            if abs(e.BoundBox.ZMin - z) < 1e-6 and abs(e.BoundBox.ZMax - z) < 1e-6]

def cyl(r, z0, z1, x, y):
    return Part.makeCylinder(r, z1 - z0, V(x, y, z0))

def box(x0, y0, z0, x1, y1, z1):
    return Part.makeBox(x1 - x0, y1 - y0, z1 - z0, V(x0, y0, z0))

def sector(x, y, a0, a1, z0, z1, r=6.0):
    """Prism over the a0..a1 (degrees, CCW from +x) sector about (x, y), out past r."""
    n = max(2, int((a1 - a0) / 10) + 2)
    angs = [math.radians(a0 + (a1 - a0) * i / (n - 1)) for i in range(n)]
    pts = [V(x, y, z0)] + [V(x + r * math.cos(a), y + r * math.sin(a), z0) for a in angs]
    return Part.Face(Part.makePolygon(pts + [pts[0]])).extrude(V(0, 0, z1 - z0))

# ----------------------------------------------------------------- shell ----
IX0, IY0 = -CLEAR, -CLEAR                    # cavity
IX1, IY1 = PCB_L + CLEAR, PCB_W + CLEAR
OX0, OY0 = IX0 - WALL, IY0 - WALL            # outer
OX1, OY1 = IX1 + WALL, IY1 + WALL
Z_BOT, Z_TOP = -FLOOR, CAVITY_TOP + ROOF

outer = rrect(OX0, OY0, OX1, OY1, R_OUT, Z_BOT, Z_TOP - Z_BOT)
outer = outer.makeFillet(FIL_TOP, edges_at_z(outer, Z_TOP))
outer = outer.makeFillet(FIL_BOT, edges_at_z(outer, Z_BOT))
cavity = rrect(IX0, IY0, IX1, IY1, R_IN, 0, CAVITY_TOP)
shell = outer.cut(cavity)

# PCB standoffs on the floor
shell = shell.fuse([cyl(STANDOFF_R, -EPS, PCB_Z, hx, hy) for (hx, hy) in HOLES.values()])

cuts = []
for (hx, hy) in HOLES.values():
    cuts.append(cyl(CBORE_R, Z_BOT - 1, Z_BOT + CBORE_D, hx, hy))          # screw head recess
    cuts.append(cyl(THRU_R, Z_BOT + CBORE_D - EPS, PCB_Z + EPS, hx, hy))   # M2 clearance
# Molex J1 (car harness) exits the left wall, J2 (tracker harness) the right
cuts.append(box(OX0 - 1, MOLEX_Y0 - CUT_MARGIN, CUT_Z0, IX0 + 1, MOLEX_Y1 + CUT_MARGIN, CUT_Z1))
cuts.append(box(IX1 - 1, MOLEX_Y0 - CUT_MARGIN, CUT_Z0, OX1 + 1, MOLEX_Y1 + CUT_MARGIN, CUT_Z1))
shell = shell.cut(cuts)

# ---------------------------------------------------------------- bottom ----
bottom = shell.common(box(-BIG, -BIG, Z_BOT - 1, BIG, BIG, SPLIT_Z))
# clearance notches where a top boss would rub the bottom wall
for (hx, hy) in HOLES.values():
    notch = cyl(BOSS_R + BOSS_CLR, PCB_TOP - EPS, SPLIT_Z + 1, hx, hy)
    if notch.common(bottom).Volume > 1e-6:
        bottom = bottom.cut(notch)
bottom = bottom.removeSplitter()

# ------------------------------------------------------------------- top ----
top = shell.common(box(-BIG, -BIG, SPLIT_Z, BIG, BIG, Z_TOP + 1))
bosses, holes = [], []
for name, (hx, hy) in HOLES.items():
    cfg = BOSSES[name]
    tip_h = cfg.get('tip_h', BOSS_TIP_H)
    ins_z0 = cfg.get('insert_z0', PCB_TOP)
    tip = cyl(BOSS_TIP_R, PCB_TOP, PCB_TOP + tip_h + EPS, hx, hy)
    if 'tip_cut' in cfg:
        a0, a1 = cfg['tip_cut']
        tip = tip.cut(sector(hx, hy, a0, a1, PCB_TOP - 1, PCB_TOP + tip_h + 1))
    b = tip.fuse(cyl(BOSS_R, PCB_TOP + tip_h, CAVITY_TOP + EPS, hx, hy))
    # bosses that graze the wall: tie them into it (above the split only)
    if hx - BOSS_R - IX0 < 1.0:
        b = b.fuse(box(IX0 - 1, hy - BOSS_R, SPLIT_Z, hx, hy + BOSS_R, CAVITY_TOP + EPS))
    if IX1 - hx - BOSS_R < 1.0:
        b = b.fuse(box(hx, hy - BOSS_R, SPLIT_Z, IX1 + 1, hy + BOSS_R, CAVITY_TOP + EPS))
    if hy - BOSS_R - IY0 < 1.0:
        b = b.fuse(box(hx - BOSS_R, IY0 - 1, SPLIT_Z, hx + BOSS_R, hy, CAVITY_TOP + EPS))
    if IY1 - hy - BOSS_R < 1.0:
        b = b.fuse(box(hx - BOSS_R, hy, SPLIT_Z, hx + BOSS_R, IY1 + 1, CAVITY_TOP + EPS))
    if 'xmax' in cfg:
        b = b.cut(box(cfg['xmax'], -BIG, -BIG, BIG, BIG, BIG))
    bosses.append(b)
    holes.append(cyl(THRU_R, PCB_TOP - EPS, ins_z0 + EPS, hx, hy))                 # screw clearance
    holes.append(cyl(INSERT_R, ins_z0, ins_z0 + INSERT_D, hx, hy))                 # heat-set insert
    holes.append(cyl(THRU_R, ins_z0 + INSERT_D - EPS, ins_z0 + INSERT_D + RELIEF_D, hx, hy))
top = top.fuse(bosses).cut(holes)
top = top.removeSplitter()

# ----------------------------------------------------------- mock board ----
mock = None
board_step = os.path.join(HERE, 'board.step')
if os.path.exists(board_step):
    import Import
    tmp = FreeCAD.newDocument('_board_tmp')
    Import.insert(board_step, tmp.Name)
    solids = []
    for o in tmp.Objects:
        if hasattr(o, 'Shape') and not o.Shape.isNull():
            bb = o.Shape.BoundBox
            if abs(bb.XMin) < 1000 and o.Shape.Solids and not any(o in p.OutList for p in tmp.Objects if p is not o and hasattr(p, 'OutList')):
                solids.extend(o.Shape.Solids)
    mock = Part.makeCompound(solids)
    mock.translate(V(-19.52, 109.2, PCB_Z))
    FreeCAD.closeDocument(tmp.Name)

# --------------------------------------------------------------- document ----
doc = FreeCAD.newDocument('battery_backup_enclosure')
if mock is not None:
    o = doc.addObject('Part::Feature', 'Mock_Board'); o.Shape = mock
ob = doc.addObject('Part::Feature', 'Enclosure_Bottom'); ob.Shape = bottom
ot = doc.addObject('Part::Feature', 'Enclosure_Top'); ot.Shape = top
doc.recompute()

# ---------------------------------------------------------------- checks ----
for name, s in (('bottom', bottom), ('top', top)):
    print(f"{name}: valid={s.isValid()} solids={len(s.Solids)} vol={s.Volume:.0f} bb={s.BoundBox}")
print(f"top ∩ bottom volume = {top.common(bottom).Volume:.4f}")
if mock is not None:
    for name, s in (('bottom', bottom), ('top', top)):
        print(f"mock ∩ {name} volume = {mock.common(s).Volume:.4f}")
    parts = [sol for sol in mock.Solids if sol.BoundBox.ZMax > PCB_TOP + 0.5]   # everything but the PCB
    for name, (hx, hy) in HOLES.items():
        col = top.common(cyl(BOSS_R + 0.01, PCB_TOP - EPS, CAVITY_TOP - EPS, hx, hy))
        d = min(sol.distToShape(col)[0] for sol in parts)
        print(f"boss {name}: nearest component {d:.2f} mm")
print(f"walls: tip ring {BOSS_TIP_R - THRU_R:.2f}, boss around insert {BOSS_R - INSERT_R:.2f}, "
      f"H3 flat to insert {BOSSES['H3']['xmax'] - HOLES['H3'][0] - INSERT_R:.2f} mm")
print(f"outer size {OX1-OX0:.2f} x {OY1-OY0:.2f} x {Z_TOP-Z_BOT:.2f} mm; split at z={SPLIT_Z}")
