"""
Converts the "civil man" character (Citizen.blend) into assets/models/character/.

Run with Blender 4.x (embedded scripts disabled):
  blender --background --disable-autoexec Citizen.blend --python tools/export_character.py -- <textures dir> <output dir>

What it does:
  - removes the teeth/tongue meshes, camera and lamp
  - scales the character to 1.8 m tall, feet on the ground, transforms applied
  - gives each material its diffuse texture (downscaled to 1024 px)
  - splits the meshes into head / upperarms / forearms / torso / legs parts (by the bone
    each face follows), so the game can hide parts for the first-person view
  - authors simple animations on the skeleton (no IK constraints; a small two-bone
    solver places arms and legs): idle, walk, run, crouch, crouch_walk (upper body
    holding a revolver two-handed) and fp_hold (first-person grip + reload)
  - exports character.glb and character.txt (which mesh is which part, grip points)

Blender space: Z up, the character faces -Y, his right side is -X.
glTF / game space: Y up, faces +Z, right side is -X.  (x, y, z) -> (x, z, -y)
"""
import bpy, bmesh, sys, os, math, json, struct
from mathutils import Vector, Matrix, Quaternion

args = sys.argv[sys.argv.index("--") + 1:]
TEX_DIR, OUT_DIR = args[0], args[1]
HEIGHT = 1.8
TEX_SIZE = 1024
FPS = 24
os.makedirs(OUT_DIR, exist_ok=True)


def log(*a):
    print("[export]", *a)


def upd():
    bpy.context.view_layer.update()


def to_gltf(v):
    return (round(v.x, 4), round(v.z, 4), round(-v.y, 4))


# --- 1. Remove what the game doesn't need ----------------------------------
for name in ["teeth_baseMesh", "tongue01Mesh", "Camera", "Lamp"]:
    o = bpy.data.objects.get(name)
    if o:
        bpy.data.objects.remove(o, do_unlink=True)

arm = bpy.data.objects["Game_engine"]
meshes = [o for o in bpy.data.objects if o.type == "MESH"]


# --- 2. Scale to HEIGHT with feet on the ground, apply transforms ----------
def z_bounds():
    dg = bpy.context.evaluated_depsgraph_get()
    zs = []
    for o in meshes:
        eo = o.evaluated_get(dg)
        me = eo.to_mesh()
        zs += [(eo.matrix_world @ v.co).z for v in me.vertices]
        eo.to_mesh_clear()
    return min(zs), max(zs)


def select_only(objs, active):
    for o in bpy.context.view_layer.objects:
        o.select_set(False)
    for o in objs:
        o.select_set(True)
    bpy.context.view_layer.objects.active = active


def apply_all(objs):
    select_only(objs, objs[0])
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    upd()


# In the file the skeleton's *rest* pose lies on its back; the character only
# stands because of a pose saved on the bones. Make that standing pose the rest
# pose: bake it into the meshes, apply it to the skeleton, re-attach the meshes.
posed = [pb.name for pb in arm.pose.bones if pb.matrix_basis != Matrix.Identity(4)]
log("bones posed in the file:", posed)
for o in meshes:
    mod = next(m for m in o.modifiers if m.type == "ARMATURE")
    select_only([o], o)
    bpy.ops.object.modifier_apply(modifier=mod.name)
select_only([arm], arm)
bpy.ops.object.mode_set(mode="POSE")
bpy.ops.pose.armature_apply(selected=False)
bpy.ops.object.mode_set(mode="OBJECT")
for o in meshes:
    m = o.modifiers.new("Armature", "ARMATURE")
    m.object = arm
upd()

# The meshes are parented through a flipped parent-inverse matrix, so applying
# everything at once doesn't work. Unparent (keeping placement), bake each
# object's transform into its data, scale them together, then parent again.
select_only(meshes, meshes[0])
bpy.ops.object.parent_clear(type="CLEAR_KEEP_TRANSFORM")
apply_all(meshes)
apply_all([arm])
zmin, zmax = z_bounds()
k = HEIGHT / (zmax - zmin)
for o in [arm] + meshes:
    o.scale = (k, k, k)
    o.location = (0, 0, -zmin * k)
apply_all([arm] + meshes)
select_only(meshes + [arm], arm)
bpy.ops.object.parent_set(type="OBJECT", keep_transform=True)
upd()
zmin, zmax = z_bounds()
log(f"height now {zmax - zmin:.3f} m (feet at {zmin:.3f})")
log("pelvis rest head", tuple(round(c, 3) for c in arm.data.bones["pelvis"].head_local))

# --- 3. Materials: diffuse texture each -----------------------------------
TEXTURES = {
    "citizen_fbx": "middleage_lightskinned_male_diffuse2.png",
    "male_casualsuit05": "male_casualsuit05_diffuse.png",
    "shoes03": "shoes03_diffuse.png",
    "low-poly": "brown_eye.png",
}
for mname, fname in TEXTURES.items():
    m = bpy.data.materials[mname]
    m.use_nodes = True
    nt = m.node_tree
    nt.nodes.clear()
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    bsdf = nt.nodes.new("ShaderNodeBsdfPrincipled")
    img = bpy.data.images.load(os.path.join(TEX_DIR, fname))
    if img.size[0] > TEX_SIZE:
        img.scale(TEX_SIZE, TEX_SIZE)
    tex = nt.nodes.new("ShaderNodeTexImage")
    tex.image = img
    nt.links.new(tex.outputs["Color"], bsdf.inputs["Base Color"])
    nt.links.new(bsdf.outputs["BSDF"], out.inputs["Surface"])
    bsdf.inputs["Roughness"].default_value = 0.8
    m.blend_method = "OPAQUE"

# --- 4. Split into parts ------------------------------------------------------
# head      head, neck          (hidden in first person: the camera is in it)
# upperarms upper arms          (hidden in first person: they'd fill the screen)
# forearms  forearms and hands  (drawn with the gun as the first-person arms)
# torso     chest and shoulders (hidden in first person so you can see your legs)
# legs      hips, waist, legs, feet
FOREARM_BONES = ("lowerarm", "hand", "index", "middle", "pinky", "ring", "thumb")
TORSO_BONES = ("spine_02", "spine_03", "clavicle")


def bone_part(bone):
    if bone.startswith("upperarm"):
        return "upperarms"
    if bone.startswith(FOREARM_BONES):
        return "forearms"
    if bone in ("head", "neck_01"):
        return "head"
    if bone.startswith(TORSO_BONES):
        return "torso"
    return "legs"


def split(obj, label, force=None):
    me = obj.data
    vpart = []
    for v in me.vertices:
        best, w = None, -1.0
        for g in v.groups:
            if g.weight > w:
                w, best = g.weight, obj.vertex_groups[g.group].name
        vpart.append(force or (bone_part(best) if best else "legs"))
    fpart = []
    for p in me.polygons:
        votes = {}
        for vi in p.vertices:
            votes[vpart[vi]] = votes.get(vpart[vi], 0) + 1
        fpart.append(max(votes, key=lambda key: (votes[key], key)))
    coll = obj.users_collection[0]
    made = []
    for part in sorted(set(fpart)):
        new = obj.copy()
        new.data = me.copy()
        new.name = new.data.name = f"{part}_{label}"
        coll.objects.link(new)
        bm = bmesh.new()
        bm.from_mesh(new.data)
        bm.faces.ensure_lookup_table()
        bmesh.ops.delete(bm, geom=[f for f in bm.faces if fpart[f.index] != part], context="FACES")
        bm.to_mesh(new.data)
        bm.free()
        made.append(new.name)
    bpy.data.objects.remove(obj, do_unlink=True)
    return made


parts = []
parts += split(bpy.data.objects["citizen_fbxMesh"], "body")
parts += split(bpy.data.objects["male_casualsuit05Mesh"], "suit")
parts += split(bpy.data.objects["shoes03Mesh"], "shoes", force="legs")
parts += split(bpy.data.objects["low-polyMesh"], "eyes", force="head")
log("parts:", parts)

# --- 5. Posing helpers (all positions in armature space == world) ---------
pbones = arm.pose.bones
for pb in pbones:
    pb.rotation_mode = "QUATERNION"
REST = {pb.name: (pb.head.copy(), pb.tail.copy()) for pb in pbones}


def reset():
    for pb in pbones:
        pb.location = (0, 0, 0)
        pb.rotation_quaternion = (1, 0, 0, 0)
        pb.scale = (1, 1, 1)
    upd()


def rotate_about(name, pivot, q):
    pb = pbones[name]
    pb.matrix = Matrix.Translation(pivot) @ q.to_matrix().to_4x4() @ Matrix.Translation(-pivot) @ pb.matrix
    upd()


def point(name, direction):
    """Turn a bone (and its children) so it points along direction."""
    pb = pbones[name]
    v = (pb.tail - pb.head).normalized()
    rotate_about(name, pb.head.copy(), v.rotation_difference(direction.normalized()))


def translate(name, offset):
    pb = pbones[name]
    pb.matrix = Matrix.Translation(offset) @ pb.matrix
    upd()


def two_bone(upper, lower, target, pole):
    """Place a two-bone limb so its end reaches target, bending toward pole."""
    a, b = pbones[upper], pbones[lower]
    S = a.head.copy()
    L1, L2 = (a.tail - a.head).length, (b.tail - b.head).length
    d = target - S
    dist = min(d.length, (L1 + L2) * 0.999)
    dn = d.normalized()
    x = (L1 * L1 - L2 * L2 + dist * dist) / (2 * dist)
    h = math.sqrt(max(L1 * L1 - x * x, 0.0))
    p = pole - dn * pole.dot(dn)
    p.normalize()
    E = S + dn * x + p * h
    point(upper, E - S)
    point(lower, (S + dn * dist) - pbones[lower].head)


def palm_normal(side):
    """Unit vector out of the palm of hand_<side>."""
    hand = pbones["hand_" + side]
    F = (pbones["middle_01_" + side].head - hand.head).normalized()
    L = (pbones["index_01_" + side].head - pbones["pinky_01_" + side].head).normalized()
    n = F.cross(L).normalized()
    thumb = pbones["thumb_01_" + side].head - hand.head
    return n if thumb.dot(n) > 0 else -n


def roll_palm(side, desired):
    """Roll hand_<side> about its own length so its palm faces `desired`."""
    hand = pbones["hand_" + side]
    axis = (hand.tail - hand.head).normalized()
    n = palm_normal(side)
    n = (n - axis * n.dot(axis)).normalized()
    d = (desired - axis * desired.dot(axis)).normalized()
    angle = n.angle(d)
    if axis.dot(n.cross(d)) < 0:
        angle = -angle
    rotate_about("hand_" + side, hand.head.copy(), Quaternion(axis, angle))


def curl_finger(side, finger, angles):
    n = palm_normal(side)
    for j, ang in enumerate(angles, start=1):
        name = f"{finger}_0{j}_{side}"
        pb = pbones[name]
        d = (pb.tail - pb.head).normalized()
        axis = d.cross(n)
        if axis.length < 1e-6:
            continue
        rotate_about(name, pb.head.copy(), Quaternion(axis.normalized(), math.radians(ang)))


def grip_hand(side, trigger_finger=False, tight=False):
    curl = (70, 80, 55) if tight else (55, 70, 45)
    for f in ("middle", "ring", "pinky"):
        curl_finger(side, f, curl)
    curl_finger(side, "index", (20, 35, 25) if trigger_finger else curl)
    curl_finger(side, "thumb", (10, 25, 20))


def shoulder_center():
    return (pbones["upperarm_r"].head + pbones["upperarm_l"].head) / 2


def hold_revolver(right_target, left_target):
    """Both arms forward, right hand on the grip, left hand cupping it."""
    two_bone("upperarm_r", "lowerarm_r", right_target, Vector((-0.6, 0.2, -1.0)))
    two_bone("upperarm_l", "lowerarm_l", left_target, Vector((0.6, 0.2, -1.0)))
    point("hand_r", Vector((0.0, -1.0, -0.55)))   # knuckles forward and down, like a pistol grip
    roll_palm("r", Vector((1.0, 0.0, 0.0)))        # palm against the grip's right side, facing left
    point("hand_l", Vector((-0.35, -1.0, -0.55)))  # support hand wraps in from the left
    roll_palm("l", Vector((-1.0, 0.0, 0.35)))      # palm facing right and a little up, cupping the right hand
    grip_hand("r", trigger_finger=True)
    grip_hand("l", tight=True)   # support fingers wrap over the shooting hand


def aim_pose():
    c = shoulder_center()
    hold_revolver(c + Vector((-0.04, -0.43, -0.05)), c + Vector((0.03, -0.40, -0.09)))


def legs(phase, stride, lift, drop, bob):
    """Feet follow a stride cycle (phase 0..1); hips drop by `drop`."""
    translate("pelvis", Vector((0, 0, -drop - bob * math.cos(4 * math.pi * phase))))
    for side, offset in (("r", 0.0), ("l", 0.5)):
        ph = (phase + offset) % 1.0
        ankle = REST["foot_" + side][0].copy()
        if ph < 0.5:  # foot planted, sliding from front to back
            fwd = stride * (1 - 4 * ph)
            up = 0.0
        else:         # foot swinging forward
            s = (ph - 0.5) * 2
            fwd = stride * (-1 + 2 * s)
            up = lift * math.sin(math.pi * s)
        target = ankle + Vector((0, -fwd, up))
        two_bone("thigh_" + side, "calf_" + side, target, Vector((0, -1.0, 0.3)))
        rest_foot = REST["foot_" + side]
        point("foot_" + side, rest_foot[1] - rest_foot[0])  # keep the foot flat


def lean(degrees):
    rotate_about("spine_01", pbones["spine_01"].head.copy(), Quaternion(Vector((1, 0, 0)), math.radians(degrees)))


# --- 6. Animations -----------------------------------------------------------
arm.animation_data_create()
CROUCH_DROP = 0.55


def make_action(name, frames, pose):
    act = bpy.data.actions.new(name)
    act.use_fake_user = True
    arm.animation_data.action = act
    for f in range(frames):
        reset()
        pose(f / frames, f)
        for pb in pbones:
            pb.keyframe_insert("rotation_quaternion", frame=f)
            pb.keyframe_insert("location", frame=f)
    log(f"action {name}: {frames} frames")
    return act


make_action("idle", 2, lambda ph, f: (aim_pose(),))
make_action("walk", 32, lambda ph, f: (legs(ph, 0.32, 0.10, 0.0, 0.015), aim_pose()))
make_action("run", 20, lambda ph, f: (legs(ph, 0.55, 0.22, 0.04, 0.03), lean(10), aim_pose()))
make_action("crouch", 2, lambda ph, f: (legs(0.0, 0.0, 0.0, CROUCH_DROP, 0.0), lean(15), aim_pose()))
make_action("crouch_walk", 36, lambda ph, f: (legs(ph, 0.22, 0.06, CROUCH_DROP, 0.01), lean(15), aim_pose()))

# First person: the arms only (the game draws them in gun space, with the right
# palm on the grip). Frames:
#   0       hip hold
#   1..30   reload (the support hand drops away and comes back)
#   31..40  hip hold -> aiming down sights (arms reach forward, elbows out)
# Palm positions match where the viewmodel puts the gun (relative to the eye).
EYE = None
FP_HIP_PALM = Vector((-0.19, -0.464, -0.22))   # 19 cm right, 46 cm ahead, 22 cm down
FP_ADS_PALM = Vector((0.0, -0.404, -0.132))    # centred under the line of sight


def fp_pose(ph, f):
    global EYE
    eyes = bpy.data.objects["head_eyes"]
    if EYE is None:
        EYE = sum((eyes.matrix_world @ v.co for v in eyes.data.vertices), Vector()) / len(eyes.data.vertices)
    aim = (f - 31) / 9 if f >= 31 else 0.0
    right = EYE + FP_HIP_PALM.lerp(FP_ADS_PALM, aim)
    left = right + Vector((0.035, 0.0, -0.035))
    if 1 <= f <= 30:
        arc = math.sin(math.pi * (f - 1) / 29)
        left = left + Vector((0.05, 0.15, -0.25)) * arc
    hold_revolver(right, left)


make_action("fp_hold", 41, fp_pose)


# --- 7. Grip points (palm centre of the right hand) ----------------------
def grip_point():
    hand, mid = pbones["hand_r"], pbones["middle_01_r"]
    return hand.head * 0.45 + mid.head * 0.55 + palm_normal("r") * 0.025


def pose_at(action, frame):
    arm.animation_data.action = bpy.data.actions[action]
    bpy.context.scene.frame_set(frame)
    upd()


pose_at("idle", 0)
aim_grip, aim_hand = grip_point(), pbones["hand_r"].head.copy()
pose_at("fp_hold", 0)
fp_grip = grip_point()
reset()
arm.animation_data.action = None

# --- 8. Export ---------------------------------------------------------------
bpy.context.scene.render.fps = FPS
glb = os.path.join(OUT_DIR, "character.glb")
bpy.ops.export_scene.gltf(
    filepath=glb, export_format="GLB", use_selection=False,
    export_apply=False, export_skins=True, export_yup=True,
    export_animations=True, export_animation_mode="ACTIONS", export_force_sampling=True,
    export_image_format="JPEG", export_materials="EXPORT", export_cameras=False, export_lights=False,
)

# Mesh order in the file is the order raylib loads them in (one mesh per primitive)
with open(glb, "rb") as fh:
    fh.read(12)
    length, _ = struct.unpack("<II", fh.read(8))
    gltf = json.loads(fh.read(length))
lines = ["# character.glb parts, in raylib mesh order: <index> <part> <object>"]
index = 0
for mesh in gltf["meshes"]:
    for _ in mesh["primitives"]:
        lines.append(f"mesh {index} {mesh['name'].split('_')[0]} {mesh['name']}")
        index += 1
lines.append("# animations: " + " ".join(a["name"] for a in gltf.get("animations", [])))
lines.append("# right-hand grip point (palm centre), model space (Y up, facing +Z), meters")
lines.append("aim_grip %.4f %.4f %.4f" % to_gltf(aim_grip))
lines.append("aim_hand %.4f %.4f %.4f" % to_gltf(aim_hand))
lines.append("fp_grip %.4f %.4f %.4f" % to_gltf(fp_grip))
lines.append("eye %.4f %.4f %.4f" % to_gltf(EYE))
with open(os.path.join(OUT_DIR, "character.txt"), "w") as fh:
    fh.write("\n".join(lines) + "\n")
log("\n".join(lines))
log("wrote", glb, os.path.getsize(glb), "bytes")
