"""
Builds the FN FAL (T48 pattern, wooden furniture) for assets/models/fal/, in
separate moving parts so the game can animate it: receiver (with barrel,
furniture and sights), bolt carrier (seen through the ejection port),
charging handle, trigger and magazine, plus a 7.62 round and a fired case.

Run with Blender 4.x:
  blender --background --factory-startup --python tools/make_fal.py -- <output dir> [preview dir]

Writes fal.glb, fal.txt (meshes, pivots and points in game gun space) and the
wood textures. Metal parts use the revolver's neutral steel textures.

Blender space: X right, Y forward (out of the barrel), Z up, meters; origin =
web of the shooting hand at the top of the pistol grip. Game gun space is
(x, z, -y).
"""
import bpy, bmesh, sys, os, math, json, struct
import numpy as np
from mathutils import Vector, Matrix

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
args = sys.argv[sys.argv.index("--") + 1:]
OUT_DIR = os.path.abspath(args[0])
PREVIEW_DIR = os.path.abspath(args[1]) if len(args) > 1 else None
os.makedirs(OUT_DIR, exist_ok=True)

bpy.ops.wm.read_factory_settings(use_empty=True)
import procgen as pg
from procgen import (mat, new_object, select_only, apply_modifiers, prism, box, cyl_y, cyl_x, tube_path, boolean,
                     finish, join, box_uvs, fbm, save, upd)
pg.OUT_DIR = OUT_DIR
scene = bpy.context.scene


def log(*a):
    print("[fal]", *a)


# --- Dimensions -----------------------------------------------------------------------
ZB = 0.045            # bore axis height above the grip's top
SIGHT = 0.098         # sight line: rear aperture centre and front post top
Y_MUZZLE = 0.775
MAG_PIVOT = (0.111, -0.010)   # (y, z): the magazine hooks in at its front and rocks back to latch
TRIGGER_PIVOT = (0.012, -0.006)

M_PARK = mat("parkerized", (0.05, 0.05, 0.055), 1.0, 0.55)
M_BRIGHT = mat("bright", (0.5, 0.5, 0.52), 1.0, 0.3)
M_WOOD = mat("falwood", (0.25, 0.10, 0.04), 0.0, 0.5)
M_BORE = mat("bore", (0.01, 0.01, 0.01), 0.0, 0.9)
M_BRASS = mat("brass", (0.62, 0.42, 0.13), 1.0, 0.32)
M_COPPER = mat("copper", (0.65, 0.33, 0.18), 1.0, 0.35)
M_PRIMER = mat("primer", (0.60, 0.60, 0.62), 1.0, 0.30)


def loft_x(name, profile_yz, rings, material, levels=2):
    """A rounded solid: a (y, z) profile swept across x through rings (x, scale about the centroid)."""
    cy = sum(p[0] for p in profile_yz) / len(profile_yz)
    cz = sum(p[1] for p in profile_yz) / len(profile_yz)
    bm = bmesh.new()
    loops = [[bm.verts.new((x, cy + (y - cy) * s, cz + (z - cz) * s)) for y, z in profile_yz] for x, s in rings]
    n = len(profile_yz)
    bm.faces.new(loops[0])
    bm.faces.new(list(reversed(loops[-1])))
    for a, b in zip(loops, loops[1:]):
        for i in range(n):
            j = (i + 1) % n
            bm.faces.new((a[i], a[j], b[j], b[i]))
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    ob = new_object(name, bm, material)
    sub = ob.modifiers.new("sub", "SUBSURF")
    sub.levels = sub.render_levels = levels
    for p in ob.data.polygons:
        p.use_smooth = True
    apply_modifiers(ob)
    return ob


def loft_y(name, section_xz, stations, material, levels=2):
    """A rounded solid along y: an (x, z) section at stations (y, scale)."""
    cz = sum(p[1] for p in section_xz) / len(section_xz)
    bm = bmesh.new()
    loops = [[bm.verts.new((x * s, y, cz + (z - cz) * s)) for x, z in section_xz] for y, s in stations]
    n = len(section_xz)
    bm.faces.new(loops[0])
    bm.faces.new(list(reversed(loops[-1])))
    for a, b in zip(loops, loops[1:]):
        for i in range(n):
            j = (i + 1) % n
            bm.faces.new((a[i], a[j], b[j], b[i]))
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    ob = new_object(name, bm, material)
    sub = ob.modifiers.new("sub", "SUBSURF")
    sub.levels = sub.render_levels = levels
    for p in ob.data.polygons:
        p.use_smooth = True
    apply_modifiers(ob)
    return ob


def rounded_rect(hx, z0, z1, r, n=4):
    """(x, z) points of a rounded rectangle, counter-clockwise."""
    pts = []
    corners = [(hx - r, z0 + r, -90), (hx - r, z1 - r, 0), (-hx + r, z1 - r, 90), (-hx + r, z0 + r, 180)]
    for cx, cz, a0 in corners:
        for i in range(n + 1):
            a = math.radians(a0 + 90 * i / n)
            pts.append((cx + r * math.cos(a), cz + r * math.sin(a)))
    return pts


def rotate_about_bore(ob, degrees, axis_z=ZB):
    ob.matrix_world = (Matrix.Translation((0, 0, axis_z)) @ Matrix.Rotation(math.radians(degrees), 4, "Y")
                       @ Matrix.Translation((0, 0, -axis_z)))
    upd()
    select_only([ob])
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    return ob


# --- Receiver (static: everything that doesn't move) ------------------------------------
parts = []
upper = prism("upper", [(-0.075, 0.020), (0.190, 0.020), (0.190, 0.068), (0.150, 0.078), (-0.050, 0.080),
                        (-0.075, 0.070)], -0.0165, 0.0165, M_PARK)
lower = prism("lower", [(-0.072, -0.012), (0.135, -0.012), (0.135, 0.022), (-0.072, 0.022)], -0.018, 0.018, M_PARK)
well = box("well", -0.0175, 0.0175, 0.030, 0.120, -0.030, -0.012, M_PARK)
# Ejection port on the right, and the magazine well opening
boolean(upper, [box("port", 0.009, 0.03, 0.030, 0.105, 0.034, 0.064, M_BORE)])
for ob in (lower, well):
    boolean(ob, [box("magcut", -0.0135, 0.0135, 0.040, 0.113, -0.06, 0.006, M_BORE)])
parts += [finish(upper, 0.0015, segments=2), finish(lower, 0.0015, segments=2), finish(well, 0.0012)]
parts.append(finish(box("cover", -0.0125, 0.0125, -0.052, 0.150, 0.074, 0.084, M_PARK), 0.003, segments=3))
parts.append(finish(box("slot", -0.0172, -0.0164, 0.070, 0.180, 0.050, 0.058, M_BORE), 0.0))  # charging handle track
# Barrel, nut, gas system, front sight, flash hider
parts.append(finish(cyl_y("nut", 0.0150, 0.186, 0.212, 0, ZB, M_PARK, seg=32), 0.0015))
barrel = cyl_y("barrel", 0.0095, 0.19, 0.70, 0, ZB, M_PARK, seg=32)
parts.append(finish(barrel, 0.0005))
parts.append(finish(cyl_y("gastube", 0.0065, 0.19, 0.55, 0, ZB + 0.021, M_PARK, seg=20), 0.0005))
parts.append(finish(box("gasblock", -0.011, 0.011, 0.545, 0.585, ZB - 0.012, ZB + 0.022, M_PARK), 0.0015))
parts.append(finish(cyl_y("regulator", 0.006, 0.585, 0.605, 0, ZB + 0.020, M_PARK, seg=16), 0.0008))
for s in (-1, 1):
    x0, x1 = sorted((s * 0.0045, s * 0.0070))
    parts.append(finish(box("fear", x0, x1, 0.552, 0.572, ZB + 0.022, SIGHT - 0.003, M_PARK), 0.001))  # ears below the post's tip
parts.append(finish(box("fpost", -0.0012, 0.0012, 0.558, 0.566, ZB + 0.022, SIGHT, M_PARK), 0.0003))
hider = cyl_y("hider", 0.0115, 0.700, Y_MUZZLE, 0, ZB, M_PARK, seg=32)
cut = [cyl_y("hbore", 0.0085, 0.70, Y_MUZZLE + 0.01, 0, ZB, M_BORE, seg=24)]
for k in range(6):
    slot = box("hslot", -0.0018, 0.0018, 0.716, Y_MUZZLE + 0.01, ZB, ZB + 0.02, M_BORE)
    cut.append(rotate_about_bore(slot, 30 + 60 * k))
boolean(hider, cut)
parts.append(finish(hider, 0.0008))
# Handguard (wood) with its steel ferrule and vents
section = rounded_rect(0.026, ZB - 0.030, ZB + 0.030, 0.012)
hg = loft_y("handguard", section, [(0.196, 0.92), (0.206, 1.0), (0.488, 1.0), (0.498, 0.94)], M_WOOD)
parts.append(hg)
parts.append(finish(box("ferrule", -0.0255, 0.0255, 0.496, 0.512, ZB - 0.028, ZB + 0.028, M_PARK), 0.004, segments=3))
for s in (-1, 1):
    for y0 in (0.30, 0.36):
        x0, x1 = sorted((s * 0.0255, s * 0.0268))
        parts.append(finish(box("vent", x0, x1, y0, y0 + 0.03, ZB + 0.008, ZB + 0.016, M_BORE), 0.0))
# Rear sight: a thin aperture ring on a short stem (a ghost ring: the eye looks through it
# rather than at it); nothing either side to block the view
parts.append(finish(box("rbase", -0.006, 0.006, -0.066, -0.046, 0.080, 0.086, M_PARK), 0.0015))
parts.append(finish(box("rstem", -0.0012, 0.0012, -0.0570, -0.0550, 0.085, SIGHT - 0.0044, M_PARK), 0.0))
ring = cyl_y("ring", 0.0049, -0.0572, -0.0552, 0, SIGHT, M_PARK, seg=32)
boolean(ring, [cyl_y("peep", 0.0042, -0.07, -0.04, 0, SIGHT, M_BORE, seg=32)])
parts.append(finish(ring, 0.0002))
# Carry handle, up (T48): a post on the left and a wooden grip above the receiver
parts.append(finish(box("hpost", -0.023, -0.018, 0.252, 0.262, 0.060, 0.126, M_PARK), 0.0012))
parts.append(finish(box("harm", -0.023, -0.018, 0.150, 0.262, 0.118, 0.126, M_PARK), 0.0012))
parts.append(finish(cyl_y("hgrip", 0.0105, 0.160, 0.245, -0.0205, 0.134, M_WOOD, seg=20), 0.002, segments=2))
# Trigger guard, magazine release, selector
parts.append(finish(tube_path("guard", [(0.030, -0.012), (0.034, -0.030), (0.022, -0.047), (0.0, -0.047),
                                        (-0.012, -0.030), (-0.010, -0.012)], 0.0028, 1.7, M_PARK), 0.0))
parts.append(finish(box("release", -0.006, 0.006, 0.022, 0.031, -0.032, -0.012, M_PARK), 0.0012))
parts.append(finish(cyl_x("selpivot", 0.0055, -0.0215, -0.017, -0.020, 0.010, M_PARK, seg=16), 0.0006))
parts.append(finish(box("sellever", -0.0225, -0.0195, -0.040, -0.018, 0.007, 0.013, M_PARK), 0.0008))
# Pistol grip and buttstock (wood), buttplate
GRIP = [(0.006, -0.004), (-0.040, -0.004), (-0.050, -0.040), (-0.062, -0.088), (-0.066, -0.110), (-0.056, -0.120),
        (-0.032, -0.118), (-0.026, -0.100), (-0.016, -0.058), (-0.004, -0.022)]
parts.append(loft_x("grip", GRIP, [(-0.016, 0.84), (-0.013, 1.0), (0.013, 1.0), (0.016, 0.84)], M_WOOD))
STOCK = [(-0.070, 0.072), (-0.070, -0.006), (-0.120, -0.022), (-0.200, -0.042), (-0.330, -0.072),
         (-0.470, -0.104), (-0.478, -0.100), (-0.478, 0.062), (-0.470, 0.066), (-0.300, 0.064), (-0.160, 0.062)]
parts.append(loft_x("stock", STOCK, [(-0.0215, 0.88), (-0.018, 1.0), (0.018, 1.0), (0.0215, 0.88)], M_WOOD))
parts.append(finish(prism("buttplate", [(-0.477, -0.101), (-0.490, -0.101), (-0.490, 0.063), (-0.477, 0.063)],
                          -0.0215, 0.0215, M_PARK), 0.004, segments=3))
receiver = join("receiver", parts)

# --- Moving parts -----------------------------------------------------------------------
bolt = finish(box("bolt", -0.012, 0.0155, 0.028, 0.108, 0.036, 0.062, M_BRIGHT), 0.0012)
bolt = join("bolt", [bolt, finish(box("extractor", 0.0150, 0.0158, 0.090, 0.106, 0.046, 0.052, M_PARK), 0.0)])
charger = join("charger", [finish(box("cstem", -0.030, -0.016, 0.165, 0.175, 0.050, 0.058, M_PARK), 0.001),
                           finish(cyl_x("cknob", 0.0062, -0.041, -0.028, 0.170, 0.054, M_PARK, seg=20), 0.0015)])
trigger = finish(prism("trigger", [(0.016, -0.004), (0.009, -0.004), (0.006, -0.018), (0.006, -0.030), (0.009, -0.037),
                                   (0.013, -0.035), (0.012, -0.024), (0.015, -0.012)], -0.004, 0.004, M_PARK),
                 0.0008, segments=2)
mag = [finish(box("mbody", -0.0125, 0.0125, 0.042, 0.111, -0.150, -0.008, M_PARK), 0.0015, segments=2),
       finish(box("mfloor", -0.0142, 0.0142, 0.038, 0.115, -0.158, -0.147, M_PARK), 0.0015, segments=2)]
for s in (-1, 1):
    x0, x1 = sorted((s * 0.0125, s * 0.0136))
    mag.append(finish(box("mrib", x0, x1, 0.055, 0.098, -0.138, -0.036, M_PARK), 0.0006))
mag = join("mag", mag)

# --- 7.62 x 51 round and fired case, at their own origin (rim face, pointing +Y) --------
CASE_LEN = 0.051


def cartridge(name, loaded):
    p = [finish(cyl_y("rim", 0.0060, 0.0, 0.0013, 0, 0, M_BRASS, seg=24), 0.0002),
         finish(cyl_y("groove", 0.0050, 0.0013, 0.0026, 0, 0, M_BRASS, seg=24), 0.0),
         finish(cyl_y("body", 0.0060, 0.0026, 0.040, 0, 0, M_BRASS, seg=24, r1=0.0057), 0.0),
         finish(cyl_y("shoulder", 0.0057, 0.040, 0.044, 0, 0, M_BRASS, seg=24, r1=0.0044), 0.0),
         finish(cyl_y("neck", 0.0044, 0.044, CASE_LEN, 0, 0, M_BRASS, seg=24), 0.0002),
         finish(cyl_y("primer", 0.0022, -0.0001, 0.0003, 0, 0, M_PRIMER, seg=16), 0.0001)]
    if loaded:
        p.append(finish(cyl_y("jacket", 0.0039, CASE_LEN, CASE_LEN + 0.009, 0, 0, M_COPPER, seg=24), 0.0))
        p.append(finish(cyl_y("ogive", 0.0039, CASE_LEN + 0.009, CASE_LEN + 0.021, 0, 0, M_COPPER, seg=24, r1=0.0008), 0.0))
    else:
        p.append(finish(cyl_y("dent", 0.0007, -0.00018, 0.0, 0, 0, M_BORE, seg=10), 0.0))
        p.append(finish(cyl_y("mouth", 0.0040, CASE_LEN - 0.001, CASE_LEN + 0.00005, 0, 0, M_BORE, seg=20), 0.0))
    ob = join(name, p)
    box_uvs(ob, 0.03)
    return ob


cartridge("round", True)
cartridge("case", False)
for ob in (receiver, bolt, charger, trigger, mag):
    box_uvs(ob, 0.25)  # wood grain runs along the gun (u follows y on the sides)

# --- Wood texture: straight-grained, reddish walnut ---------------------------------------
W = 1024
v, u = np.mgrid[0:W, 0:W] / W
warp = fbm(W, 4, 4)
lines = np.sin((v * 140 + warp * 6) * math.pi) * 0.5 + 0.5        # grain lines run across u (along the gun)
fibres = fbm(W, 2, 128)
figure = fbm(W, 3, 6)
dark, light = np.array([0.15, 0.085, 0.050]), np.array([0.33, 0.20, 0.115])
wood = dark + (light - dark) * (0.55 * figure + 0.30 * (1 - lines ** 3) + 0.15 * fibres)[..., None]
save("fal_wood_albedo", wood)
save("fal_wood_rough", np.repeat((0.45 + 0.15 * lines + 0.1 * (fibres - 0.5))[..., None], 3, axis=2))

# --- Export -------------------------------------------------------------------------------
PARTS = ["receiver", "bolt", "charger", "trigger", "mag", "round", "case"]
for ob in scene.objects:
    if ob.type == "MESH" and ob.name not in PARTS:
        log("stray object", ob.name)
glb = os.path.join(OUT_DIR, "fal.glb")
select_only([scene.objects[p] for p in PARTS])
bpy.ops.export_scene.gltf(filepath=glb, export_format="GLB", use_selection=True, export_apply=True,
                          export_yup=True, export_image_format="NONE", export_materials="EXPORT",
                          export_normals=True, export_texcoords=True, export_cameras=False, export_lights=False)
with open(glb, "rb") as fh:
    fh.read(12)
    length, _ = struct.unpack("<II", fh.read(8))
    gltf = json.loads(fh.read(length))


def game(y, z, x=0.0):
    return "%.5f %.5f %.5f" % (x, z, -y)


lines_out = ["# fal.glb meshes in raylib order: mesh <index> <part> <material>"]
index = 0
for mesh in gltf["meshes"]:
    for prim in mesh["primitives"]:
        lines_out.append(f"mesh {index} {mesh['name']} {gltf['materials'][prim['material']]['name']}")
        index += 1
lines_out += [
    "# points in gun space (x right, y up, -z out of the barrel), meters",
    "muzzle " + game(Y_MUZZLE, ZB),
    "front_sight " + game(0.562, SIGHT),
    "rear_sight " + game(-0.056, SIGHT),
    "ejection_port " + game(0.067, 0.049, 0.0170),
    "chamber " + game(0.105, ZB),
    "mag_pivot " + game(*MAG_PIVOT),
    "mag_top " + game(0.076, -0.012),
    "trigger_pivot " + game(*TRIGGER_PIVOT),
    "charger_knob " + game(0.170, 0.054, -0.040),
    "hand_grip " + game(-0.031, -0.050),
    "handguard " + game(0.25, ZB - 0.034),
    "bolt_travel 0.09000",
    "charger_travel 0.10000",
    "case_length %.5f" % CASE_LEN,
]
with open(os.path.join(OUT_DIR, "fal.txt"), "w") as fh:
    fh.write("\n".join(lines_out) + "\n")
log("\n".join(lines_out))
log("wrote", glb, os.path.getsize(glb), "bytes")

# --- Preview renders ------------------------------------------------------------------------
if PREVIEW_DIR:
    os.makedirs(PREVIEW_DIR, exist_ok=True)
    scene.objects["round"].location = (0.06, -0.20, -0.15)
    scene.objects["case"].location = (0.06, -0.12, -0.15)
    world = bpy.data.worlds.new("w")
    scene.world = world
    world.use_nodes = True
    world.node_tree.nodes["Background"].inputs["Color"].default_value = (0.55, 0.62, 0.72, 1)
    world.node_tree.nodes["Background"].inputs["Strength"].default_value = 0.8
    sun = bpy.data.objects.new("sun", bpy.data.lights.new("sun", "SUN"))
    sun.data.energy = 4.0
    sun.rotation_euler = (math.radians(50), math.radians(10), math.radians(30))
    scene.collection.objects.link(sun)
    cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam"))
    scene.collection.objects.link(cam)
    scene.camera = cam
    cam.data.lens = 50
    scene.render.engine = "BLENDER_EEVEE_NEXT"
    scene.render.resolution_x, scene.render.resolution_y = 1400, 600

    def shot(name, pos, target):
        cam.location = pos
        d = Vector(target) - Vector(pos)
        cam.rotation_euler = d.to_track_quat("-Z", "Y").to_euler()
        scene.render.filepath = os.path.join(PREVIEW_DIR, name + ".png")
        bpy.ops.render.render(write_still=True)

    shot("fal_right", (1.6, 0.14, 0.0), (0.0, 0.14, -0.02))
    shot("fal_left_rear", (-0.55, -0.55, 0.30), (0.0, 0.15, 0.0))
