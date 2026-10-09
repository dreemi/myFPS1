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

// Mechanical sounds a viewmodel's animation makes (played by the game)
enum class ViewSound {
    Latch, Open, Eject, Insert, Spin, Close,       // revolver reload
    MagOut, MagIn, Slap, ChargeBack, ChargeRelease // rifle reload
};

// Things the viewmodel's animation did this frame that the rest of the game
// reacts to (sounds, world effects)
struct ViewModelEvents {
    std::vector<ViewSound> sounds;
    struct Debris { Vector3 position, velocity; int kind; }; // kind: DebrisKind (weapon_models.hpp)
    std::vector<Debris> debris;    // cases and magazines that fell out of view: carry on in the world
    bool Has(ViewSound s) const {
        for (ViewSound x : sounds) if (x == s) return true;
        return false;
    }
};

// fp_* animation frame layout (Blender frames) for one weapon's arms
struct ArmsClip {
    CharAnim anim;
    float lastFrame;     // the animation's last Blender frame
    float adsFirst;      // hip -> aiming down sights runs adsFirst..lastFrame
};

// A first-person weapon. It lives in camera space (never in the world), is
// drawn in its own pass after the world with its own camera and field of view,
// and so can't clip into walls or be hidden by them. It only reads the
// Weapon's state; it never changes gameplay. This base holds what every
// weapon shares: movement sway and bob, recoil springs, things falling out of
// the gun, the character's arms holding it, and switching (holstering).
class WeaponView {
public:
    explicit WeaponView(const WeaponModels& models) : models(&models) {}
    virtual ~WeaponView() = default;

    // Advance animations. worldCamera (optional) lets falling cases hand over to the world.
    virtual void Update(float dt, const ViewModelMotion& motion, const Weapon& weapon, const Camera3D* worldCamera = nullptr) = 0;
    virtual void DryFire() {}             // the trigger clicked on an empty chamber
    ViewModelEvents TakeEvents();         // and clears them

    // Gun space -> camera space (+X right, +Y up, -Z ahead) for this frame
    virtual Matrix GunToCamera(const Weapon& weapon) const;
    // Character model space -> camera space for the first-person arms this frame
    Matrix ArmsToCamera(const Weapon& weapon) const;
    // A point in the viewmodel's camera space -> the world point that appears at the same place on screen
    Vector3 CameraToWorld(Vector3 cameraPoint, const Camera3D& worldCamera) const;
    Vector3 MuzzleCameraSpace(const Weapon& weapon) const;

    // Separate pass: call after EndMode3D() of the world, before 2D HUD
    virtual void Draw(const Weapon& weapon) const = 0;

    float fovy = 68.0f;   // the viewmodel's own field of view (unaffected by aim zoom)
    CharacterAssets* arms = nullptr; // the character's arms hold the gun (box hands if null)
    float holster = 0.0f; // 0 = in hand, 1 = lowered out of sight (switching weapons)

protected:
    struct Spring {
        float x = 0.0f, v = 0.0f;
        void Step(float dt, float stiffness, float damping);
    };
    struct Loose {                    // something tumbling out of the gun, camera space
        Vector3 position, velocity;
        Matrix rotation;              // orientation only
        Vector3 spinAxis;
        float spinRate;               // rad/s
        float age, lifetime;          // hands over to the world after lifetime (or once out of view)
        int kind;                     // DebrisKind
    };
    struct ReloadPose { float x, y, z, pitch, yaw, roll; };

    // Shared per-frame motion (bob, sway, sprint, landing...) and recoil springs
    void UpdateMotion(float dt, const ViewModelMotion& motion, const Weapon& weapon, bool canSprint);
    void StepSprings(float dt);
    void UpdateLoose(float dt, const Camera3D* worldCamera);
    void DrawLoose() const;
    float Random(); // -1..1

    // The arms' animation for this weapon, and which of its frames to show
    virtual ArmsClip Clip() const = 0;
    virtual float ArmsFrame(const Weapon& weapon) const = 0;
    virtual Vector3 ArmsGrip(const Weapon& weapon) const; // model-space palm pinned to the grip
    bool HandDriven(const Weapon& weapon) const;          // the reload moves the gun with the right hand
    bool HaveArms() const { return arms && arms->loaded; }
    // Gun placement before the arms' reload handling; extra = per-weapon offsets (fallback reload keys)
    Matrix BaseGunToCamera(const Weapon& weapon) const;
    virtual ReloadPose FallbackReloadPose() const { return {}; }
    Camera3D BeginViewPass() const; // depth cleared, camera at the origin: call before BeginMode3D

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
    Vector3 lastLook = { 0, 0, 0 };
    float swayYaw = 0.0f, swayPitch = 0.0f, strafeLag = 0.0f;
    Spring kickBack, kickUp, kickPitch, kickYaw, kickRoll;
    float sinceShot = 10.0f;
    unsigned noise = 12345;
    std::vector<Loose> loose;
    Vector3 gravity = { 0, -9.81f, 0 }; // camera space
    ViewModelEvents events;
};

// The revolver: hammer, trigger, cylinder, crane, ejector and cartridges are
// animated here from the weapon's state.
class ViewModel : public WeaponView {
public:
    explicit ViewModel(const WeaponModels& models) : WeaponView(models) {}

    void Update(float dt, const ViewModelMotion& motion, const Weapon& weapon, const Camera3D* worldCamera = nullptr) override;
    void DryFire() override;
    const RevolverPose& Mechanism() const { return mech; }
    void Draw(const Weapon& weapon) const override;

private:
    void OnShot(const Weapon& weapon);
    void SyncChambers(int ammo);
    void AnimateReload(float t, float dt, const Weapon& weapon);
    void ReleaseCases(const Weapon& weapon);
    ArmsClip Clip() const override;
    float ArmsFrame(const Weapon& weapon) const override;
    ReloadPose FallbackReloadPose() const override { return reloadPose; }

    RevolverPose mech;
    int lastAmmo = -1;
    float cylinderIndex = 0.0f;  // where the cylinder is (or is going), in whole chambers
    float cylinderFrom = 0.0f;   // ...and where the last turn started
    float sinceDry = 10.0f;
    bool wasReloading = false;
    float reloadTime = 0.0f;     // seconds into the current reload
    float spinRate = 0.0f;       // chambers per second while flicked spinning
    bool casesReleased = false, roundsSeated = false;
    ReloadPose reloadPose{};
};
