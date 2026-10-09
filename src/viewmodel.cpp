#include "viewmodel.hpp"
#include "raymath.h"
#include "rlgl.h"
#include "effects.hpp"

// raylib has no depth-only clear, so call OpenGL's directly (OpenGL 1.1, always available)
#if defined(_WIN32)
extern "C" __declspec(dllimport) void __stdcall glClear(unsigned int mask);
#elif defined(__APPLE__)
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif
static const unsigned int DEPTH_BUFFER_BIT = 0x00000100;

ViewModelMotion MotionOf(const Player& player) {
    ViewModelMotion m;
    m.speed = player.GetSpeed();
    m.walkSpeed = player.walkSpeed;
    m.grounded = player.IsGrounded();
    m.verticalVelocity = player.GetVerticalVelocity();
    m.crouching = player.IsCrouching();
    m.look = player.GetLookDirection();
    m.right = player.GetRight();
    m.strafe = Vector3DotProduct(player.GetVelocity(), m.right) / player.walkSpeed;
    return m;
}

static float Ease(float current, float target, float rate, float dt) {
    return current + (target - current) * (1.0f - expf(-rate * dt));
}

// 0 before a, 1 after b, smooth in between
static float Ramp(float t, float a, float b) {
    float x = Clamp((t - a) / (b - a), 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

// Like Ramp but accelerating into b (a part snapping home)
static float RampIn(float t, float a, float b) {
    float x = Clamp((t - a) / (b - a), 0.0f, 1.0f);
    return x * x;
}

// Overshoots a little past 1 and settles (a part swinging out on its hinge)
static float RampOutBack(float t, float a, float b) {
    float x = Clamp((t - a) / (b - a), 0.0f, 1.0f) - 1.0f;
    const float c1 = 1.1f, c3 = c1 + 1.0f;
    return 1.0f + c3 * x * x * x + c1 * x * x;
}

void ViewModel::Spring::Step(float dt, float stiffness, float damping) {
    // Small fixed steps keep a stiff spring stable at low frame rates
    int steps = (int)ceilf(dt / (1.0f / 240.0f));
    float h = dt / (float)(steps > 0 ? steps : 1);
    for (int i = 0; i < steps; i++) {
        v += (-stiffness * x - damping * v) * h;
        x += v * h;
    }
}

float ViewModel::Random() {
    noise = noise * 1664525u + 1013904223u;
    return (float)((noise >> 8) & 0xFFFF) / 32767.5f - 1.0f;
}

// ---------------------------------------------------------------------------
// The reload, as a timeline (seconds, for a 3 s reload):
//   0.10 thumb the latch          0.20 swing the cylinder out
//   0.45 muzzle up, 0.52 punch the ejector rod: the cases drop out
//   0.80 muzzle down, the support hand fetches a speedloader
//   1.36 line it up, 1.42 rounds in, 1.55 twist to release, 1.65 pull it away
//   2.02 flick the cylinder spinning, 2.30 swing it shut, 2.45 back on target
// ---------------------------------------------------------------------------

struct ReloadKey { float t, x, y, z, pitch, yaw, roll; };
static const ReloadKey RELOAD_KEYS[] = {
    { 0.00f,  0.000f, 0.000f, 0.000f,   0.0f,  0.0f,   0.0f },
    { 0.18f, -0.050f, 0.035f, 0.050f,   6.0f,  8.0f, -22.0f },  // canted over, thumb on the latch
    { 0.40f, -0.070f, 0.055f, 0.070f,  10.0f, 12.0f, -38.0f },  // cylinder out
    { 0.52f, -0.060f, 0.065f, 0.060f,  48.0f,  8.0f, -30.0f },  // muzzle up to punch the ejector
    { 0.64f, -0.060f, 0.075f, 0.070f,  56.0f,  8.0f, -28.0f },
    { 0.80f, -0.060f, 0.055f, 0.060f,  28.0f,  9.0f, -30.0f },
    { 1.00f, -0.120f,  0.050f, 0.080f, -42.0f, 20.0f, -30.0f },  // brought in low and close, muzzle down: into the chambers
    { 1.42f, -0.130f,  0.055f, 0.090f, -48.0f, 22.0f, -28.0f },
    { 1.62f, -0.130f,  0.055f, 0.090f, -45.0f, 22.0f, -28.0f },
    { 1.98f, -0.120f,  0.050f, 0.080f, -30.0f, 18.0f, -32.0f },
    { 2.28f, -0.110f,  0.050f, 0.080f,  -8.0f, 14.0f, -36.0f },
    { 2.42f, -0.070f,  0.030f, 0.060f,   3.0f,  8.0f, -14.0f },  // flicked shut
    { 2.70f, -0.010f, 0.005f, 0.010f,   0.0f,  2.0f,  -2.0f },
    { 3.00f,  0.000f, 0.000f, 0.000f,   0.0f,  0.0f,   0.0f },
};
static const int RELOAD_KEY_COUNT = sizeof(RELOAD_KEYS) / sizeof(RELOAD_KEYS[0]);
static const float RELOAD_SCRIPT_LENGTH = 3.0f;

// Smooth curve through the keys (Catmull-Rom tangents, still at the ends)
static float KeyChannel(const ReloadKey& k, int c) {
    const float* v = &k.x;
    return v[c];
}
static float Curve(float t, int channel) {
    if (t <= RELOAD_KEYS[0].t) return KeyChannel(RELOAD_KEYS[0], channel);
    for (int i = 0; i < RELOAD_KEY_COUNT - 1; i++) {
        const ReloadKey& a = RELOAD_KEYS[i];
        const ReloadKey& b = RELOAD_KEYS[i + 1];
        if (t > b.t) continue;
        float h = b.t - a.t;
        float s = (t - a.t) / h;
        float pa = KeyChannel(a, channel), pb = KeyChannel(b, channel);
        auto tangent = [&](int j) {
            if (j <= 0 || j >= RELOAD_KEY_COUNT - 1) return 0.0f;
            const ReloadKey& p = RELOAD_KEYS[j - 1];
            const ReloadKey& n = RELOAD_KEYS[j + 1];
            return (KeyChannel(n, channel) - KeyChannel(p, channel)) / (n.t - p.t);
        };
        float ma = tangent(i) * h, mb = tangent(i + 1) * h;
        float s2 = s * s, s3 = s2 * s;
        return (2 * s3 - 3 * s2 + 1) * pa + (s3 - 2 * s2 + s) * ma + (-2 * s3 + 3 * s2) * pb + (s3 - s2) * mb;
    }
    return KeyChannel(RELOAD_KEYS[RELOAD_KEY_COUNT - 1], channel);
}

ViewModel::ReloadPose ViewModel::ReloadOffset() const { return reloadPose; }

// ---------------------------------------------------------------------------
// Update
// ---------------------------------------------------------------------------

void ViewModel::DryFire() {
    sinceDry = 0.0f;
    cylinderFrom = cylinderIndex;
    cylinderIndex += 1.0f;
}

ViewModelEvents ViewModel::TakeEvents() {
    ViewModelEvents e = events;
    events = ViewModelEvents{};
    return e;
}

// The chambers in firing order from the one under the hammer: the first `ammo` are live
void ViewModel::SyncChambers(int ammo) {
    int k = RevolverChamberAtBarrel(cylinderIndex);
    for (int i = 0; i < 6; i++) {
        mech.chambers[k] = i < ammo ? CHAMBER_LIVE : CHAMBER_FIRED;
        k = (k + 5) % 6; // the next one to come under the hammer
    }
}

void ViewModel::OnShot(const Weapon& weapon) {
    // The chamber under the hammer fires; the cylinder indexes on to the next
    mech.chambers[RevolverChamberAtBarrel(cylinderIndex)] = CHAMBER_FIRED;
    cylinderFrom = cylinderIndex;
    cylinderIndex += 1.0f;
    sinceShot = 0.0f;

    // Kick: muzzle flips up and the gun drives back into the hand, a little off-axis each time
    float s = Lerp(1.0f, 0.55f, weapon.GetAimAmount());
    kickPitch.v += 290.0f * s;
    kickBack.v += 1.15f * s;
    kickUp.v += 0.22f * s;
    kickYaw.v += Random() * 45.0f * s;
    kickRoll.v += (35.0f + Random() * 40.0f) * s;
}

void ViewModel::Update(float dt, const ViewModelMotion& motion, const Weapon& weapon, const Camera3D* worldCamera) {
    time += dt;
    if (worldCamera) {
        Matrix view = GetCameraMatrix(*worldCamera);
        gravity = Vector3Subtract(Vector3Transform({ 0, -9.81f, 0 }, view), Vector3Transform({ 0, 0, 0 }, view));
    }

    // Walk bob: amount follows speed, phase follows distance travelled
    float targetBob = motion.grounded ? Clamp(motion.speed / motion.walkSpeed, 0.0f, 1.8f) : 0.0f;
    bobAmount = Ease(bobAmount, targetBob, 10.0f, dt);
    bobPhase += dt * motion.speed * 2.2f; // one side-to-side cycle every ~2.9 m (two steps)

    // Jumping/falling: the gun lags behind - dips while rising, lifts while falling
    float targetAir = motion.grounded ? 0.0f : Clamp(-motion.verticalVelocity * 0.004f, -0.03f, 0.035f);
    airOffset = Ease(airOffset, targetAir, 12.0f, dt);

    // Landing: a dip proportional to how hard you came down
    if (motion.grounded && !wasGrounded) landKick = Clamp(-lastVerticalVelocity * 0.005f, 0.0f, 0.05f);
    landKick = Ease(landKick, 0.0f, 8.0f, dt);
    wasGrounded = motion.grounded;
    lastVerticalVelocity = motion.verticalVelocity;

    crouchAmount = Ease(crouchAmount, motion.crouching ? 1.0f : 0.0f, 12.0f, dt);
    bool sprinting = motion.grounded && motion.speed > motion.walkSpeed * 1.25f && weapon.GetAimAmount() < 0.1f &&
                     !weapon.IsReloading() && sinceShot > 0.35f;
    sprintAmount = Ease(sprintAmount, sprinting ? 1.0f : 0.0f, sprinting ? 6.0f : 12.0f, dt);

    // Sway: turning drags the gun behind the view; strafing leaves it behind sideways
    float aimDamp = Lerp(1.0f, 0.15f, weapon.GetAimAmount());
    if (dt > 0.0f && Vector3LengthSqr(lastLook) > 0.0f) {
        Vector3 d = Vector3Subtract(motion.look, lastLook);
        float turnRight = Vector3DotProduct(d, motion.right) / dt;   // rad/s
        float turnUp = d.y / dt;
        swayYaw = Ease(swayYaw, Clamp(turnRight * 1.6f, -5.0f, 5.0f) * aimDamp, 10.0f, dt);
        swayPitch = Ease(swayPitch, Clamp(-turnUp * 1.2f, -4.0f, 4.0f) * aimDamp, 10.0f, dt);
    }
    lastLook = motion.look;
    strafeLag = Ease(strafeLag, Clamp(motion.strafe, -1.5f, 1.5f) * aimDamp, 8.0f, dt);

    // Shots: the weapon's ammo count going down is a shot
    int ammo = weapon.GetAmmo();
    bool reloading = weapon.IsReloading();
    if (lastAmmo < 0) SyncChambers(ammo);
    else if (!reloading && ammo == lastAmmo - 1) OnShot(weapon);
    else if (!reloading && !wasReloading && ammo != lastAmmo) SyncChambers(ammo); // reset (respawn)
    lastAmmo = ammo;

    // Recoil springs
    kickPitch.Step(dt, 190.0f, 12.0f);
    kickBack.Step(dt, 380.0f, 22.0f);
    kickUp.Step(dt, 300.0f, 20.0f);
    kickYaw.Step(dt, 160.0f, 13.0f);
    kickRoll.Step(dt, 150.0f, 12.0f);

    // Lockwork. Double action: the trigger has already cocked and dropped the
    // hammer when the shot goes off; the cylinder finishes indexing to the next
    // chamber and the trigger resets.
    sinceShot += dt;
    sinceDry += dt;
    float hammer = sinceShot < 0.025f ? 1.0f - sinceShot / 0.025f : 0.0f;
    float trigger = sinceShot < 0.05f ? 1.0f : Clamp(1.0f - (sinceShot - 0.05f) / 0.11f, 0.0f, 1.0f);
    float turn = Ramp(sinceShot, 0.0f, 0.07f);
    if (sinceDry < sinceShot) { // a dry pull: hammer comes back, falls on nothing
        float pull = Ramp(sinceDry, 0.0f, 0.09f);
        float drop = sinceDry > 0.09f ? Clamp(1.0f - (sinceDry - 0.09f) / 0.015f, 0.0f, 1.0f) : 1.0f;
        hammer = fmaxf(hammer, pull * drop);
        trigger = fmaxf(trigger, sinceDry < 0.12f ? pull : Clamp(1.0f - (sinceDry - 0.12f) / 0.1f, 0.0f, 1.0f));
        turn = Ramp(sinceDry, 0.0f, 0.09f);
    }
    mech.hammer = hammer;
    mech.trigger = trigger;
    if (!reloading) mech.cylinder = Lerp(cylinderFrom, cylinderIndex, turn);

    // Reload
    if (reloading) {
        if (!wasReloading) {
            reloadTime = 0.0f;
            casesReleased = roundsSeated = false;
            spinRate = 0.0f;
            cylinderFrom = cylinderIndex = mech.cylinder = roundf(mech.cylinder);
        }
        float script = RELOAD_SCRIPT_LENGTH / weapon.GetDef().reloadTime;
        float t = weapon.GetReloadProgress() * weapon.GetDef().reloadTime * script;
        AnimateReload(t, dt * script, weapon, worldCamera);
        reloadTime = t;
    } else if (wasReloading) {
        // Done (or cut short): everything home, the chambers match the ammo
        mech.crane = mech.ejector = mech.latch = mech.caseLift = 0.0f;
        mech.speedloader = false;
        cylinderFrom = cylinderIndex = mech.cylinder = roundf(mech.cylinder);
        SyncChambers(ammo);
        reloadPose = ReloadPose{};
    }
    wasReloading = reloading;

    // Cases falling clear of the gun; once out of view they carry on in the world
    for (size_t i = 0; i < loose.size();) {
        LooseCase& c = loose[i];
        c.age += dt;
        c.velocity = Vector3Add(c.velocity, Vector3Scale(gravity, dt));
        c.position = Vector3Add(c.position, Vector3Scale(c.velocity, dt));
        c.rotation = MatrixMultiply(c.rotation, MatrixRotate(c.spinAxis, c.spinRate * dt));
        if (c.age > 0.45f || c.position.y < -0.6f) {
            if (worldCamera) {
                Matrix view = GetCameraMatrix(*worldCamera);
                Matrix toWorld = MatrixInvert(view);
                Vector3 vel = Vector3Subtract(Vector3Transform(c.velocity, toWorld), Vector3Transform({ 0, 0, 0 }, toWorld));
                events.casings.push_back({ CameraToWorld(c.position, *worldCamera), vel, c.fired });
            }
            loose[i] = loose.back();
            loose.pop_back();
            continue;
        }
        i++;
    }
}

void ViewModel::ReleaseCases(const Weapon& weapon) {
    Matrix gunToCam = GunToCamera(weapon);
    Matrix cyl = RevolverCylinderTransform(mech);
    const RevolverRig& rig = GetRevolverRig();
    // The cases leave along the cylinder's axis, toward the shooter (and, muzzle up, the ground)
    Vector3 out = Vector3Subtract(Vector3Transform({ 0, 0, 1 }, gunToCam), Vector3Transform({ 0, 0, 0 }, gunToCam));
    for (int k = 0; k < 6; k++) {
        if (mech.chambers[k] == CHAMBER_EMPTY) continue;
        Vector3 rim = Vector3Add(RevolverChamberRear(mech, k), { 0, 0, mech.caseLift * rig.ejectorStroke });
        LooseCase c;
        c.position = Vector3Transform(rim, gunToCam);
        c.velocity = Vector3Add(Vector3Scale(out, 0.45f + 0.2f * Random()), { 0.15f * Random(), 0.05f * Random(), 0.12f * Random() });
        c.rotation = MatrixMultiply(cyl, gunToCam);
        c.rotation.m12 = c.rotation.m13 = c.rotation.m14 = 0.0f; // orientation only
        c.spinAxis = Vector3Normalize({ Random(), Random(), Random() + 0.01f });
        c.spinRate = 6.0f + 10.0f * fabsf(Random());
        c.age = 0.0f;
        c.fired = mech.chambers[k] == CHAMBER_FIRED;
        loose.push_back(c);
        mech.chambers[k] = CHAMBER_EMPTY;
    }
    mech.caseLift = 0.0f;
}

void ViewModel::AnimateReload(float t, float dt, const Weapon& weapon, const Camera3D* worldCamera) {
    (void)worldCamera;
    float prev = reloadTime;
    auto crossed = [&](float at) { return prev < at && t >= at; };

    reloadPose = { Curve(t, 0), Curve(t, 1), Curve(t, 2), Curve(t, 3), Curve(t, 4), Curve(t, 5) };

    // Latch thumbed forward, the cylinder swings out on the crane, and later back in
    mech.latch = Ramp(t, 0.10f, 0.15f) - Ramp(t, 0.30f, 0.36f);
    mech.crane = RampOutBack(t, 0.20f, 0.36f) * (1.0f - RampIn(t, 2.30f, 2.40f));
    if (crossed(0.11f)) events.latch = true;
    if (crossed(0.19f)) events.open = true;

    // Ejector rod punched: the cases ride out with the star, then drop free
    mech.ejector = RampIn(t, 0.52f, 0.59f) - Ramp(t, 0.64f, 0.74f);
    if (crossed(0.51f)) events.eject = true;
    if (!casesReleased) {
        mech.caseLift = mech.ejector;
        if (t >= 0.59f) {
            ReleaseCases(weapon);
            casesReleased = true;
        }
    }

    // Speedloader: brought up behind the open cylinder, rounds in, twist, away
    const RevolverRig& rig = GetRevolverRig();
    mech.speedloader = t >= 1.02f && t < 1.98f;
    if (mech.speedloader) {
        RevolverPose open = mech;
        open.crane = 1.0f;
        Vector3 rear = Vector3Transform({ rig.cylinderAxis.x, rig.cylinderAxis.y, rig.cylinderRearZ },
                                        RevolverCylinderTransform(open));
        Vector3 away = { -0.05f, -0.16f, 0.12f };
        Vector3 offset;
        if (t < 1.36f) offset = Vector3Lerp(away, { 0, 0, 0.040f }, Ramp(t, 1.02f, 1.36f));
        else if (t < 1.46f) offset = { 0, 0, 0.040f * (1.0f - RampIn(t, 1.36f, 1.46f)) };
        else if (t < 1.72f) offset = { 0, 0, 0.030f * Ramp(t, 1.62f, 1.72f) };
        else offset = Vector3Lerp({ 0, 0, 0.030f }, away, Ramp(t, 1.72f, 1.98f));
        float twist = 25.0f * Ramp(t, 1.52f, 1.58f);
        float spin = mech.cylinder * 60.0f + rig.craneOpenDegrees + twist; // line the rounds up with the chambers
        Vector3 p = Vector3Add(rear, offset);
        if (HandDriven(weapon)) {
            // Held by its knob in the left hand: the palm is 34 mm below the knob (as seen from the
            // eye) and the knob 18 mm behind the rims. It snaps onto the cylinder's axis to go in.
            Vector3 palm = Vector3Transform(CharacterFpLeftPalm(*arms, ArmsFrame(weapon)), ArmsToCamera(weapon));
            Vector3 knobGun = Vector3Transform(Vector3Add(palm, { 0, 0.034f, 0 }), MatrixInvert(GunToCamera(weapon)));
            Vector3 inHand = Vector3Subtract(knobGun, { 0, 0, 0.018f });
            float onAxis = Ramp(t, 1.26f, 1.36f) * (1.0f - Ramp(t, 1.66f, 1.76f));
            p = Vector3Lerp(inHand, p, onAxis);
        }
        mech.speedloaderTransform = MatrixMultiply(MatrixRotateZ(spin * DEG2RAD), MatrixTranslate(p.x, p.y, p.z));
        mech.speedloaderFull = t < 1.47f;
    }
    if (crossed(1.41f)) events.insert = true;
    if (!roundsSeated && t >= 1.47f) {
        for (int k = 0; k < 6; k++) mech.chambers[k] = CHAMBER_LIVE;
        roundsSeated = true;
    }

    // A flick sets the open cylinder spinning; closing it lines a chamber up with the barrel
    if (crossed(2.02f)) { spinRate = 16.0f; events.spin = true; }
    if (t < 2.30f) {
        mech.cylinder += spinRate * dt;
        spinRate *= expf(-2.4f * dt);
        cylinderFrom = cylinderIndex = mech.cylinder;
    } else {
        if (spinRate > 0.0f) { cylinderFrom = mech.cylinder; cylinderIndex = ceilf(mech.cylinder); spinRate = 0.0f; }
        mech.cylinder = Lerp(cylinderFrom, cylinderIndex, Ramp(t, 2.30f, 2.40f));
    }
    if (crossed(2.36f)) events.close = true;
}

// ---------------------------------------------------------------------------
// Placement
// ---------------------------------------------------------------------------

bool ViewModel::HandDriven(const Weapon& weapon) const {
    return weapon.IsReloading() && arms && arms->loaded && !arms->fpLeftPalm.empty();
}

float ViewModel::ArmsFrame(const Weapon& weapon) const {
    if (weapon.IsReloading()) return Lerp(FP_RELOAD_FIRST, FP_RELOAD_LAST, weapon.GetReloadProgress());
    if (weapon.GetAimAmount() > 0.0f) return Lerp(FP_ADS_FIRST, FP_LAST_FRAME, weapon.GetAimAmount());
    return 0.0f;
}

// The arms are drawn so the right palm sits on the grip. Character space faces
// +Z; gun space fires along -Z, hence the half turn. They're life size: the
// gun's enlargement is undone and the palm goes where it lands on the bigger grip.
Matrix ViewModel::ArmsToCamera(const Weapon& weapon) const {
    if (!arms || !arms->loaded) return BaseGunToCamera(weapon);
    float frame = ArmsFrame(weapon);
    // While the hand carries the gun (reload) the arms stay put and the gun follows the hand;
    // otherwise the palm is pinned to the grip in every frame
    Vector3 g = HandDriven(weapon) ? arms->fpGrip
                                   : CharacterGripPoint(*arms, CharAnim::FpHold,
                                                        CharacterFrameAt(*arms, CharAnim::FpHold, frame / FP_LAST_FRAME));
    float s = models->fpScale;
    Vector3 h = Vector3Scale(models->handGrip, s);
    Matrix toGun = MatrixMultiply(MatrixMultiply(MatrixTranslate(-g.x, -g.y, -g.z), MatrixRotateY(PI)),
                                  MatrixTranslate(h.x, h.y, h.z));
    return MatrixMultiply(MatrixMultiply(toGun, MatrixScale(1.0f / s, 1.0f / s, 1.0f / s)), BaseGunToCamera(weapon));
}

Matrix ViewModel::GunToCamera(const Weapon& weapon) const {
    Matrix base = BaseGunToCamera(weapon);
    if (!HandDriven(weapon)) return base;
    // The right hand's movement since the hip hold, applied to the gun
    int frame = CharacterFrameAt(*arms, CharAnim::FpHold, ArmsFrame(weapon) / FP_LAST_FRAME);
    Matrix hand0 = CharacterHandTransform(*arms, CharAnim::FpHold, 0);
    Matrix hand = CharacterHandTransform(*arms, CharAnim::FpHold, frame);
    Matrix moved = MatrixMultiply(MatrixInvert(hand0), hand);   // model space
    Matrix armsToCam = ArmsToCamera(weapon);
    return MatrixMultiply(MatrixMultiply(MatrixMultiply(base, MatrixInvert(armsToCam)), moved), armsToCam);
}

Matrix ViewModel::BaseGunToCamera(const Weapon& weapon) const {
    const WeaponModels& m = *models;
    float aim = weapon.GetAimAmount();
    float aimS = aim * aim * (3.0f - 2.0f * aim);  // eased: settles onto the sights
    float idleScale = 1.0f - aim;                  // fully aimed: rock steady
    float bobScale = 1.0f - 0.85f * aim;           // aimed and walking: a little sway

    // Hip -> aimed: slide to the center with the sights on the line of sight
    Vector3 aimOffset = { 0.0f, -m.frontSightTop.y * m.fpScale, -m.aimDistance };
    Vector3 offset = Vector3Lerp(m.hipOffset, aimOffset, aimS);
    float yaw = Lerp(m.hipYawDegrees, 0.0f, aimS);
    float pitch = 0.0f, roll = 0.0f;
    // ...on a slight arc, dipping and canting through the middle of the move
    float arc = sinf(PI * aim);
    offset.y -= 0.012f * arc;
    roll -= 4.0f * arc;

    // Idle: slow breathing and a little drift
    offset.y += sinf(time * 1.7f) * 0.0025f * idleScale;
    offset.x += sinf(time * 0.83f) * 0.0012f * idleScale;
    pitch += sinf(time * 1.7f + 0.5f) * 0.4f * idleScale;
    yaw += sinf(time * 0.61f) * 0.35f * idleScale;

    // Walking: figure-eight bob (side to side once per two steps, down each step)
    float b = bobAmount * bobScale;
    offset.x += sinf(bobPhase) * 0.012f * b;
    offset.y += (cosf(2.0f * bobPhase) - 1.0f) * 0.005f * b;
    roll += sinf(bobPhase) * 1.5f * b;
    pitch += (cosf(2.0f * bobPhase) - 1.0f) * 0.6f * b;

    // Sway: lags behind turning; strafing pushes it the other way and cants it
    yaw += swayYaw;
    pitch += swayPitch;
    offset.x += swayYaw * 0.0012f - strafeLag * 0.010f;
    offset.y += swayPitch * 0.0010f;
    roll -= strafeLag * 3.0f + swayYaw * 0.6f;

    // Jumping/falling and landing
    offset.y += airOffset * (1.0f - 0.7f * aim) - landKick;

    // Crouching: tucked a little lower and in, slightly canted
    offset = Vector3Add(offset, Vector3Scale({ -0.010f, -0.012f, 0.010f }, crouchAmount * idleScale));
    roll -= 3.0f * crouchAmount * idleScale;

    // Sprinting: carried low and turned in
    offset = Vector3Add(offset, Vector3Scale({ -0.035f, -0.035f, 0.030f }, sprintAmount));
    pitch -= 14.0f * sprintAmount;
    yaw += 20.0f * sprintAmount;
    roll -= 12.0f * sprintAmount;

    // Firing: the springs' kick
    offset.z += kickBack.x;
    offset.y += kickUp.x;
    pitch += kickPitch.x;
    yaw += kickYaw.x;
    roll += kickRoll.x;

    // Reloading: the choreographed handling (with arms, their animation carries the gun instead)
    if (!HandDriven(weapon)) {
        offset.x += reloadPose.x;
        offset.y += reloadPose.y;
        offset.z += reloadPose.z;
        pitch += reloadPose.pitch;
        yaw += reloadPose.yaw;
        roll += reloadPose.roll;
    }

    // Applied left to right: scale, roll, pitch, yaw, then move into place
    Matrix rot = MatrixMultiply(MatrixMultiply(MatrixRotateZ(roll * DEG2RAD), MatrixRotateX(pitch * DEG2RAD)),
                                MatrixRotateY(yaw * DEG2RAD));
    rot = MatrixMultiply(MatrixScale(m.fpScale, m.fpScale, m.fpScale), rot);
    return MatrixMultiply(rot, MatrixTranslate(offset.x, offset.y, offset.z));
}

Vector3 ViewModel::CameraToWorld(Vector3 p, const Camera3D& cam) const {
    // The viewmodel has its own field of view: scale across the screen so the
    // world point lands on the same pixel
    float s = tanf(cam.fovy * 0.5f * DEG2RAD) / tanf(fovy * 0.5f * DEG2RAD);
    Vector3 f = Vector3Normalize(Vector3Subtract(cam.target, cam.position));
    Vector3 r = Vector3Normalize(Vector3CrossProduct(f, cam.up));
    Vector3 u = Vector3CrossProduct(r, f);
    Vector3 w = cam.position;
    w = Vector3Add(w, Vector3Scale(r, p.x * s));
    w = Vector3Add(w, Vector3Scale(u, p.y * s));
    return Vector3Add(w, Vector3Scale(f, -p.z));
}

Vector3 ViewModel::MuzzleCameraSpace(const Weapon& weapon) const {
    return Vector3Transform(models->fpMuzzle, GunToCamera(weapon));
}

// ---------------------------------------------------------------------------
// Draw
// ---------------------------------------------------------------------------

void ViewModel::Draw(const Weapon& weapon) const {
    // Finish drawing the world, then forget its depth so the gun is always on top
    rlDrawRenderBatchActive();
    glClear(DEPTH_BUFFER_BIT);

    // A camera at the origin looking down -Z: world space for this pass is camera space
    Camera3D cam = { 0 };
    cam.position = { 0, 0, 0 };
    cam.target = { 0, 0, -1 };
    cam.up = { 0, 1, 0 };
    cam.fovy = fovy;
    cam.projection = CAMERA_PERSPECTIVE;

    ViewModelPose pose;
    pose.revolver = mech;
    pose.reloading = weapon.IsReloading();
    pose.reload = weapon.GetReloadProgress();
    pose.aim = weapon.GetAimAmount();
    bool realArms = arms && arms->loaded;
    pose.boxHands = !realArms;
    Matrix gunToCam = GunToCamera(weapon);

    BeginMode3D(cam);
        rlPushMatrix();
            rlMultMatrixf(MatrixToFloat(gunToCam));
            models->drawFirstPerson(pose);
        rlPopMatrix();
        if (realArms) {
            int frame = CharacterFrameAt(*arms, CharAnim::FpHold, ArmsFrame(weapon) / FP_LAST_FRAME);
            DrawCharacter(*arms, CharAnim::FpHold, frame, ArmsToCamera(weapon), PART_FOREARMS, WHITE);
        }

        // Cases tumbling away from the open cylinder
        for (const LooseCase& c : loose)
            DrawCartridge(MatrixMultiply(c.rotation, MatrixTranslate(c.position.x, c.position.y, c.position.z)), c.fired);

        if (weapon.IsMuzzleFlashing()) {
            // Smaller when aiming so it doesn't hide what you're shooting at
            Vector3 muzzle = Vector3Transform(Vector3Add(models->fpMuzzle, { 0, 0, -0.04f }), gunToCam);
            float size = Lerp(0.30f, 0.16f, weapon.GetAimAmount());
            float spin = (float)(weapon.GetAmmo() * 67 % 360);
            Fx::DrawFlashSprite(cam, muzzle, size, spin, 1.0f);
            // A revolver also spits flame sideways out of the gap between cylinder and barrel
            const RevolverRig& rig = GetRevolverRig();
            for (float side : { -1.0f, 1.0f }) {
                Vector3 gap = Vector3Transform(Vector3Add(rig.cylinderGap, { side * 0.013f, -0.004f, 0.0f }), gunToCam);
                Fx::DrawFlashSprite(cam, gap, size * 0.22f, spin + 40.0f * side, 0.8f);
            }
        }
    EndMode3D();
}
