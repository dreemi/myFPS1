#pragma once
#include "raylib.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// What a box is made of (decides its texture, and how bullets hitting it look and sound)
enum class Surface {
    Plain, Concrete, Steel, Plaster, Sandstone, Wood, Crate, Sand, Painted,
    Stone,     // rough fieldstone (the fort, foundations)
    Logs,      // horizontal round logs (cabins, the spawn palisade)
    Shingles,  // wooden roof shingles
    Bark,      // tree trunks
    Dirt,      // bare earth: roads, and the ground on the forest map
    Count
};

// Box flags
enum WorldBoxFlag : uint8_t {
    BOX_HIDDEN = 1,      // collides but isn't drawn as a box (something else draws it: a trunk, a curved wall)
    BOX_NO_BULLETS = 2,  // stops movement only (the map's invisible boundary): bullets and sight pass through
};

// A solid axis-aligned box in the world: either a wall or a shootable target.
struct WorldBox {
    Vector3 position;   // center
    Vector3 size;
    Color color;        // used when it has no texture (Plain, or textures missing)
    bool isTarget = false;
    float hitFlash = 0.0f; // seconds remaining of hit-feedback flash
    Surface surface = Surface::Plain;
    uint8_t flags = 0;

    BoundingBox Bounds() const {
        return {
            { position.x - size.x / 2, position.y - size.y / 2, position.z - size.z / 2 },
            { position.x + size.x / 2, position.y + size.y / 2, position.z + size.z / 2 },
        };
    }
    bool Hidden() const { return flags & BOX_HIDDEN; }
    bool StopsBullets() const { return !(flags & BOX_NO_BULLETS); }
};

struct SpawnPoint {
    Vector3 position;  // feet position on the ground
    float yaw;         // facing direction (0 = +Z)
    Color color;       // floor marker color
};

// The maps
enum class MapId : uint8_t { Redoubt, Dustyard, OldTown, Count };
const char* MapName(MapId id);

// Things that are drawn but aren't boxes (their collision, where they have any,
// is hidden boxes in the world). Built with the map, drawn by the renderer.
struct Scenery {
    struct Tree {
        Vector3 base;          // where the trunk meets the ground
        float height;          // to the top of the crown
        float crownBottom;     // lowest leaves
        float crownRadius;
        float trunkRadius;
        float yaw;
        unsigned seed;
        bool background;       // outside the playable area: simpler, no collision
    };
    // A round wall (or a ring-shaped step), between two angles (radians, 0 = +Z, turning toward +X)
    struct Ring {
        Vector3 center;
        float inner, outer;    // radii
        float bottom, top;     // heights
        float from, to;        // angles
        Surface surface;
    };
    // A pitched roof over a rectangle: gable (ridge along X or Z) or pyramid
    struct Roof {
        float x0, x1, z0, z1;  // eaves, overhang included
        float base, rise;      // eaves height, ridge height above it
        enum Shape { RidgeX, RidgeZ, Pyramid } shape;
        Surface surface;
    };
    // A beam from one point to another (braces, fallen logs, branches of the towers)
    struct Beam {
        Vector3 from, to;
        float width;
        Surface surface;
        bool round;            // a log rather than a sawn beam
    };
    // A model from assets/models/scans (a scanned building or wall), placed in the world.
    // Its collision is boxes in the world; the model is only drawn.
    struct Prop {
        std::string model;     // file name without .glb
        Vector3 position;      // where its origin goes (the foot of a facade, the middle of an object's base)
        float yaw;             // radians, turning its +Z (a facade's face) toward (sin yaw, 0, cos yaw)
        float scale = 1.0f;
    };
    std::vector<Tree> trees;
    std::vector<Ring> rings;
    std::vector<Prop> props;
    std::vector<Roof> roofs;
    std::vector<Beam> beams;

    // The ground: a painted mask saying what covers it where (forest floor, fields,
    // roads; grass elsewhere), one RGBA pixel per `cell` meters from `origin` (x, z)
    struct GroundMask {
        int width = 0, height = 0;   // pixels: along X, along Z
        float cell = 0.5f;
        Vector2 origin{};            // world (x, z) of pixel (0, 0)'s corner
        std::vector<uint8_t> rgba;   // r = dirt road, g = plowed field, b = forest floor, a = trampled grass
    } ground;
    bool hasGround = false;
};

// Speeds up "what's near here" questions on maps with many boxes: each box is
// listed in every 4 m square (seen from above) it overlaps
struct WorldGrid {
    float x0 = 0, z0 = 0, cell = 4.0f;
    int nx = 0, nz = 0;
    std::vector<int> start;              // per cell, into items (nx * nz + 1 entries)
    std::vector<int> items;              // box indices
    mutable std::vector<unsigned> stamp; // per box: last query that visited it
    mutable unsigned query = 0;
};

// The level: its boxes, plus what else the map says about itself
struct World : std::vector<WorldBox> {
    using std::vector<WorldBox>::vector;

    MapId map = MapId::Dustyard;
    float width = 40.0f, length = 80.0f;  // playable area: X by Z, centered on the origin
    std::vector<SpawnPoint> spawns;
    std::vector<Vector3> hotspots;        // where fights happen (bots drift toward these)
    Surface ground = Surface::Concrete;   // what bullets hitting the ground hit
    float fogDensity = 0.006f;
    float botSight = 30.0f;               // how far bots notice people on this map
    // King of the hill: whoever stands in this circle alone scores (radius 0: no hill)
    Vector3 hillCenter{};
    float hillRadius = 0.0f;
    std::shared_ptr<const Scenery> scenery;

    // Calls fn(index) once for each box whose footprint may overlap [x0,x1] x [z0,z1]
    template <class F> void ForEachNear(float x0, float x1, float z0, float z1, F fn) const;
    // Calls fn(index) for boxes along a ray, nearest squares first; fn returns the
    // nearest hit distance found so far, and the walk stops once past it
    template <class F> void ForEachOnRay(Ray ray, float maxDistance, F fn) const;

private:
    const WorldGrid& Grid() const;
    mutable std::shared_ptr<WorldGrid> grid;
    mutable const WorldBox* gridData = nullptr;
    mutable size_t gridCount = 0;
};

// Arena is ARENA_WIDTH along X and ARENA_LENGTH along Z, centered on the origin (Dustyard)
constexpr float ARENA_WIDTH = 40.0f;
constexpr float ARENA_LENGTH = 80.0f;

World BuildMap(MapId id);
World BuildTestWorld();                    // Dustyard
World BuildRedoubt();
World BuildOldTown();
std::vector<SpawnPoint> GetSpawnPoints();  // Dustyard's

// What a ray hits first: a box or the ground (the ground stops everything)
struct WorldHit {
    bool hit = false;
    float distance = 1e9f;
    Vector3 point{}, normal{};
    Surface surface = Surface::Plain;
    int box = -1;        // index into the world, -1 for the ground
};
WorldHit TraceWorld(Ray ray, const World& world, float maxDistance = 1e9f);

// Does an upright cylinder (feet position, radius, height) overlap any box?
bool CylinderCollides(Vector3 feet, float radius, float height, const World& world);
// Is the straight line between two points free of walls/cover? (Targets don't block.)
bool HasLineOfSight(Vector3 from, Vector3 to, const World& world);
// Top of the highest box at or below `y` under the point (x, z), or the ground (0)
float FloorUnder(Vector3 p, float radius, const World& world);

// ---------------------------------------------------------------------------

template <class F> void World::ForEachNear(float ax, float bx, float az, float bz, F fn) const {
    const WorldGrid& g = Grid();
    if (g.nx == 0) return;
    int i0 = (int)((ax - g.x0) / g.cell), i1 = (int)((bx - g.x0) / g.cell);
    int k0 = (int)((az - g.z0) / g.cell), k1 = (int)((bz - g.z0) / g.cell);
    if (i1 < 0 || k1 < 0 || i0 >= g.nx || k0 >= g.nz || ax > bx || az > bz) return;
    i0 = i0 < 0 ? 0 : i0; k0 = k0 < 0 ? 0 : k0;
    i1 = i1 >= g.nx ? g.nx - 1 : i1; k1 = k1 >= g.nz ? g.nz - 1 : k1;
    unsigned q = ++g.query;
    for (int i = i0; i <= i1; i++)
        for (int k = k0; k <= k1; k++) {
            int c = i * g.nz + k;
            for (int j = g.start[c]; j < g.start[c + 1]; j++) {
                int b = g.items[j];
                if (g.stamp[b] == q) continue;
                g.stamp[b] = q;
                fn(b);
            }
        }
}

template <class F> void World::ForEachOnRay(Ray ray, float maxDistance, F fn) const {
    const WorldGrid& g = Grid();
    if (g.nx == 0) return;
    const float gx1 = g.x0 + g.nx * g.cell, gz1 = g.z0 + g.nz * g.cell;
    float ox = ray.position.x, oz = ray.position.z, dx = ray.direction.x, dz = ray.direction.z;
    // Clip the ray to the grid (seen from above)
    float tmin = 0.0f, tmax = maxDistance;
    auto slab = [&](float o, float d, float lo, float hi) {
        if (d > -1e-9f && d < 1e-9f) return o >= lo && o <= hi;
        float t1 = (lo - o) / d, t2 = (hi - o) / d;
        if (t1 > t2) { float t = t1; t1 = t2; t2 = t; }
        if (t1 > tmin) tmin = t1;
        if (t2 < tmax) tmax = t2;
        return tmin <= tmax;
    };
    if (!slab(ox, dx, g.x0, gx1) || !slab(oz, dz, g.z0, gz1)) return;
    float px = ox + dx * tmin, pz = oz + dz * tmin;
    int i = (int)((px - g.x0) / g.cell), k = (int)((pz - g.z0) / g.cell);
    i = i < 0 ? 0 : (i >= g.nx ? g.nx - 1 : i);
    k = k < 0 ? 0 : (k >= g.nz ? g.nz - 1 : k);
    const float INF = 1e30f;
    int si = dx > 0 ? 1 : -1, sk = dz > 0 ? 1 : -1;
    float adx = dx < 0 ? -dx : dx, adz = dz < 0 ? -dz : dz;
    float deltaX = adx > 1e-9f ? g.cell / adx : INF, deltaZ = adz > 1e-9f ? g.cell / adz : INF;
    float nextX = adx > 1e-9f ? ((g.x0 + (i + (dx > 0 ? 1 : 0)) * g.cell) - ox) / dx : INF;
    float nextZ = adz > 1e-9f ? ((g.z0 + (k + (dz > 0 ? 1 : 0)) * g.cell) - oz) / dz : INF;
    unsigned q = ++g.query;
    float best = maxDistance;
    for (;;) {
        int c = i * g.nz + k;
        for (int j = g.start[c]; j < g.start[c + 1]; j++) {
            int b = g.items[j];
            if (g.stamp[b] == q) continue;
            g.stamp[b] = q;
            float d = fn(b);
            if (d < best) best = d;
        }
        float next = nextX < nextZ ? nextX : nextZ;
        if (next > best || next > tmax) return;
        if (nextX < nextZ) { i += si; nextX += deltaX; if (i < 0 || i >= g.nx) return; }
        else { k += sk; nextZ += deltaZ; if (k < 0 || k >= g.nz) return; }
    }
}
