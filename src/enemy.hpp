#pragma once
#include "raylib.h"
#include "world.hpp"
#include "weapon_models.hpp"
#include "character.hpp"
#include <random>
#include <vector>

struct EnemyRayHit {
    bool hit = false;
    bool headshot = false;
    float distance = 0.0f;
    Vector3 point{};
};

// What an enemy knows about the player each frame
struct EnemyTarget {
    Vector3 feet{};
    Vector3 eye{};        // where the eyes really are (lower when crouched, off to the side when leaning)
    float bodyTop = 1.45f; // top of the body hitbox above the feet; the head sphere sits above it
    bool alive = true;
};

// The player's hitboxes for enemy bullets: a body box on the feet and a head sphere at the eyes
constexpr float PLAYER_BODY_HALF_WIDTH = 0.3f;
constexpr float PLAYER_HEAD_RADIUS = 0.2f;

// A player standing (or crouched) upright: eyes straight above the feet
EnemyTarget MakeTarget(Vector3 feet, float eyeHeight = 1.7f);

struct ShotTrace {
    bool hitPlayer = false;
    bool headshot = false;
    Vector3 end{};  // where the bullet stopped (player, wall, or max range)
    bool hitWorld = false;            // stopped by a wall, cover or the ground
    Vector3 normal{};                 // surface normal there
    Surface surface = Surface::Plain;
};
// Follows an enemy bullet: hits the player unless a wall or cover is in the way
ShotTrace TraceShotAtPlayer(Ray ray, const EnemyTarget& target, const World& world);

enum class EnemyState {
    Wander,  // hasn't seen the player: strolls around
    Move,    // running to a firing position
    Aim,     // stopped, reacting before the first shot
    Shoot,   // firing its two shots
    Search,  // lost sight: heading to where it last saw the player
};

// An opponent with a pistol. When it sees the player it runs to a spot at a
// sensible range (with line of sight, ideally near cover), stops, fires two
// shots, then moves to a new spot and repeats.
class Enemy {
public:
    explicit Enemy(unsigned seed = 1);

    void Spawn(Vector3 playerFeet, const World& world); // random spot far from the player
    void SpawnAt(Vector3 feet);
    void Update(float dt, const EnemyTarget& target, const World& world);
    // Shorthands: a player standing upright with eyes at the given height, or at a given point
    void Update(float dt, Vector3 playerFeet, const World& world, float playerEyeHeight = 1.7f) {
        Update(dt, MakeTarget(playerFeet, playerEyeHeight), world);
    }
    void Update(float dt, Vector3 playerFeet, const World& world, Vector3 playerEye) {
        EnemyTarget t = MakeTarget(playerFeet);
        t.eye = playerEye;
        Update(dt, t, world);
    }

    // Bullets fired since the last call (usually 0 or 1). Caller traces them.
    std::vector<Ray> TakeShots();

    bool TakeHit(Vector3 shooterPosition, int damage);  // returns true if the hit killed it
    EnemyRayHit Raycast(Ray ray) const;  // nearest hit on the head or body hitbox
    void Draw(CharacterAssets* character) const; // null: placeholder shapes

    bool IsAlive() const { return alive; }
    bool IsAlerted() const { return alertTimer > 0.0f; } // has seen the player recently
    EnemyState GetState() const { return state; }
    CharAnim GetAnim() const;           // idle / walk / run, from how fast it is moving
    float GetAnimPhase() const { return animPhase; }
    Vector3 GetMoveTarget() const { return moveTarget; }
    Vector3 GetMuzzle() const;   // tip of its pistol, for tracers/flash
    bool IsMuzzleFlashing() const { return alive && muzzleFlash > 0.0f; }
    int GetHealth() const { return health; }
    int GetMaxHealth() const { return maxHealth; }
    Vector3 GetFeet() const { return feet; }
    float GetHeight() const { return height; }
    float GetRadius() const { return radius; }
    BoundingBox BodyBounds() const;  // the torso box
    Vector3 HeadCenter() const;      // the head sphere
    float GetHeadRadius() const { return headRadius; }

    // Tuning (public so tests can check behavior against them)
    float minRange = 8.0f;     // won't choose to stand closer than this to the player
    float idealRange = 13.0f;  // prefers firing positions about this far away
    float maxRange = 18.0f;
    float shotSpread = 3.0f;   // degrees, cone half-angle (about half its shots hit at 13 m)
    float minMove = 3.0f;      // each new firing position is at least this far from the last
    float reactionTime = 0.45f; // stopped and aiming before the first shot
    float shotInterval = 0.35f; // between the two shots (and before moving off)
    int shotsPerPosition = 2;

private:
    void Enter(EnemyState next);
    void PlanPosition(const World& world);
    void FireAt(const EnemyTarget& target, const World& world);
    void MoveToward(Vector3 goal, float speed, float dt, const World& world);
    void FaceToward(Vector3 point);
    bool PathWalkable(Vector3 from, Vector3 to, const World& world) const;
    Vector3 ChooseDirection(Vector3 goal, const World& world) const;
    bool PathClear(Vector3 dir, const World& world) const;
    Vector3 RandomOpenPoint(const World& world);
    Vector3 Eye() const { return { feet.x, feet.y + eyeHeight, feet.z }; }
    Vector3 HandPosition() const;  // where its gun hand is, in the world

    const WeaponModels* weaponModels = &REVOLVER_MODELS; // what its gun looks like to others
    float Rand(float lo, float hi);

    std::mt19937 rng;
    Vector3 feet{};
    Vector3 moveDir = { 0, 0, 1 };
    Vector3 faceDir = { 0, 0, 1 };
    Vector3 wanderTarget{};
    Vector3 moveTarget{};
    Vector3 lastKnownFeet{};
    Vector3 lastKnownEye{};
    std::vector<Ray> pendingShots;

    EnemyState state = EnemyState::Wander;
    float stateTimer = 0.0f;
    int shotsThisPosition = 0;
    bool replan = false;       // got shot: pick a position on the next update

    bool alive = true;
    int maxHealth = 100;
    int health = 100;
    float hitFlash = 0.0f;
    float muzzleFlash = 0.0f;
    float respawnTimer = 0.0f;
    float alertTimer = 0.0f;     // > 0 while it knows where the player is
    float decisionTimer = 0.0f;  // time until next steering direction choice
    bool blocked = false;

    float radius = 0.4f;
    float height = 1.8f;
    // Hitboxes match the character model: a torso box up to the neck and a head sphere
    float eyeHeight = 1.69f;
    float torsoHalfWidth = 0.25f;
    float torsoHeight = 1.55f;
    float headHeight = 1.70f;   // head center above the feet
    float headRadius = 0.12f;
    static constexpr float GRIP_ABOVE_PALM = 0.05f; // gun origin (top of the grip) above the palm

    float moveSpeed = 0.0f;     // smoothed, m/s
    float animPhase = 0.0f;     // stride cycles walked
    float walkStride = 1.4f;    // meters per walk cycle (two steps)
    float runStride = 2.2f;

    float wanderSpeed = 2.5f;
    float runSpeed = 4.5f;
    float sightRange = 30.0f;
    float alertMemory = 2.0f;    // still "alerted" this long after losing sight
    float planRadius = 10.0f;    // looks for firing positions within this distance of itself
    float moveTimeout = 4.0f;    // gives up on reaching a position after this long
    float searchTime = 4.0f;     // how long it searches before going back to wandering
    float respawnDelay = 3.0f;
    float minSpawnDistance = 20.0f;
};
