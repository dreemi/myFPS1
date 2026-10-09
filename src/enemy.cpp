#include "enemy.hpp"
#include "raymath.h"
#include "rlgl.h"

static float DistanceXZ(Vector3 a, Vector3 b) {
    return sqrtf((a.x - b.x) * (a.x - b.x) + (a.z - b.z) * (a.z - b.z));
}

static Vector3 NormalizeXZ(Vector3 v, Vector3 fallback) {
    v.y = 0.0f;
    float len = Vector3Length(v);
    return len > 1e-4f ? Vector3Scale(v, 1.0f / len) : fallback;
}

// ---------------------------------------------------------------------------
// Player hitboxes for enemy bullets
// ---------------------------------------------------------------------------

EnemyTarget MakeTarget(Vector3 feet, float eyeHeight) {
    EnemyTarget t;
    t.feet = feet;
    t.eye = { feet.x, feet.y + eyeHeight, feet.z };
    t.bodyTop = eyeHeight - 0.25f; // body box ends just below the head sphere
    return t;
}

ShotTrace TraceShotAtPlayer(Ray ray, const EnemyTarget& target, const World& world) {
    const float maxRange = 100.0f;
    float wallDist = maxRange;
    for (const WorldBox& box : world) {
        RayCollision hit = GetRayCollisionBox(ray, box.Bounds());
        if (hit.hit && hit.distance < wallDist) wallDist = hit.distance;
    }

    ShotTrace result;
    result.end = Vector3Add(ray.position, Vector3Scale(ray.direction, wallDist));
    if (!target.alive) return result;

    const float hw = PLAYER_BODY_HALF_WIDTH;
    BoundingBox body = { { target.feet.x - hw, target.feet.y, target.feet.z - hw },
                         { target.feet.x + hw, target.feet.y + target.bodyTop, target.feet.z + hw } };
    RayCollision b = GetRayCollisionBox(ray, body);
    RayCollision h = GetRayCollisionSphere(ray, target.eye, PLAYER_HEAD_RADIUS);

    float best = wallDist;
    if (b.hit && b.distance < best) { best = b.distance; result.hitPlayer = true; result.headshot = false; result.end = b.point; }
    if (h.hit && h.distance < best) { best = h.distance; result.hitPlayer = true; result.headshot = true; result.end = h.point; }
    return result;
}

// ---------------------------------------------------------------------------
// Enemy
// ---------------------------------------------------------------------------

Enemy::Enemy(unsigned seed) : rng(seed) {}

float Enemy::Rand(float lo, float hi) {
    return std::uniform_real_distribution<float>(lo, hi)(rng);
}

Vector3 Enemy::RandomOpenPoint(const World& world) {
    const float margin = 3.0f;
    Vector3 p = feet;
    for (int i = 0; i < 100; i++) {
        p = { Rand(-ARENA_WIDTH / 2 + margin, ARENA_WIDTH / 2 - margin), 0.0f,
              Rand(-ARENA_LENGTH / 2 + margin, ARENA_LENGTH / 2 - margin) };
        if (!CylinderCollides(p, radius + 0.5f, height, world)) return p;
    }
    return p;
}

void Enemy::Spawn(Vector3 playerFeet, const World& world) {
    Vector3 p = RandomOpenPoint(world);
    for (int i = 0; i < 100 && DistanceXZ(p, playerFeet) < minSpawnDistance; i++) p = RandomOpenPoint(world);
    SpawnAt(p);
}

void Enemy::SpawnAt(Vector3 position) {
    feet = position;
    wanderTarget = position; // forces a fresh wander target on the first update
    moveTarget = position;
    alive = true;
    health = maxHealth;
    hitFlash = 0.0f;
    muzzleFlash = 0.0f;
    alertTimer = 0.0f;
    decisionTimer = 0.0f;
    blocked = false;
    replan = false;
    pendingShots.clear();
    Enter(EnemyState::Wander);
}

void Enemy::Enter(EnemyState next) {
    state = next;
    stateTimer = 0.0f;
    if (next == EnemyState::Shoot) shotsThisPosition = 0;
}

BoundingBox Enemy::BodyBounds() const {
    return { { feet.x - torsoHalfWidth, feet.y, feet.z - torsoHalfWidth },
             { feet.x + torsoHalfWidth, feet.y + torsoHeight, feet.z + torsoHalfWidth } };
}

Vector3 Enemy::HeadCenter() const {
    return { feet.x, feet.y + headHeight, feet.z };
}

Vector3 Enemy::HandPosition() const {
    // Matches the character model's aiming pose: right palm at chest height,
    // half a meter in front (see aim_grip in assets/models/character/character.txt)
    Vector3 right = { -faceDir.z, 0.0f, faceDir.x };
    Vector3 palm = Vector3Add({ feet.x, feet.y + 1.354f, feet.z },
                              Vector3Add(Vector3Scale(faceDir, 0.493f), Vector3Scale(right, 0.0235f)));
    return Vector3Add(palm, { 0, GRIP_ABOVE_PALM, 0 });
}

Vector3 Enemy::GetMuzzle() const {
    // The world model's muzzle, carried into the world by the hand's position and facing
    Vector3 m = weaponModels->worldMuzzle; // gun space: barrel along -Z
    return Vector3Add(HandPosition(), Vector3Add(Vector3Scale(faceDir, -m.z), { 0, m.y, 0 }));
}

EnemyRayHit Enemy::Raycast(Ray ray) const {
    EnemyRayHit result;
    if (!alive) return result;
    RayCollision head = GetRayCollisionSphere(ray, HeadCenter(), headRadius);
    RayCollision body = GetRayCollisionBox(ray, BodyBounds());
    if (head.hit && (!body.hit || head.distance <= body.distance)) {
        result = { true, true, head.distance, head.point };
    } else if (body.hit) {
        result = { true, false, body.distance, body.point };
    }
    return result;
}

bool Enemy::TakeHit(Vector3 shooterPosition, int damage) {
    if (!alive) return false;
    health -= damage;
    hitFlash = 0.15f;
    // Getting shot tells it where the player is; if it wasn't already
    // fighting, it picks a firing position straight away
    alertTimer = alertMemory;
    lastKnownEye = shooterPosition;
    lastKnownFeet = { shooterPosition.x, 0.0f, shooterPosition.z };
    if (state == EnemyState::Wander || state == EnemyState::Search) replan = true;
    if (health <= 0) {
        alive = false;
        respawnTimer = respawnDelay;
        pendingShots.clear();
        return true;
    }
    return false;
}

std::vector<Ray> Enemy::TakeShots() {
    std::vector<Ray> shots;
    shots.swap(pendingShots);
    return shots;
}

// --- Movement -------------------------------------------------------------

// Would walking 1.5 m in this direction hit anything?
bool Enemy::PathClear(Vector3 dir, const World& world) const {
    for (float d : { 0.5f, 1.0f, 1.5f }) {
        if (CylinderCollides(Vector3Add(feet, Vector3Scale(dir, d)), radius, height, world)) return false;
    }
    return true;
}

// Can it walk from one point to another in a straight line?
bool Enemy::PathWalkable(Vector3 from, Vector3 to, const World& world) const {
    float dist = DistanceXZ(from, to);
    int steps = (int)(dist / 0.4f) + 1;
    for (int i = 1; i <= steps; i++) {
        Vector3 p = Vector3Lerp(from, to, (float)i / steps);
        if (CylinderCollides(p, radius + 0.05f, height, world)) return false;
    }
    return true;
}

// Try 16 directions and pick the open one closest to the goal
Vector3 Enemy::ChooseDirection(Vector3 goal, const World& world) const {
    Vector3 preferred = NormalizeXZ(Vector3Subtract(goal, feet), moveDir);
    Vector3 best = Vector3Negate(moveDir); // if boxed in, turn around
    float bestScore = -1e9f;
    for (int i = 0; i < 16; i++) {
        float a = i * 2.0f * PI / 16.0f;
        Vector3 dir = { sinf(a), 0.0f, cosf(a) };
        if (!PathClear(dir, world)) continue;
        // Small bonus for keeping the current heading avoids jittering
        float score = Vector3DotProduct(dir, preferred) + 0.3f * Vector3DotProduct(dir, moveDir);
        if (score > bestScore) {
            bestScore = score;
            best = dir;
        }
    }
    return best;
}

void Enemy::MoveToward(Vector3 goal, float speed, float dt, const World& world) {
    // Close to the goal: head straight for it so it stops exactly there
    float remaining = DistanceXZ(feet, goal);
    decisionTimer -= dt;
    if (remaining < 1.5f && PathWalkable(feet, goal, world)) {
        moveDir = NormalizeXZ(Vector3Subtract(goal, feet), moveDir);
    } else if (decisionTimer <= 0.0f || blocked) {
        moveDir = ChooseDirection(goal, world);
        decisionTimer = 0.2f;
        blocked = false;
    }
    faceDir = moveDir;

    // Move, resolving each axis separately so it slides along walls
    float step = fminf(speed * dt, remaining);
    Vector3 move = Vector3Scale(moveDir, step);
    Vector3 start = feet;
    Vector3 tryX = { feet.x + move.x, feet.y, feet.z };
    if (!CylinderCollides(tryX, radius, height, world)) feet = tryX;
    Vector3 tryZ = { feet.x, feet.y, feet.z + move.z };
    if (!CylinderCollides(tryZ, radius, height, world)) feet = tryZ;

    // Barely moved? Pick a new direction next frame
    if (DistanceXZ(start, feet) < 0.3f * step) blocked = true;
}

void Enemy::FaceToward(Vector3 point) {
    faceDir = NormalizeXZ(Vector3Subtract(point, feet), faceDir);
}

// --- Combat ---------------------------------------------------------------

// Pick somewhere to fire from: walkable in a straight line, ideally with a
// view of the player at about idealRange, near cover, and not too far to run.
void Enemy::PlanPosition(const World& world) {
    replan = false;
    Vector3 best = feet;
    float bestScore = -1e9f;
    for (int i = 0; i < 40; i++) {
        float angle = Rand(0.0f, 2.0f * PI);
        float r = Rand(minMove, planRadius); // always actually moves somewhere new
        Vector3 p = { feet.x + sinf(angle) * r, 0.0f, feet.z + cosf(angle) * r };
        if (fabsf(p.x) > ARENA_WIDTH / 2 - 1.0f || fabsf(p.z) > ARENA_LENGTH / 2 - 1.0f) continue;
        if (CylinderCollides(p, radius + 0.1f, height, world)) continue;
        if (!PathWalkable(feet, p, world)) continue;

        float toPlayer = DistanceXZ(p, lastKnownFeet);
        bool canSee = HasLineOfSight({ p.x, p.y + eyeHeight, p.z }, lastKnownEye, world);
        bool nearCover = CylinderCollides(p, radius + 1.2f, 1.2f, world);

        float score = (canSee ? 10.0f : 0.0f)              // must be able to shoot from there
                    - fabsf(toPlayer - idealRange)          // at a sensible range
                    - (toPlayer < minRange ? 5.0f : 0.0f)   // never close in on the player
                    - 0.08f * r                             // shorter runs are a bit better
                    + (nearCover ? 1.5f : 0.0f)             // somewhere to hide is a bonus
                    + Rand(0.0f, 0.5f);                     // a little unpredictability
        if (score > bestScore) {
            bestScore = score;
            best = p;
        }
    }
    moveTarget = best;
    blocked = false;
    decisionTimer = 0.0f;
    Enter(EnemyState::Move);
}

void Enemy::FireAt(const EnemyTarget& target, const World& world) {
    // Aim for the chest, or the head if that's all it can see
    Vector3 eye = Eye();
    Vector3 chest = { target.feet.x, target.feet.y + target.bodyTop * 0.7f, target.feet.z };
    Vector3 aimPoint = HasLineOfSight(eye, chest, world) ? chest : target.eye;
    Vector3 forward = Vector3Normalize(Vector3Subtract(aimPoint, eye));

    // Random direction inside the spread cone (same method as the player's pistol)
    Vector3 right = Vector3Normalize(Vector3CrossProduct(forward, { 0, 1, 0 }));
    Vector3 up = Vector3CrossProduct(right, forward);
    float angle = shotSpread * DEG2RAD * sqrtf(Rand(0.0f, 1.0f));
    float around = Rand(0.0f, 2.0f * PI);
    Vector3 offset = Vector3Add(Vector3Scale(right, cosf(around)), Vector3Scale(up, sinf(around)));
    Vector3 dir = Vector3Normalize(Vector3Add(forward, Vector3Scale(offset, tanf(angle))));

    // Bullets start at the eyes (never inside a wall); the tracer is drawn from the gun
    pendingShots.push_back({ eye, dir });
    shotsThisPosition++;
    muzzleFlash = 0.06f;
    FaceToward(target.feet);
}

void Enemy::Update(float dt, const EnemyTarget& target, const World& world) {
    dt = fminf(dt, 0.05f);

    if (!alive) {
        respawnTimer -= dt;
        if (respawnTimer <= 0.0f) Spawn(target.feet, world);
        return;
    }
    hitFlash = fmaxf(0.0f, hitFlash - dt);
    muzzleFlash = fmaxf(0.0f, muzzleFlash - dt);
    stateTimer += dt;

    // Can it see the player?
    Vector3 eye = Eye();
    bool sees = target.alive && Vector3Distance(eye, target.eye) < sightRange &&
                HasLineOfSight(eye, target.eye, world);
    if (sees) {
        alertTimer = alertMemory;
        lastKnownFeet = target.feet;
        lastKnownEye = target.eye;
    } else {
        alertTimer = fmaxf(0.0f, alertTimer - dt);
    }

    Vector3 before = feet;
    switch (state) {
    case EnemyState::Wander:
        if (sees || replan) { PlanPosition(world); break; }
        if (DistanceXZ(feet, wanderTarget) < 1.0f || blocked) wanderTarget = RandomOpenPoint(world);
        MoveToward(wanderTarget, wanderSpeed, dt, world);
        break;

    case EnemyState::Move:
        if (DistanceXZ(feet, moveTarget) < 0.1f || stateTimer > moveTimeout) {
            Enter(sees ? EnemyState::Aim : EnemyState::Search);
        } else {
            MoveToward(moveTarget, runSpeed, dt, world);
        }
        break;

    case EnemyState::Aim:  // standing still, lining up the first shot
        if (!sees) { Enter(EnemyState::Search); break; }
        FaceToward(target.feet);
        if (stateTimer >= reactionTime) {
            Enter(EnemyState::Shoot);
            FireAt(target, world);
        }
        break;

    case EnemyState::Shoot:  // standing still between shots
        if (stateTimer < shotInterval) { if (sees) FaceToward(target.feet); break; }
        if (shotsThisPosition >= shotsPerPosition) { PlanPosition(world); break; }
        if (!sees) { Enter(EnemyState::Search); break; }
        stateTimer = 0.0f;
        FireAt(target, world);
        break;

    case EnemyState::Search:
        if (sees || replan) { PlanPosition(world); break; }
        if (stateTimer > searchTime) { Enter(EnemyState::Wander); break; }
        if (DistanceXZ(feet, lastKnownFeet) > 0.5f) MoveToward(lastKnownFeet, runSpeed, dt, world);
        break;
    }

    // Animation: how fast it actually moved picks walk/run, and the distance
    // covered advances the stride (so feet don't slide)
    float moved = DistanceXZ(before, feet);
    if (dt > 0.0f) moveSpeed += (moved / dt - moveSpeed) * (1.0f - expf(-10.0f * dt));
    animPhase += moved / (GetAnim() == CharAnim::Run ? runStride : walkStride);
}

CharAnim Enemy::GetAnim() const {
    if (moveSpeed > 3.5f) return CharAnim::Run;
    if (moveSpeed > 0.3f) return CharAnim::Walk;
    return CharAnim::Idle;
}

// --- Drawing --------------------------------------------------------------

void Enemy::Draw(CharacterAssets* character) const {
    if (!alive) return;
    float yawDeg = atan2f(faceDir.x, faceDir.z) * RAD2DEG;

    if (character && character->loaded) {
        // The character model, turned to face where it's looking
        CharAnim anim = GetAnim();
        int frame = CharacterFrame(*character, anim, animPhase);
        Matrix toWorld = MatrixMultiply(MatrixRotateY(yawDeg * DEG2RAD), MatrixTranslate(feet.x, feet.y, feet.z));
        DrawCharacter(*character, anim, frame, toWorld, PART_ALL, hitFlash > 0.0f ? RED : WHITE);

        // The revolver's world model in its right hand (gun origin sits just above the palm)
        Vector3 palm = Vector3Transform(CharacterGripPoint(*character, anim, frame), toWorld);
        Vector3 gunOrigin = Vector3Add(palm, { 0, GRIP_ABOVE_PALM, 0 });
        rlPushMatrix();
            rlTranslatef(gunOrigin.x, gunOrigin.y, gunOrigin.z);
            rlRotatef(yawDeg + 180.0f, 0, 1, 0); // gun space has the barrel along -Z
            weaponModels->drawWorld(muzzleFlash > 0.0f);
        rlPopMatrix();
        return;
    }

    // No character model loaded: simple placeholder shapes
    Color body = IsAlerted() ? Color{ 200, 60, 200, 255 } : Color{ 140, 70, 180, 255 };
    if (hitFlash > 0.0f) body = RED;

    Vector3 torso = { feet.x, feet.y + torsoHeight / 2, feet.z };
    Vector3 torsoSize = { torsoHalfWidth * 2, torsoHeight, torsoHalfWidth * 2 };
    DrawCubeV(torso, torsoSize, body);
    DrawCubeWiresV(torso, torsoSize, Fade(BLACK, 0.5f));

    Vector3 head = HeadCenter();
    DrawSphere(head, headRadius, hitFlash > 0.0f ? RED : Color{ 230, 200, 240, 255 });

    // Eyes face where it's looking
    Vector3 side = { -faceDir.z, 0.0f, faceDir.x };
    Vector3 eyeCenter = Vector3Add(head, Vector3Add(Vector3Scale(faceDir, headRadius - 0.03f), { 0, 0.05f, 0 }));
    DrawSphere(Vector3Add(eyeCenter, Vector3Scale(side, 0.1f)), 0.05f, BLACK);
    DrawSphere(Vector3Subtract(eyeCenter, Vector3Scale(side, 0.1f)), 0.05f, BLACK);

    // Its pistol: the weapon's world model, held out in its right hand
    Vector3 hand = HandPosition();
    rlPushMatrix();
        rlTranslatef(hand.x, hand.y, hand.z);
        rlRotatef(yawDeg, 0, 1, 0);   // +Z now points where it faces
        DrawCube({ 0.0f, -0.05f, -0.30f }, 0.1f, 0.1f, 0.35f, body); // forearm
        rlRotatef(180.0f, 0, 1, 0);   // gun space has the barrel along -Z
        weaponModels->drawWorld(muzzleFlash > 0.0f);
    rlPopMatrix();
}
