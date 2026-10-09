#pragma once
#include "raylib.h"
#include "world.hpp"

// Visual effects: particles (sparks, dust, blood, smoke), bullet-hole decals,
// muzzle flash sprites and tracers. Purely cosmetic; nothing here affects
// gameplay. Textures are generated at startup.
namespace Fx {

bool Init();            // after InitWindow
void Shutdown();
void Update(float dt);
void Draw(const Camera3D& camera); // in the world pass, after opaque geometry
void Clear();

// A bullet hit a surface: sparks/dust/chips and a bullet hole
void Impact(Vector3 point, Vector3 normal, Surface surface);
// A bullet hit a character: blood spray along the bullet's direction
void Blood(Vector3 point, Vector3 bulletDir, bool headshot);
// A gun fired in the world (the enemy's): flash sprite + a puff of smoke
void MuzzleFlash(Vector3 muzzle, Vector3 dir);
// A bullet's path, briefly visible
void Tracer(Vector3 from, Vector3 to, Color color);

// The star-shaped flash sprite, for the first-person gun (drawn by the viewmodel)
void DrawFlashSprite(const Camera3D& camera, Vector3 position, float size, float rotation, float alpha);

} // namespace Fx
