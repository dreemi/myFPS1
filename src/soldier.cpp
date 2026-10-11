#include "soldier.hpp"
#include "weapon_models.hpp"
#include "helmet.hpp"
#include "raymath.h"
#include "rlgl.h"

static const float GRIP_ABOVE_PALM = 0.05f; // gun origin (top of the grip) above the palm
static const float HIT_REACT_TIME = 0.18f;
static const float FALL_TIME = 0.55f;

static const WeaponModels& ModelsOf(const SoldierView& s) {
    return s.weapon == WEAPON_SNIPER ? SNIPER_MODELS : (s.weapon == WEAPON_RIFLE ? FAL_MODELS : REVOLVER_MODELS);
}

CharAnim SoldierAnim(float speed, bool crouching, bool grounded) {
    if (crouching) return speed > 0.3f ? CharAnim::CrouchWalk : CharAnim::Crouch;
    if (!grounded) return CharAnim::Idle;
    if (speed > 6.0f) return CharAnim::Run;
    if (speed > 0.3f) return CharAnim::Walk;
    return CharAnim::Idle;
}

float SoldierStride(CharAnim anim) {
    switch (anim) {
        case CharAnim::Run: return 2.2f;
        case CharAnim::CrouchWalk: return 1.0f;
        default: return 1.4f;
    }
}

// The body's model -> world: turned to face, tipped over when hit or dead, sunk away late in death
static Matrix BodyTransform(const SoldierView& s) {
    Vector3 axis = Vector3CrossProduct({ 0, 1, 0 }, s.fallDir);
    float tilt = 0.0f, sink = 0.0f;
    if (s.alive) {
        tilt = 9.0f * (s.hitReact / HIT_REACT_TIME);
    } else {
        float t = s.deathTime;
        float fall = fminf(1.0f, t / FALL_TIME);
        tilt = 90.0f * fall * fall;                          // accelerate into the ground
        if (t > FALL_TIME && t < FALL_TIME + 0.2f) tilt -= 6.0f * sinf((t - FALL_TIME) / 0.2f * PI); // small bounce
        sink = t > 2.0f ? (t - 2.0f) * 1.0f : 0.0f;          // then sink away before respawning
    }
    Matrix m = MatrixRotateY(s.yaw);
    if (tilt != 0.0f && Vector3LengthSqr(axis) > 1e-6f) m = MatrixMultiply(m, MatrixRotate(Vector3Normalize(axis), tilt * DEG2RAD));
    return MatrixMultiply(m, MatrixTranslate(s.feet.x, s.feet.y - sink, s.feet.z));
}

Matrix SoldierGunTransform(const CharacterAssets* character, const SoldierView& s) {
    Vector3 origin;
    if (character && character->loaded) {
        int frame = CharacterFrame(*character, s.anim, s.animPhase);
        Vector3 palm = Vector3Transform(CharacterGripPoint(*character, s.anim, frame), BodyTransform(s));
        origin = Vector3Add(palm, { 0, GRIP_ABOVE_PALM, 0 });
    } else {
        Vector3 f = { sinf(s.yaw), 0, cosf(s.yaw) };
        origin = Vector3Add(Vector3Add(s.feet, { 0, s.eyeHeight - 0.3f, 0 }), Vector3Scale(f, 0.45f));
    }
    // Gun space fires along -Z: turn it to face, then tip the muzzle to the aim
    Matrix m = MatrixMultiply(MatrixRotateX(s.pitch), MatrixRotateY(s.yaw + PI));
    return MatrixMultiply(m, MatrixTranslate(origin.x, origin.y, origin.z));
}

Vector3 SoldierMuzzle(const CharacterAssets* character, const SoldierView& s) {
    return Vector3Transform(ModelsOf(s).worldMuzzle, SoldierGunTransform(character, s));
}

Matrix SoldierHelmetTransform(const CharacterAssets* character, const SoldierView& s) {
    if (character && character->loaded) {
        CharAnim anim = s.alive ? s.anim : CharAnim::Rest;
        int frame = CharacterFrame(*character, anim, s.animPhase);
        return MatrixMultiply(MatrixMultiply(HelmetOnHead(*character, anim, frame), character->model.transform), BodyTransform(s));
    }
    Vector3 head = Vector3Add(s.feet, { 0, s.eyeHeight + 0.1f, 0 });
    return MatrixMultiply(MatrixRotateY(s.yaw), MatrixTranslate(head.x, head.y, head.z));
}

void DrawSoldier(CharacterAssets* character, const SoldierView& s) {
    if (character && character->loaded) {
        CharAnim anim = s.alive ? s.anim : CharAnim::Rest; // a body lets go of the gun
        int frame = CharacterFrame(*character, anim, s.animPhase);
        DrawCharacter(*character, anim, frame, BodyTransform(s), PART_ALL, s.hitFlash > 0.0f ? Color{ 255, 150, 150, 255 } : WHITE);
        if (s.helmet) DrawHelmet(SoldierHelmetTransform(character, s));
        if (!s.alive) return; // dropped the gun
        rlPushMatrix();
            rlMultMatrixf(MatrixToFloat(SoldierGunTransform(character, s)));
            ModelsOf(s).drawWorld(s.muzzleFlash);
        rlPopMatrix();
        return;
    }
    if (!s.alive) return;
    // No character model: simple shapes
    Color body = s.hitFlash > 0.0f ? RED : (s.bot ? Color{ 140, 70, 180, 255 } : Color{ 70, 120, 180, 255 });
    float h = s.crouching ? 1.2f : 1.8f;
    DrawCubeV(Vector3Add(s.feet, { 0, h * 0.4f, 0 }), { 0.5f, h * 0.8f, 0.5f }, body);
    DrawSphere(Vector3Add(s.feet, { 0, s.eyeHeight, 0 }), 0.15f, s.hitFlash > 0.0f ? RED : Color{ 230, 200, 180, 255 });
    if (s.helmet) DrawHelmet(SoldierHelmetTransform(character, s));
    rlPushMatrix();
        rlMultMatrixf(MatrixToFloat(SoldierGunTransform(character, s)));
        ModelsOf(s).drawWorld(s.muzzleFlash);
    rlPopMatrix();
}
