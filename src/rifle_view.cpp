#include "rifle_view.hpp"
#include "view_anim.hpp"
#include "effects.hpp"
#include "render.hpp"
#include "raymath.h"
#include "rlgl.h"

using namespace ViewAnim;

// ---------------------------------------------------------------------------
// The reload (seconds). Both kinds share the magazine swap; an empty one then
// charges the rifle:
//   0.30 thumb the release, 0.33 the magazine rocks forward off its hook
//   0.45..0.62 pulled down and dropped     0.80..1.05 down to the pouch
//   1.30 the new magazine comes up, 1.42 its front hooks in, 1.58 rocked home
//   1.74 a slap to be sure                 2.00 swap done
//   tactical: 2.60 back on target
//   empty:    2.30 hand over to the charging handle, 2.62..2.85 pulled back,
//             2.92 let go: the bolt slams home on a fresh round; 3.40 on target
// With the character's arms, their animation (tools/export_character.py, which
// has the same keys) carries the gun; these keys only place it without arms.
// ---------------------------------------------------------------------------

static const PoseKey SWAP_KEYS[] = {
    { 0.00f,   0.000f,  0.000f,  0.000f,   0.0f,   0.0f,    0.0f },
    { 0.25f,  -0.060f,  0.070f, -0.060f,   4.0f,  28.0f,  -35.0f },  // turned and canted: its left side and the magazine well toward the eye
    { 0.60f,  -0.070f,  0.090f, -0.080f,   6.0f,  32.0f,  -40.0f },
    { 1.00f,  -0.070f,  0.100f, -0.080f,   6.0f,  32.0f,  -42.0f },
    { 1.60f,  -0.070f,  0.100f, -0.080f,   6.0f,  32.0f,  -40.0f },
    { 1.74f,  -0.070f,  0.110f, -0.080f,   9.0f,  32.0f,  -38.0f },  // slap
    { 2.00f,  -0.050f,  0.060f, -0.050f,   4.0f,  20.0f,  -28.0f },
};
static const PoseKey TACTICAL_KEYS[] = {
    { 2.00f,  -0.050f,  0.060f, -0.050f,   4.0f,  20.0f,  -28.0f },
    { 2.60f,   0.000f,  0.000f,  0.000f,   0.0f,   0.0f,    0.0f },
};
static const PoseKey EMPTY_KEYS[] = {
    { 2.00f,  -0.050f,  0.060f, -0.050f,   4.0f,  20.0f,  -28.0f },
    { 2.30f,  -0.070f,  0.070f, -0.070f,   4.0f,  30.0f,    5.0f },  // left side toward the eye: the charging handle
    { 2.65f,  -0.070f,  0.075f, -0.070f,   4.0f,  30.0f,    8.0f },
    { 2.90f,  -0.070f,  0.075f, -0.060f,   4.0f,  30.0f,    8.0f },
    { 2.97f,  -0.070f,  0.080f, -0.060f,   7.0f,  30.0f,    6.0f },  // the bolt slamming home jolts it
    { 3.40f,   0.000f,  0.000f,  0.000f,   0.0f,   0.0f,    0.0f },
};
#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

// Where the support hand's palm is (gun space) at the moment it takes the
// magazine (keyed the same in export_character.py)
static const Vector3 PALM_ON_MAG = { -0.032f, -0.075f, -0.078f };
static const float MAG_ROCK = 22.0f; // degrees forward to come off (and go onto) the front hook

ArmsClip RifleView::Clip() const { return { CharAnim::FpRifle, FPR_LAST_FRAME, FPR_ADS_FIRST }; }

float RifleView::ReloadSeconds(const Weapon& weapon) const {
    float length = weapon.IsEmptyReload() ? EMPTY_END : TACTICAL_END;
    return weapon.GetReloadProgress() * length;
}

float RifleView::ArmsFrame(const Weapon& weapon) const {
    if (weapon.IsReloading()) {
        float t = ReloadSeconds(weapon);
        if (t < SWAP_END) return Lerp(FPR_SWAP_FIRST, FPR_SWAP_LAST, t / SWAP_END);
        if (weapon.IsEmptyReload())
            return Lerp(FPR_EMPTY_FIRST, FPR_EMPTY_LAST, Clamp((t - SWAP_END) / (EMPTY_END - SWAP_END), 0.0f, 1.0f));
        return Lerp(FPR_TACTICAL_FIRST, FPR_TACTICAL_LAST, Clamp((t - SWAP_END) / (TACTICAL_END - SWAP_END), 0.0f, 1.0f));
    }
    if (weapon.GetAimAmount() > 0.0f) return Lerp(FPR_ADS_FIRST, FPR_LAST_FRAME, weapon.GetAimAmount());
    return 0.0f;
}

Vector3 RifleView::LeftPalmInGun(const Weapon& weapon) const {
    Vector3 palm = Vector3Transform(CharacterFpLeftPalm(*arms, CharAnim::FpRifle, ArmsFrame(weapon)), ArmsToCamera(weapon));
    return Vector3Transform(palm, MatrixInvert(GunToCamera(weapon)));
}

void RifleView::OnShot(const Weapon& weapon) {
    sinceShot = 0.0f;
    casePending = true;
    // A rifle kicks straight back into the shoulder with a smaller flip than the revolver,
    // but it keeps coming while the trigger's held
    float s = Lerp(1.0f, 0.6f, weapon.GetAimAmount());
    kickPitch.v += 125.0f * s;
    kickBack.v += 0.95f * s;
    kickUp.v += 0.12f * s;
    kickYaw.v += Random() * 28.0f * s;
    kickRoll.v += (8.0f + Random() * 16.0f) * s;
}

void RifleView::EjectCase(const Weapon& weapon) {
    Matrix gunToCam = GunToCamera(weapon);
    Matrix rot = gunToCam;
    rot.m12 = rot.m13 = rot.m14 = 0.0f;
    const FalRig& rig = GetFalRig();
    // Out of the port to the right, up and a little back, spinning end over end
    Vector3 velGun = { 2.4f + 0.5f * Random(), 1.3f + 0.4f * Random(), 0.5f + 0.3f * Random() };
    Loose c;
    c.position = Vector3Transform(Vector3Add(rig.ejectionPort, { 0.0f, 0.0f, 0.02f }), gunToCam);
    c.velocity = Vector3Transform(velGun, rot);
    c.rotation = MatrixMultiply(MatrixRotateY(PI / 2), rot); // lying across the port, mouth forward-right
    c.spinAxis = Vector3Normalize(Vector3Transform({ 0.2f * Random(), 1.0f, 0.3f }, rot));
    c.spinRate = 25.0f + 10.0f * Random();
    c.age = 0.0f;
    c.lifetime = 0.5f;
    c.kind = DEBRIS_RIFLE_CASE;
    loose.push_back(c);
}

void RifleView::Update(float dt, const ViewModelMotion& motion, const Weapon& weapon, const Camera3D* worldCamera) {
    UpdateMotion(dt, motion, weapon, true);
    int ammo = weapon.GetAmmo();
    bool reloading = weapon.IsReloading();
    if (lastAmmo >= 0 && !reloading && ammo == lastAmmo - 1) OnShot(weapon);
    lastAmmo = ammo;
    StepSprings(dt);
    sinceDry += dt;

    // The bolt: slams back and returns with each shot; stays back on an empty magazine
    if (!reloading) {
        float cycle = RampIn(sinceShot, 0.0f, 0.022f) * (1.0f - Ramp(sinceShot, 0.032f, 0.072f));
        mech.bolt = ammo == 0 ? RampIn(sinceShot, 0.0f, 0.022f) : cycle;
        mech.charger = 0.0f;
        mech.magInGun = true;
        mech.magTransform = MatrixIdentity();
        mech.magLoaded = ammo > 0;
    }
    if (casePending && sinceShot >= 0.018f) {
        EjectCase(weapon);
        casePending = false;
    }
    float pull = sinceShot < 0.05f ? 1.0f : Clamp(1.0f - (sinceShot - 0.05f) / 0.06f, 0.0f, 1.0f);
    float dry = sinceDry < 0.08f ? Ramp(sinceDry, 0.0f, 0.05f) : Clamp(1.0f - (sinceDry - 0.08f) / 0.08f, 0.0f, 1.0f);
    mech.trigger = fmaxf(pull, dry);

    if (reloading) {
        float t = ReloadSeconds(weapon);
        if (!wasReloading) {
            reloadTime = 0.0f;
            magDropped = newMag = false;
        }
        AnimateReload(t, weapon);
        reloadTime = t;
    } else if (wasReloading) {
        // Done (or cut short): magazine home, bolt closed
        reloadPose = ReloadPose{};
        mech.bolt = ammo == 0 ? 1.0f : 0.0f;
        mech.charger = 0.0f;
        sinceShot = 10.0f;
    }
    wasReloading = reloading;
    UpdateLoose(dt, worldCamera);
}

void RifleView::AnimateReload(float t, const Weapon& weapon) {
    float prev = reloadTime;
    auto crossed = [&](float at) { return prev < at && t >= at; };
    bool empty = weapon.IsEmptyReload();

    const PoseKey* keys = SWAP_KEYS;
    int count = COUNT(SWAP_KEYS);
    if (t >= SWAP_END) {
        keys = empty ? EMPTY_KEYS : TACTICAL_KEYS;
        count = empty ? COUNT(EMPTY_KEYS) : COUNT(TACTICAL_KEYS);
    }
    reloadPose = { PoseCurve(keys, count, t, 0), PoseCurve(keys, count, t, 1), PoseCurve(keys, count, t, 2),
                   PoseCurve(keys, count, t, 3), PoseCurve(keys, count, t, 4), PoseCurve(keys, count, t, 5) };

    // Where the magazine would be if the hand were carrying it
    bool hand = HandDriven(weapon);
    Vector3 inHand = hand ? Vector3Subtract(LeftPalmInGun(weapon), PALM_ON_MAG) : Vector3{ 0.0f, -0.25f * Ramp(t, 0.46f, 0.62f), 0.0f };

    // Out: thumb the release, rock it forward off the hook, pull it down and let go
    if (crossed(0.33f)) events.sounds.push_back(ViewSound::MagOut);
    if (t < 0.62f) {
        float rock = MAG_ROCK * Ramp(t, 0.32f, 0.46f);
        Vector3 off = t < 0.46f ? Vector3{ 0.0f, -0.010f * Ramp(t, 0.32f, 0.46f), 0.0f } : inHand;
        if (hand && t >= 0.46f) off = Vector3Add(off, { 0.0f, -0.010f, 0.0f });
        mech.magInGun = true;
        mech.magLoaded = weapon.GetAmmo() > 0;
        mech.magTransform = MatrixMultiply(FalMagazineRock(rock, 0.0f), MatrixTranslate(off.x, off.y, off.z));
    } else if (!magDropped) {
        // Let go: it tumbles away and lands in the world
        magDropped = true;
        Matrix gunToCam = GunToCamera(weapon);
        Matrix m = MatrixMultiply(mech.magTransform, gunToCam);
        Vector3 centre = Vector3Transform({ 0.0f, -0.08f, -0.0765f }, m);
        Matrix rot = m;
        rot.m12 = rot.m13 = rot.m14 = 0.0f;
        Loose c;
        c.position = centre;
        c.velocity = Vector3Transform({ 0.3f * Random(), -1.2f, 0.4f }, rot);
        c.rotation = rot;
        c.spinAxis = Vector3Normalize(Vector3Transform({ 1.0f, 0.2f * Random(), 0.3f * Random() }, rot));
        c.spinRate = 4.0f + 2.0f * Random();
        c.age = 0.0f;
        c.lifetime = 0.6f;
        c.kind = DEBRIS_RIFLE_MAG;
        loose.push_back(c);
        mech.magInGun = false;
    }

    // In: a fresh magazine comes up in the hand, hooks in at the front and rocks home
    if (t >= 1.05f && t < SWAP_END) {
        newMag = true;
        mech.magInGun = true;
        mech.magLoaded = true;
        Vector3 below = hand ? inHand : Vector3{ 0.0f, -0.20f * (1.0f - Ramp(t, 1.05f, 1.40f)), 0.0f };
        float onHook = Ramp(t, 1.28f, 1.42f);
        Vector3 off = Vector3Lerp(below, { 0, 0, 0 }, onHook);
        float rock = MAG_ROCK * (1.0f - RampIn(t, 1.42f, 1.58f));
        mech.magTransform = MatrixMultiply(FalMagazineRock(rock, 0.0f), MatrixTranslate(off.x, off.y, off.z));
    } else if (t >= 0.62f && t < 1.05f) {
        mech.magInGun = false;
    } else if (t >= SWAP_END) {
        mech.magInGun = true;
        mech.magLoaded = true;
        mech.magTransform = MatrixIdentity();
    }
    if (crossed(1.58f)) events.sounds.push_back(ViewSound::MagIn);
    if (crossed(1.73f)) events.sounds.push_back(ViewSound::Slap);

    // The bolt: held back from empty until the charging handle is pulled and let go
    if (empty) {
        mech.charger = Ramp(t, 2.62f, 2.85f) * (1.0f - RampIn(t, 2.90f, 2.93f));
        mech.bolt = 1.0f - RampIn(t, 2.90f, 2.935f);
        if (crossed(2.62f)) events.sounds.push_back(ViewSound::ChargeBack);
        if (crossed(2.91f)) events.sounds.push_back(ViewSound::ChargeRelease);
        if (crossed(2.92f)) { kickBack.v += 0.25f; kickPitch.v += 40.0f; }
    } else {
        mech.bolt = 0.0f;
        mech.charger = 0.0f;
    }
}

void RifleView::Draw(const Weapon& weapon) const {
    Camera3D cam = BeginViewPass();
    Matrix gunToCam = GunToCamera(weapon);
    BeginMode3D(cam);
        rlPushMatrix();
            rlMultMatrixf(MatrixToFloat(gunToCam));
            DrawFal(mech);
            Render::SurfaceObject(0.35f);
        rlPopMatrix();
        if (HaveArms()) {
            int frame = CharacterFrameAt(*arms, CharAnim::FpRifle, ArmsFrame(weapon) / FPR_LAST_FRAME);
            DrawCharacter(*arms, CharAnim::FpRifle, frame, ArmsToCamera(weapon), PART_FOREARMS, WHITE);
        }
        DrawLoose(); // cases flying out of the port, the dropped magazine

        if (weapon.IsMuzzleFlashing()) {
            // The flash hider's prongs break the flash up into a star
            Vector3 muzzle = Vector3Transform(Vector3Add(models->fpMuzzle, { 0, 0, -0.05f }), gunToCam);
            float size = Lerp(0.38f, 0.24f, weapon.GetAimAmount());
            float spin = (float)(weapon.GetAmmo() * 53 % 360);
            Fx::DrawFlashSprite(cam, muzzle, size, spin, 1.0f);
            Fx::DrawFlashSprite(cam, Vector3Transform(Vector3Add(models->fpMuzzle, { 0, 0, -0.12f }), gunToCam), size * 0.6f,
                                spin + 30.0f, 0.7f);
        }
    EndMode3D();
}
