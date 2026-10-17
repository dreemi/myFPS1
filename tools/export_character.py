"""
Converts the "civil man" character (Citizen.blend) into assets/models/character/.

Run with Blender 4.x (embedded scripts disabled):
  blender --background --disable-autoexec Citizen.blend --python tools/export_character.py -- <textures dir> <output dir> [soldier]

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
SOLDIER = len(args) > 2 and args[2] == "soldier"  # dressed as a soldier (tools/soldier_outfit.py)
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
if SOLDIER:
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import soldier_outfit
    for name, label, force in soldier_outfit.dress(arm):
        parts += split(bpy.data.objects[name], label, force=force)
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


ACTION = ""


def make_action(name, frames, pose):
    global ACTION
    ACTION = name
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
# gun's grip, and while reloading moves the gun with the hand_r bone, so the
# right hand carries it through the handling while the left hand works it.
# Everything is placed relative to where the game draws the gun, so the numbers
# here must match src/weapon_models.cpp (REVOLVER_MODELS, FAL_MODELS),
# src/viewmodel.cpp and src/rifle_view.cpp (the gun-pose keys).
#
# fp_hold (revolver):  0 hip, 1..90 reload (3 s), 91..100 hip -> aiming
# fp_rifle (FAL):      0 hip, 1..60 magazine swap (0..2 s), 61..78 finishing a
#                      reload with a round chambered (2..2.6 s), 79..120 finishing
#                      from empty: charging handle (2..3.4 s), 121..130 aiming
# fp_sniper (L42A1):   0 hip, 1..36 working the bolt, 37..136 reload, 137..146 aiming
FP_HIP_PALM = Vector((-0.19, -0.464, -0.22))   # revolver: 19 cm right, 46 cm ahead, 22 cm down
FP_ADS_PALM = Vector((0.0, -0.404, -0.132))    # revolver: centred under the line of sight
MODEL_TO_BLENDER = Matrix(((1, 0, 0, 0), (0, 0, -1, 0), (0, 1, 0, 0), (0, 0, 0, 1)))  # glTF (Y up) -> Blender (Z up)
EYE = None


def find_eye():
    global EYE
    if EYE is None:
        eyes = bpy.data.objects["head_eyes"]
        EYE = sum((eyes.matrix_world @ v.co for v in eyes.data.vertices), Vector()) / len(eyes.data.vertices)
    return EYE


def read_gun_txt(name):
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets", "models", name, name + ".txt")
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


def pose_curve(keys, t, c):
    """Channel c (x y z pitch yaw roll) of pose keys at time t: a smooth curve (Catmull-Rom tangents)."""
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


def palm_center(side):
    hand, mid = pbones["hand_" + side], pbones["middle_01_" + side]
    return hand.head * 0.45 + mid.head * 0.55 + palm_normal(side) * 0.025


class FpRig:
    """Where the game draws one gun, and how camera space maps onto the armature."""

    def __init__(self, scale, hip, hip_yaw, hand_grip, ads_offset):
        self.scale, self.hip, self.hip_yaw = scale, Vector(hip), hip_yaw
        self.hand_grip, self.ads_offset = Vector(hand_grip), Vector(ads_offset)
        self.cam_to_blender = None
        self.H0 = self.L0 = None
        self.left_palm_support = self.left_normal_support = None

    def gun_in_camera(self, keys=None, t=0.0, aim=0.0):
        """Where the game draws the gun (camera space, no scale): at `aim`, plus reload keys at time t."""
        k = [pose_curve(keys, t, c) for c in range(6)] if keys else [0.0] * 6
        a = aim * aim * (3 - 2 * aim)
        arc = math.sin(math.pi * aim)  # the slight dip and cant mid-way, as the viewmodel does
        pos = self.hip.lerp(self.ads_offset, a) + Vector(k[0:3]) + Vector((0.0, -0.012 * arc, 0.0))
        yaw = self.hip_yaw * (1 - a) + k[4]
        roll = k[5] - 4.0 * arc
        return (Matrix.Translation(pos) @ Matrix.Rotation(math.radians(yaw), 4, "Y")
                @ Matrix.Rotation(math.radians(k[3]), 4, "X") @ Matrix.Rotation(math.radians(roll), 4, "Z"))

    def calibrate(self):
        """With the right hand posed for the hip hold: map camera space onto the armature."""
        g = palm_center("r")
        g0 = Vector((g.x, g.z, -g.y))  # glTF model space
        # model -> camera, as the viewmodel draws the arms: grip point onto the hand-grip point of the gun
        model_to_cam = (self.gun_in_camera() @ Matrix.Translation(self.hand_grip * self.scale)
                        @ Matrix.Rotation(math.pi, 4, "Y") @ Matrix.Translation(-g0))
        self.cam_to_blender = MODEL_TO_BLENDER @ model_to_cam.inverted()
        self.H0 = pbones["hand_r"].matrix.copy()

    def remember_support(self):
        self.L0 = pbones["hand_l"].matrix.copy()
        self.left_palm_support = palm_center("l")
        self.left_normal_support = palm_normal("l")

    def gun_to_blender(self, cam):
        """Gun space (game units, as the model is authored), with the gun at `cam` -> Blender armature space."""
        return self.cam_to_blender @ cam @ Matrix.Scale(self.scale, 4)

    def motion(self, cam):
        """The gun's movement from the hip hold to `cam`, in Blender armature space."""
        return self.cam_to_blender @ cam @ self.gun_in_camera().inverted() @ self.cam_to_blender.inverted()

    def key_in_blender(self, key, cam):
        """(palm, fingers, normal) of a left-hand key, in Blender space, with the gun at `cam`."""
        _, space, p, f, n = key
        if space == "support":
            motion = self.motion(cam)
            palm = motion @ self.left_palm_support
            fingers = ((motion @ self.L0).to_3x3() @ Vector((0, 1, 0))).normalized()  # a bone's own +Y runs along it
            normal = (motion.to_3x3() @ self.left_normal_support).normalized()
            return palm, fingers, normal
        rot = self.cam_to_blender.to_3x3().normalized()
        if space == "grip":  # a point on the gun plus an offset and a hand turn as seen from the eye
            at, offset = p
            return self.gun_to_blender(cam) @ Vector(at) + rot @ Vector(offset), (rot @ Vector(f)).normalized(), \
                (rot @ Vector(n)).normalized()
        m = self.gun_to_blender(cam) if space == "gun" else self.cam_to_blender
        mrot = m.to_3x3().normalized()
        return m @ Vector(p), (mrot @ Vector(f)).normalized(), (mrot @ Vector(n)).normalized()

    def carry(self, cam, pole=Vector((-0.6, 0.2, -1.0))):
        """The right hand carries the gun from the hip hold to `cam`: the whole hold moves with it."""
        m = self.motion(cam) @ self.H0
        two_bone("upperarm_r", "lowerarm_r", m.translation, pole)
        miss = (m.translation - pbones["hand_r"].head).length
        if miss > 0.01:
            log("fp: right wrist misses by %.0f mm" % (miss * 1000))
        pbones["hand_r"].matrix = m
        upd()


def place_left(palm, fingers, normal, pole=Vector((0.9, 0.1, -1.0))):
    """Reach so the left palm centre lands on `palm`: aim the wrist, then correct by the miss."""
    target = palm.copy()
    for _ in range(3):
        two_bone("upperarm_l", "lowerarm_l", target, pole)
        point("hand_l", fingers)
        roll_palm("l", normal)
        target = target + (palm - palm_center("l"))
    return (palm - palm_center("l")).length


def pose_left_keys(rig, keys, t, cam):
    i = 0
    while i < len(keys) - 2 and t > keys[i + 1][0]:
        i += 1
    a, b = keys[i], keys[i + 1]
    s = max(0.0, min(1.0, (t - a[0]) / (b[0] - a[0])))
    s = s * s * (3 - 2 * s)
    if a[1] == "support" and b[1] == "support":
        m = rig.motion(cam) @ rig.L0
        two_bone("upperarm_l", "lowerarm_l", m.translation, Vector((0.6, 0.2, -1.0)))
        pbones["hand_l"].matrix = m
        upd()
        return
    pa, fa, na = rig.key_in_blender(a, cam)
    pb_, fb, nb = rig.key_in_blender(b, cam)
    miss = place_left(pa.lerp(pb_, s), fa.lerp(fb, s).normalized(), na.lerp(nb, s).normalized())
    if miss > 0.01:
        log("%s t=%.2f: left palm misses by %.0f mm" % (ACTION, t, miss * 1000))


# --- The revolver ---------------------------------------------------------------------
REV = read_gun_txt("revolver")
REV_RIG = FpRig(1.3, (0.19, -0.17, -0.48), -4.0, (0.0, -0.05, 0.02), (0.0, -0.0665 * 1.3, -0.42))
REV_RELOAD_FIRST, REV_RELOAD_LAST, REV_RELOAD_SECONDS, REV_ADS_FIRST, REV_FRAMES = 1, 90, 3.0, 91, 101
REV_KEYS = [  # seconds, camera-space offset of the gun (m), pitch, yaw, roll (deg) - as viewmodel.cpp
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


def cylinder_open_axis():
    """The open cylinder's axis point (x, y) in gun space."""
    p, a = REV["crane_pivot"], REV["cylinder_axis"]
    ang = math.radians(100.0)  # RevolverRig::craneOpenDegrees
    v = Vector((a.x - p.x, a.y - p.y))
    return Vector((p.x + v.x * math.cos(ang) - v.y * math.sin(ang), p.y + v.x * math.sin(ang) + v.y * math.cos(ang)))


# Left hand keys: (seconds, space, palm centre, finger direction, palm normal). Space "gun"
# follows the gun (game gun-space units), "cam" is fixed in camera space (off screen),
# "grip" is a point on the gun with an offset and hand turn as seen from the eye,
# "support" is the two-handed hold, moving with the gun.
def revolver_left_keys():
    ax = cylinder_open_axis()
    rear = REV["cylinder_rear"].z
    knob = 0.018    # speedloader knob centre behind its rim face (make_revolver.py)
    ready = 0.040   # speedloader held this far behind the cylinder before going in (viewmodel.cpp)
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


REV_LEFT = None
FP_LEFT_PALM = {}


def fp_hold_pose(ph, f):
    global REV_LEFT
    eye = find_eye()
    if REV_RIG.cam_to_blender is None:
        reset()
        right = eye + FP_HIP_PALM
        hold_revolver(right, right + Vector((0.035, 0.0, -0.035)))
        REV_RIG.calibrate()
        REV_RIG.remember_support()
        REV_LEFT = revolver_left_keys()
        reset()
    if f >= REV_ADS_FIRST:
        aim = (f - REV_ADS_FIRST) / (REV_FRAMES - 1 - REV_ADS_FIRST)
        right = eye + FP_HIP_PALM.lerp(FP_ADS_PALM, aim)
        hold_revolver(right, right + Vector((0.035, 0.0, -0.035)))
    elif REV_RELOAD_FIRST <= f <= REV_RELOAD_LAST:
        t = (f - REV_RELOAD_FIRST) / (REV_RELOAD_LAST - REV_RELOAD_FIRST) * REV_RELOAD_SECONDS
        right = eye + FP_HIP_PALM
        hold_revolver(right, right + Vector((0.035, 0.0, -0.035)))
        cam = REV_RIG.gun_in_camera(REV_KEYS, t)
        REV_RIG.carry(cam)
        pose_left_keys(REV_RIG, REV_LEFT, t, cam)
    else:
        right = eye + FP_HIP_PALM
        hold_revolver(right, right + Vector((0.035, 0.0, -0.035)))
    FP_LEFT_PALM[f] = palm_center("l")


make_action("fp_hold", REV_FRAMES, fp_hold_pose)


# --- The FAL ------------------------------------------------------------------------------
FAL = read_gun_txt("fal")
FAL_RIG = FpRig(1.0, (0.10, -0.19, -0.10), -2.5, tuple(FAL["hand_grip"]),
                (0.0, -FAL["front_sight"].y, -0.19))
FAL_FRAMES = 131
SWAP_FIRST, SWAP_LAST, SWAP_END = 1, 60, 2.0
TAC_FIRST, TAC_LAST, TAC_END = 61, 78, 2.6
EMPTY_FIRST, EMPTY_LAST, EMPTY_END = 79, 120, 3.4
FAL_ADS_FIRST = 121
# Gun pose keys - as rifle_view.cpp (SWAP_KEYS, then TACTICAL_KEYS or EMPTY_KEYS)
FAL_SWAP = [
    (0.00, 0.000, 0.000, 0.000, 0.0, 0.0, 0.0),
    (0.25, -0.060, 0.070, -0.060, 4.0, 28.0, -35.0),
    (0.60, -0.070, 0.090, -0.080, 6.0, 32.0, -40.0),
    (1.00, -0.070, 0.100, -0.080, 6.0, 32.0, -42.0),
    (1.60, -0.070, 0.100, -0.080, 6.0, 32.0, -40.0),
    (1.74, -0.070, 0.110, -0.080, 9.0, 32.0, -38.0),
    (2.00, -0.050, 0.060, -0.050, 4.0, 20.0, -28.0),
]
FAL_TACTICAL = [
    (2.00, -0.050, 0.060, -0.050, 4.0, 20.0, -28.0),
    (2.60, 0.000, 0.000, 0.000, 0.0, 0.0, 0.0),
]
FAL_EMPTY = [
    (2.00, -0.050, 0.060, -0.050, 4.0, 20.0, -28.0),
    (2.30, -0.070, 0.070, -0.070, 4.0, 30.0, 5.0),
    (2.65, -0.070, 0.075, -0.070, 4.0, 30.0, 8.0),
    (2.90, -0.070, 0.075, -0.060, 4.0, 30.0, 8.0),
    (2.97, -0.070, 0.080, -0.060, 7.0, 30.0, 6.0),
    (3.40, 0.000, 0.000, 0.000, 0.0, 0.0, 0.0),
]
# The support hand under the handguard: palm up, fingers wrapping round its right side
HG = FAL["handguard"]
FAL_SUPPORT = ((HG.x - 0.004, HG.y - 0.021, HG.z), (1.0, 0.5, -0.2), (0.0, 1.0, 0.0))
PALM_ON_MAG = (-0.032, -0.075, -0.078)   # as rifle_view.cpp: the palm on the magazine's left side
MAG_FINGERS, MAG_NORMAL = (0.3, -0.4, -1.0), (1.0, 0.0, 0.0)
POUCH = ("cam", (-0.05, -0.55, -0.30), (0.0, 1.0, -0.3), (1.0, 0.0, 0.0))
KNOB = FAL["charger_knob"]


def fal_left_keys(empty):
    G = "gun"
    swap = [
        (0.00, "support", None, None, None),
        (0.08, "support", None, None, None),
        (0.30, G, PALM_ON_MAG, MAG_FINGERS, MAG_NORMAL),                                # thumb on the release
        (0.45, G, (-0.032, -0.090, -0.095), MAG_FINGERS, MAG_NORMAL),                   # rocked forward off the hook
        (0.60, G, (-0.032, -0.190, -0.090), MAG_FINGERS, MAG_NORMAL),                   # pulled down, let go
        (0.80,) + POUCH,                                                                 # a fresh one from the pouch
        (1.05,) + POUCH,
        (1.30, G, (-0.032, -0.170, -0.095), MAG_FINGERS, MAG_NORMAL),                   # up under the well
        (1.42, G, (-0.032, -0.092, -0.095), MAG_FINGERS, MAG_NORMAL),                   # front hooked in
        (1.60, G, PALM_ON_MAG, MAG_FINGERS, MAG_NORMAL),                                # rocked home
        (1.66, G, (-0.010, -0.165, -0.076), (0.0, 0.0, -1.0), (0.0, 1.0, 0.0)),         # under the floorplate
        (1.74, G, (-0.010, -0.150, -0.076), (0.0, 0.0, -1.0), (0.0, 1.0, 0.0)),         # slap
        (2.00, G, (-0.020, -0.120, -0.120), (0.5, 0.2, -1.0), (0.3, 1.0, 0.0)),
    ]
    if not empty:
        return swap + [(2.45, "support", None, None, None), (2.60, "support", None, None, None)]
    return swap + [
        (2.30, G, (KNOB.x - 0.018, KNOB.y + 0.016, KNOB.z), (0.4, -0.6, -0.3), (1.0, -0.5, 0.0)),        # over the handle
        (2.62, G, (KNOB.x - 0.018, KNOB.y + 0.016, KNOB.z), (0.4, -0.6, -0.3), (1.0, -0.5, 0.0)),        # gripped
        (2.85, G, (KNOB.x - 0.018, KNOB.y + 0.016, KNOB.z + 0.100), (0.4, -0.6, -0.3), (1.0, -0.5, 0.0)),  # pulled back
        (2.95, G, (KNOB.x - 0.035, KNOB.y + 0.030, KNOB.z + 0.110), (0.4, -0.4, -0.3), (1.0, -0.3, 0.0)),  # let go
        (3.25, "support", None, None, None),
        (3.40, "support", None, None, None),
    ]


FAL_LEFT_TAC = FAL_LEFT_EMPTY = None
FP_RIFLE_LEFT = {}


def hold_rifle_right(cam):
    """The right hand on the pistol grip, with the rifle at `cam`."""
    palm = FAL_RIG.cam_to_blender @ cam @ (FAL_RIG.hand_grip * FAL_RIG.scale)
    wrist = palm + (FAL_RIG.H0.translation - FAL_RIG_PALM0)
    two_bone("upperarm_r", "lowerarm_r", wrist, Vector((-0.6, 0.2, -1.0)))
    point("hand_r", Vector((0.0, -1.0, -0.55)))
    roll_palm("r", Vector((1.0, 0.0, 0.0)))
    grip_hand("r", trigger_finger=True)


FAL_RIG_PALM0 = None


def fp_rifle_pose(ph, f):
    global FAL_LEFT_TAC, FAL_LEFT_EMPTY, FAL_RIG_PALM0
    eye = find_eye()
    if FAL_RIG.cam_to_blender is None:
        # Calibrate from a right-hand hold at roughly where the grip sits, then put the
        # support hand under the handguard and remember that hold
        reset()
        guess = FAL_RIG.gun_in_camera() @ (FAL_RIG.hand_grip * FAL_RIG.scale)
        wrist = eye + Vector((-guess.x, guess.z, guess.y)) + Vector((0.0, 0.03, 0.04))
        two_bone("upperarm_r", "lowerarm_r", wrist, Vector((-0.6, 0.2, -1.0)))
        point("hand_r", Vector((0.0, -1.0, -0.55)))
        roll_palm("r", Vector((1.0, 0.0, 0.0)))
        grip_hand("r", trigger_finger=True)
        FAL_RIG.calibrate()
        FAL_RIG_PALM0 = palm_center("r")
        cam0 = FAL_RIG.gun_in_camera()
        p, fi, n = FAL_RIG.key_in_blender((0, "gun") + FAL_SUPPORT, cam0)
        grip_hand("l", tight=False)
        place_left(p, fi, n, Vector((0.5, 0.2, -1.0)))
        FAL_RIG.remember_support()
        FAL_LEFT_TAC = fal_left_keys(False)
        FAL_LEFT_EMPTY = fal_left_keys(True)
        reset()

    def support_hold(cam, aim=0.0):
        hold_rifle_right(cam)
        grip_hand("l", tight=True)
        # Aimed, the arm is reaching further: the hand slides back along the handguard
        palm, fingers, normal = FAL_SUPPORT
        palm = (palm[0], palm[1], palm[2] + 0.07 * aim)
        p, fi, n = FAL_RIG.key_in_blender((0, "gun", palm, fingers, normal), cam)
        miss = place_left(p, fi, n, Vector((0.5, 0.2, -1.0)))
        if miss > 0.01:
            log("fp_rifle frame %d: support hand misses the handguard by %.0f mm" % (f, miss * 1000))
            if f == 0:
                up, lo = pbones["upperarm_l"], pbones["lowerarm_l"]
                reach = (up.tail - up.head).length + (lo.tail - lo.head).length
                log("  shoulder->palm %.0f mm, arm %.0f mm; eye->shoulder %s, eye->palm %s" % (
                    (p - up.head).length * 1000, reach * 1000, tuple(round(v, 3) for v in (up.head - EYE)),
                    tuple(round(v, 3) for v in (p - EYE))))

    if f >= FAL_ADS_FIRST:
        aim = (f - FAL_ADS_FIRST) / (FAL_FRAMES - 1 - FAL_ADS_FIRST)
        support_hold(FAL_RIG.gun_in_camera(aim=aim), aim)
    elif SWAP_FIRST <= f <= EMPTY_LAST:
        if f <= SWAP_LAST:
            t, keys, left = (f - SWAP_FIRST) / (SWAP_LAST - SWAP_FIRST) * SWAP_END, FAL_SWAP, FAL_LEFT_TAC
        elif f <= TAC_LAST:
            t, keys, left = SWAP_END + (f - TAC_FIRST) / (TAC_LAST - TAC_FIRST) * (TAC_END - SWAP_END), FAL_TACTICAL, FAL_LEFT_TAC
        else:
            t, keys, left = SWAP_END + (f - EMPTY_FIRST) / (EMPTY_LAST - EMPTY_FIRST) * (EMPTY_END - SWAP_END), FAL_EMPTY, FAL_LEFT_EMPTY
        hold_rifle_right(FAL_RIG.gun_in_camera())
        grip_hand("l", tight=True)
        cam = FAL_RIG.gun_in_camera(keys, t)
        FAL_RIG.carry(cam)
        pose_left_keys(FAL_RIG, left, t, cam)
    else:
        support_hold(FAL_RIG.gun_in_camera())
    FP_RIFLE_LEFT[f] = palm_center("l")


make_action("fp_rifle", FAL_FRAMES, fp_rifle_pose)


# --- The L42A1 ----------------------------------------------------------------------------
# fp_sniper: 0 hip, 1..36 working the bolt after a shot (0..1.15 s), 37..136 the reload
# (3.4 s: bolt open, magazine changed, bolt closed), 137..146 aiming. The game moves the
# gun by the pose keys below (the bolt's side, then the magazine, turned toward the eye)
# while the arms stay with the body, so both hands are placed on the gun where the keys put
# it: the right hand leaves the wrist for the bolt knob and comes back, the left changes the
# magazine. Times, keys and positions must match src/sniper_view.cpp.
L42 = read_gun_txt("l42")
L42_RIG = FpRig(1.0, (0.10, -0.17, -0.10), -2.5, tuple(L42["hand_grip"]), (0.0, -L42["front_sight"].y, -0.165))
SNIPER_FRAMES = 147
CYCLE_FIRST, CYCLE_LAST, CYCLE_END = 1, 36, 1.15
RELOAD_FIRST, RELOAD_LAST, RELOAD_END = 37, 136, 3.4
SNIPER_ADS_FIRST = 137
# Gun pose keys - as sniper_view.cpp (CYCLE_KEYS, RELOAD_KEYS)
BOLT_POSE = (-0.030, 0.000, -0.060, -22.0, -16.0, 32.0)
MAG_POSE = (-0.020, 0.020, -0.140, 20.0, 0.0, -20.0)
SNIPER_CYCLE_KEYS = [(0.00, 0, 0, 0, 0, 0, 0), (0.30, 0, 0, 0, 0, 0, 0), (0.42,) + BOLT_POSE, (0.92,) + BOLT_POSE,
                     (1.15, 0, 0, 0, 0, 0, 0)]
SNIPER_RELOAD_KEYS = [(0.00, 0, 0, 0, 0, 0, 0), (0.30,) + BOLT_POSE, (0.70,) + BOLT_POSE,
                      (0.95,) + MAG_POSE, (2.30,) + MAG_POSE[:5] + (-22.0,), (2.55,) + BOLT_POSE, (3.00,) + BOLT_POSE,
                      (3.40, 0, 0, 0, 0, 0, 0)]
SNIPER_SUPPORT = ((L42["handguard"].x - 0.004, L42["handguard"].y - 0.018, L42["handguard"].z), (1.0, 0.5, -0.2),
                  (0.0, 1.0, 0.0))
SNIPER_PALM_ON_MAG = (-0.034, -0.032, L42["mag_centre"].z)   # as sniper_view.cpp: the palm on the magazine's left side
SNIPER_MAG_FINGERS, SNIPER_MAG_NORMAL = (0.3, -0.4, -1.0), (1.0, 0.0, 0.0)


def knob_at(lift, back):
    """The bolt knob (gun space) with the handle turned up by `lift` and drawn back by `back` (0..1)."""
    k, a = L42["bolt_knob"], L42["bolt_axis"]
    ang = math.radians(lift * L42["bolt_lift"])
    x, y = k.x - a.x, k.y - a.y
    return Vector((a.x + x * math.cos(ang) - y * math.sin(ang), a.y + x * math.sin(ang) + y * math.cos(ang),
                   k.z + back * L42["bolt_travel"]))


# The right hand on the knob, as seen from the eye (camera space, however the gun is turned):
# coming from below and to the right, fingers reaching forward and up over it, the palm
# facing up and in. So the wrist and forearm stay low and out to the right, clear of the view.
KNOB_PALM = (0.020, -0.022, 0.008)
KNOB_FINGERS = (-0.50, 0.40, -0.77)
KNOB_NORMAL = (-0.60, 0.45, -0.66)


# Right hand keys: (seconds, "grip") on the wrist, or (seconds, "knob", lift, back)
CYCLE_RIGHT = [(0.00, "grip"), (0.28, "grip"), (0.40, "knob", 0.0, 0.0), (0.46, "knob", 1.0, 0.0),
               (0.50, "knob", 1.0, 0.0), (0.70, "knob", 1.0, 1.0), (0.72, "knob", 1.0, 1.0), (0.80, "knob", 1.0, 0.0),
               (0.86, "knob", 1.0, 0.0), (0.92, "knob", 0.0, 0.0), (1.10, "grip"), (1.15, "grip")]
RELOAD_RIGHT = [(0.00, "grip"), (0.15, "grip"), (0.35, "knob", 0.0, 0.0), (0.41, "knob", 1.0, 0.0),
                (0.45, "knob", 1.0, 0.0), (0.65, "knob", 1.0, 1.0), (0.70, "knob", 1.0, 1.0), (0.90, "grip"),
                (2.40, "grip"), (2.60, "knob", 1.0, 1.0), (2.70, "knob", 1.0, 0.0), (2.88, "knob", 1.0, 0.0),
                (2.94, "knob", 0.0, 0.0), (2.98, "knob", 0.0, 0.0), (3.18, "grip"), (3.40, "grip")]


def sniper_left_keys():
    G = "gun"
    m = SNIPER_PALM_ON_MAG
    p, fi, n = SNIPER_SUPPORT
    under = (G, (p[0], p[1], p[2] + 0.06), fi, n)   # back along the forend (the gun pushed out to see the bolt)
    return [
        (0.00, "support", None, None, None),
        (0.25,) + under,
        (0.40,) + under,
        (0.75, G, m, SNIPER_MAG_FINGERS, SNIPER_MAG_NORMAL),                                  # thumb on the catch
        (0.85, G, m, SNIPER_MAG_FINGERS, SNIPER_MAG_NORMAL),
        (1.00, G, (m[0], m[1] - 0.080, m[2]), SNIPER_MAG_FINGERS, SNIPER_MAG_NORMAL),         # pulled down, let go
        (1.20,) + POUCH,                                                                      # a fresh one from the pouch
        (1.45,) + POUCH,
        (1.75, G, (m[0], m[1] - 0.090, m[2]), SNIPER_MAG_FINGERS, SNIPER_MAG_NORMAL),         # up under the well
        (1.85, G, (m[0], m[1] - 0.075, m[2]), SNIPER_MAG_FINGERS, SNIPER_MAG_NORMAL),
        (2.20, G, m, SNIPER_MAG_FINGERS, SNIPER_MAG_NORMAL),                                  # pushed home
        (2.30, G, (m[0], m[1] - 0.010, m[2]), SNIPER_MAG_FINGERS, SNIPER_MAG_NORMAL),
        (2.50,) + under,
        (3.00,) + under,
        (3.30, "support", None, None, None),
        (3.40, "support", None, None, None),
    ]


SNIPER_LEFT = None
FP_SNIPER_LEFT = {}
SNIPER_PALM0 = None
GRIP_R = None   # (palm, fingers, normal) of the right hand on the wrist, Blender space


def hold_sniper_right(cam):
    """The right hand round the wrist of the stock, with the rifle at `cam`."""
    palm = L42_RIG.cam_to_blender @ cam @ (L42_RIG.hand_grip * L42_RIG.scale)
    wrist = palm + (L42_RIG.H0.translation - SNIPER_PALM0)
    two_bone("upperarm_r", "lowerarm_r", wrist, Vector((-0.6, 0.2, -1.0)))
    point("hand_r", Vector((0.0, -1.0, -0.40)))
    roll_palm("r", Vector((1.0, 0.0, 0.0)))
    grip_hand("r", trigger_finger=True)


def place_right(palm, fingers, normal, pole=Vector((-1.0, 0.3, -0.35))):
    """Reach so the right palm centre lands on `palm` (Blender space). The elbow swings out to
    the side (not down under the eye), so working the bolt the forearm comes in from the right."""
    target = palm.copy()
    for _ in range(3):
        two_bone("upperarm_r", "lowerarm_r", target, pole)
        point("hand_r", fingers)
        roll_palm("r", normal)
        target = target + (palm - palm_center("r"))
    return (palm - palm_center("r")).length


def right_key_in_blender(key, cam):
    if key[1] == "grip":   # the hold on the wrist, carried with the gun
        m = L42_RIG.motion(cam)
        return m @ GRIP_R[0], (m.to_3x3() @ GRIP_R[1]).normalized(), (m.to_3x3() @ GRIP_R[2]).normalized()
    knob = cam @ (knob_at(key[2], key[3]) * L42_RIG.scale)        # camera space
    m = L42_RIG.cam_to_blender
    rot = m.to_3x3().normalized()
    return m @ (knob + Vector(KNOB_PALM)), (rot @ Vector(KNOB_FINGERS)).normalized(), (rot @ Vector(KNOB_NORMAL)).normalized()


def pose_right_keys(keys, t, cam):
    i = 0
    while i < len(keys) - 2 and t > keys[i + 1][0]:
        i += 1
    a, b = keys[i], keys[i + 1]
    s = max(0.0, min(1.0, (t - a[0]) / (b[0] - a[0])))
    s = s * s * (3 - 2 * s)
    if a[1] == "grip" and b[1] == "grip":
        hold_sniper_right(cam)
        return
    pa, fa, na = right_key_in_blender(a, cam)
    pb_, fb, nb = right_key_in_blender(b, cam)
    miss = place_right(pa.lerp(pb_, s), fa.lerp(fb, s).normalized(), na.lerp(nb, s).normalized())
    on_knob = (a[1] == "knob" and s < 0.5) or (b[1] == "knob" and s >= 0.5)
    grip_hand("r", tight=True) if on_knob else grip_hand("r", trigger_finger=True)
    if miss > 0.01:
        log("fp_sniper t=%.2f: right palm misses by %.0f mm" % (t, miss * 1000))


def fp_sniper_pose(ph, f):
    global SNIPER_LEFT, SNIPER_PALM0, GRIP_R
    eye = find_eye()
    if L42_RIG.cam_to_blender is None:
        reset()
        guess = L42_RIG.gun_in_camera() @ (L42_RIG.hand_grip * L42_RIG.scale)
        wrist = eye + Vector((-guess.x, guess.z, guess.y)) + Vector((0.0, 0.03, 0.04))
        two_bone("upperarm_r", "lowerarm_r", wrist, Vector((-0.6, 0.2, -1.0)))
        point("hand_r", Vector((0.0, -1.0, -0.40)))
        roll_palm("r", Vector((1.0, 0.0, 0.0)))
        grip_hand("r", trigger_finger=True)
        L42_RIG.calibrate()
        SNIPER_PALM0 = palm_center("r")
        hold_sniper_right(L42_RIG.gun_in_camera())
        hand = pbones["hand_r"]
        GRIP_R = (palm_center("r"), (hand.tail - hand.head).normalized(), palm_normal("r"))
        cam0 = L42_RIG.gun_in_camera()
        p, fi, n = L42_RIG.key_in_blender((0, "gun") + SNIPER_SUPPORT, cam0)
        grip_hand("l", tight=False)
        place_left(p, fi, n, Vector((0.5, 0.2, -1.0)))
        L42_RIG.remember_support()
        SNIPER_LEFT = sniper_left_keys()
        reset()

    # (fingers curl relative to where they are: each hand is posed once per frame)
    def left_support(cam, aim=0.0, slide=0.0):
        grip_hand("l", tight=True)
        palm, fingers, normal = SNIPER_SUPPORT
        # aimed, the hand slides back along the forend; working the bolt (the gun pushed out to
        # see it) it slides back too
        palm = (palm[0], palm[1], palm[2] + 0.06 * aim + slide)
        p, fi, n = L42_RIG.key_in_blender((0, "gun", palm, fingers, normal), cam)
        miss = place_left(p, fi, n, Vector((0.5, 0.2, -1.0)))
        if miss > 0.01:
            log("fp_sniper frame %d: support hand misses the forend by %.0f mm" % (f, miss * 1000))

    def support_hold(cam, aim=0.0):
        hold_sniper_right(cam)
        left_support(cam, aim)

    if f >= SNIPER_ADS_FIRST:
        aim = (f - SNIPER_ADS_FIRST) / (SNIPER_FRAMES - 1 - SNIPER_ADS_FIRST)
        support_hold(L42_RIG.gun_in_camera(aim=aim), aim)
    elif CYCLE_FIRST <= f <= CYCLE_LAST:
        t = (f - CYCLE_FIRST) / (CYCLE_LAST - CYCLE_FIRST) * CYCLE_END
        cam = L42_RIG.gun_in_camera(SNIPER_CYCLE_KEYS, t)
        left_support(cam, slide=-pose_curve(SNIPER_CYCLE_KEYS, t, 2))
        pose_right_keys(CYCLE_RIGHT, t, cam)
    elif RELOAD_FIRST <= f <= RELOAD_LAST:
        t = (f - RELOAD_FIRST) / (RELOAD_LAST - RELOAD_FIRST) * RELOAD_END
        cam = L42_RIG.gun_in_camera(SNIPER_RELOAD_KEYS, t)
        grip_hand("l", tight=True)
        pose_left_keys(L42_RIG, SNIPER_LEFT, t, cam)
        pose_right_keys(RELOAD_RIGHT, t, cam)
    else:
        support_hold(L42_RIG.gun_in_camera())
    FP_SNIPER_LEFT[f] = palm_center("l")


make_action("fp_sniper", SNIPER_FRAMES, fp_sniper_pose)


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
pose_at("fp_rifle", 0)
fp_rifle_grip = grip_point()
pose_at("fp_sniper", 0)
fp_sniper_grip = grip_point()
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
lines.append("fp_rifle_grip %.4f %.4f %.4f" % to_gltf(fp_rifle_grip))
lines.append("fp_sniper_grip %.4f %.4f %.4f" % to_gltf(fp_sniper_grip))
lines.append("eye %.4f %.4f %.4f" % to_gltf(EYE))
lines.append("# fp_hold frames: 0 hip, %d..%d reload (%.1f s), %d..%d aim; left palm centre per frame (model space)"
             % (REV_RELOAD_FIRST, REV_RELOAD_LAST, REV_RELOAD_SECONDS, REV_ADS_FIRST, REV_FRAMES - 1))
for f in sorted(FP_LEFT_PALM):
    lines.append("fp_left %d %.4f %.4f %.4f" % ((f,) + to_gltf(FP_LEFT_PALM[f])))
lines.append("# fp_rifle frames: 0 hip, 1..60 magazine swap, 61..78 finish, 79..120 finish from empty, 121..130 aim")
for f in sorted(FP_RIFLE_LEFT):
    lines.append("fp_rifle_left %d %.4f %.4f %.4f" % ((f,) + to_gltf(FP_RIFLE_LEFT[f])))
lines.append("# fp_sniper frames: 0 hip, 1..36 working the bolt, 37..136 reload, 137..146 aim")
for f in sorted(FP_SNIPER_LEFT):
    lines.append("fp_sniper_left %d %.4f %.4f %.4f" % ((f,) + to_gltf(FP_SNIPER_LEFT[f])))
with open(os.path.join(OUT_DIR, "character.txt"), "w") as fh:
    fh.write("\n".join(lines) + "\n")
log("\n".join(lines))
log("wrote", glb, os.path.getsize(glb), "bytes")
