#include "sniper_view.hpp"
#include "view_anim.hpp"
#include "effects.hpp"
#include "render.hpp"
#include "raymath.h"
#include "rlgl.h"

using namespace ViewAnim;

// ---------------------------------------------------------------------------
// Working the bolt (seconds after the shot):
//   0.28 the right hand leaves the wrist   0.40..0.46 handle turned up (click)
//   0.50..0.70 drawn back: the case flies out   0.72..0.80 pushed home on a fresh round
//   0.86..0.92 turned down (click)         1.10 back on the wrist, 1.15 ready to fire
// The reload:
//   0.35..0.41 handle up, 0.45..0.65 bolt back (a chambered round is thrown out)
//   0.80 the magazine catch, 0.85..1.00 the magazine pulled down and dropped
//   1.20..1.45 a fresh one from the pouch, 1.85..2.20 pushed up into the well (click)
//   2.60..2.70 bolt home on a round, 2.88..2.94 handle down, 3.40 back on target
// The arms' animation (tools/export_character.py, fp_sniper) has the same keys and poses
// the hands on the gun where these put it; the sounds are cut from the recording so their
// clicks land on these moments.
// ---------------------------------------------------------------------------

static const PoseKey CYCLE_KEYS[] = {
    { 0.00f,   0.000f,  0.000f,  0.000f,   0.0f,   0.0f,   0.0f },
    { 0.30f,   0.000f,  0.000f,  0.000f,   0.0f,   0.0f,   0.0f },
    { 0.42f,  -0.030f,  0.000f, -0.060f, -22.0f, -16.0f,  32.0f },  // muzzle dipped and rolled over: the bolt (under the
    { 0.92f,  -0.030f,  0.000f, -0.060f, -22.0f, -16.0f,  32.0f },  // telescope) comes into sight as it opens
    { 1.15f,   0.000f,  0.000f,  0.000f,   0.0f,   0.0f,   0.0f },
};
static const PoseKey RELOAD_KEYS[] = {
    { 0.00f,   0.000f,  0.000f,  0.000f,   0.0f,   0.0f,   0.0f },
    { 0.30f,  -0.030f,  0.000f, -0.060f, -22.0f, -16.0f,  32.0f },  // the bolt in sight (as working it after a shot)
    { 0.70f,  -0.030f,  0.000f, -0.060f, -22.0f, -16.0f,  32.0f },
    { 0.95f,  -0.020f,  0.020f, -0.140f,  20.0f,   0.0f, -20.0f },  // muzzle up, rolled the other way: the magazine
    { 2.30f,  -0.020f,  0.020f, -0.140f,  20.0f,   0.0f, -22.0f },
    { 2.55f,  -0.030f,  0.000f, -0.060f, -22.0f, -16.0f,  32.0f },  // back over for the bolt
    { 3.00f,  -0.030f,  0.000f, -0.060f, -22.0f, -16.0f,  32.0f },
    { 3.40f,   0.000f,  0.000f,  0.000f,   0.0f,   0.0f,   0.0f },
};
#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

// Where the support hand's palm is (gun space) when it holds the magazine (export_character.py)
static Vector3 PalmOnMag() { return { -0.034f, -0.032f, GetSniperRig().magCentre.z }; }

ArmsClip SniperView::Clip() const { return { CharAnim::FpSniper, FPS_LAST_FRAME, FPS_ADS_FIRST }; }

bool SniperView::WorkingBolt(const Weapon& weapon) const {
    return weapon.IsReloading() || cycleTime >= CYCLE_ARMS_FROM;
}

float SniperView::ArmsFrame(const Weapon& weapon) const {
    if (weapon.IsReloading())
        return Lerp(FPS_RELOAD_FIRST, FPS_RELOAD_LAST, Clamp(weapon.GetReloadProgress(), 0.0f, 1.0f));
    if (cycleTime >= CYCLE_ARMS_FROM)
        return Lerp(FPS_CYCLE_FIRST, FPS_CYCLE_LAST, Clamp(cycleTime / CYCLE_END, 0.0f, 1.0f));
    if (weapon.GetAimAmount() > 0.0f) return Lerp(FPS_ADS_FIRST, FPS_LAST_FRAME, weapon.GetAimAmount());
    return 0.0f;
}

// Working the bolt the right hand leaves the wrist, so the arms are placed by the hold
// they were posed from (frame 0's) rather than by where that hand is now
Vector3 SniperView::ArmsGrip(const Weapon& weapon) const {
    if (WorkingBolt(weapon)) return CharacterGripPoint(*arms, CharAnim::FpSniper, 0);
    return WeaponView::ArmsGrip(weapon);
}

Vector3 SniperView::LeftPalmInGun(const Weapon& weapon) const {
    Vector3 palm = Vector3Transform(CharacterFpLeftPalm(*arms, CharAnim::FpSniper, ArmsFrame(weapon)), ArmsToCamera(weapon));
    return Vector3Transform(palm, MatrixInvert(GunToCamera(weapon)));
}

void SniperView::OnShot(const Weapon& weapon) {
    sinceShot = 0.0f;
    cycleTime = 0.0f;
    // A full-power round from a light rifle: a hard shove into the shoulder and a big flip
    float s = Lerp(1.0f, 0.55f, weapon.GetAimAmount());
    kickPitch.v += 230.0f * s;
    kickBack.v += 1.6f * s;
    kickUp.v += 0.18f * s;
    kickYaw.v += Random() * 30.0f * s;
    kickRoll.v += (10.0f + Random() * 18.0f) * s;
}

// Out of the action to the right: the fired case, or (opening the bolt on a reload) a live round
void SniperView::Eject(const Weapon& weapon, bool live) {
    Matrix gunToCam = GunToCamera(weapon);
    Matrix rot = gunToCam;
    rot.m12 = rot.m13 = rot.m14 = 0.0f;
    const SniperRig& rig = GetSniperRig();
    // A bolt action flicks it out less violently than a self-loader
    Vector3 velGun = { 1.6f + 0.4f * Random(), 1.1f + 0.3f * Random(), 0.4f + 0.2f * Random() };
    if (live) velGun = { 0.9f + 0.2f * Random(), 0.7f, 0.3f };
    Loose c;
    c.position = Vector3Transform(rig.ejectionPort, gunToCam);
    c.velocity = Vector3Transform(velGun, rot);
    c.rotation = MatrixMultiply(MatrixRotateY(PI / 2), rot);
    c.spinAxis = Vector3Normalize(Vector3Transform({ 0.2f * Random(), 1.0f, 0.3f }, rot));
    c.spinRate = (live ? 10.0f : 18.0f) + 8.0f * Random();
    c.age = 0.0f;
    c.lifetime = 0.6f;
    c.kind = live ? DEBRIS_RIFLE_ROUND : DEBRIS_RIFLE_CASE;
    loose.push_back(c);
}

void SniperView::Update(float dt, const ViewModelMotion& motion, const Weapon& weapon, const Camera3D* worldCamera) {
    UpdateMotion(dt, motion, weapon, true);
    int ammo = weapon.GetAmmo();
    bool reloading = weapon.IsReloading();
    if (lastAmmo >= 0 && !reloading && ammo == lastAmmo - 1) OnShot(weapon);
    lastAmmo = ammo;
    StepSprings(dt);
    sinceDry += dt;

    float pull = sinceShot < 0.06f ? 1.0f : Clamp(1.0f - (sinceShot - 0.06f) / 0.08f, 0.0f, 1.0f);
    float dry = sinceDry < 0.08f ? Ramp(sinceDry, 0.0f, 0.05f) : Clamp(1.0f - (sinceDry - 0.08f) / 0.08f, 0.0f, 1.0f);
    mech.trigger = fmaxf(pull, dry);

    if (reloading) {
        float t = weapon.GetReloadProgress() * RELOAD_END;
        if (!wasReloading) {
            reloadTime = 0.0f;
            magDropped = false;
            roundInChamber = ammo > 0;
            cycleTime = -1.0f;
        }
        AnimateReload(t, weapon);
        reloadTime = t;
    } else {
        if (wasReloading) reloadPose = ReloadPose{};
        if (cycleTime >= 0.0f) {
            float prev = cycleTime;
            cycleTime += dt;
            AnimateCycle(prev, cycleTime, weapon);
            if (cycleTime >= CYCLE_END) {
                cycleTime = -1.0f;
                reloadPose = ReloadPose{};
            }
        } else {
            mech.boltLift = mech.boltBack = 0.0f;
            mech.chambered = false;
            mech.magInGun = true;
            mech.magTransform = MatrixIdentity();
            mech.magLoaded = ammo > 0;
        }
    }
    wasReloading = reloading;
    UpdateLoose(dt, worldCamera);
}

// prev..t: this frame's stretch of the script (a moment inside it is crossed)
void SniperView::AnimateCycle(float prev, float t, const Weapon& weapon) {
    auto crossed = [&](float at) { return prev < at && t >= at; };
    reloadPose = { PoseCurve(CYCLE_KEYS, COUNT(CYCLE_KEYS), t, 0), PoseCurve(CYCLE_KEYS, COUNT(CYCLE_KEYS), t, 1),
                   PoseCurve(CYCLE_KEYS, COUNT(CYCLE_KEYS), t, 2), PoseCurve(CYCLE_KEYS, COUNT(CYCLE_KEYS), t, 3),
                   PoseCurve(CYCLE_KEYS, COUNT(CYCLE_KEYS), t, 4), PoseCurve(CYCLE_KEYS, COUNT(CYCLE_KEYS), t, 5) };
    mech.boltLift = Ramp(t, 0.40f, 0.46f) * (1.0f - Ramp(t, 0.86f, 0.92f));
    mech.boltBack = Ramp(t, 0.50f, 0.70f) * (1.0f - RampIn(t, 0.72f, 0.80f));
    mech.magInGun = true;
    mech.magTransform = MatrixIdentity();
    mech.magLoaded = weapon.GetAmmo() > 0;
    mech.chambered = weapon.GetAmmo() > 0 && t >= 0.71f;   // the next round rides the bolt face home
    if (crossed(0.40f)) events.sounds.push_back(ViewSound::BoltOpen);
    if (crossed(0.64f)) Eject(weapon, false);
    if (crossed(0.74f)) events.sounds.push_back(ViewSound::BoltClose);
    if (crossed(0.80f)) { kickBack.v += 0.12f; kickPitch.v += 12.0f; }
}

void SniperView::AnimateReload(float t, const Weapon& weapon) {
    float prev = reloadTime;
    auto crossed = [&](float at) { return prev < at && t >= at; };
    reloadPose = { PoseCurve(RELOAD_KEYS, COUNT(RELOAD_KEYS), t, 0), PoseCurve(RELOAD_KEYS, COUNT(RELOAD_KEYS), t, 1),
                   PoseCurve(RELOAD_KEYS, COUNT(RELOAD_KEYS), t, 2), PoseCurve(RELOAD_KEYS, COUNT(RELOAD_KEYS), t, 3),
                   PoseCurve(RELOAD_KEYS, COUNT(RELOAD_KEYS), t, 4), PoseCurve(RELOAD_KEYS, COUNT(RELOAD_KEYS), t, 5) };

    // The bolt: up and back, held open while the magazine is changed, home and down
    mech.boltLift = Ramp(t, 0.35f, 0.41f) * (1.0f - Ramp(t, 2.88f, 2.94f));
    mech.boltBack = Ramp(t, 0.45f, 0.65f) * (1.0f - RampIn(t, 2.60f, 2.70f));
    mech.chambered = t >= 2.58f || (roundInChamber && t < 0.60f);
    if (crossed(0.35f)) events.sounds.push_back(ViewSound::BoltOpen);
    if (roundInChamber && crossed(0.60f)) Eject(weapon, true);
    if (crossed(2.62f)) events.sounds.push_back(ViewSound::BoltCloseReload);
    if (crossed(2.70f)) { kickBack.v += 0.15f; kickPitch.v += 15.0f; }

    // The magazine: out with the hand and dropped, a new one up into the well
    bool hand = HaveArms();
    Vector3 inHand = hand ? Vector3Subtract(LeftPalmInGun(weapon), PalmOnMag()) : Vector3{ 0.0f, -0.25f, 0.0f };
    if (crossed(0.80f)) events.sounds.push_back(ViewSound::MagOut);
    if (t < 1.00f) {
        Vector3 off = t < 0.85f ? Vector3{ 0, 0, 0 } : (hand ? inHand : Vector3{ 0.0f, -0.08f * Ramp(t, 0.85f, 1.0f), 0.0f });
        mech.magInGun = true;
        mech.magLoaded = weapon.GetAmmo() > (roundInChamber ? 1 : 0);
        mech.magTransform = MatrixTranslate(off.x, off.y, off.z);
    } else if (!magDropped) {
        magDropped = true;
        Matrix m = MatrixMultiply(mech.magTransform, GunToCamera(weapon));
        Matrix rot = m;
        rot.m12 = rot.m13 = rot.m14 = 0.0f;
        Loose c;
        c.position = Vector3Transform(GetSniperRig().magCentre, m);
        c.velocity = Vector3Transform({ 0.2f * Random(), -1.2f, 0.3f }, rot);
        c.rotation = rot;
        c.spinAxis = Vector3Normalize(Vector3Transform({ 1.0f, 0.2f * Random(), 0.3f * Random() }, rot));
        c.spinRate = 4.0f + 2.0f * Random();
        c.age = 0.0f;
        c.lifetime = 0.6f;
        c.kind = DEBRIS_SNIPER_MAG;
        loose.push_back(c);
        mech.magInGun = false;
    }
    if (t >= 1.00f && t < 1.60f) mech.magInGun = false;
    if (t >= 1.60f) {
        mech.magInGun = true;
        mech.magLoaded = true;
        Vector3 below = hand ? inHand : Vector3{ 0.0f, -0.12f * (1.0f - Ramp(t, 1.60f, 1.85f)), 0.0f };
        Vector3 off = Vector3Lerp(below, { 0, 0, 0 }, Ramp(t, 1.85f, 2.20f));
        if (t >= 2.20f) off = { 0, 0, 0 };
        mech.magTransform = MatrixTranslate(off.x, off.y, off.z);
    }
    if (crossed(1.84f)) events.sounds.push_back(ViewSound::SniperMagIn);
}

void SniperView::Draw(const Weapon& weapon) const {
    if (weapon.IsScoped()) return;   // the telescope fills the view (the HUD draws it)
    Camera3D cam = BeginViewPass();
    Matrix gunToCam = GunToCamera(weapon);
    BeginMode3D(cam);
        rlPushMatrix();
            rlMultMatrixf(MatrixToFloat(gunToCam));
            DrawSniper(mech);
            Render::SurfaceObject(0.35f);
        rlPopMatrix();
        if (HaveArms()) {
            int frame = CharacterFrameAt(*arms, CharAnim::FpSniper, ArmsFrame(weapon) / FPS_LAST_FRAME);
            DrawCharacter(*arms, CharAnim::FpSniper, frame, ArmsToCamera(weapon), PART_FOREARMS, WHITE);
        }
        DrawLoose();

        if (weapon.IsMuzzleFlashing()) {
            Vector3 muzzle = Vector3Transform(Vector3Add(models->fpMuzzle, { 0, 0, -0.05f }), gunToCam);
            float spin = (float)(weapon.GetAmmo() * 53 % 360);
            Fx::DrawFlashSprite(cam, muzzle, 0.42f, spin, 1.0f);
        }
    EndMode3D();
}
