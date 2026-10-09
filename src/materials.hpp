#pragma once
#include "raylib.h"
#include "world.hpp"
#include <string>

// Textures for the world's surfaces (see Surface in world.hpp)
struct WorldMaterials {
    Texture2D concrete{};  // ground and walls (colour with ambient occlusion baked in)
    Texture2D concreteNormal{}, concreteRough{};
    Texture2D steel{};     // cover
    Texture2D steelNormal{}, steelRough{};
    float concreteTile = 3.0f; // meters per texture repeat
    float steelTile = 2.0f;
    bool loaded = false;
};

// Loads textures from assetDir (with a trailing slash). Falls back to plain
// colors if anything is missing.
WorldMaterials LoadWorldMaterials(const std::string& assetDir);
void UnloadWorldMaterials(WorldMaterials& m);

// Draws a world box, textured by its surface. Textures are mapped in world
// space, so neighbouring boxes line up and the texture never stretches.
void DrawWorldBox(const WorldBox& box, const WorldMaterials& m);

// A flat ground quad the size of the arena, textured with concrete. Unload it
// with UnloadGroundModel (it borrows the concrete texture).
Model LoadGroundModel(float width, float length, const WorldMaterials& m);
void UnloadGroundModel(Model& ground);
