#pragma once
#include "raylib.h"
#include "weapon.hpp"
#include "weapon_models.hpp"
#include "character.hpp"
#include <vector>

// How the local player is moving, for viewmodel animation
struct ViewModelMotion {
    float speed = 0.0f;            // horizontal m/s
    float walkSpeed = 5.0f;
    bool grounded = true;
    float verticalVelocity = 0.0f; // m/s, + is up
    bool crouching = false;
    float strafe = 0.0f;           // sideways speed / walk speed, + = moving right
    Vector3 look = { 0, 0, 1 };    // view direction (world), for sway when turning
    Vector3 right = { -1, 0, 0 };  // flat right (world)
};
ViewModelMotion MotionOf(const Player& player);

// Things the viewmodel's animation did this frame that the rest of the game
// reacts to (sounds, world effects)
struct ViewModelEvents {
    bool latch = false, open = false, eject = false, insert = false, spin = false, close = false; // reload steps
    struct Casing { Vector3 position, velocity; bool fired; };
    std::vector<Casing> casings;   // cases that fell out of view: carry on in the world (world space)
};

// The local player's first-person weapon. It lives in camera space (never in
// the world), is drawn in its own pass after the world with its own camera and
// field of view, and so can't clip into walls or be hidden by them. It only
// reads the Weapon's state; it never changes gameplay. The revolver's
// mechanism (hammer, trigger, cylinder, crane, ejector, cartridges) is animated
// here from that state.
class ViewModel {
public:
    explicit ViewModel(const WeaponModels& models) : models(&models) {}

    // Advance animations. worldCamera (optional) lets ejected cases hand over to the world.
    void Update(float dt, const ViewModelMotion& motion, const Weapon& weapon, const Camera3D* worldCamera = nullptr);
    void DryFire();                       // the trigger clicked on an empty chamber
    ViewModelEvents TakeEvents();         // and clears them

    // Gun space -> camera space (+X right, +Y up, -Z ahead) for this frame
    Matrix GunToCamera(const Weapon& weapon) const;
    // Character model space -> camera space for the first-person arms this frame
    Matrix ArmsToCamera(const Weapon& weapon) const;
    // A point in the viewmodel's camera space -> the world point that appears at the same place on screen
    Vector3 CameraToWorld(Vector3 cameraPoint, const Camera3D& worldCamera) const;
    Vector3 MuzzleCameraSpace(const Weapon& weapon) const;
    const RevolverPose& Mechanism() const { return mech; }

    // Separate pass: call after EndMode3D() of the world, before 2D HUD
    void Draw(const Weapon& weapon) const;

    float fovy = 68.0f;  // the viewmodel's own field of view (unaffected by aim zoom)
    CharacterAssets* arms = nullptr; // the character's arms hold the gun (box hands if null)

private:
    struct Spring {
        float x = 0.0f, v = 0.0f;
        void Step(float dt, float stiffness, float damping);
    };
    struct LooseCase {
        Vector3 position, velocity;   // camera space
        Matrix rotation;              // case space -> camera space orientation
        Vector3 spinAxis;
        float spinRate;               // rad/s
        float age;
        bool fired;
    };
    struct ReloadPose { float x, y, z, pitch, yaw, roll; };

    void OnShot(const Weapon& weapon);
    void SyncChambers(int ammo);
    void AnimateReload(float t, float dt, const Weapon& weapon, const Camera3D* worldCamera);
    void ReleaseCases(const Weapon& weapon);
    ReloadPose ReloadOffset() const;
    Matrix BaseGunToCamera(const Weapon& weapon) const; // without the arms' reload handling
    float ArmsFrame(const Weapon& weapon) const;         // Blender frame of fp_hold
    bool HandDriven(const Weapon& weapon) const;        // the reload moves the gun with the right hand

    const WeaponModels* models;
    float time = 0.0f;          // drives idle breathing
    float bobPhase = 0.0f;      // advances with distance walked
    float bobAmount = 0.0f;     // 0 standing still, 1 walking, more sprinting
    float airOffset = 0.0f;     // gun lags behind vertical motion
    float landKick = 0.0f;      // dip on landing
    float crouchAmount = 0.0f;
    float sprintAmount = 0.0f;  // lowered, turned-in carry while sprinting
    bool wasGrounded = true;
    float lastVerticalVelocity = 0.0f;

    // Sway: the gun lags behind turning and strafing
    Vector3 lastLook = { 0, 0, 0 };
    float swayYaw = 0.0f, swayPitch = 0.0f, strafeLag = 0.0f;

    // Recoil: springs kicked by each shot
    Spring kickBack, kickUp, kickPitch, kickYaw, kickRoll;
    unsigned noise = 12345;
    float Random(); // -1..1

    // Mechanism
    RevolverPose mech;
    int lastAmmo = -1;
    float cylinderIndex = 0.0f;  // where the cylinder is (or is going), in whole chambers
    float cylinderFrom = 0.0f;   // ...and where the last turn started
    float sinceShot = 10.0f, sinceDry = 10.0f;
    bool wasReloading = false;
    float reloadTime = 0.0f;     // seconds into the current reload
    float spinRate = 0.0f;       // chambers per second while flicked spinning
    bool casesReleased = false, roundsSeated = false;
    ReloadPose reloadPose{};
    std::vector<LooseCase> loose;
    Vector3 gravity = { 0, -9.81f, 0 }; // camera space
    ViewModelEvents events;
};
