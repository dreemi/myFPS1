// Enemy and weapon tests: drive Enemy::Update, Weapon::Fire and the ViewModel with scripted input, no window needed.
#include "../src/enemy.hpp"
#include "../src/weapon.hpp"
#include "../src/viewmodel.hpp"
#include "raymath.h"
#include <cmath>
#include <cstdio>

static int failures = 0;

#define CHECK(cond, ...)                                   \
    do {                                                   \
        if (cond) { std::printf("  pass: "); }             \
        else { std::printf("  FAIL: "); failures++; }      \
        std::printf(__VA_ARGS__);                          \
        std::printf("\n");                                 \
    } while (0)

static const float DT = 1.0f / 60.0f;

static float DistXZ(Vector3 a, Vector3 b) { return sqrtf((a.x - b.x) * (a.x - b.x) + (a.z - b.z) * (a.z - b.z)); }

static bool InsideArena(Vector3 p) {
    return fabsf(p.x) < ARENA_WIDTH / 2 && fabsf(p.z) < ARENA_LENGTH / 2;
}

// Just the four outer walls, for open-field tests
static World OuterWalls() {
    World full = BuildTestWorld();
    return World(full.begin(), full.begin() + 4);
}

static Camera3D CameraAt(Vector3 eye, Vector3 lookAt) {
    Camera3D c = { 0 };
    c.position = eye;
    c.target = lookAt;
    c.up = { 0, 1, 0 };
    c.fovy = 75;
    c.projection = CAMERA_PERSPECTIVE;
    return c;
}

// Crouched and fully aimed: spread is 0.18 deg, too small to miss the exact
// spots the damage tests aim at
static Weapon PrecisePistol() {
    Weapon p(REVOLVER, 1);
    for (int i = 0; i < 120; i++) {
        p.UpdateAim(DT, true);
        p.UpdateSpread(DT, Stance::CrouchStill);
    }
    return p;
}

int main() {
    const World arena = BuildTestWorld();
    const Vector3 spawnA = GetSpawnPoints()[0].position;

    std::printf("Spawning\n");
    {
        int ok = 0, total = 200;
        float closest = 1e9f;
        for (unsigned seed = 1; seed <= (unsigned)total; seed++) {
            Enemy e(seed);
            e.Spawn(spawnA, arena);
            float d = DistXZ(e.GetFeet(), spawnA);
            closest = fminf(closest, d);
            if (d >= 20.0f && InsideArena(e.GetFeet()) &&
                !CylinderCollides(e.GetFeet(), e.GetRadius(), e.GetHeight(), arena))
                ok++;
        }
        CHECK(ok == total, "%d/%d random spawns are open, inside the arena and >= 20 m from the player (closest %.1f m)",
              ok, total, closest);
    }

    std::printf("Wanders when it can't see the player\n");
    {
        // A wall between them blocks line of sight
        World w = OuterWalls();
        w.push_back({ { 0, 2, 0 }, { 30, 4, 1 }, GRAY });
        Enemy e(7);
        e.SpawnAt({ 0, 0, 10 });
        Vector3 player = { 0, 0, -10 };
        float travelled = 0;
        bool everAlerted = false;
        Vector3 prev = e.GetFeet();
        for (int i = 0; i < 5 * 60; i++) {
            e.Update(DT, player, w);
            travelled += DistXZ(prev, e.GetFeet());
            prev = e.GetFeet();
            everAlerted |= e.IsAlerted();
        }
        CHECK(!everAlerted, "never noticed the player through the wall");
        CHECK(travelled > 8.0f && travelled < 13.0f, "walked around at wander speed (%.1f m in 5 s, expected ~12.5)",
              travelled);
    }

    std::printf("Fights: move to a position, stop, shoot twice, move on\n");
    {
        World w = OuterWalls();
        w.push_back({ { 6, 0.75f, -12 }, { 2, 1.5f, 2 }, GRAY }); // a bit of cover around
        w.push_back({ { -5, 0.75f, -6 }, { 2, 1.5f, 2 }, GRAY });
        Enemy e(3);
        Vector3 start = { 0, 0, 0 };
        e.SpawnAt(start);
        EnemyTarget player = MakeTarget({ 0, 0, -25 });

        struct Shot { float time; Vector3 from; bool movedThisFrame; Ray ray; };
        std::vector<Shot> shots;
        std::vector<Vector3> positions;      // where it stopped to shoot
        std::vector<Vector3> plannedTargets; // where it decided to go
        std::vector<float> arriveTimes;
        EnemyState prevState = e.GetState();
        float t = 0;
        for (int i = 0; i < 20 * 60; i++, t += DT) {
            Vector3 before = e.GetFeet();
            e.Update(DT, player, w);
            EnemyState s = e.GetState();
            if (s == EnemyState::Move && prevState != EnemyState::Move) plannedTargets.push_back(e.GetMoveTarget());
            if (s == EnemyState::Aim && prevState != EnemyState::Aim) arriveTimes.push_back(t);
            prevState = s;
            for (const Ray& r : e.TakeShots()) {
                bool moved = DistXZ(before, e.GetFeet()) > 1e-5f;
                shots.push_back({ t, e.GetFeet(), moved, r });
                if (positions.empty() || DistXZ(positions.back(), e.GetFeet()) > 1e-3f) positions.push_back(e.GetFeet());
            }
        }

        CHECK(!plannedTargets.empty() && DistXZ(plannedTargets[0], start) >= 2.0f,
              "on spotting the player it picks a spot %.1f m away and runs there",
              plannedTargets.empty() ? 0.0f : DistXZ(plannedTargets[0], start));
        bool allStill = true;
        for (const Shot& s : shots) allStill &= !s.movedThisFrame;
        CHECK(!shots.empty() && allStill, "never fires while moving (%d shots)", (int)shots.size());

        // Group shots by firing position
        int groupsOfTwo = 0, groups = 0;
        bool intervalOk = true;
        for (size_t i = 0; i < shots.size();) {
            size_t j = i;
            while (j < shots.size() && DistXZ(shots[j].from, shots[i].from) < 1e-3f) j++;
            groups++;
            if (j - i == 2) {
                groupsOfTwo++;
                intervalOk &= fabsf(shots[i + 1].time - shots[i].time - e.shotInterval) < 2 * DT;
            }
            i = j;
        }
        CHECK(groups >= 3 && groupsOfTwo == groups, "fires exactly 2 shots per position (%d positions, all pairs: %s)",
              groups, groupsOfTwo == groups ? "yes" : "no");
        CHECK(intervalOk, "the 2 shots are %.2f s apart", e.shotInterval);

        bool spacedOut = true;
        float smallestGap = 1e9f;
        for (size_t i = 1; i < positions.size(); i++) {
            float gap = DistXZ(positions[i], positions[i - 1]);
            smallestGap = fminf(smallestGap, gap);
            spacedOut &= gap >= e.minMove - 0.1f;
        }
        CHECK(spacedOut, "moves at least %.0f m between firing positions (smallest move %.1f m)", e.minMove,
              smallestGap);

        bool inRange = true, canSee = true;
        float minD = 1e9f, maxD = 0;
        for (Vector3 p : positions) {
            float d = DistXZ(p, player.feet);
            minD = fminf(minD, d);
            maxD = fmaxf(maxD, d);
            inRange &= d >= e.minRange && d <= e.maxRange + 0.5f;
            canSee &= HasLineOfSight({ p.x, 1.69f, p.z }, player.eye, w);
        }
        CHECK(inRange, "every firing position is %.0f-%.0f m from the player (actual %.1f-%.1f)", e.minRange,
              e.maxRange, minD, maxD);
        CHECK(canSee, "every firing position has a clear view of the player");

        // Reaction time: first shot at each position comes after a pause
        bool reacted = true;
        for (float arrive : arriveTimes) {
            for (const Shot& s : shots) {
                if (s.time >= arrive - 1e-4f) {
                    reacted &= s.time - arrive >= e.reactionTime - 2 * DT;
                    break;
                }
            }
        }
        CHECK(reacted, "waits %.2f s after stopping before the first shot", e.reactionTime);

        // Accuracy: aimed at the chest, within the spread cone
        bool inCone = true;
        int hits = 0;
        for (const Shot& s : shots) {
            Vector3 chest = { player.feet.x, player.bodyTop * 0.7f, player.feet.z };
            float off = Vector3Angle(s.ray.direction, Vector3Normalize(Vector3Subtract(chest, s.ray.position))) * RAD2DEG;
            inCone &= off <= e.shotSpread + 1e-3f;
            if (TraceShotAtPlayer(s.ray, player, w).hitPlayer) hits++;
        }
        CHECK(inCone, "every shot is within %.1f deg of the player's chest", e.shotSpread);
        CHECK(hits > 0 && hits < (int)shots.size(), "it can miss: %d of %d shots would have hit (%.0f%%)", hits,
              (int)shots.size(), 100.0f * hits / shots.size());
    }

    std::printf("Won't stand too close\n");
    {
        World w = OuterWalls();
        bool ok = true;
        float closest = 1e9f;
        for (unsigned seed = 1; seed <= 20; seed++) {
            Enemy e(seed);
            e.SpawnAt({ 0, 0, 0 });
            EnemyTarget player = MakeTarget({ 0, 0, -4 }); // only 4 m away
            e.Update(DT, player, w);
            float d = DistXZ(e.GetMoveTarget(), player.feet);
            closest = fminf(closest, d);
            ok &= e.GetState() == EnemyState::Move && d >= e.minRange;
        }
        CHECK(ok, "with the player 4 m away it backs off to at least 8 m (closest pick %.1f m)", closest);
    }

    std::printf("Holds fire without a target\n");
    {
        World w = OuterWalls();
        w.push_back({ { 0, 2, -5 }, { 38, 4, 1 }, GRAY }); // wall between them
        Enemy e(9);
        e.SpawnAt({ 0, 0, 5 });
        int shots = 0;
        for (int i = 0; i < 10 * 60; i++) {
            e.Update(DT, MakeTarget({ 0, 0, -15 }), w);
            shots += (int)e.TakeShots().size();
        }
        CHECK(shots == 0, "player behind a wall for 10 s: %d shots", shots);

        World open = OuterWalls();
        Enemy e2(9);
        e2.SpawnAt({ 0, 0, 0 });
        EnemyTarget dead = MakeTarget({ 0, 0, -13 });
        dead.alive = false;
        shots = 0;
        for (int i = 0; i < 10 * 60; i++) {
            e2.Update(DT, dead, open);
            shots += (int)e2.TakeShots().size();
        }
        CHECK(shots == 0 && !e2.IsAlerted(), "dead player in plain view for 10 s: %d shots", shots);

        // Killed mid-fight: no more shots
        Enemy e3(4);
        e3.SpawnAt({ 0, 0, 0 });
        EnemyTarget p = MakeTarget({ 0, 0, -13 });
        for (int i = 0; i < 10 * 60 && e3.GetState() != EnemyState::Shoot; i++) e3.Update(DT, p, open);
        e3.TakeShots();
        e3.TakeHit(p.eye, 100);
        shots = 0;
        for (int i = 0; i < (int)(2.5f * 60); i++) {
            e3.Update(DT, p, open);
            shots += (int)e3.TakeShots().size();
        }
        CHECK(shots == 0 && !e3.IsAlive(), "killed mid-fight: stops shooting");
    }

    std::printf("Reacts to being shot from out of sight\n");
    {
        World w = OuterWalls();
        w.push_back({ { 0, 2, -5 }, { 38, 4, 1 }, GRAY });
        Enemy e(12);
        e.SpawnAt({ 0, 0, 5 });
        EnemyTarget p = MakeTarget({ 0, 0, -15 });
        e.Update(DT, p, w);
        CHECK(e.GetState() == EnemyState::Wander, "unaware while hidden");
        e.TakeHit(p.eye, 25);
        e.Update(DT, p, w);
        CHECK(e.GetState() == EnemyState::Move, "after being hit it heads for a new position");
    }

    std::printf("Player hitboxes (for enemy bullets)\n");
    {
        World w = OuterWalls();
        auto shoot = [&](Vector3 from, Vector3 at, const EnemyTarget& t) {
            return TraceShotAtPlayer({ from, Vector3Normalize(Vector3Subtract(at, from)) }, t, w);
        };
        Vector3 gun = { 0, 1.4f, 0 };
        EnemyTarget standing = MakeTarget({ 0, 0, 10 });
        EnemyTarget crouched = MakeTarget({ 0, 0, 10 }, 1.1f);
        ShotTrace s = shoot(gun, { 0, 1.4f, 10 }, standing);
        CHECK(s.hitPlayer && !s.headshot, "standing: a shot at 1.4 m hits the body");
        s = shoot(gun, { 0, 1.4f, 10 }, crouched);
        CHECK(!s.hitPlayer, "crouched: the same shot goes over");
        s = shoot(gun, { 0, 1.7f, 10 }, standing);
        CHECK(s.hitPlayer && s.headshot, "a shot at eye level is a headshot");

        EnemyTarget leaning = standing;
        leaning.eye = { 0.45f, 1.64f, 10 };
        leaning.bodyTop = 1.64f - 0.25f;
        s = shoot(gun, { 0.45f, 1.64f, 10 }, leaning);
        CHECK(s.hitPlayer && s.headshot, "leaning: the head moves with the lean and can be hit there");
        s = shoot(gun, { 0, 1.64f, 10 }, leaning);
        CHECK(!s.hitPlayer, "leaning: the spot the head left is empty");

        w.push_back({ { 0, 2, 5 }, { 4, 4, 1 }, GRAY });
        s = shoot(gun, { 0, 1.4f, 10 }, standing);
        CHECK(!s.hitPlayer && fabsf(s.end.z - 4.5f) < 0.01f, "walls stop enemy bullets (stopped at z=%.2f)", s.end.z);
    }

    std::printf("Crouching behind low cover hides the player\n");
    {
        World w = OuterWalls();
        w.push_back({ { 0, 0.6f, -9 }, { 4, 1.2f, 0.5f }, GRAY }); // 1.2 m wall just in front of the player
        Vector3 player = { 0, 0, -10 };

        Enemy standingCheck(21);
        standingCheck.SpawnAt({ 0, 0, 5 });
        standingCheck.Update(DT, player, w, 1.7f);
        CHECK(standingCheck.IsAlerted(), "standing (eyes at 1.7 m): seen over the wall");

        Enemy crouchCheck(21);
        crouchCheck.SpawnAt({ 0, 0, 5 });
        crouchCheck.Update(DT, player, w, 1.1f);
        CHECK(!crouchCheck.IsAlerted(), "crouched (eyes at 1.1 m): hidden behind the wall");
    }

    std::printf("Leaning out from cover\n");
    {
        // Tall wall ending at x = 0; the player hides just behind its end
        World w = OuterWalls();
        w.push_back({ { -5, 2, -8 }, { 10, 4, 1 }, GRAY });
        Player player;
        player.Init({ -0.5f, 0, -10 }, 0.0f); // facing +Z, so leaning left moves toward +X
        PlayerInput none, peek;
        peek.leanLeft = true;
        for (int i = 0; i < 30; i++) player.Simulate(DT, none, w);

        Enemy hidden(31);
        hidden.SpawnAt({ 3, 0, 5 });
        hidden.Update(DT, player.GetFeet(), w, player.GetCamera().position);
        CHECK(!hidden.IsAlerted(), "upright behind the corner: not seen");

        for (int i = 0; i < 60; i++) player.Simulate(DT, peek, w);
        Enemy peeked(31);
        peeked.SpawnAt({ 3, 0, 5 });
        peeked.Update(DT, player.GetFeet(), w, player.GetCamera().position);
        CHECK(peeked.IsAlerted(), "leaning toward the corner: seen around it (head moved %.2f -> %.2f; wall ends at 0)",
              player.GetFeet().x, player.GetCamera().position.x);
        CHECK(fabsf(player.GetFeet().x + 0.5f) < 1e-4f, "while the feet stay behind the wall");
    }

    std::printf("Calms down after losing sight\n");
    {
        World w = OuterWalls();
        Enemy e(5);
        e.SpawnAt({ 0, 0, 0 });
        e.Update(DT, Vector3{ 0, 0, -10 }, w);
        CHECK(e.IsAlerted(), "alerted while visible");
        // Player "disappears" behind a new wall
        w.push_back({ { 0, 2, -5 }, { 38, 4, 1 }, GRAY });
        for (int i = 0; i < (int)(2.2f * 60); i++) e.Update(DT, Vector3{ 0, 0, -10 }, w);
        CHECK(!e.IsAlerted(), "back to wandering ~2 s after losing sight");
    }

    std::printf("Never ends up inside walls (60 s, player chasing it, 10 seeds)\n");
    {
        int bad = 0, frames = 0;
        float closestApproach = 1e9f;
        for (unsigned seed = 1; seed <= 10; seed++) {
            Enemy e(seed);
            Vector3 player = spawnA;
            e.Spawn(player, arena);
            for (int i = 0; i < 60 * 60; i++) {
                // Player walks straight at the enemy (ignores walls; just pressure)
                Vector3 to = Vector3Subtract(e.GetFeet(), player);
                to.y = 0;
                if (Vector3Length(to) > 0.5f) player = Vector3Add(player, Vector3Scale(Vector3Normalize(to), 5.0f * DT));
                e.Update(DT, player, arena);
                frames++;
                Vector3 f = e.GetFeet();
                if (!InsideArena(f) || CylinderCollides(f, e.GetRadius(), e.GetHeight(), arena)) bad++;
                closestApproach = fminf(closestApproach, DistXZ(f, player));
            }
        }
        CHECK(bad == 0, "%d of %d frames had the enemy inside a wall or outside the arena", bad, frames);
        std::printf("  info: closest a walking chaser got: %.2f m\n", closestApproach);
    }

    std::printf("Shooting\n");
    {
        World w = OuterWalls();
        std::vector<Enemy> enemies(1, Enemy(11));
        enemies[0].SpawnAt({ 0, 0, 10 });
        Weapon pistol = PrecisePistol();
        Vector3 eye = { 0, 1.7f, 0 };
        Camera3D aim = CameraAt(eye, { 0, 1.0f, 10 });

        // Body shots: 4 to kill
        pistol.Fire(aim, w, enemies);
        CHECK(enemies[0].GetHealth() == 75 && enemies[0].IsAlerted(),
              "body shot: health 100 -> %d and it knows where you are", enemies[0].GetHealth());
        pistol.Fire(aim, w, enemies);
        pistol.Fire(aim, w, enemies);
        CHECK(enemies[0].IsAlive() && enemies[0].GetHealth() == 25, "still alive after 3 body shots (health %d)",
              enemies[0].GetHealth());
        pistol.Fire(aim, w, enemies);
        CHECK(!enemies[0].IsAlive() && pistol.GetKills() == 1, "4th body shot kills it (alive=%d, kills=%d)",
              enemies[0].IsAlive(), pistol.GetKills());

        // Headshots: 2 to kill
        Camera3D headAim = CameraAt(eye, enemies[0].HeadCenter());
        enemies[0].SpawnAt({ 0, 0, 10 });
        pistol.Fire(headAim, w, enemies);
        CHECK(enemies[0].IsAlive() && enemies[0].GetHealth() == 50, "headshot: health 100 -> %d",
              enemies[0].GetHealth());
        pistol.Fire(headAim, w, enemies);
        CHECK(!enemies[0].IsAlive() && pistol.GetKills() == 2, "2nd headshot kills it (alive=%d, kills=%d)",
              enemies[0].IsAlive(), pistol.GetKills());

        // Mixed: 1 head + 2 body = 100
        enemies[0].SpawnAt({ 0, 0, 10 });
        pistol.Fire(headAim, w, enemies);
        pistol.Fire(aim, w, enemies);
        CHECK(enemies[0].IsAlive(), "1 head + 1 body leaves it alive (health %d)", enemies[0].GetHealth());
        pistol.Fire(aim, w, enemies);
        CHECK(!enemies[0].IsAlive(), "1 head + 2 body kills it");

        // Hitbox edges
        enemies[0].SpawnAt({ 0, 0, 10 });
        pistol.Fire(CameraAt(eye, { 0, 1.75f, 10 }), w, enemies);
        CHECK(enemies[0].GetHealth() == 50, "top of the head counts as a headshot (health %d)", enemies[0].GetHealth());
        enemies[0].SpawnAt({ 0, 0, 10 });
        pistol.Fire(CameraAt(eye, { 0, 1.2f, 10 }), w, enemies);
        CHECK(enemies[0].GetHealth() == 75, "upper chest counts as a body shot (health %d)", enemies[0].GetHealth());
        enemies[0].SpawnAt({ 0, 0, 10 });
        pistol.Fire(CameraAt(eye, { 0.32f, 1.45f, 10 }), w, enemies);
        CHECK(enemies[0].GetHealth() == 100, "a shot over the shoulder, beside the head, misses (health %d)",
              enemies[0].GetHealth());

        Camera3D miss = CameraAt(eye, { 3, 1.0f, 10 });
        pistol.Fire(miss, w, enemies);
        CHECK(enemies[0].GetHealth() == 100, "a shot 3 m to the side misses (health %d)", enemies[0].GetHealth());

        w.push_back({ { 0, 2, 5 }, { 4, 4, 1 }, GRAY }); // wall in between
        pistol.Fire(aim, w, enemies);
        pistol.Fire(headAim, w, enemies);
        CHECK(enemies[0].GetHealth() == 100, "walls stop bullets (health %d)", enemies[0].GetHealth());
    }

    std::printf("Aiming down sights\n");
    {
        Weapon pistol(REVOLVER);
        ViewModel vm(REVOLVER_MODELS);
        ViewModelMotion still;
        for (int i = 0; i < 60; i++) vm.Update(DT, still, pistol);
        // The world camera; shots go straight down its view axis
        Vector3 eye = { 3, 1.7f, -2 };
        Vector3 forward = Vector3Normalize({ 0.3f, 0.0f, 1 }); // level, so the target stands on the ground
        Camera3D cam = CameraAt(eye, Vector3Add(eye, forward));

        // The viewmodel is drawn in camera space, so "dead center" is the -Z axis
        auto inCameraSpace = [&](Vector3 gunPoint) { return Vector3Transform(gunPoint, vm.GunToCamera(pistol)); };
        auto angleOff = [&](Vector3 gunPoint) {
            return Vector3Angle(inCameraSpace(gunPoint), { 0, 0, -1 }) * RAD2DEG;
        };
        const Vector3 FRONT = REVOLVER_MODELS.frontSightTop, REAR = REVOLVER_MODELS.rearNotch;

        Vector3 hipFront = inCameraSpace(FRONT);
        CHECK(pistol.GetAimAmount() == 0.0f && hipFront.x > 0 && hipFront.y < 0 && angleOff(FRONT) > 10,
              "at the hip the gun is down and to the right (%.0f deg off center)", angleOff(FRONT));

        pistol.UpdateAim(DT, true);
        float afterOneFrame = pistol.GetAimAmount();
        for (int i = 0; i < 30; i++) pistol.UpdateAim(DT, true);
        CHECK(afterOneFrame > 0.1f && afterOneFrame < 0.5f && pistol.GetAimAmount() == 1.0f,
              "eases in: %.2f after 1 frame, fully aimed after 0.5 s", afterOneFrame);

        float front = angleOff(FRONT), rear = angleOff(REAR);
        CHECK(front < 0.01f && rear < 0.01f,
              "aimed: front sight and rear notch are dead center (%.4f and %.4f deg off)", front, rear);
        CHECK(inCameraSpace(FRONT).z < inCameraSpace(REAR).z, "front sight is farther from the eye than the rear notch");

        for (int i = 0; i < 30; i++) pistol.UpdateAim(DT, false);
        CHECK(pistol.GetAimAmount() == 0.0f, "back to the hip after letting go");

        // What the sights cover is what gets hit
        std::vector<Enemy> enemies(1, Enemy(17));
        enemies[0].SpawnAt(Vector3Add(Vector3Add(eye, Vector3Scale(forward, 12)), { 0, -enemies[0].HeadCenter().y + enemies[0].GetFeet().y, 0 })); // head on the sightline
        World w = OuterWalls();
        for (int i = 0; i < 60; i++) {
            pistol.UpdateAim(DT, true);
            pistol.UpdateSpread(DT, Stance::Still); // standing, aimed: 0.45 deg spread
        }
        int hits = 0;
        for (int i = 0; i < 20; i++) {
            enemies[0].SpawnAt(enemies[0].GetFeet());
            pistol.Fire(cam, w, enemies);
            if (enemies[0].GetHealth() == 50) hits++;
        }
        CHECK(hits == 20, "standing still and aimed, %d/20 shots hit the head under the sights (12 m away)", hits);
    }

    std::printf("First-person viewmodel animation\n");
    {
        // Where the gun's origin is in camera space, and how far the muzzle is tilted up (deg)
        auto gunPos = [](const ViewModel& vm, const Weapon& w) {
            return Vector3Transform({ 0, 0, 0 }, vm.GunToCamera(w));
        };
        auto muzzleUp = [](const ViewModel& vm, const Weapon& w) {
            Vector3 dir = Vector3Subtract(Vector3Transform({ 0, 0, -1 }, vm.GunToCamera(w)),
                                          Vector3Transform({ 0, 0, 0 }, vm.GunToCamera(w)));
            return asinf(Vector3Normalize(dir).y) * RAD2DEG;
        };
        // Run the viewmodel for some seconds and report the gun's movement range
        auto range = [&](ViewModel& vm, const Weapon& w, ViewModelMotion m, float seconds) {
            Vector3 lo = { 1e9f, 1e9f, 1e9f }, hi = { -1e9f, -1e9f, -1e9f };
            for (int i = 0; i < (int)(seconds * 60); i++) {
                vm.Update(DT, m, w);
                Vector3 p = gunPos(vm, w);
                lo = Vector3Min(lo, p);
                hi = Vector3Max(hi, p);
            }
            return Vector3Length(Vector3Subtract(hi, lo));
        };

        Weapon pistol(REVOLVER);
        ViewModel vm(REVOLVER_MODELS);
        ViewModelMotion still, walking, rising, falling, crouched;
        walking.speed = 5.0f;
        rising.grounded = false; rising.verticalVelocity = 8.0f;
        falling.grounded = false; falling.verticalVelocity = -8.0f;
        crouched.crouching = true;

        range(vm, pistol, still, 1.0f);
        Vector3 rest = gunPos(vm, pistol);
        float idleMove = range(vm, pistol, still, 2.0f);
        CHECK(idleMove > 0.0005f && idleMove < 0.006f, "idle: gentle breathing (%.1f mm of movement)", idleMove * 1000);
        float walkMove = range(vm, pistol, walking, 2.0f);
        CHECK(walkMove > 0.015f && walkMove > 4 * idleMove, "walking: bobs (%.0f mm of movement)", walkMove * 1000);

        ViewModel jumpVm(REVOLVER_MODELS);
        range(jumpVm, pistol, rising, 0.3f);
        float riseY = gunPos(jumpVm, pistol).y - rest.y;
        range(jumpVm, pistol, falling, 0.4f);
        float fallY = gunPos(jumpVm, pistol).y - rest.y;
        CHECK(riseY < -0.015f && fallY > 0.015f, "jumping: gun dips while rising (%.0f mm), lifts while falling (+%.0f mm)",
              riseY * 1000, fallY * 1000);
        jumpVm.Update(DT, still, pistol); // land
        for (int i = 0; i < 3; i++) jumpVm.Update(DT, still, pistol);
        float landY = gunPos(jumpVm, pistol).y - rest.y;
        range(jumpVm, pistol, still, 1.0f);
        float settledY = gunPos(jumpVm, pistol).y - rest.y;
        CHECK(landY < -0.005f && fabsf(settledY) < 0.004f, "landing: dips %.0f mm below rest (net of the fall lift unwinding), then settles", landY * 1000);

        ViewModel crouchVm(REVOLVER_MODELS);
        range(crouchVm, pistol, crouched, 1.0f);
        CHECK(gunPos(crouchVm, pistol).y < rest.y - 0.008f, "crouching: gun tucks lower");

        // Firing: kick back and up, then recover
        Weapon fired(REVOLVER);
        ViewModel fireVm(REVOLVER_MODELS);
        range(fireVm, fired, still, 0.5f);
        float restUp = muzzleUp(fireVm, fired);
        World nothing;
        std::vector<Enemy> noEnemies;
        float cylinderBefore = fireVm.Mechanism().cylinder;
        fired.Fire(CameraAt({ 0, 1.7f, 0 }, { 0, 1.7f, 1 }), nothing, noEnemies);
        bool flashed = fired.IsMuzzleFlashing();
        float kickedBack = 0.0f, kickedUp = 0.0f, hammerAtShot = -1.0f, triggerAtShot = -1.0f;
        for (int i = 0; i < 8; i++) { // the springs peak a few frames after the shot
            fireVm.Update(DT, still, fired);
            if (i == 0) { hammerAtShot = fireVm.Mechanism().hammer; triggerAtShot = fireVm.Mechanism().trigger; }
            kickedBack = fmaxf(kickedBack, gunPos(fireVm, fired).z - rest.z);
            kickedUp = fmaxf(kickedUp, muzzleUp(fireVm, fired) - restUp);
            fired.Tick(DT, Stance::Still);
        }
        CHECK(kickedBack > 0.02f && kickedUp > 5.0f && flashed,
              "firing: gun kicks back %.0f mm and the muzzle up %.0f deg, with a flash", kickedBack * 1000, kickedUp);
        CHECK(triggerAtShot > 0.9f && hammerAtShot < 0.5f, "at the shot the trigger is back and the hammer has fallen");
        CHECK(fabsf(fireVm.Mechanism().cylinder - cylinderBefore - 1.0f) < 0.01f &&
              fireVm.Mechanism().chambers[RevolverChamberAtBarrel(cylinderBefore)] == CHAMBER_FIRED,
              "the cylinder indexes one chamber; the fired chamber holds an empty case");
        for (int i = 0; i < 30; i++) { fired.Tick(DT, Stance::Still); fireVm.Update(DT, still, fired); }
        CHECK(fabsf(muzzleUp(fireVm, fired) - restUp) < 1.0f && fireVm.Mechanism().trigger == 0.0f,
              "recoil recovers and the trigger resets within 0.6 s");

        // Reloading: cylinder out, cases dumped, a speedloader in, cylinder shut, back on target
        CHECK(fired.StartReload(), "reload starts (magazine not full)");
        auto runFor = [&](float seconds) {
            for (int i = 0; i < (int)(seconds * 60); i++) { fired.Tick(DT, Stance::Still); fireVm.Update(DT, still, fired); }
        };
        float t = 0.0f;
        auto runTo = [&](float seconds) { runFor((seconds - t) * REVOLVER.reloadTime / 3.0f); t = seconds; };
        runTo(0.45f);
        CHECK(fireVm.Mechanism().crane > 0.95f, "the cylinder swings out (crane %.2f)", fireVm.Mechanism().crane);
        runTo(0.585f);
        CHECK(fireVm.Mechanism().ejector > 0.3f && fireVm.Mechanism().caseLift > 0.3f, "the ejector pushes the cases out");
        runTo(0.9f);
        int emptyAfterEject = 0;
        for (int k = 0; k < 6; k++) emptyAfterEject += fireVm.Mechanism().chambers[k] == CHAMBER_EMPTY;
        CHECK(emptyAfterEject == 6, "all six chambers are emptied");
        runTo(1.30f);
        CHECK(fireVm.Mechanism().speedloader && fireVm.Mechanism().speedloaderFull, "a full speedloader comes up");
        runTo(1.60f);
        int liveAfterLoad = 0;
        for (int k = 0; k < 6; k++) liveAfterLoad += fireVm.Mechanism().chambers[k] == CHAMBER_LIVE;
        CHECK(liveAfterLoad == 6 && !fireVm.Mechanism().speedloaderFull, "six rounds go in");
        runTo(2.50f);
        float closedAt = fireVm.Mechanism().cylinder;
        CHECK(fireVm.Mechanism().crane < 0.01f && fabsf(closedAt - roundf(closedAt)) < 0.01f,
              "the cylinder closes with a chamber lined up (%.2f chambers turned)", closedAt);
        runFor(REVOLVER.reloadTime * 0.25f);
        CHECK(!fired.IsReloading() && fired.GetAmmo() == REVOLVER.magazineSize && Vector3Distance(gunPos(fireVm, fired), rest) < 0.006f,
              "reloading: returns on target with a full cylinder");
        ViewModelEvents ev = fireVm.TakeEvents();
        (void)ev;

        // Dying mid-reload with an empty gun: the next life starts full
        Weapon dead(REVOLVER);
        for (int i = 0; i < REVOLVER.magazineSize; i++) dead.Fire(CameraAt({ 0, 1.7f, 0 }, { 0, 1.7f, 1 }), nothing, noEnemies);
        dead.StartReload();
        dead.Reset();
        CHECK(dead.GetAmmo() == REVOLVER.magazineSize && !dead.IsReloading(), "respawning refills the cylinder (%d)", dead.GetAmmo());
    }

    std::printf("Bullet spread\n");
    {
        Vector3 eye = { 0, 1.7f, 0 };
        Vector3 forward = Vector3Normalize({ -0.4f, 0.1f, 1 });
        Camera3D cam = CameraAt(eye, Vector3Add(eye, forward));

        // Fire lots of shots in a stance and measure how far they stray (degrees)
        struct Measured { float max, mean; Vector3 meanDir; };
        auto measure = [&](Stance stance, bool aimed) {
            Weapon p(REVOLVER, 42);
            for (int i = 0; i < 120; i++) {
                p.UpdateAim(DT, aimed);
                p.UpdateSpread(DT, stance);
            }
            Measured m = { 0, 0, { 0, 0, 0 } };
            const int n = 4000;
            for (int i = 0; i < n; i++) {
                Vector3 d = p.ShotRay(cam).direction;
                float a = Vector3Angle(d, forward) * RAD2DEG;
                m.max = fmaxf(m.max, a);
                m.mean += a / n;
                m.meanDir = Vector3Add(m.meanDir, d);
            }
            m.meanDir = Vector3Normalize(m.meanDir);
            return m;
        };

        struct Row { const char* name; Stance stance; };
        const Row rows[] = { { "airborne", Stance::Airborne },        { "sprinting", Stance::Sprinting },
                             { "walking", Stance::Walking },          { "standing still", Stance::Still },
                             { "crouch-walking", Stance::CrouchMoving }, { "crouched still", Stance::CrouchStill } };
        float prevHip = 1e9f;
        bool ordered = true, aimAlwaysTighter = true, centered = true, filled = true;
        for (const Row& r : rows) {
            Measured hip = measure(r.stance, false);
            Measured ads = measure(r.stance, true);
            float expectHip = REVOLVER.HipSpread(r.stance), expectAds = expectHip * REVOLVER.aimSpreadScale;
            std::printf("  info: %-15s hip: max %.2f deg (cone %.2f)   aimed: max %.2f deg (cone %.2f)\n", r.name,
                        hip.max, expectHip, ads.max, expectAds);
            // Never outside the cone, and actually uses most of it
            filled &= hip.max <= expectHip + 1e-3f && hip.max > 0.95f * expectHip;
            filled &= ads.max <= expectAds + 1e-3f && ads.max > 0.95f * expectAds;
            // Uniform over the cone's cross-section: average stray is 2/3 of the cone
            filled &= fabsf(hip.mean / expectHip - 2.0f / 3.0f) < 0.03f;
            centered &= Vector3Angle(hip.meanDir, forward) * RAD2DEG < 0.05f * expectHip;
            ordered &= hip.max < prevHip;
            aimAlwaysTighter &= ads.max < 0.35f * hip.max;
            prevHip = hip.max;
        }
        CHECK(ordered, "spread order: airborne > sprinting > walking > standing > crouch-walking > crouched");
        CHECK(aimAlwaysTighter, "aiming down sights cuts spread to 30%% in every stance");
        CHECK(filled, "shots stay inside the cone and spread evenly across it");
        CHECK(centered, "spread is centered on where you're looking (no drift)");

        // Spread widens fast and recovers more slowly
        Weapon p(REVOLVER, 3);
        for (int i = 0; i < 120; i++) p.UpdateSpread(DT, Stance::CrouchStill);
        for (int i = 0; i < 6; i++) p.UpdateSpread(DT, Stance::Sprinting);
        float afterSprintStart = p.GetSpread();
        for (int i = 0; i < 120; i++) p.UpdateSpread(DT, Stance::Sprinting);
        for (int i = 0; i < 6; i++) p.UpdateSpread(DT, Stance::CrouchStill);
        float afterStop = p.GetSpread();
        for (int i = 0; i < 60; i++) p.UpdateSpread(DT, Stance::CrouchStill);
        CHECK(afterSprintStart > 5.0f, "0.1 s after starting to sprint, spread is already %.1f deg (of 7)",
              afterSprintStart);
        CHECK(afterStop > 3.0f && p.GetSpread() < 0.7f,
              "after stopping to crouch: still %.1f deg at 0.1 s, %.2f deg at 1 s (target 0.6)", afterStop,
              p.GetSpread());
    }

    std::printf("Respawns after being killed\n");
    {
        std::vector<Enemy> enemies(1, Enemy(13));
        Vector3 player = spawnA;
        Vector3 target = Vector3Add(player, { 0, 0, 4 }); // between the spawn and its shield wall
        enemies[0].SpawnAt(target);
        Weapon pistol = PrecisePistol();
        Camera3D aim = CameraAt({ player.x, 1.7f, player.z }, { target.x, 1.0f, target.z });
        for (int i = 0; i < 4; i++) pistol.Fire(aim, const_cast<World&>(arena), enemies);
        CHECK(!enemies[0].IsAlive(), "killed");

        for (int i = 0; i < (int)(2.9f * 60); i++) enemies[0].Update(DT, player, arena);
        CHECK(!enemies[0].IsAlive(), "still gone after 2.9 s");
        for (int i = 0; i < (int)(0.2f * 60); i++) enemies[0].Update(DT, player, arena);
        float d = DistXZ(enemies[0].GetFeet(), player);
        CHECK(enemies[0].IsAlive() && enemies[0].GetHealth() == 100 && d >= 20.0f,
              "back after 3 s with full health, %.1f m from the player", d);
    }

    std::printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
