"""
Builds the revolver (assets/models/revolver/) procedurally: a medium-frame,
six-shot, double-action revolver with a swing-out cylinder, made of separate
moving parts so the game can animate it mechanically.

Run with Blender 4.x:
  blender --background --factory-startup --python tools/make_revolver.py -- <output dir> [preview dir]

Writes:
  revolver.glb   one mesh object per part (frame, grip, cylinder, crane, ejector,
                 hammer, trigger, latch) plus loose items (round, case, speedloader)
  revolver.txt   mesh index -> part/material, and the pivots and points the game needs
  steel_albedo.png, steel_rough.png, wood_albedo.png, wood_rough.png, wood_normal.png

Blender space: X right, Y forward (out of the barrel), Z up, meters. Origin =
web of the shooting hand at the top of the grip. The glTF export turns this into
the game's gun space: (x, y, z) -> (x, z, -y), i.e. -Z out of the barrel.
"""
import bpy, bmesh, sys, os, math, json, struct
import numpy as np
from mathutils import Vector, Matrix

args = sys.argv[sys.argv.index("--") + 1:]
OUT_DIR = os.path.abspath(args[0])
PREVIEW_DIR = args[1] if len(args) > 1 else None
os.makedirs(OUT_DIR, exist_ok=True)


def log(*a):
    print("[revolver]", *a)


# --- Dimensions (about 1.15x a real medium-frame .38, so it reads at arm's length) ---
ZB = 0.048            # bore axis height
RC = 0.0135           # chamber circle radius
ZC = ZB - RC          # cylinder axis height
R_CYL = 0.0205        # cylinder radius
Y_CYL0, Y_CYL1 = 0.0263, 0.0712   # cylinder rear / front faces
Y_BARREL0 = 0.0725    # barrel rear (the cylinder gap is between Y_CYL1 and here)
Y_MUZZLE = 0.245
R_BARREL = 0.0098
R_CHAMBER = 0.0052
R_RIM = 0.0062
CRANE_PIVOT = (-0.0090, 0.0125)   # (x, z) of the crane's hinge axis (parallel to the bore)
HAMMER_PIVOT = (-0.002, 0.030)    # (y, z)
TRIGGER_PIVOT = (0.036, 0.004)    # (y, z)
SIGHT_TOP = 0.0665    # top of the front sight = top of the rear sight's ears
RIB_TOP = 0.060
EJECTOR_HEAD = 0.129  # front of the ejector rod's head

# --- Scene -------------------------------------------------------------------------
bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene


def upd():
    bpy.context.view_layer.update()


MATS = {}


def mat(name, color, metallic, rough):
    """A material (the names are what the game keys on); the colours only matter for previews."""
    if name in MATS:
        return MATS[name]
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    b = m.node_tree.nodes["Principled BSDF"]
    b.inputs["Base Color"].default_value = (*color, 1.0)
    b.inputs["Metallic"].default_value = metallic
    b.inputs["Roughness"].default_value = rough
    MATS[name] = m
    return m


M_STEEL = mat("steel", (0.035, 0.040, 0.050), 1.0, 0.30)
M_BRIGHT = mat("bright", (0.55, 0.55, 0.57), 1.0, 0.22)
M_CASE = mat("hardened", (0.20, 0.19, 0.18), 1.0, 0.28)
M_WOOD = mat("wood", (0.10, 0.04, 0.015), 0.0, 0.55)
M_BRASS = mat("brass", (0.62, 0.42, 0.13), 1.0, 0.32)
M_LEAD = mat("lead", (0.22, 0.22, 0.24), 0.0, 0.65)
M_PRIMER = mat("primer", (0.60, 0.60, 0.62), 1.0, 0.30)
M_BORE = mat("bore", (0.01, 0.01, 0.01), 0.0, 0.9)
M_RED = mat("red", (0.60, 0.03, 0.02), 0.0, 0.40)
M_GOLD = mat("gold", (0.70, 0.50, 0.15), 1.0, 0.25)
M_BLACK = mat("black", (0.02, 0.02, 0.022), 0.0, 0.45)


def new_object(name, bm, material):
    me = bpy.data.meshes.new(name)
    bm.to_mesh(me)
    bm.free()
    ob = bpy.data.objects.new(name, me)
    scene.collection.objects.link(ob)
    ob.data.materials.append(material)
    return ob


def select_only(obs, active=None):
    bpy.ops.object.select_all(action="DESELECT")
    for o in obs:
        o.select_set(True)
    bpy.context.view_layer.objects.active = active or obs[0]


def apply_modifiers(ob):
    select_only([ob])
    for m in list(ob.modifiers):
        bpy.ops.object.modifier_apply(modifier=m.name)


# --- Primitive builders ------------------------------------------------------------

def prism(name, pts_yz, x0, x1, material):
    """Extrude a polygon given in the (y, z) plane from x0 to x1."""
    bm = bmesh.new()
    area = sum(pts_yz[i][0] * pts_yz[(i + 1) % len(pts_yz)][1] - pts_yz[(i + 1) % len(pts_yz)][0] * pts_yz[i][1]
               for i in range(len(pts_yz)))
    pts = pts_yz if area > 0 else list(reversed(pts_yz))
    a = [bm.verts.new((x0, y, z)) for y, z in pts]
    b = [bm.verts.new((x1, y, z)) for y, z in pts]
    n = len(pts)
    bm.faces.new(list(reversed(b)))  # +X side (normals fixed below anyway)
    bm.faces.new(a)
    for i in range(n):
        j = (i + 1) % n
        bm.faces.new((a[i], a[j], b[j], b[i]))
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    return new_object(name, bm, material)


def box(name, x0, x1, y0, y1, z0, z1, material):
    return prism(name, [(y0, z0), (y1, z0), (y1, z1), (y0, z1)], x0, x1, material)


def cyl_y(name, r, y0, y1, x, z, material, seg=32, r1=None):
    """A cylinder (or frustum, r -> r1) along +Y from y0 to y1, axis at (x, z)."""
    bm = bmesh.new()
    bmesh.ops.create_cone(bm, cap_ends=True, cap_tris=False, segments=seg, radius1=r,
                          radius2=r if r1 is None else r1, depth=y1 - y0)
    # create_cone runs along Z, centred on the origin: turn it onto +Y
    bmesh.ops.rotate(bm, verts=bm.verts, cent=(0, 0, 0), matrix=Matrix.Rotation(-math.pi / 2, 3, "X"))
    bmesh.ops.translate(bm, verts=bm.verts, vec=(x, (y0 + y1) / 2, z))
    return new_object(name, bm, material)


def cyl_x(name, r, x0, x1, y, z, material, seg=24):
    bm = bmesh.new()
    bmesh.ops.create_cone(bm, cap_ends=True, cap_tris=False, segments=seg, radius1=r, radius2=r, depth=x1 - x0)
    bmesh.ops.rotate(bm, verts=bm.verts, cent=(0, 0, 0), matrix=Matrix.Rotation(math.pi / 2, 3, "Y"))
    bmesh.ops.translate(bm, verts=bm.verts, vec=((x0 + x1) / 2, y, z))
    return new_object(name, bm, material)


def capsule_y(name, r, y0, y1, x, z, material):
    bm = bmesh.new()
    bmesh.ops.create_uvsphere(bm, u_segments=20, v_segments=12, radius=r)
    for v in bm.verts:  # stretch the sphere's middle into a capsule
        v.co.z += (y1 - y0) / 2 * (1 if v.co.z > 0 else -1)
    bmesh.ops.rotate(bm, verts=bm.verts, cent=(0, 0, 0), matrix=Matrix.Rotation(-math.pi / 2, 3, "X"))
    bmesh.ops.translate(bm, verts=bm.verts, vec=(x, (y0 + y1) / 2, z))
    return new_object(name, bm, material)


def tube_path(name, pts_yz, radius, x_scale, material, seg=12):
    """A round bar along a polyline in the (y, z) plane (trigger guard)."""
    cu = bpy.data.curves.new(name, "CURVE")
    cu.dimensions = "3D"
    sp = cu.splines.new("NURBS")
    sp.points.add(len(pts_yz) - 1)
    for p, (y, z) in zip(sp.points, pts_yz):
        p.co = (0.0, y, z, 1.0)
    sp.order_u = 3
    sp.use_endpoint_u = True
    sp.resolution_u = 10
    cu.bevel_depth = radius
    cu.bevel_resolution = seg // 4
    cu.use_fill_caps = True
    ob = bpy.data.objects.new(name, cu)
    scene.collection.objects.link(ob)
    select_only([ob])
    bpy.ops.object.convert(target="MESH")
    ob = bpy.context.view_layer.objects.active
    for v in ob.data.vertices:
        v.co.x *= x_scale
    ob.data.materials.clear()
    ob.data.materials.append(material)
    return ob


def boolean(target, cutters, op="DIFFERENCE"):
    for c in cutters:
        m = target.modifiers.new("bool", "BOOLEAN")
        m.object = c
        m.operation = op
        m.solver = "EXACT"
        m.material_mode = "TRANSFER"
        apply_modifiers(target)
        bpy.data.objects.remove(c, do_unlink=True)


def finish(ob, bevel=0.0006, segments=2, angle=35.0, smooth=True):
    """Bevel the hard edges and give the surface hard-surface normals."""
    if bevel > 0:
        b = ob.modifiers.new("bevel", "BEVEL")
        b.width = bevel
        b.segments = segments
        b.limit_method = "ANGLE"
        b.angle_limit = math.radians(angle)
        b.use_clamp_overlap = True
        b.harden_normals = False
    if smooth:
        for p in ob.data.polygons:
            p.use_smooth = True
        w = ob.modifiers.new("wn", "WEIGHTED_NORMAL")
        w.keep_sharp = True
        w.weight = 50
        w.mode = "FACE_AREA"
    apply_modifiers(ob)
    return ob


def join(name, obs):
    select_only(obs, obs[0])
    bpy.ops.object.join()
    ob = bpy.context.view_layer.objects.active
    ob.name = name
    ob.data.name = name
    return ob


def box_uvs(ob, size=0.08):
    """World-scale box projection: 1 texture repeat per `size` meters."""
    me = ob.data
    uv = me.uv_layers.new(name="UVMap")
    for poly in me.polygons:
        n = poly.normal
        a = (abs(n.x), abs(n.y), abs(n.z))
        for li in poly.loop_indices:
            co = me.vertices[me.loops[li].vertex_index].co
            if a[0] >= a[1] and a[0] >= a[2]:
                u, v = co.y, co.z
            elif a[1] >= a[2]:
                u, v = co.x, co.z
            else:
                u, v = co.y, co.x
            uv.data[li].uv = (u / size, v / size)


# Grip panel texture covers this (y, z) rectangle
GRIP_UV_Y = (-0.060, 0.016)
GRIP_UV_Z = (-0.116, 0.022)


def side_uvs(ob):
    me = ob.data
    uv = me.uv_layers.new(name="UVMap")
    for li, loop in enumerate(me.loops):
        co = me.vertices[loop.vertex_index].co
        u = (co.y - GRIP_UV_Y[0]) / (GRIP_UV_Y[1] - GRIP_UV_Y[0])
        v = (co.z - GRIP_UV_Z[0]) / (GRIP_UV_Z[1] - GRIP_UV_Z[0])
        uv.data[li].uv = (u, v)


# --- Frame (static) ------------------------------------------------------------------
frame_parts = []
FW = 0.0098  # frame half-width

# Lower frame under the cylinder window, running from the grip to the barrel
frame_parts.append(finish(prism("lower", [(-0.012, -0.004), (0.050, -0.004), (0.070, -0.0025), (0.084, 0.0030),
                                          (0.092, 0.0095), (0.092, 0.0125), (-0.012, 0.0125)], -FW, FW, M_STEEL),
                          0.0012, segments=3))
# Rear block: hammer housing, sloping down behind so the hammer spur shows
frame_parts.append(finish(prism("rear", [(-0.013, -0.004), (0.0215, -0.004), (0.0215, 0.0625), (0.010, 0.0625),
                                         (0.004, 0.0600), (-0.002, 0.0550), (-0.007, 0.0480), (-0.011, 0.0380),
                                         (-0.0135, 0.0260), (-0.0145, 0.0120)],
                                -0.0092, 0.0092, M_STEEL), 0.0015, segments=3))
# Recoil shield: the round boss behind the cylinder (covers the chambers' rear)
shield = cyl_y("shield", R_CYL + 0.0004, 0.0200, Y_CYL0 - 0.0003, 0.0, ZC, M_STEEL, seg=48)
frame_parts.append(finish(shield, 0.0007))
frame_parts.append(finish(cyl_y("bushing", 0.0030, Y_CYL0 - 0.0006, Y_CYL0 - 0.0002, 0.0, ZB, M_BRIGHT, seg=20), 0.0002))
# Top strap over the cylinder, and the rear sight's two ears
frame_parts.append(finish(box("strap", -0.0085, 0.0085, 0.016, 0.0800, ZC + R_CYL + 0.0012, 0.0625, M_STEEL),
                          0.0020, segments=4))
for s in (-1, 1):
    x0, x1 = sorted((s * 0.0025, s * 0.0060))
    frame_parts.append(finish(prism("ear", [(0.016, 0.0620), (0.025, 0.0620), (0.025, 0.0640), (0.019, SIGHT_TOP),
                                            (0.016, SIGHT_TOP)], x0, x1, M_STEEL), 0.0003))
# Front of the frame: around the barrel shank, and below it ahead of the crane
frame_parts.append(finish(box("shank", -0.0100, 0.0100, Y_BARREL0, 0.0920, 0.0385, 0.0625, M_STEEL), 0.0008))
frame_parts.append(finish(box("front", -FW, FW, 0.0790, 0.0920, 0.0100, 0.0400, M_STEEL), 0.0008))
# Barrel with a top rib and a half-length underlug for the ejector rod
barrel = cyl_y("barrel", R_BARREL, Y_BARREL0, Y_MUZZLE, 0.0, ZB, M_STEEL, seg=48)
boolean(barrel, [cyl_y("bore", 0.0046, Y_MUZZLE - 0.06, Y_MUZZLE + 0.01, 0.0, ZB, M_BORE, seg=32)])
frame_parts.append(finish(barrel, 0.0009, segments=3))
frame_parts.append(finish(box("rib", -0.0047, 0.0047, 0.0800, Y_MUZZLE - 0.0004, ZB, RIB_TOP, M_STEEL), 0.0007))
lug = join("lug", [cyl_y("lugr", 0.0068, 0.131, Y_MUZZLE - 0.0006, 0.0, ZC + 0.001, M_STEEL, seg=32),
                   box("lugb", -0.0068, 0.0068, 0.131, Y_MUZZLE - 0.0006, ZC + 0.001, ZB, M_STEEL)])
frame_parts.append(finish(lug, 0.0012, segments=3))
# Front sight: a ramp with a red insert
frame_parts.append(finish(prism("fsight", [(0.222, RIB_TOP - 0.001), (0.237, SIGHT_TOP), (0.2425, SIGHT_TOP),
                                           (0.2425, RIB_TOP - 0.001)], -0.0016, 0.0016, M_STEEL), 0.0003))
frame_parts.append(finish(prism("insert", [(0.2275, 0.0618), (0.2340, 0.0653), (0.2350, 0.0653), (0.2350, 0.0618)],
                                -0.0017, 0.0017, M_RED), 0.0))
# Trigger guard
guard = tube_path("guard", [(0.058, -0.002), (0.060, -0.020), (0.050, -0.034), (0.032, -0.038), (0.016, -0.034),
                            (0.008, -0.024), (0.005, -0.010), (0.004, -0.002)], 0.0023, 1.7, M_STEEL)
frame_parts.append(finish(guard, 0.0, smooth=True))
# Side-plate screws (right side) and the latch's rail (left)
for (y, z) in ((0.002, 0.004), (0.046, 0.004), (-0.004, 0.024)):
    frame_parts.append(finish(cyl_x("screw", 0.0021, FW - 0.0004, FW + 0.0005, y, z, M_BRIGHT, seg=20), 0.0003))
frame = join("frame", frame_parts)
box_uvs(frame)

# --- Grip (wood) ---------------------------------------------------------------------
GRIP = [(0.008, -0.003), (0.004, 0.004), (-0.005, 0.008), (-0.013, 0.017), (-0.020, 0.014), (-0.024, 0.000),
        (-0.030, -0.025), (-0.039, -0.055), (-0.048, -0.082), (-0.054, -0.098), (-0.052, -0.106), (-0.038, -0.110),
        (-0.024, -0.108), (-0.018, -0.100), (-0.010, -0.075), (-0.004, -0.052), (0.001, -0.030), (0.005, -0.014)]


def loft_grip():
    cy = sum(p[0] for p in GRIP) / len(GRIP)
    cz = sum(p[1] for p in GRIP) / len(GRIP)
    rings = [(-0.0170, 0.80), (-0.0150, 0.97), (-0.0110, 1.0), (0.0110, 1.0), (0.0150, 0.97), (0.0170, 0.80)]
    bm = bmesh.new()
    loops = []
    for x, s in rings:
        loops.append([bm.verts.new((x, cy + (y - cy) * s, cz + (z - cz) * s)) for y, z in GRIP])
    n = len(GRIP)
    bm.faces.new(loops[0])
    bm.faces.new(list(reversed(loops[-1])))
    for a, b in zip(loops, loops[1:]):
        for i in range(n):
            j = (i + 1) % n
            bm.faces.new((a[i], a[j], b[j], b[i]))
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    ob = new_object("grip", bm, M_WOOD)
    sub = ob.modifiers.new("sub", "SUBSURF")
    sub.levels = 3
    sub.render_levels = 3
    for p in ob.data.polygons:
        p.use_smooth = True
    apply_modifiers(ob)
    return ob


grip = loft_grip()
side_uvs(grip)
# Gold medallions, one on each panel
MEDALLION = (-0.012, -0.014)  # (y, z)
gx = max(v.co.x for v in grip.data.vertices if abs(v.co.y - MEDALLION[0]) < 0.004 and abs(v.co.z - MEDALLION[1]) < 0.004)
for s in (-1, 1):
    x0, x1 = sorted((s * (gx - 0.0010), s * (gx + 0.0004)))
    m = finish(cyl_x("medallion", 0.0042, x0, x1, MEDALLION[0], MEDALLION[1], M_GOLD, seg=32), 0.0003)
    box_uvs(m)
    grip = join("grip", [grip, m])

# --- Cylinder ------------------------------------------------------------------------
def chamber_xz(k, r=RC, offset=0.0):
    a = math.radians(90.0 + 60.0 * k + offset)
    return r * math.cos(a), ZC + r * math.sin(a)


cylinder = cyl_y("cylinder", R_CYL, Y_CYL0, Y_CYL1, 0.0, ZC, M_STEEL, seg=72)
cutters = []
for k in range(6):
    x, z = chamber_xz(k)
    cutters.append(cyl_y("ch", R_CHAMBER, Y_CYL0 - 0.01, Y_CYL1 + 0.01, x, z, M_BORE, seg=28))
    cutters.append(cyl_y("cb", R_RIM, Y_CYL0 - 0.01, Y_CYL0 + 0.0012, x, z, M_STEEL, seg=28))  # rim counterbore
    fx, fz = chamber_xz(k, R_CYL + 0.0012, 30.0)
    cutters.append(capsule_y("flute", 0.0041, 0.0365, 0.0600, fx, fz, M_STEEL))
    # Bolt notch: where the cylinder stop locks each chamber in line with the bore
    nx, nz = chamber_xz(k, R_CYL, 0.0)
    a = math.radians(90.0 + 60.0 * k)
    notch = box("notch", -0.0013, 0.0013, 0.0300, 0.0345, -0.0015, 0.0015, M_STEEL)
    notch.matrix_world = Matrix.Translation((nx, 0, nz)) @ Matrix.Rotation(-(a - math.pi / 2), 4, "Y")
    upd()
    select_only([notch])
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    cutters.append(notch)
cutters.append(cyl_y("star_recess", 0.0106, Y_CYL0 - 0.01, Y_CYL0 + 0.0018, 0.0, ZC, M_STEEL, seg=48))
cutters.append(cyl_y("pin", 0.0029, Y_CYL0 - 0.01, Y_CYL1 + 0.01, 0.0, ZC, M_BORE, seg=24))
boolean(cylinder, cutters)
cylinder = finish(cylinder, 0.0006, segments=2, angle=40)
box_uvs(cylinder)

# Extractor star and ejector rod (one piece: it rotates with the cylinder and is
# pushed back through it to throw out the cases)
star = cyl_y("star", 0.0104, Y_CYL0, Y_CYL0 + 0.0018, 0.0, ZC, M_BRIGHT, seg=48)
boolean(star, [cyl_y("sc", R_CHAMBER - 0.0002, Y_CYL0 - 0.01, Y_CYL0 + 0.01, *chamber_xz(k), M_BRIGHT, seg=28)
               for k in range(6)])
star = finish(star, 0.0002)
rod = finish(cyl_y("rod", 0.0026, Y_CYL0 + 0.001, EJECTOR_HEAD - 0.008, 0.0, ZC, M_STEEL, seg=20), 0.0)
head = finish(cyl_y("head", 0.0034, EJECTOR_HEAD - 0.009, EJECTOR_HEAD, 0.0, ZC, M_STEEL, seg=24), 0.0006, segments=2)
ejector = join("ejector", [star, rod, head])
box_uvs(ejector)

# Crane (yoke): hinges out of the frame to the left carrying the cylinder
px, pz = CRANE_PIVOT
crane = join("crane", [
    finish(box("arm", -FW, FW, Y_CYL1 - 0.0001, 0.0790, pz, ZC + 0.002, M_STEEL), 0.0006),
    finish(cyl_y("tube", 0.0040, Y_CYL1 - 0.0001, 0.0790, 0.0, ZC, M_STEEL, seg=24), 0.0004),
    finish(cyl_y("hinge", 0.0034, Y_CYL1 - 0.0001, 0.0900, px, pz, M_STEEL, seg=20), 0.0004),
])
box_uvs(crane)

# --- Lockwork: hammer, trigger, latch -----------------------------------------------
HAMMER = [(0.0100, 0.0400), (0.0100, 0.0565), (0.0040, 0.0605), (-0.0040, 0.0605), (-0.0120, 0.0610),
          (-0.0190, 0.0625), (-0.0235, 0.0615), (-0.0225, 0.0580), (-0.0160, 0.0560), (-0.0100, 0.0530),
          (-0.0080, 0.0460), (-0.0080, 0.0340), (-0.0040, 0.0260), (0.0040, 0.0260), (0.0090, 0.0320)]
hammer = finish(prism("hammer", HAMMER, -0.0033, 0.0033, M_CASE), 0.0006)
spur = finish(prism("spur", [(-0.0120, 0.0612), (-0.0195, 0.0627), (-0.0237, 0.0616), (-0.0228, 0.0578),
                             (-0.0160, 0.0562), (-0.0115, 0.0560)], -0.0046, 0.0046, M_CASE), 0.0008, segments=2)
hammer = join("hammer", [hammer, spur])
box_uvs(hammer)

TRIGGER = [(0.0405, 0.0070), (0.0330, 0.0070), (0.0300, -0.0030), (0.0285, -0.0130), (0.0292, -0.0215),
           (0.0318, -0.0262), (0.0352, -0.0248), (0.0352, -0.0180), (0.0362, -0.0080), (0.0410, 0.0010)]
trigger = finish(prism("trigger", TRIGGER, -0.0040, 0.0040, M_CASE), 0.0008, segments=3)
box_uvs(trigger)

latch = finish(prism("latch", [(0.0020, 0.0270), (0.0160, 0.0270), (0.0175, 0.0330), (0.0150, 0.0370),
                               (0.0035, 0.0370)], -FW - 0.0026, -FW + 0.0002, M_STEEL), 0.0007, segments=2)
box_uvs(latch)

# --- Loose items, built at their own origin (rim face centre, pointing +Y) ------------
CASE_LEN = 0.0330


def cartridge(name, loaded):
    parts = [finish(cyl_y("rim", R_RIM - 0.0002, 0.0, 0.0012, 0, 0, M_BRASS, seg=28), 0.0002),
             finish(cyl_y("casebody", 0.0049, 0.0012, CASE_LEN, 0, 0, M_BRASS, seg=28), 0.0002),
             finish(cyl_y("primer", 0.0021, -0.0001, 0.0003, 0, 0, M_PRIMER, seg=20), 0.0001)]
    if loaded:  # lead semi-wadcutter
        parts.append(finish(cyl_y("bullet", 0.0046, CASE_LEN, CASE_LEN + 0.0030, 0, 0, M_LEAD, seg=28), 0.0002))
        parts.append(finish(cyl_y("nose", 0.0040, CASE_LEN + 0.0030, CASE_LEN + 0.0080, 0, 0, M_LEAD, seg=28,
                                  r1=0.0026), 0.0003))
    else:       # struck primer and an open mouth
        parts.append(finish(cyl_y("dent", 0.0007, -0.00018, 0.0, 0, 0, M_BORE, seg=12), 0.0))
        parts.append(finish(cyl_y("mouth", 0.0044, CASE_LEN - 0.0010, CASE_LEN + 0.00005, 0, 0, M_BORE, seg=24),
                            0.0))
    ob = join(name, parts)
    box_uvs(ob, 0.03)
    return ob


cartridge("round", True)
cartridge("case", False)

sl_body = finish(cyl_y("slbody", 0.0175, -0.0100, -0.0006, 0, 0, M_BLACK, seg=48), 0.0010, segments=3)
sl_knob = finish(cyl_y("slknob", 0.0072, -0.0270, -0.0100, 0, 0, M_BLACK, seg=24), 0.0008, segments=2)
ridges = [finish(box("ridge", -0.0009, 0.0009, -0.0255, -0.0115, 0.0068, 0.0080, M_BLACK), 0.0002) for _ in range(12)]
for i, r in enumerate(ridges):
    r.matrix_world = Matrix.Rotation(math.radians(30 * i), 4, "Y")
    upd()
    select_only([r])
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
speedloader = join("speedloader", [sl_body, sl_knob] + ridges)
box_uvs(speedloader, 0.04)

# --- Textures ------------------------------------------------------------------------
rng = np.random.default_rng(7)


def fbm(size, octaves, base):
    """Tileable value noise, 0..1."""
    out = np.zeros((size, size))
    amp, total = 1.0, 0.0
    for o in range(octaves):
        cells = base * 2 ** o
        g = rng.random((cells, cells))
        idx = np.arange(size) * cells / size
        i0 = np.floor(idx).astype(int)
        f = idx - i0
        f = f * f * (3 - 2 * f)
        i1 = (i0 + 1) % cells
        a = g[i0][:, i0] * (1 - f)[None, :] + g[i0][:, i1] * f[None, :]
        b = g[i1][:, i0] * (1 - f)[None, :] + g[i1][:, i1] * f[None, :]
        out += amp * (a * (1 - f)[:, None] + b * f[:, None])
        total += amp
        amp *= 0.5
    return out / total


def save(name, rgb):
    """rgb: (h, w, 3) floats 0..1, row 0 = bottom (Blender's convention)."""
    h, w = rgb.shape[:2]
    img = bpy.data.images.new(name, w, h, alpha=False)
    px = np.ones((h, w, 4), dtype=np.float32)
    px[..., :3] = np.clip(rgb, 0, 1)
    img.pixels.foreach_set(px.ravel())
    img.filepath_raw = os.path.join(OUT_DIR, name + ".png")
    img.file_format = "PNG"
    img.save()
    log("wrote", name)
    return img


S = 512
# Steel: neutral (the game tints it), with uneven bluing and fine grain
low = fbm(S, 3, 4)
fine = fbm(S, 2, 64)
steel = 0.62 + 0.10 * (low - 0.5) + 0.06 * (fine - 0.5)
save("steel_albedo", np.repeat(steel[..., None], 3, axis=2))
rough = 0.30 + 0.10 * (low - 0.5) + 0.08 * (fbm(S, 2, 32) - 0.5)
# Fine handling scratches: short straight lines, a little rougher
for _ in range(160):
    x0, y0 = rng.random(2) * S
    ang = rng.normal(0.0, 0.5)
    length = rng.random() * 40 + 8
    for t in np.linspace(0, length, int(length * 2)):
        xi, yi = int(x0 + t * math.cos(ang)) % S, int(y0 + t * math.sin(ang)) % S
        rough[yi, xi] = min(rough[yi, xi] + 0.18, 0.9)
save("steel_rough", np.repeat(rough[..., None], 3, axis=2))

# Walnut grip with a checkered panel
W = 1024
v, u = np.mgrid[0:W, 0:W] / W
Y = GRIP_UV_Y[0] + u * (GRIP_UV_Y[1] - GRIP_UV_Y[0])
Z = GRIP_UV_Z[0] + v * (GRIP_UV_Z[1] - GRIP_UV_Z[0])
# Grain runs down the grip's rake
along = Y - 0.30 * Z
warp = fbm(W, 4, 4)
grain = np.sin(along * 2600 + warp * 14 + fbm(W, 2, 16) * 3) * 0.5 + 0.5
grain = grain ** 2
pores = fbm(W, 1, 256)
figure = fbm(W, 3, 8)
dark = np.array([0.12, 0.075, 0.048])
light = np.array([0.30, 0.195, 0.125])
wood = dark + (light - dark) * (0.70 * figure + 0.30 * (1 - grain) - 0.10 * pores)[..., None]
# Checkered panel: a raked rounded rectangle on each side
centre = -0.0205 + (Z + 0.050) * 0.30
d_side = np.abs(Y - centre) - 0.0105
d_end = np.maximum(-0.096 - Z, Z + 0.026)
dist = np.maximum(d_side, d_end)  # < 0 inside
inside = dist < 0
a = (Y * math.cos(0.9) + Z * math.sin(0.9)) * 1100
b = (Y * math.cos(-0.9 + math.pi) + Z * math.sin(-0.9 + math.pi)) * 1100
tri = lambda t: 1 - 2 * np.abs(t - np.floor(t) - 0.5)
pyr = np.minimum(tri(a), tri(b))                    # pyramids: 1 at the points, 0 in the grooves
border = np.exp(-(dist / 0.0007) ** 2)              # a cut line round the panel
height = np.where(inside, 0.4 + 0.6 * pyr, 1.0) - 0.8 * border
cavity = np.where(inside, 1 - pyr, 0.0)
wood = wood * (1 - 0.45 * cavity[..., None] - 0.5 * border[..., None])
save("wood_albedo", wood)
wr = 0.50 + 0.10 * (1 - grain) + 0.25 * cavity + 0.2 * border
save("wood_rough", np.repeat(np.clip(wr, 0, 1)[..., None], 3, axis=2))
# Normal map (OpenGL: +Y up the texture) from the height field, plus a hint of grain
height = height * 0.6 + grain * 0.04
texel = (GRIP_UV_Y[1] - GRIP_UV_Y[0]) / W
dhx = (np.roll(height, -1, 1) - np.roll(height, 1, 1))
dhy = (np.roll(height, -1, 0) - np.roll(height, 1, 0))
strength = 0.9
n = np.stack([-dhx * strength, -dhy * strength, np.ones_like(height)], axis=2)
n /= np.linalg.norm(n, axis=2, keepdims=True)
save("wood_normal", n * 0.5 + 0.5)

# Preview materials get the textures (the export leaves images out; the game loads the PNGs)
def preview_texture(m, name, input_name, non_color=False):
    nt = m.node_tree
    tex = nt.nodes.new("ShaderNodeTexImage")
    tex.image = bpy.data.images[name]
    if non_color:
        tex.image.colorspace_settings.name = "Non-Color"
    bsdf = nt.nodes["Principled BSDF"]
    if input_name == "Normal":
        nm = nt.nodes.new("ShaderNodeNormalMap")
        nt.links.new(tex.outputs["Color"], nm.inputs["Color"])
        nt.links.new(nm.outputs["Normal"], bsdf.inputs["Normal"])
    elif input_name == "Base Color":
        mix = nt.nodes.new("ShaderNodeMixRGB")
        mix.blend_type = "MULTIPLY"
        mix.inputs["Fac"].default_value = 1.0
        mix.inputs["Color2"].default_value = bsdf.inputs["Base Color"].default_value
        nt.links.new(tex.outputs["Color"], mix.inputs["Color1"])
        nt.links.new(mix.outputs["Color"], bsdf.inputs["Base Color"])
    else:
        nt.links.new(tex.outputs["Color"], bsdf.inputs[input_name])


preview_texture(M_WOOD, "wood_albedo", "Base Color")
M_WOOD.node_tree.nodes["Principled BSDF"].inputs["Base Color"].default_value = (1, 1, 1, 1)
preview_texture(M_WOOD, "wood_normal", "Normal", True)
preview_texture(M_WOOD, "wood_rough", "Roughness", True)

# --- Export --------------------------------------------------------------------------
PARTS = ["frame", "grip", "cylinder", "crane", "ejector", "hammer", "trigger", "latch", "round", "case", "speedloader"]
for ob in scene.objects:
    if ob.type == "MESH" and ob.name not in PARTS:
        log("stray object", ob.name)
glb = os.path.join(OUT_DIR, "revolver.glb")
select_only([scene.objects[p] for p in PARTS])
bpy.ops.export_scene.gltf(filepath=glb, export_format="GLB", use_selection=True, export_apply=True,
                          export_yup=True, export_image_format="NONE", export_materials="EXPORT",
                          export_normals=True, export_texcoords=True, export_cameras=False, export_lights=False)

with open(glb, "rb") as fh:
    fh.read(12)
    length, _ = struct.unpack("<II", fh.read(8))
    gltf = json.loads(fh.read(length))


def game(y, z, x=0.0):
    """Blender (x, y, z) -> game gun space (x, z, -y)."""
    return "%.5f %.5f %.5f" % (x, z, -y)


lines = ["# revolver.glb meshes in raylib order: mesh <index> <part> <material>"]
index = 0
for mesh in gltf["meshes"]:
    for prim in mesh["primitives"]:
        lines.append(f"mesh {index} {mesh['name']} {gltf['materials'][prim['material']]['name']}")
        index += 1
lines += [
    "# points in gun space (x right, y up, -z out of the barrel), meters",
    "cylinder_axis " + game(0.0, ZC),           # the cylinder turns about +z through here
    "chamber_radius %.5f" % RC,
    "cylinder_rear " + game(Y_CYL0, ZC),
    "cylinder_gap " + game((Y_CYL1 + Y_BARREL0) / 2, ZB),
    "crane_pivot " + game(0.0, CRANE_PIVOT[1], CRANE_PIVOT[0]),
    "hammer_pivot " + game(*HAMMER_PIVOT),
    "trigger_pivot " + game(*TRIGGER_PIVOT),
    "muzzle " + game(Y_MUZZLE, ZB),
    "front_sight " + game(0.2405, SIGHT_TOP),
    "rear_notch " + game(0.0205, SIGHT_TOP),
    "case_length %.5f" % CASE_LEN,
]
with open(os.path.join(OUT_DIR, "revolver.txt"), "w") as fh:
    fh.write("\n".join(lines) + "\n")
log("\n".join(lines))
log("wrote", glb, os.path.getsize(glb), "bytes")

# --- Preview renders -----------------------------------------------------------------
if PREVIEW_DIR:
    os.makedirs(PREVIEW_DIR, exist_ok=True)
    for name, loc in (("round", (0.06, -0.06, 0.0)), ("case", (0.075, -0.06, 0.0)), ("speedloader", (0.11, -0.06, 0.0))):
        scene.objects[name].location = loc
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
    scene.render.resolution_x, scene.render.resolution_y = 1200, 700
    scene.view_settings.view_transform = "AgX"

    def shot(name, pos, target):
        cam.location = pos
        d = Vector(target) - Vector(pos)
        cam.rotation_euler = d.to_track_quat("-Z", "Y").to_euler()
        scene.render.filepath = os.path.join(PREVIEW_DIR, name + ".png")
        bpy.ops.render.render(write_still=True)

    shot("rev_right", (0.45, 0.07, 0.01), (0.0, 0.07, 0.0))
    shot("rev_left_rear", (-0.25, -0.20, 0.14), (0.0, 0.07, 0.01))
    # Open the cylinder for a look at the chambers
    q = Matrix.Translation((CRANE_PIVOT[0], 0, CRANE_PIVOT[1])) @ Matrix.Rotation(math.radians(-105), 4, "Y") \
        @ Matrix.Translation((-CRANE_PIVOT[0], 0, -CRANE_PIVOT[1]))
    for name in ("cylinder", "crane", "ejector"):
        scene.objects[name].matrix_world = q
    shot("rev_open", (-0.22, -0.16, 0.16), (-0.02, 0.06, 0.02))
