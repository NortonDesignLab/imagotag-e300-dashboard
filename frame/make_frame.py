#!/usr/bin/env python3
"""
Two-piece 3D-printable frame for the 7.5" e-paper panel (HINK-E075A07-A0 / GDEH075Z90).

Part A  front bezel frame : window + pocket the panel drops into from the BACK, plus 6 snap posts
Part B  back plate        : closes the pocket; FPC slot at the bottom edge; two keyholes
SNAP TOGETHER - no screws. Part B presses down over Part A's barbed posts and clicks home.
The posts are split by a slot so the two halves flex; pull the back plate off to release.

All dimensions in mm. Tweak the PANEL_* / OVERLAP_* numbers and re-run.
Outputs: frame_front.stl, frame_back.stl, frame_both.stl (both parts laid out for one print)
"""
import struct, sys, pathlib
import numpy as np
from manifold3d import Manifold, CrossSection, JoinType

# ---------------- parameters ----------------
PANEL_W, PANEL_H   = 170.0, 112.0     # measured outer glass size
PANEL_T            = 1.2              # glass thickness (GDEH075Z90 ~1.18)
CLEAR              = 0.3              # per-side pocket clearance around the glass
POCKET_T           = 3.5              # 6 Oct: was 4.0 and the panel rattled. Taking 0.5 mm out
                                      # clamps the glass between the bezel and the back plate.
                                      # THIS is the rattle knob - BEZEL_T is the front face and
                                      # has no effect on how tightly the panel is held.

OVERLAP_SIDE       = 2.5              # panel's own inactive border is ~3.0 all round;
OVERLAP_TOP        = 2.5              # 2.5 keeps the active area clear even with the
OVERLAP_BOTTOM     = 2.5              # 0.3 mm pocket float, leaving ~0.5 mm of border visible

MARGIN             = 7.0              # rim width outside the pocket
CORNER_R           = 4.0
BEZEL_T            = 1.6              # front plate thickness
BACK_T             = 2.0              # back plate thickness

# snap fit (replaces the M3 screws)
SNAP_POST_D        = 3.8              # 6 Oct: -0.2, pegs were too tight
SNAP_HOLE_D        = 4.25             # matching clearance hole in Part B
SNAP_BARB_D        = 5.2              # -0.2 too. THIS sets the push force: interference
                                      # vs SNAP_HOLE_D drops 1.15 -> 0.95 mm diametral
SNAP_BARB_H        = 1.4              # barb height; tapers 5.2 -> 3.8 going UP, so it leads in
SNAP_SLOT_W        = 1.2              # slot splitting each post so the halves can flex
SNAP_INSET         = 4.5              # post center from outer edge

# FPC slot, measured from the LEFT outer edge. CLOSED rectangle (no longer open to the bottom
# edge) so the enclosure's bottom wall lands on solid plate instead of bridging the gap.
FPC_SLOT_FROM_L    = 35.0             # slot starts 35 mm from the left edge
FPC_SLOT_TO_L      = 50.0             # ...and ends 50 mm from the left edge
FPC_SLOT_FROM_BOT  = 5.0              # inner edge of the box wall is at 2 mm - clear it
FPC_SLOT_TO_BOT    = 17.0             # reaches past the pocket edge so the ribbon folds through

# ---- ESP32 driver-board enclosure on the BACK of Part B ----
ENC_W, ENC_H, ENC_D = 70.0, 100.0, 20.0   # outer box, matches rectangle.3mf
ENC_WALL           = 2.0
LID_T              = 2.4
BOARD_W, BOARD_H   = 29.46, 48.25     # Waveshare e-Paper ESP32 Driver Board PCB (from their wiki)
BOARD_CLEAR        = 0.6              # per-side slack in the locating guides
GUIDE_T, GUIDE_L, GUIDE_H = 1.6, 10.0, 4.0
BOARD_BOTTOM_Y     = -44.0            # board's FPC-connector edge, just above the ribbon slot
CABLE_W, CABLE_H   = 14.0, 9.0        # USB-C cable exit through the bottom wall
CABLE_OFFSET_X     = 12.0             # from the box centre; +20 fouled the right lid pillar
FPC_SLOT_DEPTH_B   = 6.0              # how far the back-plate slot reaches inward from the pocket edge

KEYHOLE_BIG_D      = 7.0
KEYHOLE_SLOT_W     = 3.6
KEYHOLE_SLOT_L     = 8.0
KEYHOLE_INSET_X    = 35.0             # from outer side edges
KEYHOLE_FROM_TOP   = 14.0             # big-hole center from outer top edge
# --------------------------------------------

POCKET_W = PANEL_W + 2 * CLEAR
POCKET_H = PANEL_H + 2 * CLEAR
OUT_W    = POCKET_W + 2 * MARGIN
OUT_H    = POCKET_H + 2 * MARGIN
SEG      = 48

def rrect(w, h, z, r=CORNER_R):
    """rounded-rectangle slab centered on origin in XY, from z=0 to z."""
    cs = CrossSection.square([w - 2 * r, h - 2 * r], True).offset(r, JoinType.Round, 2.0, SEG)
    return cs.extrude(z)

def box(w, h, z, cx=0, cy=0, z0=0):
    return Manifold.cube([w, h, z], True).translate([cx, cy, z0 + z / 2])

def cyl(d, z, cx=0, cy=0, z0=0):
    return Manifold.cylinder(z, d / 2, d / 2, SEG).translate([cx, cy, z0])

FPC_SLOT_W  = FPC_SLOT_TO_L - FPC_SLOT_FROM_L
FPC_SLOT_CX = -OUT_W / 2 + (FPC_SLOT_FROM_L + FPC_SLOT_TO_L) / 2
FPC_SLOT_H  = FPC_SLOT_TO_BOT - FPC_SLOT_FROM_BOT
FPC_SLOT_CY = -OUT_H / 2 + (FPC_SLOT_FROM_BOT + FPC_SLOT_TO_BOT) / 2
ENC_CX = FPC_SLOT_CX                       # box centred on the ribbon exit
ENC_CY = -OUT_H / 2 + ENC_H / 2            # ...and sitting against the bottom edge

def lid_snap_positions():
    """4 posts on internal corner pillars, inset from the box's inner wall."""
    hx = (ENC_W - 2 * ENC_WALL) / 2 - 5.0
    hy = (ENC_H - 2 * ENC_WALL) / 2 - 5.0
    return [(ENC_CX - hx, ENC_CY - hy), (ENC_CX + hx, ENC_CY - hy),
            (ENC_CX - hx, ENC_CY + hy), (ENC_CX + hx, ENC_CY + hy)]

def snap_positions():
    """4 corners + 2 mid-span on the long edges. x=0 keeps the bottom one clear of the FPC slot."""
    x = OUT_W / 2 - SNAP_INSET
    y = OUT_H / 2 - SNAP_INSET
    return [(-x, -y), (x, -y), (-x, y), (x, y), (0, -y), (0, y)]

# ---------------- Part A : front bezel frame ----------------
A_T = BEZEL_T + POCKET_T
a = rrect(OUT_W, OUT_H, A_T)
# panel pocket, open toward the back (z from BEZEL_T to A_T)
a -= box(POCKET_W, POCKET_H, POCKET_T + 1, z0=BEZEL_T)
# bezel window through the front plate
win_w = POCKET_W - 2 * OVERLAP_SIDE
win_h = POCKET_H - OVERLAP_TOP - OVERLAP_BOTTOM
win_cy = (OVERLAP_BOTTOM - OVERLAP_TOP) / 2
a -= box(win_w, win_h, BEZEL_T + 2, cy=win_cy, z0=-1)
# notch in the bottom rim wall so the FPC can exit in-plane (rim only, not the bezel plate)
a -= box(FPC_SLOT_W, MARGIN + 1, POCKET_T + 1, cx=FPC_SLOT_CX,
         cy=-(POCKET_H / 2 + MARGIN / 2), z0=BEZEL_T)
# snap posts: shaft the thickness of Part B, then a barb that tapers 5.4 -> 4.0 upward so the
# plate leads in on the way down and catches on the flat underside once it clicks past.
for (x, y) in snap_positions():
    a += cyl(SNAP_POST_D, BACK_T, x, y, A_T)
    a += Manifold.cylinder(SNAP_BARB_H, SNAP_BARB_D / 2, SNAP_POST_D / 2, SEG) \
                 .translate([x, y, A_T + BACK_T])
    a -= box(SNAP_SLOT_W, SNAP_BARB_D + 2, BACK_T + SNAP_BARB_H + 2, cx=x, cy=y, z0=A_T)

# ---------------- Part B : back plate ----------------
b = rrect(OUT_W, OUT_H, BACK_T)
# FPC slot: open to the bottom edge, reaching FPC_SLOT_DEPTH_B into the pocket area
b -= box(FPC_SLOT_W, FPC_SLOT_H, BACK_T + 2, cx=FPC_SLOT_CX, cy=FPC_SLOT_CY, z0=-1)
# snap holes
for (x, y) in snap_positions():
    b -= cyl(SNAP_HOLE_D, BACK_T + 2, x, y, -1)
# keyholes (big hole low, slot running upward)
for sx in (-1, 1):
    kx = sx * (OUT_W / 2 - KEYHOLE_INSET_X)
    ky = OUT_H / 2 - KEYHOLE_FROM_TOP - KEYHOLE_SLOT_L
    b -= cyl(KEYHOLE_BIG_D, BACK_T + 2, kx, ky, -1)
    b -= box(KEYHOLE_SLOT_W, KEYHOLE_SLOT_L, BACK_T + 2, cx=kx, cy=ky + KEYHOLE_SLOT_L / 2, z0=-1)

# ---------------- enclosure on the back of Part B ----------------
Z0 = BACK_T                      # the back plate IS the enclosure floor
ZT = BACK_T + ENC_D              # open face where the lid lands

# walls (no floor - the plate serves); open at the top
b += (box(ENC_W, ENC_H, ENC_D, cx=ENC_CX, cy=ENC_CY, z0=Z0)
      - box(ENC_W - 2 * ENC_WALL, ENC_H - 2 * ENC_WALL, ENC_D + 2, cx=ENC_CX, cy=ENC_CY, z0=Z0))

# corner pillars + barbed posts the lid snaps onto
for (x, y) in lid_snap_positions():
    b += box(10.0, 10.0, ENC_D, cx=x, cy=y, z0=Z0)
    b += cyl(SNAP_POST_D, LID_T, x, y, ZT)
    b += Manifold.cylinder(SNAP_BARB_H, SNAP_BARB_D / 2, SNAP_POST_D / 2, SEG) \
                 .translate([x, y, ZT + LID_T])
    b -= box(SNAP_SLOT_W, SNAP_BARB_D + 2, LID_T + SNAP_BARB_H + 2, cx=x, cy=y, z0=ZT)

# L-brackets that locate the PCB without trapping it
pw, ph = BOARD_W + 2 * BOARD_CLEAR, BOARD_H + 2 * BOARD_CLEAR
bx, by = ENC_CX, BOARD_BOTTOM_Y + BOARD_H / 2
for sx in (-1, 1):
    for sy in (-1, 1):
        b += box(GUIDE_T, GUIDE_L, GUIDE_H,
                 cx=bx + sx * (pw / 2 + GUIDE_T / 2), cy=by + sy * (ph / 2 - GUIDE_L / 2), z0=Z0)
        b += box(GUIDE_L, GUIDE_T, GUIDE_H,
                 cx=bx + sx * (pw / 2 - GUIDE_L / 2), cy=by + sy * (ph / 2 + GUIDE_T / 2), z0=Z0)

# USB-C cable exit through the bottom wall
b -= box(CABLE_W, ENC_WALL + 2, CABLE_H, cx=ENC_CX + CABLE_OFFSET_X,
         cy=-OUT_H / 2 + ENC_WALL / 2, z0=Z0)

# ---------------- Part C : enclosure lid ----------------
lid = rrect(ENC_W, ENC_H, LID_T, r=3.0)
for (x, y) in lid_snap_positions():
    lid -= cyl(SNAP_HOLE_D, LID_T + 2, x - ENC_CX, y - ENC_CY, -1)

# ---------------- export ----------------
def write_stl(m: Manifold, path: pathlib.Path):
    mesh = m.to_mesh()
    v = np.asarray(mesh.vert_properties)[:, :3]
    t = np.asarray(mesh.tri_verts)
    tri = v[t]                                   # (n,3,3)
    n = np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0])
    n /= np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-12)
    with open(path, "wb") as f:
        f.write(b"e-paper frame".ljust(80, b"\0"))
        f.write(struct.pack("<I", len(t)))
        rec = np.zeros(len(t), dtype=[("n", "<3f4"), ("v", "<9f4"), ("a", "<u2")])
        rec["n"] = n
        rec["v"] = tri.reshape(-1, 9)
        f.write(rec.tobytes())
    bb = m.bounding_box()
    print(f"{path.name:18s} tris={len(t):6d}  bbox={np.round(bb[:3],2)} -> {np.round(bb[3:],2)}  vol={m.volume():.0f} mm^3")

out = pathlib.Path(__file__).parent
write_stl(a, out / "frame_front.stl")
write_stl(b, out / "frame_back.stl")
write_stl(lid, out / "frame_lid.stl")
both = a + b.translate([0, OUT_H + 10, 0]) + lid.translate([OUT_W / 2 + ENC_W / 2 + 10, 0, 0])
write_stl(both, out / "frame_both.stl")

print(f"\nouter {OUT_W:.1f} x {OUT_H:.1f} mm | pocket {POCKET_W:.1f} x {POCKET_H:.1f} x {POCKET_T} | "
      f"window {win_w:.1f} x {win_h:.1f} | front {A_T:.1f} thick, back {BACK_T:.1f} thick | "
      f"assembled {A_T + BACK_T:.1f}")
print(f"FPC slot {FPC_SLOT_W:.1f} wide, {FPC_SLOT_FROM_L:.0f}-{FPC_SLOT_TO_L:.0f} mm from the left edge "
      f"(center x={FPC_SLOT_CX:+.1f})")
print(f"{len(snap_positions())} snap posts, {SNAP_POST_D} shaft / {SNAP_BARB_D} barb into {SNAP_HOLE_D} holes")
print(f"enclosure {ENC_W} x {ENC_H} x {ENC_D} at x={ENC_CX:+.1f} y={ENC_CY:+.1f}, "
      f"walls {ENC_WALL}, lid {LID_T} on 4 snaps")
print(f"PCB guides for {BOARD_W} x {BOARD_H} (+{BOARD_CLEAR}/side), cable exit {CABLE_W} x {CABLE_H}")
print(f"total depth front-to-lid: {A_T + BACK_T + ENC_D + LID_T:.1f} mm")
