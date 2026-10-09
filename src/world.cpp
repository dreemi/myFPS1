#include "world.hpp"
#include "raymath.h"
#include <utility>

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
// Map: "Dustyard", a sun-baked town. Two spawns at the ends; the layout is
// rotationally symmetric (every box at (x, z) has a twin at (-x, -z)) so both
// sides get the same routes without it looking mirrored. Top view of team A's
// half (south, -Z); team B's half is the same turned half a circle:
//
//  x = -20      -12    -6         0         6     12          20
//   +-----------+------+--------- z = 0 -----+------+-----------+
//   |                  CENTRAL PLAZA  (well)                     |
//   |      [block]                             [balcony overhang]|  z = -8
//   |  LONG     +------+  gate (arch)  +------+  APARTMENTS      |  z = -12
//   |  lane     |      |   MID     cat-|      |  covered ground  |
//   |  crates   |block |  doors ==  walk|block |  corridor, plank  |
//   |           |      |  (arch)    |  |      |  terrace on top  |
//   |== doors ==|      |          stairs|      |   stairs up -->  |  z = -24
//   +-----------+------+--------------+------+-----------------+  z = -30
//   |                    SPAWN A plaza (crates)                  |
//   +------------------------------------------------------------+  z = -40
//
// - Mid is the short way, through an arched door with a raised catwalk beside
//   it that overlooks the doors.
// - Long is the wide outer lane through a wall with an arched door.
// - The apartments route runs under a plank terrace; stairs climb onto it and
//   it ends in a balcony over the central plaza: the high ground.
// Spawns are 4 m in from the end walls and can't see each other.
// ---------------------------------------------------------------------------

namespace {

const Color PLASTER_COLOR = { 214, 196, 162, 255 };
const Color SANDSTONE_COLOR = { 205, 172, 120, 255 };
const Color WOOD_COLOR = { 130, 105, 80, 255 };
const Color CRATE_COLOR = { 180, 140, 90, 255 };
const Color SAND_COLOR = { 210, 184, 140, 255 };
const Color PAINTED_COLOR = { 90, 140, 150, 255 };

Color PlainColor(Surface s) {
    switch (s) {
        case Surface::Plaster: return PLASTER_COLOR;
        case Surface::Sandstone: return SANDSTONE_COLOR;
        case Surface::Wood: return WOOD_COLOR;
        case Surface::Crate: return CRATE_COLOR;
        case Surface::Sand: return SAND_COLOR;
        case Surface::Painted: return PAINTED_COLOR;
        default: return GRAY;
    }
}

// Builds the map from boxes given by their extents; `Twin` also adds the
// half-turned copy for the other side
struct MapBuilder {
    World w;

    void Add(float x0, float x1, float y0, float y1, float z0, float z1, Surface s) {
        WorldBox b;
        b.position = { (x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2 };
        b.size = { x1 - x0, y1 - y0, z1 - z0 };
        b.color = PlainColor(s);
        b.surface = s;
        w.push_back(b);
    }
    void Twin(float x0, float x1, float y0, float y1, float z0, float z1, Surface s) {
        Add(x0, x1, y0, y1, z0, z1, s);
        Add(-x1, -x0, y0, y1, -z1, -z0, s);
    }

    // A plastered building: a sandstone plinth at the foot, a cornice at the
    // top, and shuttered windows with sills along the upper floor
    void Building(float x0, float x1, float z0, float z1, float h) {
        Twin(x0, x1, 0, h, z0, z1, Surface::Plaster);
        const float p = 0.06f, c = 0.14f;
        Twin(x0 - p, x1 + p, 0, 0.7f, z0 - p, z1 + p, Surface::Sandstone);
        Twin(x0 - c, x1 + c, h - 0.35f, h, z0 - c, z1 + c, Surface::Sandstone);
        if (h < 5.0f) return;
        // Windows along each face, one every 4 m
        auto windows = [&](bool alongX, float fixed, float out, float a0, float a1) {
            int count = (int)((a1 - a0) / 4.0f);
            for (int i = 0; i < count; i++) {
                float mid = a0 + (a1 - a0) * (i + 0.5f) / count;
                for (float side : { -1.0f, 1.0f }) { // two shutter leaves
                    float s0 = mid + side * 0.02f, s1 = mid + side * 0.47f;
                    if (s0 > s1) std::swap(s0, s1);
                    float f0 = fixed, f1 = fixed + out * 0.06f;
                    if (f0 > f1) std::swap(f0, f1);
                    if (alongX) Twin(s0, s1, 3.3f, 4.7f, f0, f1, Surface::Painted);
                    else Twin(f0, f1, 3.3f, 4.7f, s0, s1, Surface::Painted);
                }
                float g0 = fixed, g1 = fixed + out * 0.14f; // sill
                if (g0 > g1) std::swap(g0, g1);
                if (alongX) Twin(mid - 0.6f, mid + 0.6f, 3.15f, 3.3f, g0, g1, Surface::Sandstone);
                else Twin(g0, g1, 3.15f, 3.3f, mid - 0.6f, mid + 0.6f, Surface::Sandstone);
            }
        };
        windows(true, z0, -1, x0, x1);
        windows(true, z1, 1, x0, x1);
        windows(false, x0, -1, z0, z1);
        windows(false, x1, 1, z0, z1);
    }

    // A wall across a lane with an arched opening (posts are the wall either side, a lintel on top)
    void ArchedWall(float x0, float x1, float z0, float z1, float gap0, float gap1, float h, float doorH, Surface s) {
        Twin(x0, gap0, 0, h, z0, z1, s);
        Twin(gap1, x1, 0, h, z0, z1, s);
        Twin(gap0, gap1, doorH, h, z0, z1, s);
    }

    // Stairs climbing along +Z from z0, each step a solid block from the ground
    void StairsNorth(float x0, float x1, float z0, float topY, int steps) {
        float rise = topY / steps, run = 0.5f;
        for (int i = 0; i < steps; i++)
            Twin(x0, x1, 0, rise * (i + 1), z0 + run * i, z0 + run * (i + 1), Surface::Sandstone);
    }

    // A wooden crate (optionally a second, smaller one on top)
    void Crate(float x, float z, float size, float top = 0.0f) {
        Twin(x - size / 2, x + size / 2, 0, size, z - size / 2, z + size / 2, Surface::Crate);
        if (top > 0.0f) Twin(x - top / 2 + 0.1f, x + top / 2 + 0.1f, size, size + top, z - top / 2, z + top / 2, Surface::Crate);
    }
};

} // namespace

World BuildTestWorld() {
    MapBuilder m;
    const float halfW = ARENA_WIDTH / 2, halfL = ARENA_LENGTH / 2;
    const float outerH = 6.0f; // the town wall: too tall to see or climb over, even from the terrace

    // Outer walls (keep these first: tests use the first four as an empty arena)
    m.Add(-halfW, halfW, 0, outerH, halfL - 0.5f, halfL + 0.5f, Surface::Sandstone);
    m.Add(-halfW, halfW, 0, outerH, -halfL - 0.5f, -halfL + 0.5f, Surface::Sandstone);
    m.Add(halfW - 0.5f, halfW + 0.5f, 0, outerH, -halfL, halfL, Surface::Sandstone);
    m.Add(-halfW - 0.5f, -halfW + 0.5f, 0, outerH, -halfL, halfL, Surface::Sandstone);

    // Sand underfoot along long and out into the plaza's edges (paving elsewhere); thin
    // enough to walk straight onto
    m.Twin(-19.5f, -12, 0, 0.02f, -30, -8, Surface::Sand);
    m.Twin(-19.5f, -12, 0, 0.02f, -8, 8, Surface::Sand);

    // --- The blocks between the routes ---
    m.Building(-12, -6, -30, -12, 6.5f);    // between long and mid
    m.Building(6, 12, -30, -12, 6.5f);      // between mid and apartments
    m.Building(-12, -6, -7, -2, 5.0f);      // in the plaza: breaks long's view across it

    // --- Long ---
    m.ArchedWall(-19.5f, -12, -24.4f, -23.6f, -16.5f, -14.0f, 4.5f, 2.9f, Surface::Sandstone);
    m.Twin(-14.08f, -13.96f, 0, 2.8f, -23.6f, -22.4f, Surface::Painted); // its door, swung open
    m.Crate(-18.2f, -19.0f, 1.2f, 0.9f);
    m.Crate(-13.4f, -15.5f, 1.1f);
    m.Crate(-17.6f, -11.0f, 1.0f);
    m.Crate(-15.0f, -28.0f, 1.2f);

    // --- Mid ---
    // Catwalk: a raised walk along the east side, stairs up from spawn, railing toward mid
    const float catY = 1.9f;
    m.Twin(2.8f, 6, 0, catY, -26, -12, Surface::Sandstone);
    m.StairsNorth(2.8f, 6, -29, catY, 6);
    m.Twin(2.8f, 3.05f, catY, catY + 0.95f, -22, -14, Surface::Plaster);   // railing (gaps at each end)
    m.Twin(2.75f, 3.1f, catY + 0.95f, catY + 1.05f, -22, -14, Surface::Sandstone);
    // Mid doors: an arched door in a wall across mid, off-centre so spawn can't see spawn
    m.ArchedWall(-6, 2.8f, -15.4f, -14.6f, -4.5f, -1.5f, 4.5f, 2.9f, Surface::Plaster);
    m.Twin(-4.5f, -4.38f, 0, 2.8f, -14.6f, -13.3f, Surface::Painted);      // door leaf, swung open
    m.Crate(-1.0f, -19.0f, 1.1f);
    m.Crate(-4.6f, -26.5f, 1.2f, 0.8f);
    // A stone gate where mid opens into the plaza (high enough to walk under on the catwalk)
    m.Twin(-6, 6, 3.8f, 4.6f, -12.4f, -11.6f, Surface::Sandstone);

    // --- Apartments: a covered corridor with a plank terrace on top ---
    const float deckY = 3.4f;
    m.Twin(12, 19.5f, 3.0f, deckY, -24, -8, Surface::Wood);                // terrace / corridor roof
    m.StairsNorth(16.5f, 19.5f, -29.5f, deckY, 11);                        // stairs up from spawn
    m.Twin(15.6f, 16.0f, 0, 3.0f, -17.2f, -16.8f, Surface::Plaster);       // pillar in the corridor
    m.Twin(12.5f, 12.75f, 0, 3.0f, -8.4f, -8.15f, Surface::Wood);          // posts under the balcony
    m.Twin(19.0f, 19.25f, 0, 3.0f, -8.4f, -8.15f, Surface::Wood);
    m.Twin(13.4f, 19.5f, deckY, deckY + 1.0f, -8.2f, -8.0f, Surface::Plaster); // balcony railing (drop off its west end)
    m.Twin(13.4f, 19.5f, deckY + 1.0f, deckY + 1.1f, -8.25f, -7.95f, Surface::Sandstone);
    m.Twin(16.6f, 17.6f, deckY, deckY + 1.0f, -16.5f, -15.5f, Surface::Crate); // cover on the terrace
    m.Crate(13.4f, -20.0f, 1.0f);                                          // in the corridor

    // --- Spawn plaza ---
    m.Crate(-8.0f, -34.0f, 1.2f, 0.9f);
    m.Crate(-16.5f, -36.5f, 1.2f);
    m.Crate(9.0f, -36.0f, 1.1f);
    m.Crate(13.5f, -33.0f, 1.0f);

    // --- Central plaza ---
    // A roofed well in the very middle (its own twin)
    m.Add(-1.2f, 1.2f, 0, 0.9f, -1.2f, 1.2f, Surface::Sandstone);
    for (float x : { -1.1f, 0.95f })
        for (float z : { -1.1f, 0.95f }) m.Add(x, x + 0.15f, 0.9f, 2.6f, z, z + 0.15f, Surface::Wood);
    m.Add(-1.5f, 1.5f, 2.6f, 2.85f, -1.5f, 1.5f, Surface::Wood);
    m.Twin(6.5f, 10.5f, 0, 1.1f, -3.0f, -2.4f, Surface::Sandstone);       // low wall
    m.Crate(-15.5f, -4.0f, 1.2f, 0.9f);
    m.Crate(4.5f, -6.5f, 1.0f);
    m.Crate(15.0f, -3.0f, 1.1f);
    return m.w;
}

std::vector<SpawnPoint> GetSpawnPoints() {
    const float z = ARENA_LENGTH / 2 - 5.0f; // 5 m in from the end walls
    return {
        { { 0, 0, -z }, 0.0f, { 60, 120, 230, 255 } },  // blue (team A), south end, facing north (+Z)
        { { 0, 0,  z }, PI,   { 220, 60, 60, 255 } },   // red (team B), north end, facing south (-Z)
    };
}
