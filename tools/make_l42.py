"""
Builds the Enfield L42A1 sniper rifle for assets/models/l42/, in separate moving parts so the
game can animate it: receiver (with barrel, stock, cut-down forend and the telescope on its
mount), bolt (turns up on the bore axis, then slides back), trigger and the 10-round
magazine. Its 7.62 rounds and cases are the FAL's.

Run with Blender 4.x:
  blender --background --factory-startup --python tools/make_l42.py -- <output dir> [preview dir]

Writes l42.glb and l42.txt (meshes, pivots and points in game gun space). The wood uses the
FAL's walnut texture ("falwood"), the metal the revolver's neutral steel textures.

Blender space: X right, Y forward (out of the barrel), Z up, meters; origin = web of the
shooting hand at the top of the stock's wrist. Game gun space is (x, z, -y).
"""
import bpy, bmesh, sys, os, math, json, struct
from mathutils import Vector, Matrix

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
args = sys.argv[sys.argv.index("--") + 1:]
OUT_DIR = os.path.abspath(args[0])
PREVIEW_DIR = os.path.abspath(args[1]) if len(args) > 1 else None
os.makedirs(OUT_DIR, exist_ok=True)

bpy.ops.wm.read_factory_settings(use_empty=True)
import procgen as pg
from procgen import (mat, new_object, select_only, apply_modifiers, prism, box, cyl_y, cyl_x, tube_path, boolean,
                     finish, join, box_uvs, upd)
pg.OUT_DIR = OUT_DIR
scene = bpy.context.scene


def log(*a):
    print("[l42]", *a)


# --- Dimensions -----------------------------------------------------------------------
ZB = 0.040            # bore axis height above the wrist's top
SCOPE_Z = 0.100       # the telescope's axis
Y_MUZZLE = 0.800
Y_BUTT = -0.362
EYEPIECE_Y = -0.112   # rear of the eyepiece
OBJECTIVE_Y = 0.178   # front of the objective bell
BOLT_TRAVEL = 0.095
BOLT_LIFT = 62.0      # degrees the handle turns up to unlock
HANDLE_Y = -0.012     # where the handle leaves the bolt
KNOB = (0.048, HANDLE_Y + 0.002, ZB - 0.022)   # (x, y, z) the knob's centre, bolt closed
TRIGGER_PIVOT = (0.032, 0.000)  # (y, z)
MAG_Y0, MAG_Y1, MAG_TOP, MAG_BOTTOM = 0.046, 0.118, 0.016, -0.058

M_STEEL = mat("steel", (0.04, 0.04, 0.045), 1.0, 0.45)
M_BRIGHT = mat("bright", (0.5, 0.5, 0.52), 1.0, 0.3)
M_BLACK = mat("black", (0.02, 0.02, 0.02), 0.0, 0.6)
M_WOOD = mat("falwood", (0.30, 0.13, 0.05), 0.0, 0.5)
M_GLASS = mat("glass", (0.05, 0.08, 0.12), 1.0, 0.05)
M_BORE = mat("bore", (0.01, 0.01, 0.01), 0.0, 0.9)


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


def cyl_z(name, r, z0, z1, x, y, material, seg=24):
    bm = bmesh.new()
    bmesh.ops.create_cone(bm, cap_ends=True, cap_tris=False, segments=seg, radius1=r, radius2=r, depth=z1 - z0)
    bmesh.ops.translate(bm, verts=bm.verts, vec=(x, y, (z0 + z1) / 2))
    return new_object(name, bm, material)


def sphere(name, r, at, material, scale=(1, 1, 1)):
    bm = bmesh.new()
    bmesh.ops.create_uvsphere(bm, u_segments=20, v_segments=12, radius=r)
    for v in bm.verts:
        v.co = Vector((v.co.x * scale[0], v.co.y * scale[1], v.co.z * scale[2])) + Vector(at)
    ob = new_object(name, bm, material)
    for p in ob.data.polygons:
        p.use_smooth = True
    return ob


def tilt_about_y(ob, degrees, x0, z0):
    """Turn an object about the Y axis through (x0, z0) (positive: +x tips down)."""
    ob.matrix_world = (Matrix.Translation((x0, 0, z0)) @ Matrix.Rotation(math.radians(degrees), 4, "Y")
                       @ Matrix.Translation((-x0, 0, -z0)))
    upd()
    select_only([ob])
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    return ob


def ring(name, r_out, r_in, y0, y1, z, material, seg=32):
    ob = cyl_y(name, r_out, y0, y1, 0, z, material, seg=seg)
    boolean(ob, [cyl_y(name + "_in", r_in, y0 - 0.01, y1 + 0.01, 0, z, M_BORE, seg=seg)])
    return ob


# --- Receiver (static: everything that doesn't move) ------------------------------------
parts = []
# The action body, sitting in the stock, open on top and on the right over the magazine
body = loft_y("action", rounded_rect(0.0150, 0.016, 0.058, 0.008), [(0.000, 0.96), (0.006, 1.0), (0.180, 1.0),
                                                                       (0.186, 0.96)], M_STEEL)
boolean(body, [box("opening", -0.0045, 0.03, 0.030, 0.128, ZB - 0.002, 0.08, M_BORE),
               cyl_y("boltway", 0.0092, -0.01, 0.20, 0, ZB, M_BORE, seg=32),
               box("magway", -0.0128, 0.0128, MAG_Y0 - 0.001, MAG_Y1 + 0.001, -0.02, ZB, M_BORE)])
parts.append(finish(body, 0.0010))
# Receiver ring and the barrel (heavy, tapering a little to the muzzle)
parts.append(finish(cyl_y("rring", 0.0150, 0.150, 0.192, 0, ZB, M_STEEL, seg=40), 0.0012))
barrel = cyl_y("barrel", 0.0118, 0.192, Y_MUZZLE, 0, ZB, M_STEEL, seg=40, r1=0.0098)
boolean(barrel, [cyl_y("bore", 0.0039, Y_MUZZLE - 0.03, Y_MUZZLE + 0.01, 0, ZB, M_BORE, seg=20)])
parts.append(finish(barrel, 0.0006))
# Foresight block at the muzzle, with its protector wings
parts.append(finish(box("fsbase", -0.0070, 0.0070, Y_MUZZLE - 0.040, Y_MUZZLE - 0.012, ZB + 0.004, ZB + 0.016,
                        M_STEEL), 0.0012))
for s in (-1, 1):
    x0, x1 = sorted((s * 0.0062, s * 0.0085))
    parts.append(finish(box("fswing", x0, x1, Y_MUZZLE - 0.036, Y_MUZZLE - 0.016, ZB + 0.012, ZB + 0.032, M_STEEL),
                        0.0008))
parts.append(finish(box("fsblade", -0.0011, 0.0011, Y_MUZZLE - 0.030, Y_MUZZLE - 0.022, ZB + 0.012, ZB + 0.028,
                        M_STEEL), 0.0003))
# The bolt's guide behind the receiver (the bolt slides back over the wrist)
parts.append(finish(box("tang", -0.0080, 0.0080, -0.060, 0.004, 0.026, 0.031, M_STEEL), 0.0008))

# --- The stock: butt and wrist (a slight pistol grip), cut-down forend, all one piece -----
STOCK = [(0.420, ZB + 0.006), (0.186, ZB + 0.006), (0.186, 0.034), (-0.030, 0.034), (-0.046, 0.016),
         (-0.100, 0.011), (-0.150, 0.032), (-0.225, 0.048), (-0.350, 0.052), (Y_BUTT + 0.002, 0.050),
         (Y_BUTT, -0.090), (Y_BUTT + 0.008, -0.098), (-0.225, -0.066), (-0.112, -0.046), (-0.078, -0.058),
         (-0.046, -0.056), (-0.020, -0.030), (-0.008, -0.006), (0.088, -0.006), (0.185, 0.001), (0.400, 0.012),
         (0.420, 0.018)]
stock = loft_x("stock", STOCK, [(-0.0205, 0.88), (-0.0175, 1.0), (0.0175, 1.0), (0.0205, 0.88)], M_WOOD)
boolean(stock, [box("well", -0.0136, 0.0136, MAG_Y0 - 0.002, MAG_Y1 + 0.002, -0.03, 0.05, M_BORE),
                cyl_y("channel", 0.0122, 0.18, 0.43, 0, ZB, M_BORE, seg=32)])
parts.append(stock)
# Upper handguard over the barrel, and the band holding both
hg = loft_y("handguard", rounded_rect(0.0170, ZB + 0.002, ZB + 0.021, 0.0095), [(0.196, 0.9), (0.204, 1.0),
                                                                                   (0.392, 1.0), (0.398, 0.92)], M_WOOD)
boolean(hg, [cyl_y("hgchannel", 0.0122, 0.18, 0.42, 0, ZB, M_BORE, seg=32)])
parts.append(hg)
parts.append(finish(loft_y("band", rounded_rect(0.0215, 0.009, ZB + 0.024, 0.012), [(0.398, 1.0), (0.414, 1.0)],
                           M_STEEL, levels=1), 0.0))
# The cheek pad screwed on the comb (a higher face for the telescope), and the butt plate
parts.append(loft_x("cheek", [(-0.305, 0.026), (-0.150, 0.016), (-0.150, 0.052), (-0.185, 0.064), (-0.305, 0.066)],
                    [(-0.0215, 0.9), (-0.019, 1.0), (0.010, 1.0), (0.013, 0.9)], M_WOOD, levels=1))
parts.append(finish(prism("buttplate", [(Y_BUTT + 0.001, -0.092), (Y_BUTT - 0.007, -0.094), (Y_BUTT - 0.007, 0.050),
                                        (Y_BUTT + 0.001, 0.048)], -0.0195, 0.0195, M_STEEL), 0.003, segments=3))
# Trigger guard
parts.append(finish(tube_path("guard", [(0.088, -0.004), (0.084, -0.026), (0.062, -0.044), (0.020, -0.046),
                                        (-0.006, -0.030), (-0.008, -0.004)], 0.0028, 1.8, M_STEEL), 0.0))

# --- The telescope (No. 32 pattern) on its mount on the left of the action ----------------
scope = [
    cyl_y("tube", 0.0125, -0.062, 0.128, 0, SCOPE_Z, M_BLACK, seg=40),
    cyl_y("eyecone", 0.0176, -0.074, -0.060, 0, SCOPE_Z, M_BLACK, seg=40, r1=0.0125),
    cyl_y("eyepiece", 0.0176, EYEPIECE_Y + 0.006, -0.074, 0, SCOPE_Z, M_BLACK, seg=40),
    cyl_y("objcone", 0.0125, 0.126, 0.142, 0, SCOPE_Z, M_BLACK, seg=40, r1=0.0168),
    cyl_y("objective", 0.0168, 0.142, OBJECTIVE_Y, 0, SCOPE_Z, M_BLACK, seg=40),
]
# Eyepiece rim (a rubber ring) and the lenses, set back in their bells
eyerim = ring("eyerim", 0.0184, 0.0150, EYEPIECE_Y, EYEPIECE_Y + 0.008, SCOPE_Z, M_BLACK)
objrim = ring("objrim", 0.0172, 0.0150, OBJECTIVE_Y - 0.004, OBJECTIVE_Y, SCOPE_Z, M_BLACK)
boolean(scope[2], [cyl_y("eyehole", 0.0150, EYEPIECE_Y, EYEPIECE_Y + 0.010, 0, SCOPE_Z, M_BORE, seg=40)])
boolean(scope[4], [cyl_y("objhole", 0.0150, OBJECTIVE_Y - 0.008, OBJECTIVE_Y + 0.01, 0, SCOPE_Z, M_BORE, seg=40)])
scope += [eyerim, objrim]
for ob in scope:
    parts.append(finish(ob, 0.0006))
parts.append(finish(cyl_y("eyelens", 0.0151, EYEPIECE_Y + 0.008, EYEPIECE_Y + 0.010, 0, SCOPE_Z, M_GLASS, seg=40), 0.0))
parts.append(finish(cyl_y("objlens", 0.0151, OBJECTIVE_Y - 0.010, OBJECTIVE_Y - 0.008, 0, SCOPE_Z, M_GLASS, seg=40),
                    0.0))
# The turret saddle, its drums (range on top, deflection on the left) and their caps
parts.append(finish(box("saddle", -0.0150, 0.0150, 0.008, 0.040, SCOPE_Z - 0.006, SCOPE_Z + 0.012, M_BLACK), 0.004,
                    segments=3))
parts.append(finish(cyl_z("rdrum", 0.0128, SCOPE_Z + 0.012, SCOPE_Z + 0.030, 0, 0.024, M_BLACK, seg=32), 0.0012))
parts.append(finish(cyl_z("rcap", 0.0100, SCOPE_Z + 0.030, SCOPE_Z + 0.034, 0, 0.024, M_STEEL, seg=32), 0.0008))
parts.append(finish(cyl_x("ddrum", 0.0122, -0.032, -0.015, 0.024, SCOPE_Z, M_BLACK, seg=32), 0.0012))
parts.append(finish(cyl_x("dcap", 0.0095, -0.036, -0.032, 0.024, SCOPE_Z, M_STEEL, seg=32), 0.0008))
for k in range(12):  # knurling ridges round the range drum
    a = 2 * math.pi * k / 12
    parts.append(cyl_z("knurl", 0.0011, SCOPE_Z + 0.013, SCOPE_Z + 0.029, 0.0130 * math.cos(a),
                       0.024 + 0.0130 * math.sin(a), M_BLACK, seg=6))
# Mount: two rings clamping the tube, on arms reaching down to a bracket on the action's left
for y in (-0.040, 0.100):
    parts.append(finish(ring("mring", 0.0152, 0.0124, y - 0.008, y + 0.008, SCOPE_Z, M_STEEL), 0.0008))
    parts.append(finish(box("marm", -0.026, -0.006, y - 0.007, y + 0.007, 0.052, SCOPE_Z - 0.010, M_STEEL), 0.0015))
parts.append(finish(box("bracket", -0.0215, -0.0150, -0.050, 0.110, 0.030, 0.058, M_STEEL), 0.0015))
for y in (-0.030, 0.035, 0.095):  # its screws
    parts.append(finish(cyl_x("screw", 0.0028, -0.0230, -0.0210, y, 0.044, M_BRIGHT, seg=12), 0.0))

receiver = join("receiver", parts)

# --- Moving parts -----------------------------------------------------------------------
# The bolt, closed: its body in the action, the cocking piece behind, the handle turned down
# on the right. It turns about the bore axis and slides straight back.
bolt = [finish(cyl_y("bbody", 0.0086, -0.034, 0.126, 0, ZB, M_BRIGHT, seg=32), 0.0008),
        finish(cyl_y("bhead", 0.0089, 0.112, 0.128, 0, ZB, M_BRIGHT, seg=32), 0.0006),
        finish(cyl_y("cocking", 0.0078, -0.064, -0.034, 0, ZB, M_STEEL, seg=32), 0.0010),
        finish(box("cockstud", -0.0040, 0.0040, -0.064, -0.040, ZB + 0.004, ZB + 0.014, M_STEEL), 0.0012),
        finish(box("lug", 0.0060, 0.0115, 0.080, 0.104, ZB - 0.003, ZB + 0.003, M_BRIGHT), 0.0006)]
arm = cyl_x("harm", 0.0042, 0.004, 0.046, HANDLE_Y, ZB, M_STEEL, seg=16)
bolt.append(finish(tilt_about_y(arm, 26.0, 0.0, ZB), 0.0006))
bolt.append(finish(cyl_x("hroot", 0.0070, 0.000, 0.012, HANDLE_Y, ZB, M_STEEL, seg=20), 0.0008))
bolt.append(sphere("knob", 0.0092, KNOB, M_STEEL, scale=(1.0, 1.15, 1.0)))
bolt = join("bolt", bolt)
trigger = finish(prism("trigger", [(0.037, -0.002), (0.030, -0.002), (0.027, -0.016), (0.027, -0.029), (0.030, -0.036),
                                   (0.034, -0.034), (0.033, -0.023), (0.036, -0.010)], -0.004, 0.004, M_STEEL),
                 0.0008, segments=2)
mag = [finish(box("mbody", -0.0122, 0.0122, MAG_Y0 + 0.002, MAG_Y1 - 0.002, MAG_BOTTOM + 0.006, MAG_TOP, M_STEEL),
              0.0015, segments=2),
       finish(box("mfloor", -0.0138, 0.0138, MAG_Y0, MAG_Y1, MAG_BOTTOM, MAG_BOTTOM + 0.008, M_STEEL), 0.0015,
              segments=2)]
for s in (-1, 1):
    x0, x1 = sorted((s * 0.0122, s * 0.0132))
    mag.append(finish(box("mrib", x0, x1, MAG_Y0 + 0.012, MAG_Y1 - 0.012, MAG_BOTTOM + 0.012, -0.010, M_STEEL),
                      0.0006))
mag = join("mag", mag)
for ob in (receiver, bolt, trigger, mag):
    box_uvs(ob, 0.25)  # wood grain runs along the gun (u follows y on the sides)

# --- Export -------------------------------------------------------------------------------
PARTS = ["receiver", "bolt", "trigger", "mag"]
for ob in scene.objects:
    if ob.type == "MESH" and ob.name not in PARTS:
        log("stray object", ob.name)
glb = os.path.join(OUT_DIR, "l42.glb")
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


lines_out = ["# l42.glb meshes in raylib order: mesh <index> <part> <material>"]
index = 0
for mesh in gltf["meshes"]:
    for prim in mesh["primitives"]:
        lines_out.append(f"mesh {index} {mesh['name']} {gltf['materials'][prim['material']]['name']}")
        index += 1
lines_out += [
    "# points in gun space (x right, y up, -z out of the barrel), meters",
    "muzzle " + game(Y_MUZZLE, ZB),
    "front_sight " + game(OBJECTIVE_Y, SCOPE_Z),     # the telescope's axis, front and back
    "rear_sight " + game(EYEPIECE_Y, SCOPE_Z),
    "ejection_port " + game(0.080, ZB + 0.010, 0.012),
    "chamber " + game(0.128, ZB),
    "bolt_axis " + game(0.0, ZB),
    "bolt_knob " + game(KNOB[1], KNOB[2], KNOB[0]),
    "mag_top " + game((MAG_Y0 + MAG_Y1) / 2, MAG_TOP),
    "mag_centre " + game((MAG_Y0 + MAG_Y1) / 2, (MAG_TOP + MAG_BOTTOM) / 2),
    "trigger_pivot " + game(*TRIGGER_PIVOT),
    "hand_grip " + game(-0.036, -0.030),
    "handguard " + game(0.235, ZB - 0.036),
    "bolt_travel %.5f" % BOLT_TRAVEL,
    "bolt_lift %.5f" % BOLT_LIFT,
]
with open(os.path.join(OUT_DIR, "l42.txt"), "w") as fh:
    fh.write("\n".join(lines_out) + "\n")
log("\n".join(lines_out))
log("wrote", glb, os.path.getsize(glb), "bytes")

# --- Preview renders ------------------------------------------------------------------------
if PREVIEW_DIR:
    os.makedirs(PREVIEW_DIR, exist_ok=True)
    world = bpy.data.worlds.new("w")
    scene.world = world
    world.use_nodes = True
    world.node_tree.nodes["Background"].inputs["Color"].default_value = (0.9, 0.9, 0.9, 1)
    world.node_tree.nodes["Background"].inputs["Strength"].default_value = 1.0
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

    def shot(name, pos, target, lens=50):
        cam.data.lens = lens
        cam.location = pos
        d = Vector(target) - Vector(pos)
        cam.rotation_euler = d.to_track_quat("-Z", "Y").to_euler()
        scene.render.filepath = os.path.join(PREVIEW_DIR, name + ".png")
        bpy.ops.render.render(write_still=True)

    shot("l42_right", (2.0, 0.22, 0.02), (0.0, 0.22, 0.0))
    shot("l42_action", (0.35, -0.05, 0.22), (0.0, 0.03, 0.04), lens=60)
    shot("l42_left", (-2.0, 0.22, 0.02), (0.0, 0.22, 0.0))
    # Bolt open: turned up and drawn back
    b = scene.objects["bolt"]
    b.matrix_world = (Matrix.Translation((0, -BOLT_TRAVEL, 0)) @ Matrix.Translation((0, 0, ZB))
                      @ Matrix.Rotation(math.radians(-BOLT_LIFT), 4, "Y") @ Matrix.Translation((0, 0, -ZB)))
    shot("l42_open", (0.35, -0.05, 0.22), (0.0, 0.03, 0.04), lens=60)
