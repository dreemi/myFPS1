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
    return m;
}

static float Ease(float current, float target, float rate, float dt) {
    return current + (target - current) * (1.0f - expf(-rate * dt));
}

void ViewModel::Update(float dt, const ViewModelMotion& motion, const Weapon& weapon) {
    (void)weapon;
    time += dt;

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
}

Matrix ViewModel::GunToCamera(const Weapon& weapon) const {
    const WeaponModels& m = *models;
    float aim = weapon.GetAimAmount();
    float idleScale = 1.0f - aim;          // fully aimed: rock steady
    float bobScale = 1.0f - 0.85f * aim;   // aimed and walking: a little sway

    // Hip -> aimed: slide to the center with the sights on the line of sight
    Vector3 aimOffset = { 0.0f, -m.frontSightTop.y, -m.aimDistance };
    Vector3 offset = Vector3Lerp(m.hipOffset, aimOffset, aim);
    float yaw = Lerp(m.hipYawDegrees, 0.0f, aim);
    float pitch = 0.0f, roll = 0.0f;

    // Idle: slow breathing
    offset.y += sinf(time * 1.7f) * 0.0025f * idleScale;
    pitch += sinf(time * 1.7f + 0.5f) * 0.4f * idleScale;

    // Walking: figure-eight bob (side to side once per two steps, down each step)
    float b = bobAmount * bobScale;
    offset.x += sinf(bobPhase) * 0.012f * b;
    offset.y += (cosf(2.0f * bobPhase) - 1.0f) * 0.005f * b;
    roll += sinf(bobPhase) * 1.5f * b;

    // Jumping/falling and landing
    offset.y += airOffset * (1.0f - 0.7f * aim) - landKick;

    // Crouching: tucked a little lower and in, slightly canted
    offset = Vector3Add(offset, Vector3Scale({ -0.010f, -0.012f, 0.010f }, crouchAmount * idleScale));
    roll -= 3.0f * crouchAmount * idleScale;

    // Firing: kick back and muzzle up (less when aiming)
    float kick = weapon.GetRecoil() * Lerp(1.0f, 0.5f, aim);
    offset.z += 0.035f * kick;
    pitch += 9.0f * kick;

    // Reloading: dip down and roll the gun over
    if (weapon.IsReloading()) {
        float dip = sinf(PI * weapon.GetReloadProgress());
        offset.y -= 0.09f * dip;
        pitch -= 30.0f * dip;
        roll += 18.0f * dip;
        yaw += 10.0f * dip;
    }

    // Applied left to right: roll, pitch, yaw, then move into place
    Matrix rot = MatrixMultiply(MatrixMultiply(MatrixRotateZ(roll * DEG2RAD), MatrixRotateX(pitch * DEG2RAD)),
                                MatrixRotateY(yaw * DEG2RAD));
    return MatrixMultiply(rot, MatrixTranslate(offset.x, offset.y, offset.z));
}

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
    pose.slideBack = Clamp(weapon.GetRecoil() * 1.5f, 0.0f, 1.0f);
    pose.reloading = weapon.IsReloading();
    pose.reload = weapon.GetReloadProgress();
    pose.muzzleFlash = weapon.IsMuzzleFlashing();
    pose.aim = weapon.GetAimAmount();
    bool realArms = arms && arms->loaded;
    pose.boxHands = !realArms;

    BeginMode3D(cam);
        rlPushMatrix();
            rlMultMatrixf(MatrixToFloat(GunToCamera(weapon)));
            models->drawFirstPerson(pose);
            if (realArms) {
                // The character's arms in their first-person pose, placed so the right
                // palm sits on the grip. Character space faces +Z; gun space fires
                // along -Z, hence the half turn. Blender frames of fp_hold:
                // 0 hip, 1..30 reload, 31..40 hip -> aiming down sights.
                float blenderFrame = 0.0f;
                if (weapon.IsReloading()) blenderFrame = 1.0f + 29.0f * weapon.GetReloadProgress();
                else if (weapon.GetAimAmount() > 0.0f) blenderFrame = 31.0f + 9.0f * weapon.GetAimAmount();
                int frame = CharacterFrameAt(*arms, CharAnim::FpHold, blenderFrame / 40.0f);
                Vector3 g = CharacterGripPoint(*arms, CharAnim::FpHold, frame);
                Vector3 h = models->handGrip;
                Matrix toGun = MatrixMultiply(MatrixMultiply(MatrixTranslate(-g.x, -g.y, -g.z), MatrixRotateY(PI)),
                                              MatrixTranslate(h.x, h.y, h.z));
                DrawCharacter(*arms, CharAnim::FpHold, frame, toGun, PART_FOREARMS, WHITE);
            }
        rlPopMatrix();
        if (weapon.IsMuzzleFlashing()) {
            // Smaller when aiming so it doesn't hide what you're shooting at
            Vector3 muzzle = Vector3Transform(Vector3Add(models->fpMuzzle, { 0, 0, -0.04f }), GunToCamera(weapon));
            float size = Lerp(0.30f, 0.16f, weapon.GetAimAmount());
            Fx::DrawFlashSprite(cam, muzzle, size, (float)(weapon.GetAmmo() * 67 % 360), 1.0f);
        }
    EndMode3D();
}
