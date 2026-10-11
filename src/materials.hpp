#pragma once
#include "raylib.h"
#include "world.hpp"
#include <string>

// How one kind of surface looks (see Surface in world.hpp)
struct SurfaceLook {
    Texture2D albedo{};      // colour (ambient occlusion baked in where there is one)
    Texture2D normal{}, rough{};
    float tile = 2.0f;       // meters per texture repeat (world-mapped surfaces)
    float specular = 0.25f;  // highlight strength
    bool faceMapped = false; // the texture covers each face once (crates) instead of repeating in world space
    Color tint = WHITE;      // multiplies the colour map (painted wood)
};

// The ground's textures on maps with a painted ground (Scenery::GroundMask): each
// tiles in world space, blended by the mask
struct TerrainLook {
    Texture2D grass{}, dirt{}, field{}, forest{};
    float grassTile = 3.0f, dirtTile = 3.0f, fieldTile = 4.0f, forestTile = 3.0f;
};

// Textures for the world's surfaces
struct WorldMaterials {
    SurfaceLook surfaces[(int)Surface::Count];
    TerrainLook terrain;
    Texture2D leaves{};      // leaf clusters with alpha, for tree crowns
    bool loaded = false;
    const SurfaceLook& Of(Surface s) const { return surfaces[(int)s]; }
};

// Loads textures from assetDir (with a trailing slash). Falls back to plain
// colors if anything is missing.
WorldMaterials LoadWorldMaterials(const std::string& assetDir);
void UnloadWorldMaterials(WorldMaterials& m);

// Draws a world box, textured by its surface (fallback when the lit renderer is unavailable)
void DrawWorldBox(const WorldBox& box, const WorldMaterials& m);

// A flat ground quad the size of the arena, textured with paving. Unload it
// with UnloadGroundModel (it borrows the texture).
Model LoadGroundModel(float width, float length, const WorldMaterials& m);
void UnloadGroundModel(Model& ground);
