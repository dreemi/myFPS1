#pragma once
#include "raylib.h"
#include <vector>

// What a box is made of (decides its texture)
enum class Surface { Plain, Concrete, Steel };

// A solid axis-aligned box in the world: either a wall or a shootable target.
struct WorldBox {
    Vector3 position;   // center
    Vector3 size;
    Color color;        // used when it has no texture (Plain, or textures missing)
    bool isTarget = false;
    float hitFlash = 0.0f; // seconds remaining of hit-feedback flash
    Surface surface = Surface::Plain;

    BoundingBox Bounds() const {
        return {
            { position.x - size.x / 2, position.y - size.y / 2, position.z - size.z / 2 },
            { position.x + size.x / 2, position.y + size.y / 2, position.z + size.z / 2 },
        };
    }
};

using World = std::vector<WorldBox>;

struct SpawnPoint {
    Vector3 position;  // feet position on the ground
    float yaw;         // facing direction (0 = +Z)
    Color color;       // floor marker color
};

// Arena is ARENA_WIDTH along X and ARENA_LENGTH along Z, centered on the origin
constexpr float ARENA_WIDTH = 40.0f;
constexpr float ARENA_LENGTH = 80.0f;

World BuildTestWorld();

// Does an upright cylinder (feet position, radius, height) overlap any box?
bool CylinderCollides(Vector3 feet, float radius, float height, const World& world);
// Is the straight line between two points free of walls/cover? (Targets don't block.)
bool HasLineOfSight(Vector3 from, Vector3 to, const World& world);
std::vector<SpawnPoint> GetSpawnPoints(); // one at each end, facing the other
