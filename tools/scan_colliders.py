"""Collision boxes for a scanned model: the 50 cm squares of ground it covers (above knee
height), each as tall as the model is there, merged into as few boxes as reasonable.
Prints them in the game's model space (Y up), as C++ initialisers, plus the model's bounds.

blender --background --factory-startup --disable-autoexec --python tools/scan_colliders.py -- <model.glb> [cell] [min_height]
"""
import bpy, sys, os
import numpy as np

argv = sys.argv[sys.argv.index("--") + 1:]
src = os.path.abspath(argv[0])
CELL = float(argv[1]) if len(argv) > 1 else 0.5
MIN_H = float(argv[2]) if len(argv) > 2 else 0.6
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=src)
pts = []
for o in bpy.context.scene.objects:
    if o.type != 'MESH':
        continue
    me = o.data
    me.calc_loop_triangles()
    co = np.empty(len(me.vertices) * 3); me.vertices.foreach_get("co", co)
    m = np.array(o.matrix_world)
    co = co.reshape(-1, 3) @ m[:3, :3].T + m[:3, 3]
    tv = np.empty(len(me.loop_triangles) * 3, np.int64); me.loop_triangles.foreach_get("vertices", tv)
    tri = co[tv.reshape(-1, 3)]
    area = 0.5 * np.linalg.norm(np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0]), axis=1)
    n = max(1, int(area.sum() / (CELL * CELL / 40)))
    rng = np.random.default_rng(3)
    pick = rng.choice(len(tri), size=n, p=area / area.sum())
    r1, r2 = rng.random(n), rng.random(n)
    f = r1 + r2 > 1
    r1[f], r2[f] = 1 - r1[f], 1 - r2[f]
    t = tri[pick]
    pts.append(t[:, 0] + (t[:, 1] - t[:, 0]) * r1[:, None] + (t[:, 2] - t[:, 0]) * r2[:, None])
P = np.concatenate(pts)
# Blender (Z up) -> game (Y up): x, y_game = z, z_game = -y
G = np.stack([P[:, 0], P[:, 2], -P[:, 1]], axis=1)
lo, hi = G.min(axis=0), G.max(axis=0)
print("[colliders] bounds x %.2f..%.2f  y %.2f..%.2f  z %.2f..%.2f" % (lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]))
tall = G[G[:, 1] > MIN_H]
x0, z0 = lo[0], lo[2]
nx, nz = int((hi[0] - x0) / CELL) + 1, int((hi[2] - z0) / CELL) + 1
height = np.zeros((nx, nz))
ix = ((tall[:, 0] - x0) / CELL).astype(int)
iz = ((tall[:, 2] - z0) / CELL).astype(int)
np.maximum.at(height, (ix, iz), tall[:, 1])
# Fill single-cell holes inside the footprint (a scan's thin spots)
solid = height > 0
pad = np.pad(solid, 1)
neigh = pad[:-2, 1:-1].astype(int) + pad[2:, 1:-1] + pad[1:-1, :-2] + pad[1:-1, 2:]
holes = ~solid & (neigh >= 3)
for i, k in zip(*np.nonzero(holes)):
    height[i, k] = max(height[max(0, i - 1):i + 2, max(0, k - 1):k + 2].max(), MIN_H)
solid = height > 0
# Greedy rectangles: grow along z, then along x while the rows match; height rounded up to 0.5 m
q = np.ceil(height * 2) / 2
used = np.zeros_like(solid)
boxes = []
for i in range(nx):
    for k in range(nz):
        if not solid[i, k] or used[i, k]:
            continue
        h = q[i, k]
        k1 = k
        while k1 + 1 < nz and solid[i, k1 + 1] and not used[i, k1 + 1] and q[i, k1 + 1] == h:
            k1 += 1
        i1 = i
        while i1 + 1 < nx and all(solid[i1 + 1, kk] and not used[i1 + 1, kk] and q[i1 + 1, kk] == h for kk in range(k, k1 + 1)):
            i1 += 1
        used[i:i1 + 1, k:k1 + 1] = True
        boxes.append((x0 + i * CELL, x0 + (i1 + 1) * CELL, z0 + k * CELL, z0 + (k1 + 1) * CELL, h))
print("[colliders] %d boxes" % len(boxes))
for b in boxes:
    print("[colliders]     { %.2ff, %.2ff, %.2ff, %.2ff, %.1ff }," % b)
