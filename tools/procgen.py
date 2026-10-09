"""
Shared helpers for building props procedurally in Blender (used by make_fal.py):
materials, extruded/lofted primitives, booleans, bevelled hard-surface finishing,
box-projected UVs, tileable noise and PNG output. Blender space: X right,
Y forward, Z up, meters. Callers set OUT_DIR before saving textures.
"""
import bpy, bmesh, os, math
import numpy as np
from mathutils import Vector, Matrix


OUT_DIR = "."


def log(*a):
    print("[procgen]", *a)


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




def new_object(name, bm, material):
    me = bpy.data.meshes.new(name)
    bm.to_mesh(me)
    bm.free()
    ob = bpy.data.objects.new(name, me)
    bpy.context.scene.collection.objects.link(ob)
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
    bpy.context.scene.collection.objects.link(ob)
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


def side_uvs(ob, GRIP_UV_Y, GRIP_UV_Z):
    me = ob.data
    uv = me.uv_layers.new(name="UVMap")
    for li, loop in enumerate(me.loops):
        co = me.vertices[loop.vertex_index].co
        u = (co.y - GRIP_UV_Y[0]) / (GRIP_UV_Y[1] - GRIP_UV_Y[0])
        v = (co.z - GRIP_UV_Z[0]) / (GRIP_UV_Z[1] - GRIP_UV_Z[0])
        uv.data[li].uv = (u, v)




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


