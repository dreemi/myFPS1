#pragma once
#include "raylib.h"
#include "world.hpp"
#include "materials.hpp"
#include <string>

// Lighting and the frame's render passes:
//   1. shadow pass   - depth from the sun over the whole arena
//   2. sky           - full-screen gradient with the sun
//   3. world pass    - everything lit (sun, sky ambient, shadows, fog, flashes)
//   4. viewmodel     - the first-person gun/arms with the same light, in camera space
// One lit shader is shared by every model; its uniforms are switched per pass.
namespace Render {

bool Init(const std::string& assetDir);
void Shutdown();
bool Ready();
Shader LitShader();
void ApplyLitShader(Model& model);  // light a loaded model (characters, guns)
void DetachLitShader(Model& model); // before UnloadModel, so the shared shader isn't freed with it

// The level's boxes, merged into one static mesh per material, plus the ground
void BuildLevel(const World& world, const WorldMaterials& materials);
void DrawLevel();       // inside a world or shadow pass

// 1. Shadow pass: draw shadow casters between these (inside, the camera is the sun)
void BeginShadowPass();
void EndShadowPass();

// 2. Sky (call after BeginDrawing, before the world)
void DrawSky(const Camera3D& camera, int screenW, int screenH);

// 3. World pass: like BeginMode3D, with lighting on
void BeginWorldPass(const Camera3D& camera);
void EndWorldPass();

// 4. Viewmodel pass lighting: camera-space sun for the gun drawn at the origin.
//    sunVisibility: 0 if the player stands in shadow, 1 in sunlight.
void SetViewModelLighting(const Camera3D& worldCamera, float sunVisibility);

// Surface kinds (switching flushes pending batched geometry)
// Level boxes/ground: world-space UVs + normal maps (faceMapped: the texture covers each face once)
void SurfaceWorldMapped(float tileSize, float specular, bool faceMapped = false);
void SurfaceObject(float specular);                       // models with their own UVs (characters)
// Physically based metal/dielectric with sky reflections (weapons, casings). Roughness multiplies
// the roughness map (texture1.r) when roughMap; normalMap uses texture2. Occlusion scales sky light.
void SurfacePbr(float metallic, float roughness, bool roughMap, bool normalMap, float occlusion = 1.0f);

// Short-lived point lights for muzzle flashes (slot 0 or 1), radius 0 turns it off
void SetFlashLight(int slot, Vector3 position, Vector3 color, float radius);

// CPU check: is the sun visible from this point? (for the viewmodel's lighting)
bool SunVisible(Vector3 point, const World& world);

Vector3 SunDirection(); // direction sunlight travels

} // namespace Render
