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

    for (Impact& i : impacts) i.life -= dt;
    impacts.erase(std::remove_if(impacts.begin(), impacts.end(),
                                 [](const Impact& i) { return i.life <= 0.0f; }),
                  impacts.end());

    if (reloadTimer > 0.0f) {
        reloadTimer -= dt;
        if (reloadTimer <= 0.0f) {
            reloadTimer = 0.0f;
            ammo = def->magazineSize;
        }
    }
}

bool Weapon::StartReload() {
    if (IsReloading() || ammo >= def->magazineSize) return false;
    reloadTimer = def->reloadTime;
    return true;
}

bool Weapon::Update(float dt, const Camera3D& camera, World& world, std::vector<Enemy>& enemies, Stance stance) {
    UpdateAim(dt, IsMouseButtonDown(MOUSE_BUTTON_RIGHT));
    Tick(dt, stance);
    if (IsReloading()) return false;
    if (IsKeyPressed(KEY_R) && StartReload()) return false;

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && cooldownTimer <= 0.0f) {
        if (ammo > 0) {
            Fire(camera, world, enemies);
            return true;
        }
        StartReload(); // auto-reload on empty click
    }
    return false;
}

void Weapon::Fire(const Camera3D& camera, World& world, std::vector<Enemy>& enemies) {
    ammo--;
    cooldownTimer = def->fireCooldown;
    muzzleFlash = 0.05f;
    recoil = 1.0f;

    Ray ray = ShotRay(camera);

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
        return;
    }

    if (!closest) return;

    // Nudge the marker slightly off the surface so it isn't z-fighting
    Vector3 p = Vector3Add(closestHit.point, Vector3Scale(closestHit.normal, 0.01f));
    impacts.push_back({ p, 3.0f });

    if (closest->isTarget) {
        closest->hitFlash = 0.15f;
        hitMarker = 0.12f;
        lastHitKilled = false;
        lastHitHeadshot = false;
        TraceLog(LOG_INFO, "Hit target at (%.2f, %.2f, %.2f), distance %.2f",
                 closestHit.point.x, closestHit.point.y, closestHit.point.z, closestHit.distance);
    }
}

void Weapon::Draw3D() const {
    for (const Impact& i : impacts) {
        float alpha = std::min(1.0f, i.life);
        DrawSphere(i.point, 0.04f, Fade(DARKGRAY, alpha));
    }
}

void Weapon::DrawHUD(int w, int h, float fovy) const {
    int cx = w / 2, cy = h / 2;

    // Crosshair: the gap shows the current spread cone on screen (plus a
    // little kick from recoil). Fades out when aiming down sights, since the
    // sights take its place.
    float crosshairAlpha = Clamp(1.0f - aimAmount * 2.0f, 0.0f, 1.0f);
    if (crosshairAlpha > 0.0f) {
        float spreadPx = tanf(spread * DEG2RAD) / tanf(fovy * 0.5f * DEG2RAD) * (h * 0.5f);
        int gap = 3 + (int)(spreadPx + recoil * 6);
        int len = 8;
        Color ch = Fade(RAYWHITE, crosshairAlpha);
        DrawRectangle(cx - gap - len, cy - 1, len, 2, ch);
        DrawRectangle(cx + gap, cy - 1, len, 2, ch);
        DrawRectangle(cx - 1, cy - gap - len, 2, len, ch);
        DrawRectangle(cx - 1, cy + gap, 2, len, ch);
        DrawRectangle(cx - 1, cy - 1, 2, 2, ch);
    }

    // Hit marker: little X, yellow for headshots, bigger and thicker on a kill
    if (hitMarker > 0.0f) {
        float o = lastHitKilled ? 18.0f : 12.0f, in = 5.0f, t = lastHitKilled ? 3.0f : 2.0f;
        float x = (float)cx, y = (float)cy;
        Color c = lastHitHeadshot ? GOLD : RED;
        DrawLineEx({ x - o, y - o }, { x - in, y - in }, t, c);
        DrawLineEx({ x + o, y - o }, { x + in, y - in }, t, c);
        DrawLineEx({ x - o, y + o }, { x - in, y + in }, t, c);
        DrawLineEx({ x + o, y + o }, { x + in, y + in }, t, c);
        if (lastHitHeadshot && lastHitKilled) DrawText("HEADSHOT", cx - MeasureText("HEADSHOT", 20) / 2, cy + 28, 20, GOLD);
    }

    DrawText(TextFormat("Kills: %d", kills), w - 140, 10, 26, RAYWHITE);

    // Ammo counter
    if (reloadTimer > 0.0f) {
        DrawText("RELOADING...", w - 200, h - 50, 24, ORANGE);
    } else {
        DrawText(TextFormat("%d / %d", ammo, def->magazineSize), w - 140, h - 50, 30,
                 ammo == 0 ? RED : RAYWHITE);
    }
}
