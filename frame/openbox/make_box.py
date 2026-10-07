#!/usr/bin/env python3
"""Open-top box: solid floor + 4 walls, no lid. Edit the numbers and re-run.
Default matches rectangle.3mf (70 x 100 x 20 outer) with 1 mm walls and floor."""
import struct, pathlib, numpy as np
from manifold3d import Manifold

OUT_X, OUT_Y, OUT_Z = 70.0, 100.0, 20.0   # outer size
WALL   = 2.0                               # side wall thickness
FLOOR  = 2.0                               # bottom thickness

box    = Manifold.cube([OUT_X, OUT_Y, OUT_Z], True).translate([0, 0, OUT_Z / 2])
cavity = Manifold.cube([OUT_X - 2 * WALL, OUT_Y - 2 * WALL, OUT_Z - FLOOR + 1], True) \
                 .translate([0, 0, FLOOR + (OUT_Z - FLOOR + 1) / 2])   # +1 so the top is open
part = box - cavity

def write_stl(m, path):
    mesh = m.to_mesh()
    v = np.asarray(mesh.vert_properties)[:, :3]; t = np.asarray(mesh.tri_verts)
    tri = v[t]
    n = np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0])
    n /= np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-12)
    rec = np.zeros(len(t), dtype=[("n", "<3f4"), ("v", "<9f4"), ("a", "<u2")])
    rec["n"] = n; rec["v"] = tri.reshape(-1, 9)
    with open(path, "wb") as f:
        f.write(b"open box".ljust(80, b"\0")); f.write(struct.pack("<I", len(t))); f.write(rec.tobytes())
    bb = m.bounding_box()
    print(f"{path.name}: {len(t)} tris  bbox {np.round(bb[:3],2)} -> {np.round(bb[3:],2)}  volume {m.volume():.0f} mm^3")

out = pathlib.Path(__file__).parent / f"open_box_{OUT_X:.0f}x{OUT_Y:.0f}x{OUT_Z:.0f}_wall{WALL:g}.stl"
write_stl(part, out)
print(f"inner cavity {OUT_X-2*WALL:.1f} x {OUT_Y-2*WALL:.1f} x {OUT_Z-FLOOR:.1f} mm")
print(f"solid block would be {OUT_X*OUT_Y*OUT_Z:.0f} mm^3, so shell check = {OUT_X*OUT_Y*OUT_Z - (OUT_X-2*WALL)*(OUT_Y-2*WALL)*(OUT_Z-FLOOR):.0f}")
