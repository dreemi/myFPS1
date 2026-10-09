#pragma once
#include "raylib.h"
#include "weapon.hpp"
#include "weapon_models.hpp"
#include "character.hpp"

// How the local player is moving, for viewmodel animation
struct ViewModelMotion {
    float speed = 0.0f;            // horizontal m/s
    float walkSpeed = 5.0f;
    bool grounded = true;
    float verticalVelocity = 0.0f; // m/s, + is up
    bool crouching = false;
};
ViewModelMotion MotionOf(const Player& player);

// The local player's first-person weapon. It lives in camera space (never in
// the world), is drawn in its own pass after the world with its own camera and
// field of view, and so can't clip into walls or be hidden by them. It only
// reads the Weapon's state; it never changes gameplay.
class ViewModel {
public:
    explicit ViewModel(const WeaponModels& models) : models(&models) {}

    // Advance animations (bob, jump/land, crouch). Recoil, reload and aiming
    // come straight from the weapon's state.
    void Update(float dt, const ViewModelMotion& motion, const Weapon& weapon);

    // Gun space -> camera space (+X right, +Y up, -Z ahead) for this frame
    Matrix GunToCamera(const Weapon& weapon) const;

    // Separate pass: call after EndMode3D() of the world, before 2D HUD
    void Draw(const Weapon& weapon) const;

    float fovy = 68.0f;  // the viewmodel's own field of view (unaffected by aim zoom)
    CharacterAssets* arms = nullptr; // the character's arms hold the gun (box hands if null)

private:
    const WeaponModels* models;
    float time = 0.0f;          // drives idle breathing
    float bobPhase = 0.0f;      // advances with distance walked
    float bobAmount = 0.0f;     // 0 standing still, 1 walking, more sprinting
    float airOffset = 0.0f;     // gun lags behind vertical motion
    float landKick = 0.0f;      // dip on landing
    float crouchAmount = 0.0f;
    bool wasGrounded = true;
    float lastVerticalVelocity = 0.0f;
};
