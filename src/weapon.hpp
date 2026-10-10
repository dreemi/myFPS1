#pragma once
#include "raylib.h"
#include "world.hpp"
#include "enemy.hpp"
#include "player.hpp"
#include <random>
#include <vector>

// ---------------------------------------------------------------------------
// Weapon gameplay. Nothing in here knows how the weapon looks: the
// first-person viewmodel and the world model (see viewmodel.hpp and
// weapon_models.hpp) only read this state to animate.
// ---------------------------------------------------------------------------

// Fixed stats for one kind of weapon
struct WeaponDef {
    const char* name;
    int bodyDamage;
    int headDamage;
    float fireCooldown;   // seconds between shots
    int magazineSize;
    float reloadTime;     // seconds
    // Spread cone half-angle (degrees) by stance, before aiming
    float spreadAirborne, spreadSprinting, spreadWalking, spreadStill, spreadCrouchMoving, spreadCrouchStill;
    float aimSpreadScale; // aiming down sights multiplies spread by this
    bool automatic = false;       // keeps firing while the trigger is held
    float reloadEmptyTime = 0.0f; // reload from empty (has to be charged), if longer than reloadTime
    float bloomPerShot = 0.0f;    // degrees each shot adds to the spread (sustained fire walks off)

    float HipSpread(Stance stance) const;
};

// Enemies have 100 health.
extern const WeaponDef REVOLVER; // 4 body shots or 2 headshots
extern const WeaponDef FAL;      // 3 body shots or 1 headshot, automatic

// What one shot hit (for effects and sounds; damage is already applied)
struct ShotResult {
    bool hit = false;          // hit anything at all
    bool hitEnemy = false;
    bool headshot = false;
    bool killed = false;
    bool hitTarget = false;    // a practice target
    Vector3 point{};
    Vector3 normal{};          // surface normal (world hits)
    Vector3 direction{};       // the bullet's direction
    Surface surface = Surface::Plain;
};

// What the trigger finger (and the rest) wants this frame
struct WeaponInput {
    bool fireHeld = false;    // trigger held (automatic weapons keep firing)
    bool firePressed = false; // pressed this frame
    bool aim = false;
    bool reload = false;
    static WeaponInput FromKeyboardAndMouse();
};

// The parts of a weapon's state that the host is the authority on (synced to clients)
struct WeaponState {
    int ammo = 0;
    float reloadTimer = 0.0f, reloadDuration = 1.0f;
    bool emptyReload = false;
};

// One weapon being carried: ammo, timers, recoil, spread, aiming
class Weapon {
public:
    explicit Weapon(const WeaponDef& def, unsigned seed = 1);

    // One frame from intent: aiming, reloading, firing. Returns true if a shot went
    // off, with its path (within the spread) in *shot. Says nothing about what it
    // hit: that's for whoever owns the world (the game simulation).
    bool Step(float dt, const WeaponInput& in, Stance stance, const Camera3D& camera, Ray* shot);
    // The shot hit someone: hit marker (bigger if it killed, yellow for the head)
    void RegisterHit(bool killed, bool headshot);
    WeaponState GetState() const { return { ammo, reloadTimer, reloadDuration, emptyReload }; }
    void SetState(const WeaponState& s);

    // Single-player shorthand: reads the keyboard and mouse, Steps, and resolves the shot
    // against the world and the enemies. Returns true if a shot was fired this frame.
    bool Update(float dt, const Camera3D& camera, World& world, std::vector<Enemy>& enemies, Stance stance);
    // Advances timers, recoil and spread without reading input
    void Tick(float dt, Stance stance);
    bool StartReload(); // false if already reloading or the magazine is full
    void CancelReload(); // put away mid-reload: nothing changes
    void Reset();       // a fresh weapon: full, not reloading, at the hip (on respawn)
    // Fires one shot from the camera within the current spread (no input/cooldown checks)
    ShotResult Fire(const Camera3D& camera, World& world, std::vector<Enemy>& enemies);
    const ShotResult& LastShot() const { return lastShot; } // the shot Update fired this frame
    bool TakeDryFire() { bool d = dryFire; dryFire = false; return d; } // clicked on empty?
    // A random bullet path: straight ahead, tilted by up to the current spread
    Ray ShotRay(const Camera3D& camera);
    // Single-player: what a shot along `ray` hits among the world and enemies (damage applied)
    ShotResult Resolve(Ray ray, Vector3 shooter, World& world, std::vector<Enemy>& enemies);

    // Bullet spread: a cone around the aim direction, set by stance and aiming
    void UpdateSpread(float dt, Stance stance);
    float GetSpread() const { return spread; } // current cone half-angle, degrees

    // Aiming down sights: eases toward 1 while held (not while reloading)
    void UpdateAim(float dt, bool wantAim);
    float GetAimAmount() const { return aimAmount; } // 0 = hip, 1 = fully aimed

    const WeaponDef& GetDef() const { return *def; }
    int GetAmmo() const { return ammo; }
    bool IsReloading() const { return reloadTimer > 0.0f; }
    float GetReloadProgress() const { return IsReloading() ? 1.0f - reloadTimer / reloadDuration : 0.0f; } // 0..1
    float GetReloadDuration() const { return reloadDuration; }   // of the current (or last) reload
    bool IsEmptyReload() const { return emptyReload; }           // the current reload started from empty
    float GetRecoil() const { return recoil; }            // 1 right after a shot, decays to 0
    bool IsMuzzleFlashing() const { return muzzleFlash > 0.0f; }
    int GetKills() const { return kills; }
    // Hit feedback for the HUD: seconds left on the hit marker, and what the last hit was
    float GetHitMarker() const { return hitMarker; }
    bool LastHitKilled() const { return lastHitKilled; }
    bool LastHitHeadshot() const { return lastHitHeadshot; }

private:
    const WeaponDef* def;
    int ammo;
    float cooldownTimer = 0.0f;
    float reloadTimer = 0.0f;
    float reloadDuration = 1.0f;
    bool emptyReload = false;
    float recoil = 0.0f;         // 0..1, decays over time
    float aimAmount = 0.0f;      // 0..1, eased
    float aimEaseRate = 15.0f;
    float spread;                // degrees, eased toward the stance's value
    float spreadGrowRate = 20.0f;   // spread widens quickly when you start moving...
    float spreadRecoverRate = 6.0f; // ...and tightens back up more slowly
    std::mt19937 rng;
    float muzzleFlash = 0.0f;    // seconds remaining
    float hitMarker = 0.0f;      // seconds remaining
    bool lastHitKilled = false;  // draws a bigger hit marker
    bool lastHitHeadshot = false; // draws the marker in yellow
    int kills = 0;

    ShotResult lastShot;
    bool dryFire = false;
};
