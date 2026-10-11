"""Scanned facade (a wall, usually with a roof slope above it) -> game asset.

Rebuilds the low-poly mesh as a regular grid over the scan: a sheet for the wall, and
where the scan has one, a strip under the eaves and a sheet for the roof slope. Then
bakes the scan's colour and surface detail onto it and exports a GLB with its textures,
plus comparison renders.

blender --background --factory-startup --disable-autoexec --python tools/scan_facade.py --
    <in.glb> <out_dir> <name> [max_tris] [tex_size] [--flat] [--wall-height=H] [--no-roof]

--flat           keep the wall and the roof as true planes, with all the relief in the
                 normal map: a cleaner bake on flat facades (windows and doors don't smear).
                 Without it, every grid point is snapped onto the scan.
--wall-height=H  scale the scan so the wall is H metres to its eaves (or its top, with no
                 roof). For scans without a real-world scale. Otherwise centimetre scans are
                 detected by their size and everything else is taken as metres.
--no-roof        a wall only, even if something sits above it

The scan may face any way: the wall's face is turned to look toward the game's +Z.

Used for (from the 4k GLBs, 2000 tris, 2048, --flat): see assets/models/scans/README.md
"""
import bpy, bmesh, sys, os, math, time
import numpy as np
from mathutils import Vector, Matrix
from mathutils.bvhtree import BVHTree

argv = sys.argv[sys.argv.index("--") + 1:]
args = [a for a in argv if not a.startswith("--")]
opts = {a.split("=")[0]: (a.split("=")[1] if "=" in a else True) for a in argv if a.startswith("--")}
src, out, name = os.path.abspath(args[0]), os.path.abspath(args[1]), args[2]
MAX_TRIS = int(args[3]) if len(args) > 3 else 2000
TEX = int(args[4]) if len(args) > 4 else 2048
FLAT = "--flat" in opts
NO_ROOF = "--no-roof" in opts
WALL_HEIGHT = float(opts["--wall-height"]) if "--wall-height" in opts else None
os.makedirs(out, exist_ok=True)
t0 = time.time()
def log(*a):
    print("[facade %5.1fs]" % (time.time() - t0), *a, flush=True)

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=src)
meshes = [o for o in bpy.context.scene.objects if o.type == 'MESH']

# The scan's materials, rebuilt to show just their texture as light (whatever shading the
# file came with), so baking "emission" gives the scan's colour exactly
for m in bpy.data.materials:
    if not m.use_nodes:
        continue
    nt = m.node_tree
    tex = [n for n in nt.nodes if n.type == 'TEX_IMAGE' and n.image]
    if not tex:
        continue
    tex.sort(key=lambda n: n.image.size[0] * n.image.size[1], reverse=True)
    colour = next((n for n in tex if n.image.colorspace_settings.name == 'sRGB'), tex[0])
    out_node = next((n for n in nt.nodes if n.type == 'OUTPUT_MATERIAL'), None) or nt.nodes.new('ShaderNodeOutputMaterial')
    emit = nt.nodes.new('ShaderNodeEmission')
    nt.links.new(colour.outputs["Color"], emit.inputs["Color"])
    nt.links.new(emit.outputs["Emission"], out_node.inputs["Surface"])

# --- One mesh -------------------------------------------------------------------------------
for o in bpy.context.scene.objects:
    o.select_set(o in meshes)
bpy.context.view_layer.objects.active = meshes[0]
bpy.ops.object.parent_clear(type='CLEAR_KEEP_TRANSFORM')
bpy.ops.object.join()
high = bpy.context.view_layer.objects.active
high.name = "high"
bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
for o in list(bpy.context.scene.objects):
    if o.type == 'EMPTY':
        bpy.data.objects.remove(o)
me = high.data

def points():
    co = np.empty(len(me.vertices) * 3)
    me.vertices.foreach_get("co", co)
    return co.reshape(-1, 3)

def set_points(co):
    me.vertices.foreach_set("co", co.ravel())
    me.update()

# --- Turn it to face the front: the wall runs along X, its face looks toward -Y --------------
# Only upright surfaces count (the ground, roofs and clutter would skew it), by area
def faces():
    n = len(me.polygons)
    nor, cen, area = np.empty(n * 3), np.empty(n * 3), np.empty(n)
    me.polygons.foreach_get("normal", nor)
    me.polygons.foreach_get("center", cen)
    me.polygons.foreach_get("area", area)
    return nor.reshape(-1, 3), cen.reshape(-1, 3), area

co = points()
nor, cen, area = faces()
zmin, zmax = co[:, 2].min(), co[:, 2].max()
upright = (np.abs(nor[:, 2]) < 0.3) & (cen[:, 2] < zmin + (zmax - zmin) * 0.6)
# The wall's direction: the dominant horizontal direction of the upright faces' normals is
# across the wall; weighted by area
nh = nor[upright][:, :2]
w = area[upright]
cov = (nh * w[:, None]).T @ nh
evals, evecs = np.linalg.eigh(cov)
across = evecs[:, np.argmax(evals)]
facing = (nh * w[:, None]).sum(axis=0)
if np.dot(across, facing) < 0:
    across = -across
# Turn so `across` points to -Y
turn = math.atan2(-1, 0) - math.atan2(across[1], across[0])
rot = np.array(Matrix.Rotation(turn, 3, 'Z'))
co = co @ rot.T
set_points(co)
log("turned %.0f degrees to face the front" % (math.degrees(turn) % 360))

# --- Size: metres, the wall's foot at z = 0, its face in the plane y = 0 ----------------------
nor, cen, area = faces()
upright = (np.abs(nor[:, 2]) < 0.3) & (nor[:, 1] < -0.5)       # facing the front
span = (co.max(axis=0) - co.min(axis=0)).max()
# The wall plane: the most common depth among front-facing upright surface, by area
hist, edges = np.histogram(cen[upright, 1], bins=200, weights=area[upright])
k = int(np.argmax(hist))
wy = float((edges[k] + edges[k + 1]) / 2)
tol = span * 0.01
on_face = upright & (np.abs(cen[:, 1] - wy) < tol)
foot = float(np.percentile(cen[on_face, 2], 0.3))
top = float(np.percentile(cen[on_face, 2], 99.0))
if WALL_HEIGHT:
    scale = WALL_HEIGHT / (top - foot)
elif span > 50:
    scale = 0.01                                     # centimetres
else:
    scale = 1.0
x_lo = float(np.percentile(cen[on_face, 0], 0.3))
x_hi = float(np.percentile(cen[on_face, 0], 99.7))
shift = np.array([(x_lo + x_hi) / 2, wy, foot])
co = (co - shift) * scale
cen = (cen - shift) * scale
area = area * scale * scale
set_points(co)
half = (x_hi - x_lo) / 2 * scale
x_lo, x_hi = -half, half
log("scale %.4f; wall %.2f m wide" % (scale, x_hi - x_lo))

# --- Each column's profile: how tall the wall is there, and where its roof runs -------------------
deps = bpy.context.evaluated_depsgraph_get()
bvh = BVHTree.FromObject(high, deps)
# Measure from points spread evenly over the scanned surface (by area), not its vertices:
# a scan's vertices crowd where it's detailed and thin out where it's plain
me.calc_loop_triangles()
tri_v = np.empty(len(me.loop_triangles) * 3, np.int64)
me.loop_triangles.foreach_get("vertices", tri_v)
tri = co[tri_v.reshape(-1, 3)]
tri_area = 0.5 * np.linalg.norm(np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0]), axis=1)
rng = np.random.default_rng(1)
pick = rng.choice(len(tri), size=300000, p=tri_area / tri_area.sum())
r1, r2 = rng.random(len(pick)), rng.random(len(pick))
flip = r1 + r2 > 1
r1[flip], r2[flip] = 1 - r1[flip], 1 - r2[flip]
t = tri[pick]
co = t[:, 0] + (t[:, 1] - t[:, 0]) * r1[:, None] + (t[:, 2] - t[:, 0]) * r2[:, None]
y, z = co[:, 1], co[:, 2]
near_wall = np.abs(y) < 0.12
wall_top = float(np.percentile(z[near_wall & (co[:, 0] > x_lo) & (co[:, 0] < x_hi)], 99.0))
roofish = (z > 0.6 * wall_top) & (y > 0.05)
has_roof = not NO_ROOF and roofish.sum() > 0.04 * len(co)

budget_quads = MAX_TRIS // 2
# Columns: about square cells over the whole facade (wall plus roof, if any)
approx_len = wall_top * (2.2 if has_roof else 1.0)
cols = max(8, int(round(math.sqrt(budget_quads * (x_hi - x_lo) / approx_len))))
xs = np.linspace(x_lo, x_hi, cols + 1)
colw = (x_hi - x_lo) / cols

def robust_line(yv, zv):
    """z = a*y + c through the points, ignoring stragglers"""
    keep = np.ones(len(yv), bool)
    a = c = 0.0
    for _ in range(4):
        if keep.sum() < 20:
            return None
        A = np.stack([yv[keep], np.ones(keep.sum())], axis=1)
        (a, c), *_ = np.linalg.lstsq(A, zv[keep], rcond=None)
        res = np.abs(zv - (a * yv + c))
        mad = np.median(res[keep]) + 1e-6
        keep = res < max(0.08, 3.0 * mad)
    return a, c

eave = np.zeros(cols + 1)                      # wall top per column
roof = [None] * (cols + 1)                      # (a, c, s_lo, s_hi) per column
for i, x in enumerate(xs):
    sel = np.abs(co[:, 0] - x) < colw * 0.75
    wz = z[sel & near_wall]
    eave[i] = np.percentile(wz, 98.5) if len(wz) > 30 else np.nan
    if not has_roof:
        continue
    rsel = sel & (z > eave[i] + 0.25 if not np.isnan(eave[i]) else sel) & (y > 0.05)
    if rsel.sum() < 60:
        continue
    fit = robust_line(y[rsel], z[rsel])
    if fit is None or abs(fit[0]) < 0.12:
        continue
    roof[i] = fit
# Fill gaps from neighbours, then smooth along the wall
def fill(arr):
    arr = np.array(arr, float)
    good = ~np.isnan(arr)
    if not good.any():
        return arr
    idx = np.arange(len(arr))
    return np.interp(idx, idx[good], arr[good])
def smooth(arr, reach=1):
    """Median over each value and `reach` neighbours either side: removes one-off blips
    (a chimney, a hole in the scan) but keeps real steps (where one roof meets another)"""
    out = arr.copy()
    for i in range(len(arr)):
        out[i] = np.median(arr[max(0, i - reach):i + reach + 1])
    return out
eave = smooth(fill(eave))
roof_cols = sum(r is not None for r in roof)
has_roof = has_roof and roof_cols > 0.5 * (cols + 1)
if has_roof:
    ra = smooth(fill([r[0] if r else np.nan for r in roof]), 3)
    rc = smooth(fill([r[1] if r else np.nan for r in roof]), 3)
    # Along each column's roof: from its lower edge to its top, from the scan
    s_lo, s_hi, base_y, up_dirs = np.zeros(cols + 1), np.zeros(cols + 1), np.zeros(cols + 1), []
    for i, x in enumerate(xs):
        a_, c_ = ra[i], rc[i]
        up = np.array([0, 1, a_]) / math.sqrt(1 + a_ * a_) * (1 if a_ > 0 else -1)
        by = (eave[i] - c_) / a_                 # where this column's roof plane is at its eave height
        base = np.array([x, by, eave[i]])
        sel = (np.abs(co[:, 0] - x) < colw * 0.75) & (z > eave[i] - 0.5)
        res = np.abs(z[sel] - (a_ * y[sel] + c_))
        on_roof = sel.copy()
        on_roof[sel] = res < 0.25
        sv = (co[on_roof] - base) @ up
        s_lo[i] = np.percentile(sv, 1.0) if len(sv) > 30 else np.nan
        s_hi[i] = np.percentile(sv, 98.5) if len(sv) > 30 else np.nan
        base_y[i] = by
        up_dirs.append(up)
    s_lo, s_hi = smooth(fill(s_lo), 2), smooth(fill(s_hi), 2)
    log("wall %.2f-%.2f m tall, roof pitch %.0f-%.0f deg, %.1f-%.1f m along the slope" %
        (eave.min(), eave.max(), math.degrees(math.atan(np.abs(ra).min())), math.degrees(math.atan(np.abs(ra).max())),
         (s_hi - s_lo).min(), (s_hi - s_lo).max()))
else:
    log("wall only: %.2f-%.2f m tall" % (eave.min(), eave.max()))

# --- The grid -----------------------------------------------------------------------------
if has_roof:
    roof_len = float(np.mean(s_hi - s_lo))
    total_len = float(np.mean(eave)) + 0.35 + roof_len
    rows = budget_quads // cols
    wall_rows = max(3, round(rows * np.mean(eave) / total_len))
    eave_rows = 1
    roof_rows = rows - wall_rows - eave_rows
else:
    rows = budget_quads // cols
    wall_rows, eave_rows, roof_rows = rows, 0, 0
quad_rows = wall_rows + eave_rows + roof_rows
log("grid: %d columns x %d rows (wall %d, eave %d, roof %d) = %d tris" %
    (cols, quad_rows, wall_rows, eave_rows, roof_rows, cols * quad_rows * 2))

def snap(p, n, reach=0.6):
    """Move p onto the scanned surface along +-n (nearest hit within reach)."""
    if FLAT:
        return p
    best = None
    for d in (n, -n):
        hit = bvh.ray_cast(p - d * reach, d, reach * 2)
        if hit[0] is not None and (best is None or (hit[0] - p).length < (best - p).length):
            best = hit[0]
    return best if best is not None else p

# Each column's run of points, foot to top
columns = []
for i, x in enumerate(xs):
    pts = []
    for r in range(wall_rows + 1):
        pts.append(snap(Vector((x, 0, eave[i] * r / wall_rows)), Vector((0, -1, 0))))
    if has_roof:
        up = Vector(up_dirs[i].tolist())
        normal = Vector((0, -ra[i], 1)).normalized() * (1 if ra[i] > 0 else -1)
        if normal.z < 0:
            normal = -normal
        base = Vector((x, base_y[i], eave[i]))
        lower = base + up * s_lo[i]
        for r in range(roof_rows + 1):
            s = s_lo[i] + (s_hi[i] - s_lo[i]) * r / roof_rows
            pts.append(snap(base + up * s, normal))
        _ = lower
    columns.append(pts)
# v runs along each column by distance (so the texture isn't stretched), normalised per column
vcoord = []
for pts in columns:
    d = [0.0]
    for k in range(1, len(pts)):
        d.append(d[-1] + (pts[k] - pts[k - 1]).length)
    vcoord.append([t / d[-1] for t in d])

lowme = bpy.data.meshes.new(name)
bm = bmesh.new()
uv_layer = bm.loops.layers.uv.new("UVMap")
verts = [[bm.verts.new(p) for p in col] for col in columns]
nrows = len(columns[0])
for r in range(nrows - 1):
    for i in range(cols):
        a_, b_, c_, d_ = verts[i][r], verts[i + 1][r], verts[i + 1][r + 1], verts[i][r + 1]
        u0, u1 = i / cols, (i + 1) / cols
        uva, uvb, uvc, uvd = (u0, vcoord[i][r]), (u1, vcoord[i + 1][r]), (u1, vcoord[i + 1][r + 1]), (u0, vcoord[i][r + 1])
        for tri, uvs in (((a_, b_, c_), (uva, uvb, uvc)), ((a_, c_, d_), (uva, uvc, uvd))):
            f = bm.faces.new(tri)
            for loop, uv in zip(f.loops, uvs):
                loop[uv_layer].uv = uv
bm.normal_update()
# Faces must look toward the front (-Y) and up (the roof)
front = sum((f.normal * f.calc_area() for f in bm.faces), Vector())
if front.y > 0:
    bmesh.ops.reverse_faces(bm, faces=bm.faces)
bm.to_mesh(lowme)
bm.free()
eave_z = float(np.mean(eave))
lowobj = bpy.data.objects.new(name, lowme)
bpy.context.scene.collection.objects.link(lowobj)
ntris = len(lowme.polygons)
log("low mesh: %d tris, %d verts" % (ntris, len(lowme.vertices)))
assert ntris <= MAX_TRIS

# --- Bake ---------------------------------------------------------------------------------
scene = bpy.context.scene
scene.render.engine = 'CYCLES'
scene.cycles.device = 'CPU'
scene.cycles.samples = 4
scene.render.bake.use_selected_to_active = True
scene.render.bake.cage_extrusion = 0.3
scene.render.bake.max_ray_distance = 0.7
scene.render.bake.margin = 16
mat = bpy.data.materials.new(name)
mat.use_nodes = True
lowme.materials.append(mat)
nt = mat.node_tree
def fill_holes(img, flat_normal):
    """Pixels the bake didn't reach (black, alpha 0) get the colour round them (push-pull)."""
    px = np.empty(TEX * TEX * 4, np.float32)
    img.pixels.foreach_get(px)
    px = px.reshape(TEX, TEX, 4)
    hit = (px[..., :3].max(axis=2) > 1e-4)
    missing = (~hit).mean()
    if flat_normal:
        px[~hit, :3] = (0.5, 0.5, 1.0)
    else:
        # Pull: averages of the good pixels at halving resolutions; push: fill from them
        levels = [(px[..., :3] * hit[..., None], hit.astype(np.float32))]
        while levels[-1][1].shape[0] > 1:
            c, w = levels[-1]
            n = c.shape[0] // 2
            c2 = c.reshape(n, 2, n, 2, 3).sum(axis=(1, 3))
            w2 = w.reshape(n, 2, n, 2).sum(axis=(1, 3))
            levels.append((c2, w2))
        fillc = levels[-1][0] / np.maximum(levels[-1][1], 1e-6)[..., None]
        for c, w in reversed(levels[:-1]):
            up = np.repeat(np.repeat(fillc, 2, axis=0), 2, axis=1)
            mean = c / np.maximum(w, 1e-6)[..., None]
            fillc = np.where((w > 0)[..., None], mean, up)
        px[..., :3] = np.where(hit[..., None], px[..., :3], fillc)
    px[..., 3] = 1.0
    img.pixels.foreach_set(px.ravel())
    log("  %s: %.1f%% of the texture filled in (the scan wasn't there)" % (img.name, missing * 100))

def bake(label, kind, colorspace, fill):
    img = bpy.data.images.new(name + "_" + label, TEX, TEX, alpha=True)
    img.colorspace_settings.name = colorspace
    img.generated_color = fill
    node = nt.nodes.new('ShaderNodeTexImage')
    node.image = img
    nt.nodes.active = node
    for o in scene.objects:
        o.select_set(o in (high, lowobj))
    bpy.context.view_layer.objects.active = lowobj
    if kind == 'NORMAL':
        scene.render.bake.normal_space = 'TANGENT'
    # Mark everything as "not baked" (alpha 0); the bake writes alpha 1 where it hits
    img.alpha_mode = 'STRAIGHT'
    px = np.zeros((TEX, TEX, 4), np.float32)
    img.pixels.foreach_set(px.ravel())
    bpy.ops.object.bake(type=kind)
    fill_holes(img, flat_normal=(kind == 'NORMAL'))
    img.filepath_raw = os.path.join(out, name + "_" + label + ".png")
    img.file_format = 'PNG'
    img.save()
    return img, node
color, cnode = bake("color", 'EMIT', 'sRGB', (0.55, 0.52, 0.47, 1))
log("baked colour")
normal, nnode = bake("normal", 'NORMAL', 'Non-Color', (0.5, 0.5, 1.0, 1))
log("baked normals")

bsdf = nt.nodes["Principled BSDF"]
bsdf.inputs["Roughness"].default_value = 0.9
nt.links.new(cnode.outputs["Color"], bsdf.inputs["Base Color"])
nm = nt.nodes.new('ShaderNodeNormalMap')
nt.links.new(nnode.outputs["Color"], nm.inputs["Color"])
nt.links.new(nm.outputs["Normal"], bsdf.inputs["Normal"])
for o in scene.objects:
    o.select_set(o == lowobj)
bpy.context.view_layer.objects.active = lowobj
bpy.ops.export_scene.gltf(filepath=os.path.join(out, name + ".glb"), use_selection=True, export_format='GLB',
                          export_yup=True, export_apply=True, export_image_format='AUTO')
size = Vector(lowobj.dimensions)
log("exported %s: %.2f m along, %.2f m deep, %.2f m tall" % (name + ".glb", size.x, size.y, size.z))

# --- Comparison renders -------------------------------------------------------------------
scene.render.engine = 'BLENDER_EEVEE_NEXT'
scene.render.resolution_x, scene.render.resolution_y = 960, 600
world = bpy.data.worlds.new("w"); scene.world = world
world.use_nodes = True
world.node_tree.nodes["Background"].inputs[0].default_value = (0.55, 0.6, 0.68, 1)
sun = bpy.data.objects.new("sun", bpy.data.lights.new("sun", 'SUN'))
sun.data.energy = 3.5
sun.rotation_euler = (math.radians(55), 0, math.radians(-35))
scene.collection.objects.link(sun)
cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam"))
scene.collection.objects.link(cam)
scene.camera = cam
cam.data.lens = 30
zs_low = [v.co.z for v in lowme.vertices]
mid = Vector((0, 0, (min(zs_low) + max(zs_low)) / 2))
span = (x_hi - x_lo)
shots = {"front": (Vector((0, -1, 0.12)), span * 1.05, Vector()), "angle": (Vector((0.75, -0.8, 0.3)), span * 1.0, Vector()),
         "close": (Vector((-0.25, -0.5, 0.05)), span * 0.42, Vector((-2.5, 0, -1.2))), "under": (Vector((0.1, -0.6, -0.35)), span * 0.45, Vector((1.5, 0, 0.8)))}
for which, obj in (("scan", high), ("low", lowobj)):
    high.hide_render = obj is not high
    lowobj.hide_render = obj is not lowobj
    for shot, (d, dist, off) in shots.items():
        target = mid + off
        cam.location = target + d.normalized() * dist
        cam.rotation_euler = (target - cam.location).to_track_quat('-Z', 'Y').to_euler()
        scene.render.filepath = os.path.join(out, "compare_%s_%s.png" % (shot, which))
        bpy.ops.render.render(write_still=True)
high.hide_render = True
lowobj.hide_render = False
wire = lowobj.modifiers.new("wire", 'WIREFRAME')
wire.thickness = 0.012
wmat = bpy.data.materials.new("wire"); wmat.use_nodes = True
wmat.node_tree.nodes["Principled BSDF"].inputs["Base Color"].default_value = (0.05, 0.05, 0.05, 1)
lowme.materials.append(wmat)
wire.material_offset = 1
d, dist, off = shots["angle"]
cam.location = mid + d.normalized() * dist
cam.rotation_euler = (mid - cam.location).to_track_quat('-Z', 'Y').to_euler()
scene.render.filepath = os.path.join(out, "compare_wire.png")
bpy.ops.render.render(write_still=True)
log("done")
