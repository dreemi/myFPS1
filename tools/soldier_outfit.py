"""
Dresses the "civil man" as a tactical soldier. Used by export_character.py when it's
given "soldier" as a third argument; everything else (skeleton, animations, parts,
grip points) is the same as the plain character, so the game animates both alike.

  blender --background --disable-autoexec Citizen.blend --python tools/export_character.py -- <Citizen textures dir> assets/models/soldier soldier

What it changes, after the textures are set up and before the meshes are split:
  - the jacket and jeans become woodland camouflage fatigues (the seams, pockets and
    folds of the original texture are kept as shading); the striped shirt a dark olive tee
  - the shoes become black boots, the hands black gloves
  - a load-bearing vest (a shell over the jacket's torso) with three magazine pouches
    across the front, a pouch on each side and a radio pouch on the back
All new geometry follows the skeleton (weighted like the jacket underneath).
Blender space: Z up, the character faces -Y, his right side is -X.
"""
import bpy, bmesh
import numpy as np
from mathutils import Vector
from mathutils.bvhtree import BVHTree

ARM_BONES = ("upperarm", "lowerarm", "hand", "index", "middle", "pinky", "ring", "thumb")
HAND_BONES = ("hand", "index", "middle", "pinky", "ring", "thumb")

WOODLAND = np.array([[0.42, 0.41, 0.29],   # light olive
                     [0.24, 0.30, 0.17],   # green
                     [0.33, 0.24, 0.15],   # brown
                     [0.09, 0.09, 0.08]])  # black
TEE = np.array([0.15, 0.16, 0.12])
WEBBING = np.array([0.30, 0.30, 0.20])


def log(*a):
    print("[soldier]", *a)


# --- Images -----------------------------------------------------------------
def get_pixels(img):
    w, h = img.size
    px = np.empty(w * h * 4, dtype=np.float32)
    img.pixels.foreach_get(px)
    return px.reshape(h, w, 4)


def set_pixels(img, px):
    img.pixels.foreach_set(px.astype(np.float32).ravel())
    img.update()


def blur(a, r):
    """Box blur (twice, close to a gaussian) of a 2D array, radius r pixels, wrapping."""
    for _ in range(2):
        for axis in (0, 1):
            c = np.cumsum(np.concatenate([a.take(range(-r - 1, 0), axis=axis), a, a.take(range(0, r), axis=axis)],
                                         axis=axis), axis=axis)
            n = a.shape[axis]
            hi = c.take(range(2 * r + 1, 2 * r + 1 + n), axis=axis)
            lo = c.take(range(0, n), axis=axis)
            a = (hi - lo) / (2 * r + 1)
    return a


def noise(h, w, cell, rng):
    """Smooth value noise, roughly `cell` pixels per blob."""
    g = rng.random((h // cell + 2, w // cell + 2))
    big = np.kron(g, np.ones((cell, cell)))[:h, :w]
    return blur(big, max(1, cell // 2))


def woodland(h, w, seed=7):
    rng = np.random.default_rng(seed)
    out = np.empty((h, w, 3))
    out[:] = WOODLAND[0]
    for k, (cell, level) in enumerate([(h // 30, 0.50), (h // 40, 0.56), (h // 56, 0.62)]):
        n = noise(h, w, cell, rng)
        n = (n - n.min()) / (np.ptp(n) + 1e-6)
        out[n > level] = WOODLAND[k + 1]
    return out


def new_image(name, size, rgb):
    img = bpy.data.images.new(name, size, size)
    px = np.ones((size, size, 4))
    px[..., :3] = rgb
    set_pixels(img, px)
    img.pack()
    return img


def material_image(mat):
    return next(n.image for n in mat.node_tree.nodes if n.type == "TEX_IMAGE")


def textured_material(name, img):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    nt.nodes.clear()
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    bsdf = nt.nodes.new("ShaderNodeBsdfPrincipled")
    tex = nt.nodes.new("ShaderNodeTexImage")
    tex.image = img
    nt.links.new(tex.outputs["Color"], bsdf.inputs["Base Color"])
    nt.links.new(bsdf.outputs["BSDF"], out.inputs["Surface"])
    bsdf.inputs["Roughness"].default_value = 0.85
    return m


def camo_suit(img):
    """Jacket and jeans -> woodland fatigues; the striped shirt -> an olive tee."""
    px = get_pixels(img)
    rgb = px[..., :3].astype(np.float64)
    L = rgb @ np.array([0.299, 0.587, 0.114])
    mask = (L > 0.03).astype(np.float64)
    # Shading: the texture's brightness against its local average (keeps seams, folds)
    local = blur(L * mask, 14) / np.maximum(blur(mask, 14), 1e-3)
    shade = np.clip(L / np.maximum(local, 1e-3), 0.55, 1.45)
    # The shirt's stripes vary a lot over a few pixels; jeans and jacket don't
    m1 = blur(L, 4)
    std = np.sqrt(np.maximum(blur(L * L, 4) - m1 * m1, 0.0))
    shirt = blur(((std / np.maximum(m1, 1e-3)) > 0.32).astype(np.float64), 6) > 0.35
    h, w = L.shape
    base = woodland(h, w)
    base[shirt] = TEE
    shade[shirt] = 1.0 + 0.25 * (shade[shirt] - 1.0)
    out = np.clip(base * shade[..., None], 0, 1) * mask[..., None]
    px[..., :3] = out
    set_pixels(img, px)


def darken(img, target, keep):
    """Desaturate toward `target`, keeping `keep` of the original's shading."""
    px = get_pixels(img)
    L = px[..., :3] @ np.array([0.299, 0.587, 0.114])
    v = target * (1 - keep) + L * keep
    px[..., :3] = v[..., None] * np.array([1.0, 1.0, 0.95])
    set_pixels(img, px)


def webbing_image(size=256):
    rng = np.random.default_rng(3)
    y = np.arange(size)[:, None]
    weave = 1.0 + 0.08 * np.sin(y * 2.4) + 0.05 * (rng.random((size, size)) - 0.5)
    grime = 0.88 + 0.12 * noise(size, size, 32, rng)
    img = new_image("webbing", size, WEBBING)
    px = get_pixels(img)
    px[..., :3] = np.clip(WEBBING * (weave * grime)[..., None], 0, 1)
    set_pixels(img, px)
    return img


def leather_image(size=128):
    rng = np.random.default_rng(5)
    v = 0.07 + 0.03 * noise(size, size, 8, rng) + 0.015 * rng.random((size, size))
    img = new_image("gloves", size, (0.08, 0.08, 0.08))
    px = get_pixels(img)
    px[..., :3] = v[..., None]
    set_pixels(img, px)
    return img


# --- Geometry -----------------------------------------------------------------
def dominant(obj, v):
    best, w = None, -1.0
    for g in v.groups:
        if g.weight > w:
            w, best = g.weight, obj.vertex_groups[g.group].name
    return best or ""


def select_faces(obj, keep):
    """Copy of obj holding only the faces where keep(face, vertex bone names) is true."""
    me = obj.data
    bones = [dominant(obj, v) for v in me.vertices]
    new = obj.copy()
    new.data = me.copy()
    obj.users_collection[0].objects.link(new)
    bm = bmesh.new()
    bm.from_mesh(new.data)
    bm.faces.ensure_lookup_table()
    drop = [f for f in bm.faces if not keep(f, [bones[v.index] for v in f.verts])]
    bmesh.ops.delete(bm, geom=drop, context="FACES")
    bm.to_mesh(new.data)
    bm.free()
    return new


def shell(obj, gap, thickness, z_lo, z_hi):
    """Turn a patch of faces into a solid layer standing `gap` off the surface, cut
    straight across at z_lo and z_hi."""
    arm_mod = [m for m in obj.modifiers if m.type == "ARMATURE"]
    for m in arm_mod:
        obj.modifiers.remove(m)
    me = obj.data
    bm = bmesh.new()
    bm.from_mesh(me)
    for z, below in ((z_lo, True), (z_hi, False)):
        geom = bm.verts[:] + bm.edges[:] + bm.faces[:]
        bmesh.ops.bisect_plane(bm, geom=geom, plane_co=(0, 0, z), plane_no=(0, 0, 1),
                               clear_inner=below, clear_outer=not below)
    bm.to_mesh(me)
    bm.free()
    me.update()
    normals = [v.normal.copy() for v in me.vertices]
    for v, n in zip(me.vertices, normals):
        v.co += n * gap
    sol = obj.modifiers.new("Solidify", "SOLIDIFY")
    sol.thickness = thickness
    sol.offset = 1.0
    sol.use_rim = True
    bpy.context.view_layer.objects.active = obj
    for o in bpy.context.view_layer.objects:
        o.select_set(o == obj)
    bpy.ops.object.modifier_apply(modifier=sol.name)


def set_material(obj, mat):
    obj.data.materials.clear()
    obj.data.materials.append(mat)
    for p in obj.data.polygons:
        p.material_index = 0


def box(name, center, size, bone, mat, arm, coll):
    me = bpy.data.meshes.new(name)
    bm = bmesh.new()
    bm.loops.layers.uv.new()
    bmesh.ops.create_cube(bm, size=1.0, calc_uvs=True)
    for v in bm.verts:
        v.co = Vector((v.co.x * size[0], v.co.y * size[1], v.co.z * size[2])) + center
    bm.to_mesh(me)
    bm.free()
    obj = bpy.data.objects.new(name, me)
    coll.objects.link(obj)
    obj.parent = arm
    g = obj.vertex_groups.new(name=bone)
    g.add(list(range(len(me.vertices))), 1.0, "REPLACE")
    obj.modifiers.new("Armature", "ARMATURE").object = arm
    set_material(obj, mat)
    return obj


def join(objs, name):
    for o in bpy.context.view_layer.objects:
        o.select_set(o in objs)
    bpy.context.view_layer.objects.active = objs[0]
    bpy.ops.object.join()
    objs[0].name = objs[0].data.name = name
    return objs[0]


def dress(arm):
    """Returns the new objects to split into parts, as (object name, label, forced part)."""
    suit = bpy.data.objects["male_casualsuit05Mesh"]
    body = bpy.data.objects["citizen_fbxMesh"]
    coll = suit.users_collection[0]
    bones = arm.data.bones

    camo_suit(material_image(bpy.data.materials["male_casualsuit05"]))
    darken(material_image(bpy.data.materials["shoes03"]), 0.06, 0.35)
    webbing = textured_material("webbing", webbing_image())
    gloves = textured_material("gloves", leather_image())

    # Gloves: the hand faces of the body get their own material
    body.data.materials.append(gloves)
    gi = len(body.data.materials) - 1
    vb = [dominant(body, v) for v in body.data.vertices]
    n = 0
    for p in body.data.polygons:
        if sum(vb[i].startswith(HAND_BONES) for i in p.vertices) * 2 > len(p.vertices):
            p.material_index = gi
            n += 1
    log("glove faces", n)

    pelvis_z = bones["pelvis"].head_local.z
    chest_z = bones["spine_03"].tail_local.z
    log(f"pelvis {pelvis_z:.3f} chest top {chest_z:.3f}")

    def centre(f):
        return sum((v.co for v in f.verts), Vector()) / len(f.verts)

    # Vest: the jacket's torso, from the waist to under the collar, as a padded shell
    vest_lo, vest_hi = pelvis_z + 0.08, chest_z - 0.06
    vest = select_faces(suit, lambda f, b: not any(x.startswith(ARM_BONES) for x in b)
                        and vest_lo - 0.06 < centre(f).z < vest_hi + 0.06)
    vest.name = vest.data.name = "vest"
    shell(vest, 0.012, 0.012, vest_lo, vest_hi)
    set_material(vest, webbing)
    vest.modifiers.new("Armature", "ARMATURE").object = arm
    bpy.context.view_layer.update()

    # Pouches sit where a ray meets the vest, following the bone of the nearest vest vertex
    tree = BVHTree.FromObject(vest, bpy.context.evaluated_depsgraph_get())
    verts = vest.data.vertices

    def pouch(name, origin, direction, size, out):
        hit = tree.ray_cast(origin, direction)[0]
        if hit is None:
            log("missed", name)
            return None
        near = min(verts, key=lambda v: (v.co - hit).length_squared)
        # Sunk 6 mm into the vest so no gap shows
        centre = hit + out * (size[0 if out.x else 1] / 2 - 0.006)
        return box(name, centre, size, dominant(vest, near), webbing, arm, coll)

    front, back = Vector((0, -1, 0)), Vector((0, 1, 0))
    pouches = []
    mag_z = vest_lo + 0.10
    for i, x in enumerate((-0.09, 0.0, 0.09)):   # three magazine pouches across the front
        pouches.append(pouch(f"mag{i}", Vector((x, -1.0, mag_z)), back, (0.078, 0.045, 0.125), front))
    for i, side in enumerate((-1, 1)):           # a pouch low on each side
        out = Vector((side, 0, 0))
        pouches.append(pouch(f"side{i}", Vector((side, 0.02, vest_lo + 0.07)), -out, (0.05, 0.11, 0.10), out))
    pouches.append(pouch("radio", Vector((0.06, 1.0, vest_lo + 0.17)), front, (0.10, 0.05, 0.16), back))
    pouches = [p for p in pouches if p]
    log("pouches", len(pouches))

    made = [(vest.name, "vest", None)]
    if pouches:
        p = join(pouches, "pouches")
        made.append((p.name, "pouches", None))
    return made
