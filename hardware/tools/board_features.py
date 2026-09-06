#!/usr/bin/env python3
"""
board_features.py - annotate a photo of an assembled board with feature
callouts, using the KiCad PCB file to locate the components.

How it works
------------
1. The .kicad_pcb file is parsed (no KiCad python bindings needed) to get
   the board outline, every footprint's position/rotation/outline and the
   mounting holes.
2. The board is located in the photo: the PCB is segmented from the (plain)
   background by colour, its four edges are fitted with RANSAC (so overhanging
   connectors and wires are ignored) and the corners are intersected.
3. The photo-to-board orientation is picked automatically by checking which
   of the candidate rotations puts the mounting holes over bright (see-
   through) spots in the photo, then a homography (board mm -> photo px)
   is solved and refined against the detected hole centres.
4. Features listed in a YAML config (groups of reference designators plus a
   label) are projected into the photo and drawn as leader lines to labels
   placed in the margins around the board.

Usage
-----
    board_features.py features.yaml                # render
    board_features.py features.yaml --check        # also write a registration overlay
    board_features.py features.yaml --corners "x,y x,y x,y x,y"   # manual board corners (TL TR BR BL)
    board_features.py features.yaml --rotate 180   # force orientation
    board_features.py --init --pcb b.kicad_pcb --photo p.png > features.yaml
    board_features.py --list --pcb b.kicad_pcb     # dump footprints

Dependencies: numpy, scipy, Pillow, PyYAML.
"""

from __future__ import annotations

import argparse
import math
import os
import sys
from dataclasses import dataclass, field

import numpy as np
import yaml
from PIL import Image, ImageDraw, ImageFont
from scipy import ndimage

# --------------------------------------------------------------------------
# S-expression parsing
# --------------------------------------------------------------------------


class Sym(str):
    """A bare (unquoted) token in an s-expression."""


def parse_sexp(text: str):
    stack = [[]]
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c.isspace():
            i += 1
        elif c == "(":
            stack.append([])
            i += 1
        elif c == ")":
            lst = stack.pop()
            stack[-1].append(lst)
            i += 1
        elif c == '"':
            j = i + 1
            buf = []
            while j < n:
                ch = text[j]
                if ch == "\\":
                    buf.append(text[j + 1])
                    j += 2
                    continue
                if ch == '"':
                    break
                buf.append(ch)
                j += 1
            stack[-1].append("".join(buf))
            i = j + 1
        else:
            j = i
            while j < n and not text[j].isspace() and text[j] not in "()":
                j += 1
            tok = text[i:j]
            try:
                v = int(tok)
            except ValueError:
                try:
                    v = float(tok)
                except ValueError:
                    v = Sym(tok)
            stack[-1].append(v)
            i = j
    return stack[0]


def kids(node, key):
    for x in node:
        if isinstance(x, list) and x and x[0] == key:
            yield x


def kid(node, key, default=None):
    for x in kids(node, key):
        return x
    return default


def prop(node, name):
    for p in kids(node, "property"):
        if len(p) > 2 and p[1] == name:
            return p[2]
    return None


# --------------------------------------------------------------------------
# PCB model
# --------------------------------------------------------------------------


@dataclass
class Footprint:
    name: str
    ref: str
    value: str
    layer: str
    x: float
    y: float
    rot: float
    local_pts: list  # local-frame points contributing to the outline
    holes: list = field(default_factory=list)  # (x, y, drill) board frame

    def to_board(self, x, y):
        t = math.radians(self.rot)
        c, s = math.cos(t), math.sin(t)
        if self.layer.startswith("B."):
            x = -x
        return self.x + x * c + y * s, self.y - x * s + y * c

    @property
    def outline(self):
        """Board-frame polygon of the footprint's local bounding box."""
        if not self.local_pts:
            return [(self.x, self.y)] * 4
        xs = [p[0] for p in self.local_pts]
        ys = [p[1] for p in self.local_pts]
        x0, x1, y0, y1 = min(xs), max(xs), min(ys), max(ys)
        return [self.to_board(*p) for p in ((x0, y0), (x1, y0), (x1, y1), (x0, y1))]

    @property
    def center(self):
        pts = self.outline
        return sum(p[0] for p in pts) / 4, sum(p[1] for p in pts) / 4


@dataclass
class Board:
    outline_pts: list  # all Edge.Cuts points
    footprints: dict  # ref -> Footprint

    @property
    def bbox(self):
        xs = [p[0] for p in self.outline_pts]
        ys = [p[1] for p in self.outline_pts]
        return min(xs), min(ys), max(xs), max(ys)

    @property
    def corners(self):
        """TL, TR, BR, BL in board mm (KiCad y-down)."""
        x0, y0, x1, y1 = self.bbox
        return [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]

    def mounting_holes(self, refs=None):
        holes = []
        for fp in self.footprints.values():
            if refs is not None:
                if fp.ref not in refs:
                    continue
            elif "MountingHole" not in fp.name:
                continue
            holes.extend(fp.holes)
        return holes


def _shape_points(node):
    """Points of an fp_line/fp_rect/fp_circle/fp_arc/fp_poly node (local frame)."""
    key = node[0]
    pts = []
    if key in ("fp_line", "fp_arc"):
        for k in ("start", "mid", "end"):
            p = kid(node, k)
            if p:
                pts.append((p[1], p[2]))
    elif key == "fp_rect":
        s, e = kid(node, "start", [0, 0, 0]), kid(node, "end", [0, 0, 0])
        pts += [(s[1], s[2]), (e[1], e[2])]
    elif key == "fp_circle":
        c, e = kid(node, "center", [0, 0, 0]), kid(node, "end", [0, 0, 0])
        r = math.hypot(e[1] - c[1], e[2] - c[2])
        pts += [(c[1] - r, c[2] - r), (c[1] + r, c[2] + r)]
    elif key == "fp_poly":
        p = kid(node, "pts")
        if p:
            for xy in kids(p, "xy"):
                pts.append((xy[1], xy[2]))
    return pts


def load_board(path: str) -> Board:
    with open(path, encoding="utf-8") as f:
        tree = parse_sexp(f.read())
    root = tree[0]
    if not root or root[0] != "kicad_pcb":
        raise SystemExit(f"{path}: not a kicad_pcb file")

    outline = []
    for node in root:
        if not isinstance(node, list) or not node:
            continue
        key = node[0]
        if key not in ("gr_line", "gr_arc", "gr_rect", "gr_circle", "gr_poly"):
            continue
        layer = kid(node, "layer")
        if not layer or layer[1] != "Edge.Cuts":
            continue
        fake = [Sym("fp_" + key[3:])] + node[1:]
        outline.extend(_shape_points(fake))

    fps = {}
    for node in kids(root, "footprint"):
        name = node[1]
        layer = (kid(node, "layer") or [None, "F.Cu"])[1]
        at = kid(node, "at") or [None, 0, 0, 0]
        x, y = float(at[1]), float(at[2])
        rot = float(at[3]) if len(at) > 3 else 0.0
        ref = prop(node, "Reference") or "?"
        value = prop(node, "Value") or ""
        fp = Footprint(name, ref, value, layer, x, y, rot, [])

        crtyd, fab, pads = [], [], []
        for child in node:
            if not isinstance(child, list) or not child:
                continue
            k = child[0]
            if k in ("fp_line", "fp_rect", "fp_circle", "fp_arc", "fp_poly"):
                lay = kid(child, "layer")
                lay = lay[1] if lay else ""
                if lay.endswith("CrtYd"):
                    crtyd.extend(_shape_points(child))
                elif lay.endswith("Fab"):
                    fab.extend(_shape_points(child))
            elif k == "pad":
                pat = kid(child, "at") or [None, 0, 0]
                px, py = float(pat[1]), float(pat[2])
                size = kid(child, "size") or [None, 0, 0]
                w, h = float(size[1]) / 2, float(size[2]) / 2
                # pad rotation is absolute in the file; the pad's own rotation
                # relative to the footprint is (pad_rot - fp_rot)
                prot = float(pat[3]) if len(pat) > 3 else rot
                t = math.radians(prot - rot)
                c, s = math.cos(t), math.sin(t)
                for dx, dy in ((-w, -h), (w, -h), (w, h), (-w, h)):
                    pads.append((px + dx * c + dy * s, py - dx * s + dy * c))
                if child[2] == "np_thru_hole":
                    d = kid(child, "drill")
                    if d and isinstance(d[1], (int, float)):
                        bx, by = fp.to_board(px, py)
                        fp.holes.append((bx, by, float(d[1])))
                # Edge.Cuts drawn inside footprints (rare) also count
        fp.local_pts = crtyd or (fab + pads) or pads
        fps[ref] = fp
        for child in kids(node, "fp_line"):
            lay = kid(child, "layer")
            if lay and lay[1] == "Edge.Cuts":
                outline.extend(fp.to_board(*p) for p in _shape_points(child))

    if not outline:
        raise SystemExit("no Edge.Cuts outline found in the PCB")
    return Board(outline, fps)


# --------------------------------------------------------------------------
# Geometry helpers
# --------------------------------------------------------------------------


def homography(src, dst):
    """Least-squares homography mapping src (Nx2) -> dst (Nx2), N >= 4."""
    src = np.asarray(src, float)
    dst = np.asarray(dst, float)

    def normalise(p):
        m = p.mean(0)
        s = math.sqrt(2) / max(np.sqrt(((p - m) ** 2).sum(1)).mean(), 1e-9)
        T = np.array([[s, 0, -s * m[0]], [0, s, -s * m[1]], [0, 0, 1]])
        ph = np.c_[p, np.ones(len(p))] @ T.T
        return ph, T

    s, Ts = normalise(src)
    d, Td = normalise(dst)
    rows = []
    for (x, y, _), (u, v, _) in zip(s, d):
        rows.append([-x, -y, -1, 0, 0, 0, u * x, u * y, u])
        rows.append([0, 0, 0, -x, -y, -1, v * x, v * y, v])
    _, _, vt = np.linalg.svd(np.array(rows))
    H = vt[-1].reshape(3, 3)
    H = np.linalg.inv(Td) @ H @ Ts
    return H / H[2, 2]


def project(H, pts):
    p = np.c_[np.asarray(pts, float).reshape(-1, 2), np.ones(len(pts))] @ H.T
    return p[:, :2] / p[:, 2:3]


def local_scale(H, pt):
    """Approximate px-per-unit scale of H around pt."""
    p0 = project(H, [pt])[0]
    px = project(H, [(pt[0] + 1, pt[1])])[0]
    py = project(H, [(pt[0], pt[1] + 1)])[0]
    return math.sqrt(np.linalg.norm(px - p0) * np.linalg.norm(py - p0))


def ransac_line(pts, tol, iters=400, rng=None):
    """Fit a line to a noisy point set. Returns (point, unit direction)."""
    pts = np.asarray(pts, float)
    rng = rng or np.random.default_rng(0)
    best_inl, best = 0, None
    n = len(pts)
    for _ in range(iters):
        a, b = pts[rng.choice(n, 2, replace=False)]
        d = b - a
        L = np.linalg.norm(d)
        if L < 1e-6:
            continue
        d /= L
        nrm = np.array([-d[1], d[0]])
        dist = np.abs((pts[:, 0] - a[0]) * nrm[0] + (pts[:, 1] - a[1]) * nrm[1])
        inl = dist < tol
        cnt = int(inl.sum())
        if cnt > best_inl:
            best_inl, best = cnt, inl
    sel = pts[best]
    m = sel.mean(0)
    _, _, vt = np.linalg.svd(sel - m)
    return m, vt[0]


def intersect(l1, l2):
    (p1, d1), (p2, d2) = l1, l2
    A = np.array([d1, -d2]).T
    t = np.linalg.solve(A, p2 - p1)
    return p1 + t[0] * d1


# --------------------------------------------------------------------------
# Photo analysis
# --------------------------------------------------------------------------


def otsu(values):
    hist, edges = np.histogram(values, bins=256, range=(0, 256))
    hist = hist.astype(float)
    total = hist.sum()
    sum_all = (hist * np.arange(256)).sum()
    w_b = sum_b = 0.0
    best, thr = 0.0, 128
    for t in range(256):
        w_b += hist[t]
        if w_b == 0:
            continue
        w_f = total - w_b
        if w_f == 0:
            break
        sum_b += t * hist[t]
        m_b = sum_b / w_b
        m_f = (sum_all - sum_b) / w_f
        v = w_b * w_f * (m_b - m_f) ** 2
        if v > best:
            best, thr = v, t
    return thr


def detect_board_quad(rgb: np.ndarray, work_width=1400):
    """Find the PCB's four corners in the photo (TL, TR, BR, BL, image px)."""
    h, w = rgb.shape[:2]
    f = max(1, int(round(w / work_width)))
    small = rgb[::f, ::f].astype(int)
    # the board is the largest blob of pixels whose colour differs from the
    # background (estimated from the image border); works for any mask colour
    bg = background_colour(rgb).astype(int)
    dist = np.abs(small - bg).max(axis=2).clip(0, 255)
    dark = dist > otsu(dist)
    dark = ndimage.binary_opening(dark, iterations=2)
    lab, n = ndimage.label(dark)
    if n == 0:
        raise SystemExit("could not separate the board from the background; use --corners")
    sizes = ndimage.sum(dark, lab, range(1, n + 1))
    comp = lab == (int(np.argmax(sizes)) + 1)
    comp = ndimage.binary_fill_holes(comp)

    ys, xs = np.nonzero(comp)
    H, W = comp.shape
    big = 10**9
    left = np.full(H, big)
    right = np.full(H, -big)
    top = np.full(W, big)
    bottom = np.full(W, -big)
    np.minimum.at(left, ys, xs)
    np.maximum.at(right, ys, xs)
    np.minimum.at(top, xs, ys)
    np.maximum.at(bottom, xs, ys)
    rows = np.nonzero(left < big)[0]
    cols = np.nonzero(top < big)[0]
    # trim the extreme 5% so corner rounding does not bias the fits
    rt = rows[int(len(rows) * 0.05): int(len(rows) * 0.95)]
    ct = cols[int(len(cols) * 0.05): int(len(cols) * 0.95)]
    tol = 1.5
    lines = {
        "left": ransac_line(np.c_[left[rt], rt], tol),
        "right": ransac_line(np.c_[right[rt], rt], tol),
        "top": ransac_line(np.c_[ct, top[ct]], tol),
        "bottom": ransac_line(np.c_[ct, bottom[ct]], tol),
    }
    tl = intersect(lines["left"], lines["top"])
    tr = intersect(lines["right"], lines["top"])
    br = intersect(lines["right"], lines["bottom"])
    bl = intersect(lines["left"], lines["bottom"])
    quad = np.array([tl, tr, br, bl]) * f + (f - 1) / 2.0
    return quad


def background_colour(rgb):
    b = np.concatenate([rgb[0], rgb[-1], rgb[:, 0], rgb[:, -1]])
    return np.median(b, axis=0)


def sample_disc(V, cx, cy, r, r_in=0.0):
    """Mean of V inside the disc (or annulus r_in..r) centred on cx, cy."""
    h, w = V.shape
    x0, x1 = max(0, int(cx - r)), min(w, int(cx + r) + 1)
    y0, y1 = max(0, int(cy - r)), min(h, int(cy + r) + 1)
    if x1 <= x0 or y1 <= y0:
        return None
    yy, xx = np.mgrid[y0:y1, x0:x1]
    d2 = (xx - cx) ** 2 + (yy - cy) ** 2
    m = (d2 <= r * r) & (d2 >= r_in * r_in)
    if not m.any():
        return None
    return float(V[y0:y1, x0:x1][m].mean())


def choose_orientation(board: Board, quad, V, board_V, holes, force=None, mirror=False):
    """Try the candidate corner correspondences and score with the holes."""
    corners = board.corners
    if mirror:
        x0, _, x1, _ = board.bbox
        corners = [(x0 + x1 - x, y) for x, y in corners]
        corners = [corners[1], corners[0], corners[3], corners[2]]
    cands = []
    for rot in (0, 90, 180, 270):
        k = rot // 90
        src = corners[k:] + corners[:k]
        H = homography(src, quad)
        # reject mappings that squash one axis (wrong aspect)
        sx = local_scale_axis(H, board, 0)
        sy = local_scale_axis(H, board, 1)
        aniso = max(sx, sy) / max(min(sx, sy), 1e-9)
        score = 0.0
        if holes:
            vals = []
            for hx, hy, d in holes:
                px, py = project(H, [(hx, hy)])[0]
                sc = d * local_scale(H, (hx, hy))
                # a hole shows the background through a disc that is
                # surrounded by ordinary board; a bright connector is not
                disc = sample_disc(V, px, py, 0.35 * sc)
                ring = sample_disc(V, px, py, 1.5 * sc, 1.0 * sc)
                if disc is None or ring is None:
                    vals.append(0.0)
                else:
                    vals.append(max(0.0, abs(disc - board_V) - abs(ring - board_V)))
            score = float(np.mean(vals))
        cands.append((rot, H, aniso, score))
    for rot, H, aniso, score in cands:
        print(f"  orientation {rot:3d}: anisotropy {aniso:.2f}, hole contrast {score:5.1f}")
    if force is not None:
        return next(c for c in cands if c[0] == force)[1], force
    ok = [c for c in cands if c[2] < 1.25] or cands
    best = max(ok, key=lambda c: c[3])
    return best[1], best[0]


def local_scale_axis(H, board, axis):
    x0, y0, x1, y1 = board.bbox
    if axis == 0:
        a, b = project(H, [(x0, (y0 + y1) / 2), (x1, (y0 + y1) / 2)])
        return np.linalg.norm(b - a) / (x1 - x0)
    a, b = project(H, [((x0 + x1) / 2, y0), ((x0 + x1) / 2, y1)])
    return np.linalg.norm(b - a) / (y1 - y0)


def refine_with_holes(H, holes, V, board_V, bg_V):
    """Locate the mounting holes near their predicted spots and refit H."""
    thr = board_V + 0.5 * (bg_V - board_V)
    src, dst = [], []
    h, w = V.shape
    for hx, hy, d in holes:
        px, py = project(H, [(hx, hy)])[0]
        r = 0.5 * d * local_scale(H, (hx, hy))
        R = int(2.5 * r)
        x0, x1 = max(0, int(px) - R), min(w, int(px) + R)
        y0, y1 = max(0, int(py) - R), min(h, int(py) + R)
        if x1 - x0 < 4 or y1 - y0 < 4:
            continue
        win = V[y0:y1, x0:x1] > thr
        lab, n = ndimage.label(win)
        best = None
        for i in range(1, n + 1):
            m = lab == i
            area = m.sum()
            if not (0.3 * math.pi * r * r < area < 2.5 * math.pi * r * r):
                continue
            cy, cx = ndimage.center_of_mass(m)
            dist = math.hypot(cx + x0 - px, cy + y0 - py)
            if dist < 1.5 * r and (best is None or dist < best[0]):
                best = (dist, cx + x0, cy + y0)
        if best:
            src.append((hx, hy))
            dst.append((best[1], best[2]))
    return src, dst


# --------------------------------------------------------------------------
# Registration driver
# --------------------------------------------------------------------------


@dataclass
class Registration:
    H: np.ndarray  # board mm -> photo px
    quad: np.ndarray  # detected board corners in photo px
    rotation: int
    hole_pts: list  # (photo px) detected hole centres
    crop: tuple  # x0, y0, x1, y1 in photo px


def register(board: Board, photo: Image.Image, cfg: dict, args) -> Registration:
    rgb = np.asarray(photo.convert("RGB"))
    V = rgb.max(axis=2).astype(float)
    if args.corners:
        vals = [float(t) for t in args.corners.replace(",", " ").split()]
        quad = np.array(vals).reshape(4, 2)
        print("using manual board corners")
    else:
        quad = detect_board_quad(rgb)
    print("board corners (px): " + "  ".join(f"({x:.0f},{y:.0f})" for x, y in quad))

    # median board brightness inside the quad, background from the border
    yy, xx = np.mgrid[0: V.shape[0]: 4, 0: V.shape[1]: 4]
    inside = point_in_quad(np.c_[xx.ravel(), yy.ravel()], quad)
    board_V = float(np.median(V[::4, ::4].ravel()[inside]))
    bg_V = float(background_colour(rgb).max())

    mirror = cfg.get("side", "front") == "back"
    holes = board.mounting_holes(cfg.get("holes"))
    H, rot = choose_orientation(board, quad, V, board_V, holes, args.rotate, mirror)
    print(f"orientation: {rot} deg" + (" (mirrored, back side)" if mirror else ""))

    hole_pts = []
    if holes and not args.no_refine:
        src, dst = refine_with_holes(H, holes, V, board_V, bg_V)
        hole_pts = dst
        if len(src) >= 2:
            corners = board.corners
            if mirror:
                x0, _, x1, _ = board.bbox
                corners = [(x0 + x1 - x, y) for x, y in corners]
                corners = [corners[1], corners[0], corners[3], corners[2]]
            k = rot // 90
            corners = corners[k:] + corners[:k]
            H2 = homography(list(corners) + src, list(quad) + dst)
            resid = np.linalg.norm(project(H2, src) - np.array(dst), axis=1)
            print(f"refined with {len(src)} mounting holes, residual {resid.mean():.1f} px")
            H = H2
        else:
            print("mounting holes not located, using corner-only registration")

    # crop window: board quad plus foreground (connector overhang), capped
    scale = local_scale(H, ((board.bbox[0] + board.bbox[2]) / 2, (board.bbox[1] + board.bbox[3]) / 2))
    pad = float(cfg.get("crop_pad_mm", 3.0)) * scale
    cap = cfg.get("crop_max_mm", 15.0)
    if not isinstance(cap, dict):
        cap = {k: cap for k in ("left", "right", "top", "bottom")}
    cap = {k: float(cap.get(k, 15.0)) * scale for k in ("left", "right", "top", "bottom")}
    qx0, qy0 = quad.min(0)
    qx1, qy1 = quad.max(0)
    small = rgb[::2, ::2].astype(int)
    Vs = small.max(axis=2)
    sat = Vs - small.min(axis=2)
    fg = (Vs < 0.55 * bg_V) | (sat > 70)
    fg = ndimage.binary_opening(fg, iterations=2)
    yy, xx = np.mgrid[0: fg.shape[0], 0: fg.shape[1]]
    fg |= point_in_quad(np.c_[xx.ravel() * 2, yy.ravel() * 2], quad).reshape(fg.shape)
    lab, n = ndimage.label(fg)
    cx, cy = quad.mean(0) / 2
    keep = lab == lab[int(cy), int(cx)]
    ys, xs = np.nonzero(keep)
    fx0, fx1 = xs.min() * 2, xs.max() * 2
    fy0, fy1 = ys.min() * 2, ys.max() * 2
    x0 = max(0, min(qx0 - pad, max(fx0 - pad, qx0 - cap["left"])))
    y0 = max(0, min(qy0 - pad, max(fy0 - pad, qy0 - cap["top"])))
    x1 = min(rgb.shape[1], max(qx1 + pad, min(fx1 + pad, qx1 + cap["right"])))
    y1 = min(rgb.shape[0], max(qy1 + pad, min(fy1 + pad, qy1 + cap["bottom"])))
    print(f"crop (px): {int(x0)},{int(y0)} - {int(x1)},{int(y1)}")
    crop = (int(x0), int(y0), int(x1), int(y1))
    return Registration(H, quad, rot, hole_pts, crop)


def point_in_quad(pts, quad):
    inside = np.ones(len(pts), bool)
    for i in range(4):
        a, b = quad[i], quad[(i + 1) % 4]
        cross = (b[0] - a[0]) * (pts[:, 1] - a[1]) - (b[1] - a[1]) * (pts[:, 0] - a[0])
        inside &= cross >= 0
    return inside


# --------------------------------------------------------------------------
# Rendering
# --------------------------------------------------------------------------

FONT_CANDIDATES = [
    ("/System/Library/Fonts/Helvetica.ttc", 0, 1),
    ("/System/Library/Fonts/Supplemental/Arial.ttf", None, "/System/Library/Fonts/Supplemental/Arial Bold.ttf"),
    ("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", None, "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"),
    ("/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf", None,
     "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf"),
]


def load_fonts(size, bold_size, cfg):
    reg, bold = cfg.get("font"), cfg.get("font_bold")
    if reg:
        return ImageFont.truetype(reg, size), ImageFont.truetype(bold or reg, bold_size)
    for path, idx, bidx in FONT_CANDIDATES:
        if not os.path.exists(path):
            continue
        try:
            if isinstance(bidx, str):
                return ImageFont.truetype(path, size), ImageFont.truetype(bidx, bold_size)
            return ImageFont.truetype(path, size, index=idx), ImageFont.truetype(path, bold_size, index=bidx)
        except OSError:
            continue
    f = ImageFont.load_default()
    return f, f


@dataclass
class Callout:
    label_lines: list
    anchor: tuple  # canvas px
    side: str
    w: int = 0
    h: int = 0
    pos: tuple = (0, 0)  # canvas px of the label box top-left
    attach: tuple = (0, 0)  # canvas px where the leader meets the label
    key: float = 0.0


def measure(draw, lines, font, bold, gap):
    w = h = 0
    for i, line in enumerate(lines):
        f = bold if i == 0 else font
        b = draw.textbbox((0, 0), line, font=f)
        w = max(w, b[2] - b[0])
        h += (b[3] - b[1]) + (gap if i else 0)
    return w, h


def spread(desired, sizes, lo, hi, iters=300):
    """1-D placement: keep order, honour sizes, stay close to desired centres."""
    c = np.array(desired, float)
    s = np.array(sizes, float)
    if len(c) == 0:
        return c
    if s.sum() > hi - lo:
        # not enough room: distribute evenly
        return np.linspace(lo + s[0] / 2, hi - s[-1] / 2, len(c)) if len(c) > 1 else np.array([(lo + hi) / 2])
    for _ in range(iters):
        moved = False
        for i in range(len(c) - 1):
            need = (s[i] + s[i + 1]) / 2
            gap = c[i + 1] - c[i]
            if gap < need - 0.01:
                shift = (need - gap) / 2
                c[i] -= shift
                c[i + 1] += shift
                moved = True
        c[0] = max(c[0], lo + s[0] / 2)
        c[-1] = min(c[-1], hi - s[-1] / 2)
        if not moved:
            break
    return c


def render(board: Board, photo: Image.Image, reg: Registration, cfg: dict, out_path: str):
    style = cfg.get("style", {})
    board_w = int(style.get("board_width", 1800))
    fsize = int(style.get("font_size", 26))
    bsize = int(style.get("title_font_size", fsize + 2))
    accent = style.get("accent", "#ff6a00")
    text_col = style.get("text", "#222222")
    sub_col = style.get("text_secondary", "#555555")
    bg = style.get("background", "#ffffff")
    leader_w = int(style.get("leader_width", 3))
    dot_r = int(style.get("dot_radius", 9))
    gap_edge = int(style.get("label_gap", 60))  # white space between label and photo
    line_gap = int(fsize * 0.35)
    auto_sides = cfg.get("auto_sides", ["left", "right"])

    x0, y0, x1, y1 = reg.crop
    crop = photo.crop((x0, y0, x1, y1)).convert("RGB")
    s = board_w / crop.width
    crop = crop.resize((board_w, int(round(crop.height * s))), Image.LANCZOS)
    bw, bh = crop.size

    tmp = ImageDraw.Draw(Image.new("RGB", (10, 10)))
    font, bold = load_fonts(fsize, bsize, style)

    # build callouts
    callouts = []
    for feat in cfg.get("features", []):
        refs = feat.get("refs") or ([feat["ref"]] if "ref" in feat else [])
        if "at" in feat:
            ax, ay = feat["at"]
        else:
            pts = []
            for r in refs:
                fp = board.footprints.get(r)
                if fp is None:
                    print(f"warning: ref {r} not in PCB, skipped", file=sys.stderr)
                    continue
                pts.extend(fp.outline)
            if not pts:
                continue
            xs = [p[0] for p in pts]
            ys = [p[1] for p in pts]
            ax, ay = (min(xs) + max(xs)) / 2, (min(ys) + max(ys)) / 2
            if "offset" in feat:
                ax += feat["offset"][0]
                ay += feat["offset"][1]
        px, py = project(reg.H, [(ax, ay)])[0]
        cx, cy = (px - x0) * s, (py - y0) * s
        if not (0 <= cx <= bw and 0 <= cy <= bh):
            print(f"warning: feature '{feat.get('label','')[:30]}' lies outside the photo crop", file=sys.stderr)
        side = feat.get("side")
        if side is None:
            d = {"left": cx / bw, "right": 1 - cx / bw, "top": cy / bh, "bottom": 1 - cy / bh}
            side = min(auto_sides, key=lambda k: d[k])
        lines = str(feat["label"]).split("\n")
        c = Callout(lines, (cx, cy), side)
        c.w, c.h = measure(tmp, lines, font, bold, line_gap)
        callouts.append(c)

    # margins from label sizes
    by_side = {k: [c for c in callouts if c.side == k] for k in ("left", "right", "top", "bottom")}
    pad_out = 30
    m_left = (max((c.w for c in by_side["left"]), default=0) + gap_edge + pad_out) if by_side["left"] else pad_out
    m_right = (max((c.w for c in by_side["right"]), default=0) + gap_edge + pad_out) if by_side["right"] else pad_out
    def n_rows(cs):
        if not cs:
            return 0
        return min(3, int(math.ceil(sum(c.w + fsize for c in cs) / bw)))

    rows_top, rows_bot = n_rows(by_side["top"]), n_rows(by_side["bottom"])
    row_h_top = max((c.h for c in by_side["top"]), default=0) + fsize
    row_h_bot = max((c.h for c in by_side["bottom"]), default=0) + fsize
    title = cfg.get("title")
    title_h = 0
    if title:
        tfont = ImageFont.truetype(bold.path, int(bsize * 1.8)) if hasattr(bold, "path") else bold
        tb = tmp.textbbox((0, 0), title, font=tfont)
        title_h = tb[3] - tb[1] + fsize
    m_top = pad_out + title_h + (rows_top * row_h_top + gap_edge if rows_top else 0)
    m_bot = pad_out + (rows_bot * row_h_bot + gap_edge if rows_bot else 0)

    W, Hh = int(m_left + bw + m_right), int(m_top + bh + m_bot)
    canvas = Image.new("RGB", (W, Hh), bg)
    canvas.paste(crop, (int(m_left), int(m_top)))
    draw = ImageDraw.Draw(canvas)
    for c in callouts:
        c.anchor = (c.anchor[0] + m_left, c.anchor[1] + m_top)

    # place labels
    for side in ("left", "right"):
        cs = sorted(by_side[side], key=lambda c: c.anchor[1])
        centres = spread([c.anchor[1] for c in cs], [c.h + fsize * 0.6 for c in cs], pad_out, Hh - pad_out)
        for c, cy in zip(cs, centres):
            if side == "left":
                lx = m_left - gap_edge - c.w
                c.attach = (m_left - gap_edge + fsize * 0.3, cy)
            else:
                lx = m_left + bw + gap_edge
                c.attach = (lx - fsize * 0.3, cy)
            c.pos = (lx, cy - c.h / 2)
    for side, rows, row_h in (("top", rows_top, row_h_top), ("bottom", rows_bot, row_h_bot)):
        cs = sorted(by_side[side], key=lambda c: c.anchor[0])
        for r in range(rows):
            grp = cs[r::rows]
            centres = spread([c.anchor[0] for c in grp], [c.w + fsize for c in grp], m_left, m_left + bw)
            for c, cx in zip(grp, centres):
                if side == "top":
                    ly = m_top - gap_edge - (r + 1) * row_h + fsize / 2
                    c.attach = (cx, ly + c.h + fsize * 0.3)
                else:
                    ly = m_top + bh + gap_edge + r * row_h + fsize / 2
                    c.attach = (cx, ly - fsize * 0.3)
                c.pos = (cx - c.w / 2, ly)

    # leaders, dots, labels
    for c in callouts:
        draw.line([c.attach, c.anchor], fill=accent, width=leader_w)
    for c in callouts:
        x, y = c.anchor
        draw.ellipse([x - dot_r - 3, y - dot_r - 3, x + dot_r + 3, y + dot_r + 3], fill="#ffffff")
        draw.ellipse([x - dot_r, y - dot_r, x + dot_r, y + dot_r], fill=accent)
    for c in callouts:
        lx, ly = c.pos
        draw.rectangle([lx - 6, ly - 4, lx + c.w + 6, ly + c.h + 4], fill=bg)
        yy = ly
        for i, line in enumerate(c.label_lines):
            f = bold if i == 0 else font
            b = draw.textbbox((0, 0), line, font=f)
            tx = lx if c.side != "right" else lx
            if c.side in ("top", "bottom"):
                tx = lx + (c.w - (b[2] - b[0])) / 2
            elif c.side == "left":
                tx = lx + c.w - (b[2] - b[0])
            draw.text((tx - b[0], yy - b[1]), line, font=f, fill=text_col if i == 0 else sub_col)
            yy += (b[3] - b[1]) + line_gap
    if title:
        draw.text((pad_out, pad_out), title, font=tfont, fill=text_col)
        sub = cfg.get("subtitle")
        if sub:
            tb = draw.textbbox((0, 0), title, font=tfont)
            draw.text((pad_out, pad_out + tb[3] + 8), sub, font=font, fill=sub_col)

    canvas.save(out_path)
    print(f"wrote {out_path} ({W}x{Hh})")


def render_check(board: Board, photo: Image.Image, reg: Registration, out_path: str):
    """Registration overlay: every footprint outline + ref on the photo."""
    im = photo.convert("RGB").copy()
    draw = ImageDraw.Draw(im)
    font, _ = load_fonts(max(10, int(im.width / 250)), 10, {})
    q = [tuple(p) for p in reg.quad]
    draw.polygon(q, outline="#00ff00", width=3)
    draw.rectangle(reg.crop, outline="#00ff00", width=2)
    for hx, hy in reg.hole_pts:
        draw.ellipse([hx - 10, hy - 10, hx + 10, hy + 10], outline="#00ffff", width=3)
    for fp in board.footprints.values():
        if fp.layer.startswith("B."):
            continue
        pts = [tuple(p) for p in project(reg.H, fp.outline)]
        draw.polygon(pts, outline="#ff00ff", width=2)
        cx, cy = project(reg.H, [fp.center])[0]
        draw.text((cx, cy), fp.ref, font=font, fill="#ffff00", anchor="mm")
    im.save(out_path)
    print(f"wrote {out_path}")


# --------------------------------------------------------------------------
# Config helpers
# --------------------------------------------------------------------------

INTERESTING = ("Connector", "Package_", "libs:", "LED", "TestPoint", "MountingHole", "PinHeader", "Fuse")
BORING = ("R_0402", "C_0402", "R_0603", "C_0603", "R_0805", "C_0805", "SOT-23", "SOD-123",
          "SC-70", "L_0402", "L_0603", "C_1210", "R_0508")


def init_config(pcb_path, photo_path):
    board = load_board(pcb_path)
    feats = []
    for fp in sorted(board.footprints.values(), key=lambda f: (f.y, f.x)):
        if fp.layer.startswith("B."):
            continue
        if any(b in fp.name for b in BORING):
            continue
        if not any(k in fp.name for k in INTERESTING):
            continue
        feats.append({"refs": [fp.ref], "label": f"{fp.value}\n{fp.name.split(':')[-1]}"})
    cfg = {
        "pcb": pcb_path,
        "photo": photo_path,
        "output": "images/features.png",
        "title": None,
        "style": {"board_width": 1800, "font_size": 26, "accent": "#ff6a00"},
        "features": feats,
    }
    print("# Generated by board_features.py --init; edit labels, delete or merge entries.")
    print("# Each feature: refs (list of designators, grouped), label (first line bold,")
    print("# '\\n' for more lines), optional side (left/right/top/bottom), offset [mm,mm]")
    print("# or at [x_mm, y_mm] to place the anchor by hand.")
    print(yaml.safe_dump(cfg, sort_keys=False, allow_unicode=True))


def list_footprints(pcb_path):
    board = load_board(pcb_path)
    for fp in sorted(board.footprints.values(), key=lambda f: f.ref):
        print(f"{fp.ref:10} {fp.layer:6} {fp.x:8.2f} {fp.y:8.2f} {fp.rot:6.1f}  {fp.value:32} {fp.name}")


# --------------------------------------------------------------------------


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("config", nargs="?", help="features YAML")
    ap.add_argument("--init", action="store_true", help="print a starter config for --pcb/--photo")
    ap.add_argument("--list", action="store_true", help="list footprints in --pcb")
    ap.add_argument("--pcb")
    ap.add_argument("--photo")
    ap.add_argument("-o", "--output")
    ap.add_argument("--check", action="store_true", help="also write <output>_check.png registration overlay")
    ap.add_argument("--corners", help='manual board corners in photo px: "x,y x,y x,y x,y" (TL TR BR BL)')
    ap.add_argument("--rotate", type=int, choices=(0, 90, 180, 270), help="force photo orientation")
    ap.add_argument("--no-refine", action="store_true", help="skip mounting-hole refinement")
    args = ap.parse_args()

    if args.init or args.list:
        if not args.pcb:
            ap.error("--pcb is required")
        if args.list:
            list_footprints(args.pcb)
        else:
            init_config(args.pcb, args.photo or "images/board.png")
        return
    if not args.config:
        ap.error("config file required")

    with open(args.config, encoding="utf-8") as f:
        cfg = yaml.safe_load(f)
    base = os.path.dirname(os.path.abspath(args.config))

    def rel(p):
        return p if os.path.isabs(p) else os.path.join(base, p)

    pcb_path = rel(args.pcb or cfg["pcb"])
    photo_path = rel(args.photo or cfg["photo"])
    out_path = rel(args.output or cfg.get("output", "features.png"))

    board = load_board(pcb_path)
    print(f"{os.path.basename(pcb_path)}: {len(board.footprints)} footprints, "
          f"board {board.bbox[2]-board.bbox[0]:.2f} x {board.bbox[3]-board.bbox[1]:.2f} mm")
    photo = Image.open(photo_path)
    reg = register(board, photo, cfg, args)
    if args.check:
        root, ext = os.path.splitext(out_path)
        render_check(board, photo, reg, root + "_check" + (ext or ".png"))
    render(board, photo, reg, cfg, out_path)


if __name__ == "__main__":
    main()
