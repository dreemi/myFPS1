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

    float HipSpread(Stance stance) const;
};

// Enemies have 100 health: 4 body shots or 2 headshots to kill
extern const WeaponDef REVOLVER;

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

// One weapon being carried: ammo, timers, recoil, spread, aiming
class Weapon {
public:
    explicit Weapon(const WeaponDef& def, unsigned seed = 1);

    // Reads input (fire, reload, aim) and calls Tick. Returns true if a shot was fired this frame.
    bool Update(float dt, const Camera3D& camera, World& world, std::vector<Enemy>& enemies, Stance stance);
    // Advances timers, recoil and spread without reading input
    void Tick(float dt, Stance stance);
    bool StartReload(); // false if already reloading or the magazine is full
    // Fires one shot from the camera within the current spread (no input/cooldown checks)
    ShotResult Fire(const Camera3D& camera, World& world, std::vector<Enemy>& enemies);
    const ShotResult& LastShot() const { return lastShot; } // the shot Update fired this frame
    bool TakeDryFire() { bool d = dryFire; dryFire = false; return d; } // clicked on empty?
    // A random bullet path: straight ahead, tilted by up to the current spread
    Ray ShotRay(const Camera3D& camera);

    // Bullet spread: a cone around the aim direction, set by stance and aiming
    void UpdateSpread(float dt, Stance stance);
    float GetSpread() const { return spread; } // current cone half-angle, degrees

    // Aiming down sights: eases toward 1 while held (not while reloading)
    void UpdateAim(float dt, bool wantAim);
    float GetAimAmount() const { return aimAmount; } // 0 = hip, 1 = fully aimed

    const WeaponDef& GetDef() const { return *def; }
    int GetAmmo() const { return ammo; }
    bool IsReloading() const { return reloadTimer > 0.0f; }
    float GetReloadProgress() const { return IsReloading() ? 1.0f - reloadTimer / def->reloadTime : 0.0f; } // 0..1
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
