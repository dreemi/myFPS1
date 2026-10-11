"""Scanned object (a whole building, a ruin, a room corner) -> game asset.

For scans that aren't a single facade: cleans the scan up, simplifies it to a triangle
budget (flat areas first, so edges where the surface really bends survive), unwraps
it, bakes the scan's colour and surface detail onto it, and exports a GLB with its
textures, plus comparison renders.

blender --background --factory-startup --disable-autoexec --python tools/scan_object.py --
    <in.glb> <out_dir> <name> [max_tris] [tex_size] [--height=H] [--trim-ground] [--up=-y]

--height=H     scale so the object is H metres tall (for scans without a real scale);
               otherwise centimetre scans are detected by size, anything else is metres
--trim-ground  remove the patch of scanned ground the object stands on
--reach=M      how far from the simplified surface the bake looks for the scan (default:
               2.5% of the object's size)
--up=AXIS      the scan's up axis if it isn't up already (x, -x, y, -y, z, -z)
--weld=M       join vertices closer than M metres (default 0.004): wider for scans whose
               pieces don't quite meet
--voxel=V      rebuild the shape as one closed shell (thickened, then voxels V metres)
               before simplifying: for scans whose surface simplifying can't handle
               (it stalls, or falls apart into islands). The bake still uses the original.

The object stands on the ground at the origin, centred over it.
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
HEIGHT = float(opts["--height"]) if "--height" in opts else None
TRIM_GROUND = "--trim-ground" in opts
UP = opts.get("--up", "z")
REMESH = float(opts["--voxel"]) if "--voxel" in opts else None
WELD = float(opts["--weld"]) if "--weld" in opts else 0.004
os.makedirs(out, exist_ok=True)
t0 = time.time()
def log(*a):
    print("[object %5.1fs]" % (time.time() - t0), *a, flush=True)

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=src)
meshes = [o for o in bpy.context.scene.objects if o.type == 'MESH']

# The scan's materials show just their texture as light, so baking "emission" is its colour
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

# Up axis
ups = {"z": Matrix.Identity(3), "-z": Matrix.Rotation(math.pi, 3, 'X'), "y": Matrix.Rotation(math.pi / 2, 3, 'X'),
       "-y": Matrix.Rotation(-math.pi / 2, 3, 'X'), "x": Matrix.Rotation(-math.pi / 2, 3, 'Y'), "-x": Matrix.Rotation(math.pi / 2, 3, 'Y')}
co = np.empty(len(me.vertices) * 3)
me.vertices.foreach_get("co", co)
co = co.reshape(-1, 3) @ np.array(ups[UP]).T

# Scale and stand it on the ground
lo, hi = np.percentile(co, 0.2, axis=0), np.percentile(co, 99.8, axis=0)
if HEIGHT:
    scale = HEIGHT / (hi[2] - lo[2])
elif (hi - lo).max() > 50:
    scale = 0.01
else:
    scale = 1.0
co = (co - np.array([(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, lo[2]])) * scale
me.vertices.foreach_set("co", co.ravel())
me.update()

bm = bmesh.new()
bm.from_mesh(me)
bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=WELD)  # weld cracks between pieces
if TRIM_GROUND:
    # The ground patch: faces looking up, near the lowest level, out where nothing stands on them
    ground_z = float(np.percentile([v.co.z for v in bm.verts], 2.0))
    cut = [f for f in bm.faces if f.normal.z > 0.75 and f.calc_center_median().z < ground_z + 0.25]
    bmesh.ops.delete(bm, geom=cut, context='FACES')
    log("trimmed %d ground faces" % len(cut))
# Drop floating scraps
bm.faces.ensure_lookup_table()
seen, parts = set(), []
for f in bm.faces:
    if f.index in seen:
        continue
    stack, part = [f], []
    seen.add(f.index)
    while stack:
        g = stack.pop()
        part.append(g)
        for e in g.edges:
            for h in e.link_faces:
                if h.index not in seen:
                    seen.add(h.index)
                    stack.append(h)
    parts.append(part)
total = sum(len(p) for p in parts)
scraps = [f for p in parts if len(p) < total * 0.005 for f in p]
bmesh.ops.delete(bm, geom=scraps, context='FACES')
bmesh.ops.delete(bm, geom=[v for v in bm.verts if not v.link_faces], context='VERTS')
bm.to_mesh(me)
bm.free()
# Back on the ground after trimming
co = np.empty(len(me.vertices) * 3)
me.vertices.foreach_get("co", co)
co = co.reshape(-1, 3)
co[:, 2] -= np.percentile(co[:, 2], 0.2)
me.vertices.foreach_set("co", co.ravel())
me.update()
size = np.percentile(co, 99.8, axis=0) - np.percentile(co, 0.2, axis=0)
log("cleaned: %d tris in %d pieces (the largest %.0f%%; %d scrap faces dropped), %.2f x %.2f x %.2f m" %
    (sum(len(p.vertices) - 2 for p in me.polygons), len(parts), 100.0 * max(len(p) for p in parts) / total, len(scraps), *size))

# --- Simplify -----------------------------------------------------------------------------------
lowobj = high.copy()
lowobj.data = high.data.copy()
lowobj.name = name
bpy.context.scene.collection.objects.link(lowobj)
for o in bpy.context.scene.objects:
    o.select_set(o == lowobj)
bpy.context.view_layer.objects.active = lowobj
lowobj.data.materials.clear()
if REMESH:
    # Voxels need a closed volume: give the (one-sided) scan some thickness first
    mod = lowobj.modifiers.new("thicken", 'SOLIDIFY')
    mod.thickness = REMESH * 1.6
    mod.offset = 0.0
    bpy.ops.object.modifier_apply(modifier=mod.name)
    mod = lowobj.modifiers.new("remesh", 'REMESH')
    mod.mode = 'VOXEL'
    mod.voxel_size = REMESH
    mod.use_smooth_shade = False
    bpy.ops.object.modifier_apply(modifier=mod.name)
    log("rebuilt as one shell: %d faces" % len(lowobj.data.polygons))
def tri_count(o):
    return sum(len(p.vertices) - 2 for p in o.data.polygons)
if not REMESH:  # (a rebuilt surface goes straight to collapsing)
    mod = lowobj.modifiers.new("planar", 'DECIMATE')
    mod.decimate_type = 'DISSOLVE'
    mod.angle_limit = math.radians(4.0)
    bpy.ops.object.modifier_apply(modifier=mod.name)
bpy.ops.object.mode_set(mode='EDIT')
bpy.ops.mesh.select_all(action='SELECT')
bpy.ops.mesh.quads_convert_to_tris()
bpy.ops.object.mode_set(mode='OBJECT')
for attempt in range(10):
    n = tri_count(lowobj)
    if n <= MAX_TRIS:
        break
    mod = lowobj.modifiers.new("collapse", 'DECIMATE')
    mod.decimate_type = 'COLLAPSE'
    mod.ratio = max(0.0003, (MAX_TRIS * 0.96) / n)
    mod.use_collapse_triangulate = True
    bpy.ops.object.modifier_apply(modifier=mod.name)
    log("collapse pass %d: %d -> %d tris" % (attempt + 1, n, tri_count(lowobj)))
# Still over: drop the smallest separate islands (bits too small to keep their own triangles)
if tri_count(lowobj) > MAX_TRIS:
    bm = bmesh.new()
    bm.from_mesh(lowobj.data)
    bm.faces.ensure_lookup_table()
    seen, islands = set(), []
    for f in bm.faces:
        if f.index in seen:
            continue
        stack, isl = [f], []
        seen.add(f.index)
        while stack:
            g = stack.pop()
            isl.append(g)
            for e in g.edges:
                for h in e.link_faces:
                    if h.index not in seen:
                        seen.add(h.index)
                        stack.append(h)
        islands.append(isl)
    islands.sort(key=lambda isl: sum(f.calc_area() for f in isl))
    total_area = sum(f.calc_area() for f in bm.faces)
    count, dropped_area, drop = len(bm.faces), 0.0, []
    for isl in islands:
        if count <= MAX_TRIS:
            break
        drop += isl
        count -= len(isl)
        dropped_area += sum(f.calc_area() for f in isl)
    if dropped_area > 0.1 * total_area:
        raise SystemExit("[object] simplifying broke the scan into %d islands: try --remesh=8 or --weld" % len(islands))
    bmesh.ops.delete(bm, geom=drop, context='FACES')
    bm.to_mesh(lowobj.data)
    bm.free()
    log("dropped %d small islands (%.1f%% of the surface) to fit the budget" % (len(islands) and sum(1 for _ in drop), 100 * dropped_area / total_area))
bpy.ops.object.mode_set(mode='EDIT')
bpy.ops.mesh.select_all(action='SELECT')
bpy.ops.mesh.delete_loose()
bpy.ops.mesh.dissolve_degenerate(threshold=0.002)
bpy.ops.mesh.quads_convert_to_tris()
bpy.ops.object.mode_set(mode='OBJECT')
# A fresh unwrap in a fresh UV map (the scan's own UVs are meaningless after simplifying,
# and a scan joined from pieces can carry several UV maps)
while lowobj.data.uv_layers:
    lowobj.data.uv_layers.remove(lowobj.data.uv_layers[0])
uv = lowobj.data.uv_layers.new(name="UVMap")
lowobj.data.uv_layers.active = uv
uv.active_render = True
bpy.ops.object.mode_set(mode='EDIT')
bpy.ops.mesh.select_all(action='SELECT')
bpy.ops.uv.smart_project(angle_limit=math.radians(55), island_margin=0.003, area_weight=0.0, correct_aspect=True)
bpy.ops.uv.pack_islands(margin=0.003)
bpy.ops.object.mode_set(mode='OBJECT')
lowobj.data.shade_flat()
# Faces must agree with their neighbours (the bake pushes its search surface out along
# the averaged direction of the faces at each corner, so a mixed-up patch looks nowhere).
# Then each connected piece as a whole faces the way most of the scan under it faces; a
# rebuilt closed shell simply faces outward.
bpy.ops.object.mode_set(mode='EDIT')
bpy.ops.mesh.select_all(action='SELECT')
bpy.ops.mesh.normals_make_consistent(inside=False)
bpy.ops.object.mode_set(mode='OBJECT')
flip = []
if not REMESH:
    deps = bpy.context.evaluated_depsgraph_get()
    bvh = BVHTree.FromObject(high, deps)
    bm = bmesh.new()
    bm.from_mesh(lowobj.data)
    bm.faces.ensure_lookup_table()
    seen = set()
    for f in bm.faces:
        if f.index in seen:
            continue
        stack, piece = [f], []
        seen.add(f.index)
        while stack:
            g = stack.pop()
            piece.append(g)
            for e in g.edges:
                for h in e.link_faces:
                    if h.index not in seen:
                        seen.add(h.index)
                        stack.append(h)
        vote = 0.0
        for g in piece:
            hit = bvh.find_nearest(g.calc_center_median())
            if hit[1] is not None:
                vote += g.calc_area() * (1 if g.normal.dot(hit[1]) >= 0 else -1)
        if vote < 0:
            flip += piece
    bmesh.ops.reverse_faces(bm, faces=flip)
    bm.to_mesh(lowobj.data)
    bm.free()
log("simplified: %d tris, %d verts (%d faces turned to face the way the scan does)" % (tri_count(lowobj), len(lowobj.data.vertices), len(flip)))
assert tri_count(lowobj) <= MAX_TRIS

# --- Bake ---------------------------------------------------------------------------------
scene = bpy.context.scene
scene.render.engine = 'CYCLES'
scene.cycles.device = 'CPU'
scene.cycles.samples = 4
scene.render.bake.use_selected_to_active = True
diag = float(np.linalg.norm(size))
# How far from the simplified surface to look for the scan: a big object simplified to
# a couple of thousand triangles strays further from it (override: --reach=M)
reach = float(opts["--reach"]) if "--reach" in opts else max(0.08, diag * 0.025)
scene.render.bake.cage_extrusion = reach
scene.render.bake.max_ray_distance = reach * 2.5
scene.render.bake.margin = 8
mat = bpy.data.materials.new(name)
mat.use_nodes = True
lowobj.data.materials.append(mat)
nt = mat.node_tree

def fill_holes(img, flat_normal):
    """Pixels the bake didn't reach get the colour round them (push-pull)."""
    px = np.empty(TEX * TEX * 4, np.float32)
    img.pixels.foreach_get(px)
    px = px.reshape(TEX, TEX, 4)
    hit = (px[..., :3].max(axis=2) > 1e-4)
    log("  %s: the bake reached %.0f%% of the texture" % (img.name, 100.0 * hit.mean()))
    if flat_normal:
        px[~hit, :3] = (0.5, 0.5, 1.0)
    else:
        levels = [(px[..., :3] * hit[..., None], hit.astype(np.float32))]
        while levels[-1][1].shape[0] > 1:
            c, w = levels[-1]
            n = c.shape[0] // 2
            levels.append((c.reshape(n, 2, n, 2, 3).sum(axis=(1, 3)), w.reshape(n, 2, n, 2).sum(axis=(1, 3))))
        fillc = levels[-1][0] / np.maximum(levels[-1][1], 1e-6)[..., None]
        for c, w in reversed(levels[:-1]):
            up = np.repeat(np.repeat(fillc, 2, axis=0), 2, axis=1)
            fillc = np.where((w > 0)[..., None], c / np.maximum(w, 1e-6)[..., None], up)
        px[..., :3] = np.where(hit[..., None], px[..., :3], fillc)
    px[..., 3] = 1.0
    img.pixels.foreach_set(px.ravel())

def bake(label, kind, colorspace):
    img = bpy.data.images.new(name + "_" + label, TEX, TEX, alpha=True)
    img.colorspace_settings.name = colorspace
    img.pixels.foreach_set(np.zeros(TEX * TEX * 4, np.float32))
    node = nt.nodes.new('ShaderNodeTexImage')
    node.image = img
    nt.nodes.active = node
    for o in scene.objects:
        o.select_set(o in (high, lowobj))
    bpy.context.view_layer.objects.active = lowobj
    if kind == 'NORMAL':
        scene.render.bake.normal_space = 'TANGENT'
    bpy.ops.object.bake(type=kind)
    fill_holes(img, kind == 'NORMAL')
    img.filepath_raw = os.path.join(out, name + "_" + label + ".png")
    img.file_format = 'PNG'
    img.save()
    return img, node
color, cnode = bake("color", 'EMIT', 'sRGB')
normal, nnode = bake("normal", 'NORMAL', 'Non-Color')
log("baked")
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
log("exported %s: %.2f x %.2f x %.2f m" % (name + ".glb", *lowobj.dimensions))

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
mid = Vector((0, 0, size[2] / 2))
shots = {"front": Vector((0, -1, 0.15)), "angle": Vector((0.8, -0.7, 0.35)), "back": Vector((-0.6, 0.8, 0.3))}
for which, obj in (("scan", high), ("low", lowobj)):
    high.hide_render = obj is not high
    lowobj.hide_render = obj is not lowobj
    for shot, d in shots.items():
        cam.location = mid + d.normalized() * diag * 0.95
        cam.rotation_euler = (mid - cam.location).to_track_quat('-Z', 'Y').to_euler()
        scene.render.filepath = os.path.join(out, "compare_%s_%s.png" % (shot, which))
        bpy.ops.render.render(write_still=True)
log("done")
