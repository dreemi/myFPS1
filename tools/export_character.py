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
    holding a revolver two-handed) and fp_hold (first-person grip, the revolver's reload, aiming)
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
make_action("rest", 2, lambda ph, f: None)  # arms down, nothing held (for bodies)
make_action("walk", 32, lambda ph, f: (legs(ph, 0.32, 0.10, 0.0, 0.015), aim_pose()))
make_action("run", 20, lambda ph, f: (legs(ph, 0.55, 0.22, 0.04, 0.03), lean(10), aim_pose()))
make_action("crouch", 2, lambda ph, f: (legs(0.0, 0.0, 0.0, CROUCH_DROP, 0.0), lean(15), aim_pose()))
make_action("crouch_walk", 36, lambda ph, f: (legs(ph, 0.22, 0.06, CROUCH_DROP, 0.01), lean(15), aim_pose()))

# First person: the arms only. The game draws them with the right palm on the
# revolver's grip. Frames:
#   0        hip hold
#   1..90    reload, 3 s (see below)
#   91..100  hip hold -> aiming down sights (arms reach forward, elbows out)
# Palm positions match where the viewmodel puts the gun (relative to the eye).
EYE = None
FP_HIP_PALM = Vector((-0.19, -0.464, -0.22))   # 19 cm right, 46 cm ahead, 22 cm down
FP_ADS_PALM = Vector((0.0, -0.404, -0.132))    # centred under the line of sight
FP_FRAMES = 101
RELOAD_FIRST, RELOAD_LAST, RELOAD_SECONDS = 1, 90, 3.0
ADS_FIRST = 91

# The reload. The right hand carries the gun through the handling (the game
# moves the gun with the hand_r bone in these frames) while the left hand thumbs
# the latch, swings the cylinder out, punches the ejector rod, fetches a
# speedloader, loads, spins the cylinder and pushes it shut. Everything is
# placed relative to where the game draws the gun, so these must match
# src/weapon_models.cpp (REVOLVER_MODELS) and src/viewmodel.cpp (RELOAD_KEYS).
GUN_SCALE = 1.3                          # REVOLVER_MODELS.fpScale
GUN_HIP = Vector((0.19, -0.17, -0.48))   # REVOLVER_MODELS.hipOffset (camera space)
GUN_HIP_YAW = -4.0                       # REVOLVER_MODELS.hipYawDegrees
HAND_GRIP = Vector((0.0, -0.05, 0.02))   # REVOLVER_MODELS.handGrip (gun space)
RELOAD_KEYS = [  # seconds, camera-space offset of the gun (m), pitch, yaw, roll (deg) - as viewmodel.cpp
    (0.00, 0.000, 0.000, 0.000, 0.0, 0.0, 0.0),
    (0.18, -0.050, 0.035, 0.050, 6.0, 8.0, -22.0),
    (0.40, -0.070, 0.055, 0.070, 10.0, 12.0, -38.0),
    (0.52, -0.060, 0.065, 0.060, 48.0, 8.0, -30.0),
    (0.64, -0.060, 0.075, 0.070, 56.0, 8.0, -28.0),
    (0.80, -0.060, 0.055, 0.060, 28.0, 9.0, -30.0),
    (1.00, -0.120, 0.050, 0.080, -42.0, 20.0, -30.0),
    (1.42, -0.130, 0.055, 0.090, -48.0, 22.0, -28.0),
    (1.62, -0.130, 0.055, 0.090, -45.0, 22.0, -28.0),
    (1.98, -0.120, 0.050, 0.080, -30.0, 18.0, -32.0),
    (2.28, -0.110, 0.050, 0.080, -8.0, 14.0, -36.0),
    (2.42, -0.070, 0.030, 0.060, 3.0, 8.0, -14.0),
    (2.70, -0.010, 0.005, 0.010, 0.0, 2.0, -2.0),
    (3.00, 0.000, 0.000, 0.000, 0.0, 0.0, 0.0),
]


def reload_curve(t, c):
    """Channel c of RELOAD_KEYS at time t: a smooth curve through the keys (Catmull-Rom tangents)."""
    keys = RELOAD_KEYS
    if t <= keys[0][0]:
        return keys[0][c + 1]
    n = len(keys)
    for i in range(n - 1):
        a, b = keys[i], keys[i + 1]
        if t > b[0]:
            continue
        h = b[0] - a[0]
        s = (t - a[0]) / h

        def tangent(j):
            if j <= 0 or j >= n - 1:
                return 0.0
            return (keys[j + 1][c + 1] - keys[j - 1][c + 1]) / (keys[j + 1][0] - keys[j - 1][0])
        ma, mb = tangent(i) * h, tangent(i + 1) * h
        s2, s3 = s * s, s * s * s
        return (2 * s3 - 3 * s2 + 1) * a[c + 1] + (s3 - 2 * s2 + s) * ma + (-2 * s3 + 3 * s2) * b[c + 1] + (s3 - s2) * mb
    return keys[-1][c + 1]


def gun_in_camera(t):
    """Where the game draws the gun at t seconds into the reload (camera space, no scale)."""
    k = [reload_curve(t, c) for c in range(6)]
    return (Matrix.Translation(GUN_HIP + Vector(k[0:3])) @ Matrix.Rotation(math.radians(GUN_HIP_YAW + k[4]), 4, "Y")
            @ Matrix.Rotation(math.radians(k[3]), 4, "X") @ Matrix.Rotation(math.radians(k[5]), 4, "Z"))


MODEL_TO_BLENDER = Matrix(((1, 0, 0, 0), (0, 0, -1, 0), (0, 1, 0, 0), (0, 0, 0, 1)))  # glTF (Y up) -> Blender (Z up)
CAM_TO_BLENDER = None   # set once the hip hold is known
H0 = L0 = None          # hand_r / hand_l matrices in the hip hold
LEFT_PALM_SUPPORT = LEFT_NORMAL_SUPPORT = None
REV = None              # revolver.txt points


def read_revolver_rig():
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets", "models", "revolver", "revolver.txt")
    rig = {}
    with open(path) as fh:
        for line in fh:
            p = line.split()
            if not p or p[0] in ("#", "mesh"):
                continue
            if len(p) == 4:
                rig[p[0]] = Vector(tuple(float(x) for x in p[1:]))
            elif len(p) == 2:
                rig[p[0]] = float(p[1])
    return rig


def palm_center(side):
    hand, mid = pbones["hand_" + side], pbones["middle_01_" + side]
    return hand.head * 0.45 + mid.head * 0.55 + palm_normal(side) * 0.025


def gun_to_blender(t):
    """Gun space (game units, as the model is authored) at time t -> Blender armature space."""
    return CAM_TO_BLENDER @ gun_in_camera(t) @ Matrix.Scale(GUN_SCALE, 4)


def gun_motion(t):
    """The gun's movement from the hip hold to time t, in Blender armature space."""
    return CAM_TO_BLENDER @ gun_in_camera(t) @ gun_in_camera(0.0).inverted() @ CAM_TO_BLENDER.inverted()


def setup_fp():
    """Pose the hip hold once and work out how game camera space maps onto the armature."""
    global EYE, CAM_TO_BLENDER, H0, L0, LEFT_PALM_SUPPORT, LEFT_NORMAL_SUPPORT, REV
    eyes = bpy.data.objects["head_eyes"]
    EYE = sum((eyes.matrix_world @ v.co for v in eyes.data.vertices), Vector()) / len(eyes.data.vertices)
    REV = read_revolver_rig()
    reset()
    right = EYE + FP_HIP_PALM
    hold_revolver(right, right + Vector((0.035, 0.0, -0.035)))
    g = palm_center("r")
    g0 = Vector((g.x, g.z, -g.y))  # glTF model space
    # model -> camera, as the viewmodel draws the arms: grip point onto the hand-grip point of the gun
    model_to_cam = (gun_in_camera(0.0) @ Matrix.Translation(HAND_GRIP * GUN_SCALE) @ Matrix.Rotation(math.pi, 4, "Y")
                    @ Matrix.Translation(-g0))
    CAM_TO_BLENDER = MODEL_TO_BLENDER @ model_to_cam.inverted()
    H0 = pbones["hand_r"].matrix.copy()
    L0 = pbones["hand_l"].matrix.copy()
    LEFT_PALM_SUPPORT = palm_center("l")
    LEFT_NORMAL_SUPPORT = palm_normal("l")
    reset()


def cylinder_open_axis():
    """The open cylinder's axis point (x, y) in gun space."""
    p, a = REV["crane_pivot"], REV["cylinder_axis"]
    ang = math.radians(100.0)  # RevolverRig::craneOpenDegrees
    v = Vector((a.x - p.x, a.y - p.y))
    return Vector((p.x + v.x * math.cos(ang) - v.y * math.sin(ang), p.y + v.x * math.sin(ang) + v.y * math.cos(ang)))


# Left hand keys: (seconds, space, palm centre, finger direction, palm normal). Space "gun"
# follows the gun (game gun-space units), "cam" is fixed in camera space (off screen),
# "support" is the two-handed hold, moving with the gun.
def left_keys():
    ax = cylinder_open_axis()
    rear = REV["cylinder_rear"].z
    knob = 0.018    # speedloader knob centre behind its rim face (make_revolver.py)
    ready = 0.040   # speedloader held this far behind the cylinder before going in (viewmodel.cpp)
    palm_off = 0.022
    G = "gun"
    return [
        (0.00, "support", None, None, None),
        (0.06, "support", None, None, None),
        (0.16, G, (-0.045, 0.005, -0.010), (0.0, 0.5, -1.0), (1.0, 0.0, 0.0)),    # thumb on the latch
        (0.30, G, (-0.062, 0.000, -0.045), (0.0, 0.6, -1.0), (1.0, 0.5, 0.0)),    # cradling the swung-out cylinder
        (0.46, G, (-0.062, 0.006, -0.062), (0.0, 0.3, -1.0), (1.0, 0.3, 0.0)),
        (0.52, G, (-0.062, 0.006, -0.062), (0.0, 0.3, -1.0), (1.0, 0.3, 0.0)),    # thumb on the ejector rod
        (0.60, G, (-0.060, 0.006, -0.042), (0.0, 0.3, -1.0), (1.0, 0.3, 0.0)),    # rod punched
        (0.70, G, (-0.068, -0.012, -0.040), (0.0, 0.3, -1.0), (1.0, 0.3, 0.0)),
        (0.95, "cam", (-0.05, -0.55, -0.30), (0.0, 1.0, -0.3), (1.0, 0.0, 0.0)),   # down to the belt for a speedloader
        (1.05, "cam", (-0.05, -0.55, -0.30), (0.0, 1.0, -0.3), (1.0, 0.0, 0.0)),
        # Pinching the speedloader's knob from the left, so the cylinder stays in view
        # Held from below by its knob (palm up), so the speedloader and its rounds show above the fingers
        (1.36, "grip", ((ax.x, ax.y, rear + ready + knob), (0.0, -0.034, 0.0)), (0.0, 0.3, -1.0), (0.0, 1.0, 0.0)),  # lined up
        (1.46, "grip", ((ax.x, ax.y, rear + knob), (0.0, -0.034, 0.0)), (0.0, 0.3, -1.0), (0.0, 1.0, 0.0)),          # rounds in
        (1.62, "grip", ((ax.x, ax.y, rear + knob), (0.0, -0.034, 0.0)), (0.5, 0.3, -1.0), (0.2, 1.0, 0.0)),          # twist
        (1.72, "grip", ((ax.x, ax.y, rear + 0.030 + knob), (0.0, -0.034, 0.0)), (0.5, 0.3, -1.0), (0.2, 1.0, 0.0)),  # pull away
        (1.98, "cam", (-0.05, -0.55, -0.30), (0.0, 1.0, -0.3), (1.0, 0.0, 0.0)),
        (2.04, G, (-0.068, -0.014, -0.050), (0.0, 0.2, -1.0), (1.0, 0.4, 0.0)),   # under the cylinder
        (2.12, G, (-0.062, 0.042, -0.050), (0.0, 0.2, -1.0), (1.0, -0.2, 0.0)),   # flick: spin it
        (2.26, G, (-0.066, 0.020, -0.050), (0.0, 0.4, -1.0), (1.0, 0.0, 0.0)),
        (2.40, G, (-0.036, 0.022, -0.050), (0.0, 0.4, -1.0), (1.0, 0.0, 0.0)),    # pushed shut
        (2.62, "support", None, None, None),
        (3.00, "support", None, None, None),
    ]


LEFT_KEYS = None


def left_key_in_blender(key, t):
    """(palm, fingers, normal) of a key, in Blender space at time t."""
    _, space, p, f, n = key
    if space == "support":
        m = gun_motion(t) @ L0
        motion = gun_motion(t)
        palm = motion @ LEFT_PALM_SUPPORT
        fingers = (m.to_3x3() @ Vector((0, 1, 0))).normalized()  # a bone's own +Y runs along it
        normal = (motion.to_3x3() @ LEFT_NORMAL_SUPPORT).normalized()
        return palm, fingers, normal
    if space == "grip":  # a point on the gun plus an offset and a hand turn as seen from the eye
        rot = CAM_TO_BLENDER.to_3x3().normalized()
        at, offset = p
        palm = gun_to_blender(t) @ Vector(at) + rot @ Vector(offset)
        return palm, (rot @ Vector(f)).normalized(), (rot @ Vector(n)).normalized()
    m = gun_to_blender(t) if space == "gun" else CAM_TO_BLENDER
    rot = m.to_3x3().normalized()
    return m @ Vector(p), (rot @ Vector(f)).normalized(), (rot @ Vector(n)).normalized()


def pose_left(t):
    keys = LEFT_KEYS
    i = 0
    while i < len(keys) - 2 and t > keys[i + 1][0]:
        i += 1
    a, b = keys[i], keys[i + 1]
    s = max(0.0, min(1.0, (t - a[0]) / (b[0] - a[0])))
    s = s * s * (3 - 2 * s)
    if a[1] == "support" and b[1] == "support":
        m = gun_motion(t) @ L0
        target = m.translation
        two_bone("upperarm_l", "lowerarm_l", target, Vector((0.6, 0.2, -1.0)))
        pbones["hand_l"].matrix = m
        upd()
        return
    pa, fa, na = left_key_in_blender(a, t)
    pb_, fb, nb = left_key_in_blender(b, t)
    palm = pa.lerp(pb_, s)
    fingers = fa.lerp(fb, s).normalized()
    if na is None:
        na = nb
    if nb is None:
        nb = na
    normal = na.lerp(nb, s).normalized()
    # Reach so the palm centre lands on the target: aim the wrist, then correct by the miss
    target = palm.copy()
    for _ in range(3):
        two_bone("upperarm_l", "lowerarm_l", target, Vector((0.9, 0.1, -1.0)))  # elbow out and down: the forearm comes in from the side
        point("hand_l", fingers)
        roll_palm("l", normal)
        target = target + (palm - palm_center("l"))
    miss = (palm - palm_center("l")).length
    if miss > 0.01:
        log("fp reload t=%.2f: left palm misses by %.0f mm (shoulder %.0f mm away)" % (t, miss * 1000, (palm - pbones["upperarm_l"].head).length * 1000))


FP_LEFT_PALM = {}


def fp_pose(ph, f):
    global LEFT_KEYS
    if EYE is None:
        setup_fp()
        LEFT_KEYS = left_keys()
    if f >= ADS_FIRST:
        aim = (f - ADS_FIRST) / (FP_FRAMES - 1 - ADS_FIRST)
        right = EYE + FP_HIP_PALM.lerp(FP_ADS_PALM, aim)
        hold_revolver(right, right + Vector((0.035, 0.0, -0.035)))
    elif RELOAD_FIRST <= f <= RELOAD_LAST:
        t = (f - RELOAD_FIRST) / (RELOAD_LAST - RELOAD_FIRST) * RELOAD_SECONDS
        right = EYE + FP_HIP_PALM
        hold_revolver(right, right + Vector((0.035, 0.0, -0.035)))
        # The right hand carries the gun: the whole hold moves with it
        m = gun_motion(t) @ H0
        two_bone("upperarm_r", "lowerarm_r", m.translation, Vector((-0.6, 0.2, -1.0)))
        miss_r = (m.translation - pbones["hand_r"].head).length
        if miss_r > 0.01:
            log("fp reload t=%.2f: right wrist misses by %.0f mm" % (t, miss_r * 1000))
        pbones["hand_r"].matrix = m
        upd()
        pose_left(t)
    else:
        right = EYE + FP_HIP_PALM
        hold_revolver(right, right + Vector((0.035, 0.0, -0.035)))
    FP_LEFT_PALM[f] = palm_center("l")


make_action("fp_hold", FP_FRAMES, fp_pose)


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
lines.append("# fp_hold frames: 0 hip, %d..%d reload (%.1f s), %d..%d aim; left palm centre per frame (model space)"
             % (RELOAD_FIRST, RELOAD_LAST, RELOAD_SECONDS, ADS_FIRST, FP_FRAMES - 1))
for f in sorted(FP_LEFT_PALM):
    lines.append("fp_left %d %.4f %.4f %.4f" % ((f,) + to_gltf(FP_LEFT_PALM[f])))
with open(os.path.join(OUT_DIR, "character.txt"), "w") as fh:
    fh.write("\n".join(lines) + "\n")
log("\n".join(lines))
log("wrote", glb, os.path.getsize(glb), "bytes")
