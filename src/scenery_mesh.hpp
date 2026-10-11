#pragma once
#include "raylib.h"
#include "world.hpp"
#include <vector>

// Meshes for a map's Scenery (trees, round walls, roofs, beams), grouped by what
// they're made of and split so no mesh has more vertices than 16-bit indices
// allow. Uploaded to the GPU; the renderer gives them materials and draws them.
struct SceneryMeshes {
    // A mesh and the box round it (meshes are split up by area so the renderer can
    // skip what's out of view)
    struct Piece {
        Mesh mesh;
        BoundingBox bounds;
        bool background;         // trees outside the playable area (they cast no shadows)
    };
    struct Group {
        Surface surface;
        std::vector<Piece> pieces;
    };
    std::vector<Group> solids;   // one group per surface
    std::vector<Piece> leaves;   // leaf cards (alpha-cut, drawn from both sides)
};

SceneryMeshes BuildSceneryMeshes(const Scenery& scenery);
