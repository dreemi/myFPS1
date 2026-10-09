#include "weapon.hpp"
#include "raymath.h"
#include <algorithm>
#include <cstdio>

const WeaponDef REVOLVER = {
    "Revolver",
    25, 50,          // body / head damage
    0.18f,           // fire cooldown
    6, 3.0f,         // six-round cylinder; reload time matches the reload sound

    8.0f, 7.0f, 3.5f, 1.5f, 1.0f, 0.6f, // spread: airborne, sprinting, walking, still, crouch-walking, crouched
    0.3f,            // aiming down sights spread multiplier
};

const WeaponDef FAL = {
    "FAL",
    34, 100,         // 7.62 NATO: three to the body, one to the head
    0.092f,          // ~650 rounds a minute
    20, 2.6f,        // 20-round magazine
    10.0f, 7.0f, 3.5f, 1.4f, 1.2f, 0.8f,
    0.25f,
    true,            // automatic
    3.4f,            // from empty: the bolt has to be charged after the magazine goes in
    0.55f,           // each shot opens the spread a little
};

float WeaponDef::HipSpread(Stance stance) const {
    switch (stance) {
        case Stance::Airborne:     return spreadAirborne;
        case Stance::Sprinting:    return spreadSprinting;
        case Stance::Walking:      return spreadWalking;
        case Stance::Still:        return spreadStill;
        case Stance::CrouchMoving: return spreadCrouchMoving;
        case Stance::CrouchStill:  return spreadCrouchStill;
    }
    return spreadStill;
}

Weapon::Weapon(const WeaponDef& d, unsigned seed) : def(&d), ammo(d.magazineSize), spread(d.spreadStill), rng(seed) {}

void Weapon::UpdateAim(float dt, bool wantAim) {
    float target = (wantAim && reloadTimer <= 0.0f) ? 1.0f : 0.0f;
    aimAmount += (target - aimAmount) * (1.0f - expf(-aimEaseRate * dt));
    if (fabsf(target - aimAmount) < 0.001f) aimAmount = target;
}

void Weapon::UpdateSpread(float dt, Stance stance) {
    float target = def->HipSpread(stance) * Lerp(1.0f, def->aimSpreadScale, aimAmount);
    float rate = target > spread ? spreadGrowRate : spreadRecoverRate;
    spread += (target - spread) * (1.0f - expf(-rate * dt));
    if (fabsf(target - spread) < 0.001f) spread = target;
}

Ray Weapon::ShotRay(const Camera3D& camera) {
    Vector3 forward = Vector3Normalize(Vector3Subtract(camera.target, camera.position));
    Vector3 right = Vector3Normalize(Vector3CrossProduct(forward, camera.up));
    Vector3 up = Vector3CrossProduct(right, forward);

    // Uniform over the cone's circular cross-section: sqrt keeps the center
    // from being over-sampled
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    float angle = spread * DEG2RAD * sqrtf(unit(rng));
    float around = 2.0f * PI * unit(rng);
    Vector3 offset = Vector3Add(Vector3Scale(right, cosf(around)), Vector3Scale(up, sinf(around)));
    Vector3 dir = Vector3Normalize(Vector3Add(forward, Vector3Scale(offset, tanf(angle))));
    return { camera.position, dir };
}

void Weapon::Tick(float dt, Stance stance) {
    UpdateSpread(dt, stance);
    cooldownTimer = std::max(0.0f, cooldownTimer - dt);
    muzzleFlash = std::max(0.0f, muzzleFlash - dt);
    hitMarker = std::max(0.0f, hitMarker - dt);
    recoil = std::max(0.0f, recoil - dt * 6.0f);



    if (reloadTimer > 0.0f) {
        reloadTimer -= dt;
        if (reloadTimer <= 0.0f) {
            reloadTimer = 0.0f;
            ammo = def->magazineSize;
        }
    }
}

void Weapon::Reset() {
    ammo = def->magazineSize;
    reloadTimer = cooldownTimer = 0.0f;
    emptyReload = false;
    recoil = muzzleFlash = hitMarker = 0.0f;
    aimAmount = 0.0f;
    spread = def->spreadStill;
    dryFire = false;
}

bool Weapon::StartReload() {
    if (IsReloading() || ammo >= def->magazineSize) return false;
    emptyReload = ammo == 0;
    reloadDuration = emptyReload && def->reloadEmptyTime > def->reloadTime ? def->reloadEmptyTime : def->reloadTime;
    reloadTimer = reloadDuration;
    return true;
}

void Weapon::CancelReload() {
    reloadTimer = 0.0f;
}

bool Weapon::Update(float dt, const Camera3D& camera, World& world, std::vector<Enemy>& enemies, Stance stance) {
    UpdateAim(dt, IsMouseButtonDown(MOUSE_BUTTON_RIGHT));
    Tick(dt, stance);
    if (IsReloading()) return false;
    if (IsKeyPressed(KEY_R) && StartReload()) return false;

    bool pressed = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    bool trigger = pressed || (def->automatic && IsMouseButtonDown(MOUSE_BUTTON_LEFT));
    if (trigger && cooldownTimer <= 0.0f) {
        if (ammo > 0) {
            lastShot = Fire(camera, world, enemies);
            return true;
        }
        if (pressed) {
            dryFire = true;
            StartReload(); // auto-reload on empty click
        }
    }
    return false;
}

ShotResult Weapon::Fire(const Camera3D& camera, World& world, std::vector<Enemy>& enemies) {
    ammo--;
    cooldownTimer = def->fireCooldown;
    muzzleFlash = 0.05f;
    recoil = 1.0f;

    Ray ray = ShotRay(camera);
    spread += def->bloomPerShot; // recovers toward the stance's spread over time
    ShotResult result;
    result.direction = ray.direction;

    // Find the closest box the ray hits
    WorldBox* closest = nullptr;
    RayCollision closestHit = { 0 };
    closestHit.distance = 1e9f;

    for (WorldBox& box : world) {
        RayCollision hit = GetRayCollisionBox(ray, box.Bounds());
        if (hit.hit && hit.distance < closestHit.distance) {
            closestHit = hit;
            closest = &box;
        }
    }
    // The ground stops bullets too
    bool ground = false;
    if (ray.direction.y < -1e-4f) {
        float t = -ray.position.y / ray.direction.y;
        if (t > 0.0f && t < closestHit.distance) {
            closestHit.distance = t;
            closestHit.point = Vector3Add(ray.position, Vector3Scale(ray.direction, t));
            closestHit.normal = { 0, 1, 0 };
            closest = nullptr;
            ground = true;
        }
    }

    // An enemy in front of everything else takes the hit
    Enemy* hitEnemy = nullptr;
    bool headshot = false;
    for (Enemy& e : enemies) {
        EnemyRayHit hit = e.Raycast(ray);
        if (hit.hit && hit.distance < closestHit.distance) {
            closestHit.distance = hit.distance;
            hitEnemy = &e;
            headshot = hit.headshot;
        }
    }
    if (hitEnemy) {
        lastHitKilled = hitEnemy->TakeHit(camera.position, headshot ? def->headDamage : def->bodyDamage);
        lastHitHeadshot = headshot;
        if (lastHitKilled) kills++;
        hitMarker = lastHitKilled ? 0.3f : 0.12f;
        result.hit = result.hitEnemy = true;
        result.headshot = headshot;
        result.killed = lastHitKilled;
        result.point = Vector3Add(ray.position, Vector3Scale(ray.direction, closestHit.distance));
        return result;
    }

    if (!closest && !ground) return result;
    result.hit = true;
    result.point = closestHit.point;
    result.normal = closestHit.normal;
    result.surface = ground ? Surface::Concrete : closest->surface;

    if (closest && closest->isTarget) {
        closest->hitFlash = 0.15f;
        hitMarker = 0.12f;
        lastHitKilled = false;
        lastHitHeadshot = false;
        result.hitTarget = true;
    }
    return result;
}
