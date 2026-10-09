#pragma once
#include "raylib.h"
#include "world.hpp"
#include <vector>

// Visual effects: particles (sparks, dust, blood, smoke), bullet-hole decals,
// muzzle flash sprites and tracers. Purely cosmetic; nothing here affects
// gameplay. Textures are generated at startup.
namespace Fx {

bool Init();            // after InitWindow
void Shutdown();
void Update(float dt);
void Draw(const Camera3D& camera); // in the world pass, after opaque geometry
void Clear();
void SetWorld(const World* world); // what falling cases and magazines land on (else just the ground)

// A bullet hit a surface: sparks/dust/chips and a bullet hole
void Impact(Vector3 point, Vector3 normal, Surface surface);
// A bullet hit a character: blood spray along the bullet's direction
void Blood(Vector3 point, Vector3 bulletDir, bool headshot);
// A gun fired in the world (the enemy's): flash sprite + a puff of smoke
void MuzzleFlash(Vector3 muzzle, Vector3 dir);
// A bullet's path, briefly visible
void Tracer(Vector3 from, Vector3 to, Color color);
// Thin smoke curling off a muzzle after a shot (amount 0..1)
void GunSmoke(Vector3 muzzle, Vector3 dir, float amount);

// Spent brass (and dumped live rounds): falls, bounces, comes to rest on the ground
// kind is the caller's (what to draw); restHeight = its centre's height lying on the ground;
// flat = comes to rest on its broad side (a magazine) rather than rolling on its axis (a case)
void Casing(Vector3 position, Vector3 velocity, int kind, float restHeight = 0.0055f, bool flat = false);
// Draws them with the given cartridge drawer (origin at the rim, bullet along -Z); in the world pass
void DrawCasings(void (*draw)(Matrix transform, int kind));
// Where cases hit the ground since the last call, and how hard (0..1), for sounds
struct CasingBounce { Vector3 position; float strength; int kind; };
std::vector<CasingBounce> TakeCasingBounces();

// A painted ring with an arrow on the ground (spawn points); yaw = arrow direction (0 = +Z)
void DrawGroundMarker(Vector3 position, float radius, float yaw, Color color);

// The star-shaped flash sprite, for the first-person gun (drawn by the viewmodel)
void DrawFlashSprite(const Camera3D& camera, Vector3 position, float size, float rotation, float alpha);

} // namespace Fx
