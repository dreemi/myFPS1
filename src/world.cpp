#include "world.hpp"
#include "raymath.h"

bool CylinderCollides(Vector3 pos, float radius, float height, const World& world) {
    for (const WorldBox& box : world) {
        BoundingBox b = box.Bounds();
        // Skip boxes entirely above the head or below the feet
        if (b.max.y <= pos.y + 0.05f || b.min.y >= pos.y + height) continue;
        // Circle on the XZ plane against the box footprint
        if (pos.x > b.min.x - radius && pos.x < b.max.x + radius &&
            pos.z > b.min.z - radius && pos.z < b.max.z + radius) {
            return true;
        }
    }
    return false;
}

bool HasLineOfSight(Vector3 from, Vector3 to, const World& world) {
    Vector3 delta = Vector3Subtract(to, from);
    float dist = Vector3Length(delta);
    if (dist < 1e-4f) return true;
    Ray ray = { from, Vector3Scale(delta, 1.0f / dist) };
    for (const WorldBox& box : world) {
        if (box.isTarget) continue;
        RayCollision hit = GetRayCollisionBox(ray, box.Bounds());
        if (hit.hit && hit.distance < dist) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Map: three routes between two spawns, mirrored across z = 0 so both teams
// get identical geometry. Top view (+Z is north, team B's end):
//
//   x = -20          -6                 12        20
//       +-------------------------------------------+  z = 40
//       |          spawn B   [shield]               |
//       |  z = 26 ----+-----------------+------------|
//       |   MAIN      |     CENTER      |   FLANK    |
//       |   13 m,     door  17 m,       window 7 m,  |
//       |   lots of   |     open with   |   nearly   |
//       |   cover     |     some cover  |   empty    |
//       |             window [block]    door  [pillar]  z = 0
//       |             |                 |            |
//       |   (mirror of the north half)  |            |
//       |  z = -26 ---+-----------------+------------|
//       |          spawn A   [shield]               |
//       +-------------------------------------------+  z = -40
//
// - Center is the shortest route but the most exposed.
// - Main is wider and full of cover: slower, safer, good for holding angles.
// - Flank is narrow and quick, with a door into the middle of center, but has
//   almost nowhere to hide if someone is watching it.
// - Doors and waist-high windows (you can shoot over or jump through them)
//   link the lanes, so no lane is ever fully safe from the others.
// ---------------------------------------------------------------------------

World BuildTestWorld() {
    World w;
    const float halfW = ARENA_WIDTH / 2, halfL = ARENA_LENGTH / 2;
    const float wallH = 4.0f;       // too tall to jump (jump reaches 1.6 m)
    const float windowH = 1.1f;     // low enough to shoot over and jump through
    const float laneEnd = 26.0f;    // lanes run from -laneEnd to +laneEnd

    const Color outerColor = { 120, 120, 135, 255 };
    const Color dividerColor = { 140, 140, 155, 255 };
    const Color windowColor = { 165, 160, 140, 255 };
    const Color shieldColor = { 110, 110, 125, 255 };
    const Color mainCrate = { 150, 110, 80, 255 };
    const Color mainPillar = { 90, 130, 100, 255 };
    const Color centerCover = { 140, 140, 90, 255 };
    const Color flankCover = { 100, 100, 150, 255 };

    // Box resting on the ground: center x/z, size, color
    auto box = [&](float x, float z, float sx, float sy, float sz, Color c) {
        w.push_back({ { x, sy / 2, z }, { sx, sy, sz }, c });
    };
    // Same box on both halves of the map
    auto mirrored = [&](float x, float z, float sx, float sy, float sz, Color c) {
        box(x, z, sx, sy, sz, c);
        box(x, -z, sx, sy, sz, c);
    };
    // Lane divider segment running north-south at x, from z0 to z1 (mirrored)
    auto divider = [&](float x, float z0, float z1, float h, Color c) {
        mirrored(x, (z0 + z1) / 2, 1.0f, h, z1 - z0, c);
    };

    // Outer walls (keep these first: tests use the first four as an empty arena)
    w.push_back({ { 0, wallH / 2,  halfL }, { ARENA_WIDTH, wallH, 1 }, outerColor });
    w.push_back({ { 0, wallH / 2, -halfL }, { ARENA_WIDTH, wallH, 1 }, outerColor });
    w.push_back({ {  halfW, wallH / 2, 0 }, { 1, wallH, ARENA_LENGTH }, outerColor });
    w.push_back({ { -halfW, wallH / 2, 0 }, { 1, wallH, ARENA_LENGTH }, outerColor });

    // --- Main | Center divider at x = -6 ---
    // Doors at |z| 11..14, waist-high window across the middle (|z| < 3)
    divider(-6, 14, laneEnd, wallH, dividerColor);
    divider(-6, 3, 11, wallH, dividerColor);
    box(-6, 0, 1.0f, windowH, 6.0f, windowColor);

    // --- Center | Flank divider at x = 12 ---
    // Door in the very middle (|z| < 2), low windows at |z| 17..21
    divider(12, 21, laneEnd, wallH, dividerColor);
    divider(12, 17, 21, windowH, windowColor);
    divider(12, 2, 17, wallH, dividerColor);

    // --- Spawn shields: stop anyone shooting straight down center into spawn ---
    mirrored(2, 29, 8, 3, 1, shieldColor);

    // --- Main lane (x -19.5 .. -6.5): dense cover, alternating sides ---
    box(-12, 0, 2, 1.5f, 2, mainCrate);              // crate in the middle
    mirrored(-16, 4, 2, 3, 2, mainPillar);           // tall pillar
    mirrored(-9.5f, 6.5f, 2, 1.5f, 2, mainCrate);    // crate by the window
    mirrored(-13, 11, 5, 1.2f, 1, mainCrate);        // low wall across the lane
    mirrored(-9.5f, 16, 1.5f, 3, 1.5f, mainPillar);  // pillar covering the door
    mirrored(-17, 17.5f, 2.5f, 1.5f, 2.5f, mainCrate);
    mirrored(-12, 21.5f, 2, 1.5f, 2, mainCrate);
    mirrored(-17.5f, 24, 2, 3, 1, mainPillar);       // pillar at the lane mouth

    // --- Center lane (x -5.5 .. 11.5): open, a few pieces of cover ---
    box(3, 0, 3, 2.5f, 3, centerCover);              // block breaks the end-to-end sightline
    mirrored(-1.5f, 7, 2, 1.2f, 2, centerCover);
    mirrored(8, 10, 2, 1.2f, 2, centerCover);
    mirrored(3, 17, 4, 1.4f, 1, centerCover);        // low wall
    mirrored(-3, 22, 1.5f, 2.5f, 1.5f, centerCover);

    // --- Flank lane (x 12.5 .. 19.5): narrow, almost no cover ---
    box(16, 0, 2, 3, 2, flankCover);                 // pillar opposite the middle door
    mirrored(18, 10, 1.5f, 1.2f, 1.5f, flankCover);
    mirrored(14.5f, 23, 1.5f, 1.5f, 1.5f, flankCover);

    // Surfaces: the arena's walls (outer walls, lane dividers and their windows,
    // spawn shields) are concrete; everything placed as cover is steel
    auto same = [](Color a, Color b) { return a.r == b.r && a.g == b.g && a.b == b.b; };
    for (WorldBox& b : w) {
        bool wall = same(b.color, outerColor) || same(b.color, dividerColor) || same(b.color, windowColor) ||
                    same(b.color, shieldColor);
        b.surface = wall ? Surface::Concrete : Surface::Steel;
    }

    // Shootable practice targets, floating above head height so they never block
    WorldBox t;
    t.size = { 0.8f, 0.8f, 0.8f };
    t.color = { 230, 120, 40, 255 };
    t.isTarget = true;
    for (Vector3 p : { Vector3{ 3, 3.2f, 0 }, Vector3{ -13, 2.8f, 8 }, Vector3{ -13, 2.8f, -8 },
                       Vector3{ 3, 2.8f, 13 }, Vector3{ 3, 2.8f, -13 }, Vector3{ 16, 2.8f, 15 },
                       Vector3{ 16, 2.8f, -15 } }) {
        t.position = p;
        w.push_back(t);
    }
    return w;
}

std::vector<SpawnPoint> GetSpawnPoints() {
    const float z = ARENA_LENGTH / 2 - 4.0f; // 4 m in from the end walls, behind the shield
    return {
        { { 2, 0, -z }, 0.0f, { 60, 120, 230, 255 } },  // blue (team A), south end, facing north (+Z)
        { { 2, 0,  z }, PI,   { 220, 60, 60, 255 } },   // red (team B), north end, facing south (-Z)
    };
}
