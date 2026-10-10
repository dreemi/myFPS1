#pragma once
#include "raylib.h"
#include "world.hpp"
#include <cmath>

// One frame of player intent. Kept separate from the keyboard/mouse so the
// movement code can be driven by tests as well as by real input.
struct PlayerInput {
    Vector2 look = { 0, 0 };  // mouse delta in pixels
    bool forward = false, back = false, left = false, right = false;
    bool sprint = false;
    bool crouch = false;      // held
    bool leanLeft = false;    // held
    bool leanRight = false;   // held
    bool jump = false;        // true on the frame jump was pressed
    // Absolute view angles (radians) instead of a mouse delta: how networked commands
    // carry the view, so the look is applied exactly as the player saw it
    bool hasAim = false;
    float aimYaw = 0.0f, aimPitch = 0.0f;
};

// What the player's body is doing; drives weapon accuracy
enum class Stance { Still, Walking, Sprinting, CrouchStill, CrouchMoving, Airborne };

// Everything that decides where the player goes next: saved and restored for
// multiplayer prediction (replaying inputs on top of the host's answer)
struct PlayerState {
    Vector3 feet{};
    float yaw = 0.0f, pitch = 0.0f;
    Vector3 velocity{};
    float velocityY = 0.0f;
    float airTickTimer = 0.0f;
    bool grounded = false, crouching = false;
    float leanAmount = 0.0f, eyeHeight = 1.7f;
    int health = 100;
    Stance stance = Stance::Still;
};

class Player {
public:
    void Init(Vector3 feetPosition, float facingYaw = 0.0f); // also used to respawn
    void Update(float dt, const World& world);  // reads keyboard + mouse
    void Simulate(float dt, const PlayerInput& input, const World& world);

    static PlayerInput ReadInput();
    // The view angles a mouse delta would turn to (without applying them)
    void LookAngles(Vector2 mouseDelta, float& yawOut, float& pitchOut) const;
    float GetYaw() const { return yaw; }
    float GetPitch() const { return pitch; }

    PlayerState GetState() const;
    void SetState(const PlayerState& s);
    void SetHealth(int h) { health = h; }

    const Camera3D& GetCamera() const { return camera; }
    Vector3 GetLookDirection() const;
    Vector3 GetFeet() const { return feet; }
    bool IsGrounded() const { return grounded; }
    bool IsCrouching() const { return crouching; }
    Stance GetStance() const { return stance; }
    Vector3 GetVelocity() const { return velocity; }  // horizontal (x, z)
    float GetVerticalVelocity() const { return velocityY; }
    float GetSpeed() const { return sqrtf(velocity.x * velocity.x + velocity.z * velocity.z); }
    float GetLean() const { return leanAmount; } // -1 = full left, 0 = upright, 1 = full right

    // Health (Init restores it)
    bool TakeDamage(int amount); // returns true if this killed the player
    int GetHealth() const { return health; }
    int GetMaxHealth() const { return maxHealth; }
    bool IsAlive() const { return health > 0; }
    Vector3 GetRight() const;                     // flat right direction
    float GetEyeHeight() const { return currentEyeHeight; } // above the feet
    float GetHeight() const { return crouching ? crouchHeight : height; }

    // Zoom: 0 = normal view, 1 = fully zoomed (aiming down sights).
    // Mouse sensitivity scales with the field of view so aiming stays steady.
    void SetZoom(float amount);

private:
    bool OverlapsXZ(Vector3 pos, const BoundingBox& b) const;
    bool CollidesAt(Vector3 feet, const World& world) const;
    float FloorBelow(Vector3 pos, const World& world) const;
    float CeilingAbove(Vector3 pos, const World& world) const;
    void AirAccelerate(Vector3 wishDir, float wishSpeed, float tick);
    Vector3 HeadPosition(float lean) const;
    void UpdateCamera();
    bool HeadBlocked(float lean, const World& world) const;

    Camera3D camera{};
    Vector3 feet{};       // position on the ground; camera sits eyeHeight above
    float yaw = 0.0f;     // radians, rotation around Y
    float pitch = 0.0f;   // radians, look up/down
    float velocityY = 0.0f;
    Vector3 velocity = { 0, 0, 0 }; // horizontal; y unused (see velocityY)
    float airTickTimer = 0.0f;      // time banked toward the next air-acceleration tick
    bool grounded = false;
    bool crouching = false;
    Stance stance = Stance::Still;
    float leanAmount = 0.0f;
    int maxHealth = 100;
    int health = 100;
    float currentEyeHeight = 1.7f; // eases between standing and crouched
    float stepSmooth = 0.0f;       // camera lag (m) after stepping up or down a stair, eases to 0

public:
    // Movement tuning (public so tests can derive expected numbers)
    float stepHeight = 0.45f;      // walking climbs (and descends) ledges up to this high: stairs
    float walkSpeed = 5.0f;
    float sprintSpeed = 8.5f;
    float strafeScale = 0.9f;      // sideways top speed vs forward
    float backpedalScale = 0.8f;   // backwards top speed vs forward
    float groundAccel = 40.0f;     // m/s^2: walk speed in 0.125 s, sprint in ~0.2 s
    float groundDecel = 30.0f;     // m/s^2 with no keys held: walk -> stop in ~0.17 s
                                   // (pressing the opposite way brakes at accel + decel)
    // Air strafing (Source-style, see AirAccelerate in player.cpp)
    float airAccelerate = 10.0f;   // like sv_airaccelerate: how hard the keys push in the air
    float airWishSpeedCap = 0.57f; // keys only push until your speed *along* them reaches this
                                   // (Source's 30 units/s in meters)
                                   // (bigger = more direct air control, less like Source)
    float maxAirSpeed = 11.0f;     // air strafing can't push horizontal speed past this
    float airTickRate = 66.0f;     // air acceleration runs at this fixed rate (Source's tick)
private:
    float mouseSensitivity = 0.0025f;
    float baseFovy = 75.0f;
    float zoomFovy = 55.0f;
    float radius = 0.35f;
    float eyeHeight = 1.7f;
    float height = 1.8f;  // top of the head, for ceiling hits
    float crouchSpeed = 2.5f;
    float crouchEyeHeight = 1.1f;
    float crouchHeight = 1.2f;  // fully hidden behind the 1.2 m+ cover
    float crouchEaseRate = 12.0f; // how fast the camera moves down/up
    float leanDistance = 0.45f;   // head moves this far sideways at full lean
    float leanDrop = 0.06f;       // and this far down (leaning bends at the waist)
    float leanRollDegrees = 12.0f;
    float leanEaseRate = 10.0f;
    float jumpSpeed = 8.0f;  // with gravity 20 this gives a 1.6 m jump
    float gravity = 20.0f;
};
