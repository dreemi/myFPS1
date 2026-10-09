// Movement tests: drive Player::Simulate with scripted input, no window needed.
#include "../src/player.hpp"
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

static void Settle(Player& p, const World& w) {
    for (int i = 0; i < 10; i++) p.Simulate(DT, {}, w);
}

// Runs until the player lands (or a time limit). Returns peak feet height.
static float RunJump(Player& p, const World& w, PlayerInput held, int doubleJumpFrame = -1,
                     float* airTime = nullptr, float dt = DT) {
    float peak = p.GetFeet().y;
    int frames = 0;
    PlayerInput first = held;
    first.jump = true;
    p.Simulate(dt, first, w);
    peak = fmaxf(peak, p.GetFeet().y);
    frames++;
    while (!p.IsGrounded() && frames < 6000) {
        PlayerInput in = held;
        in.jump = (frames == doubleJumpFrame);
        p.Simulate(dt, in, w);
        peak = fmaxf(peak, p.GetFeet().y);
        frames++;
    }
    if (airTime) *airTime = frames * dt;
    return peak;
}

int main() {
    // Expected from jumpSpeed 8, gravity 20: peak v^2/2g = 1.6 m, air time 2v/g = 0.8 s
    std::printf("Jump on flat ground\n");
    {
        World w;
        Player p;
        p.Init({ 0, 0, 0 });
        Settle(p, w);
        CHECK(p.IsGrounded() && p.GetFeet().y == 0.0f, "starts on the ground (y=%.3f)", p.GetFeet().y);

        float air = 0;
        float peak = RunJump(p, w, {}, -1, &air);
        CHECK(fabsf(peak - 1.6f) < 0.01f, "jump peak is 1.6 m (got %.3f)", peak);
        CHECK(fabsf(air - 0.8f) < 0.05f, "air time is ~0.8 s (got %.3f)", air);
        CHECK(p.IsGrounded() && p.GetFeet().y == 0.0f, "lands back exactly on the ground (y=%.3f)", p.GetFeet().y);
    }

    std::printf("Same physics at any frame rate\n");
    for (float fps : { 20.0f, 30.0f, 60.0f, 144.0f, 240.0f }) {
        float dt = 1.0f / fps;
        World w;

        Player p;
        p.Init({ 0, 0, 0 });
        for (int i = 0; i < 10; i++) p.Simulate(dt, {}, w);
        float air = 0;
        float peak = RunJump(p, w, {}, -1, &air, dt);
        // Peak is sampled on frames, so it can miss the true top by at most g*dt^2/8
        CHECK(fabsf(peak - 1.6f) < 0.01f && fabsf(air - 0.8f) <= dt + 1e-4f,
              "%3.0f FPS: jump peak %.3f m, air time %.3f s", fps, peak, air);

        Player walker;
        walker.Init({ 0, 0, 0 });
        walker.Simulate(dt, {}, w); // land first (spawning starts a frame "in the air")
        PlayerInput fwd;
        fwd.forward = true;
        int frames = (int)lroundf(fps); // one second
        for (int i = 0; i < frames; i++) walker.Simulate(dt, fwd, w);
        // Accelerating from rest: v*t - v^2/(2a) = 5 - 25/80
        float expected = walker.walkSpeed - walker.walkSpeed * walker.walkSpeed / (2 * walker.groundAccel);
        CHECK(fabsf(walker.GetFeet().z - expected) < 0.02f, "%3.0f FPS: walked %.3f m in 1 s from rest (expected %.3f)",
              fps, walker.GetFeet().z, expected);
    }

    std::printf("Acceleration and deceleration\n");
    {
        World w;
        PlayerInput none, fwd, sprint, back, leftKey, rightKey;
        fwd.forward = true;
        sprint.forward = true; sprint.sprint = true;
        back.back = true;
        leftKey.left = true;
        rightKey.right = true;
        auto fresh = [&]() { Player p; p.Init({ 0, 0, 0 }); Settle(p, w); return p; };
        auto run = [&](Player& p, PlayerInput in, float seconds) {
            for (int i = 0; i < (int)lroundf(seconds * 60); i++) p.Simulate(DT, in, w);
        };

        Player p = fresh();
        run(p, fwd, 0.1f);
        float at01 = p.GetSpeed();
        run(p, fwd, 0.1f);
        CHECK(fabsf(at01 - 4.0f) < 0.05f && fabsf(p.GetSpeed() - 5.0f) < 1e-3f,
              "walk: 4.0 m/s after 0.1 s (%.2f), full 5 m/s by 0.2 s (%.2f)", at01, p.GetSpeed());

        p = fresh();
        run(p, sprint, 0.1f);
        float s01 = p.GetSpeed();
        run(p, sprint, 0.2f);
        CHECK(fabsf(s01 - 4.0f) < 0.05f && fabsf(p.GetSpeed() - 8.5f) < 1e-3f,
              "sprint: builds up the same way, tops out at 8.5 m/s (%.2f)", p.GetSpeed());

        // Letting go: slide to a stop
        p = fresh();
        run(p, fwd, 0.5f);
        float z0 = p.GetFeet().z;
        run(p, none, 0.1f);
        float after01 = p.GetSpeed();
        run(p, none, 0.2f);
        float slide = p.GetFeet().z - z0;
        CHECK(fabsf(after01 - 2.0f) < 0.05f && p.GetSpeed() == 0.0f,
              "let go at walking speed: 2.0 m/s after 0.1 s (%.2f), stopped by 0.3 s", after01);
        CHECK(fabsf(slide - 25.0f / 60.0f) < 0.02f, "slides %.2f m before stopping (expected %.2f)", slide,
              25.0f / 60.0f);

        // Counter-strafing brakes harder than letting go
        Player release = fresh(), counter = fresh();
        run(release, rightKey, 0.5f);
        run(counter, rightKey, 0.5f);
        Vector3 right = release.GetRight();
        run(release, none, 4.0f / 60.0f);
        run(counter, leftKey, 4.0f / 60.0f);
        float relV = Vector3DotProduct(release.GetVelocity(), right);
        float ctrV = Vector3DotProduct(counter.GetVelocity(), right);
        CHECK(relV > 2.0f && ctrV <= 0.05f,
              "counter-strafe: after 0.067 s, letting go still drifts %.2f m/s, pressing the other way %.2f m/s", relV,
              ctrV);

        // Stance follows actual speed, so stopping restores accuracy
        p = fresh();
        run(p, sprint, 0.5f);
        CHECK(p.GetStance() == Stance::Sprinting, "at full sprint: sprinting");
        run(p, none, 0.1f);
        Stance mid = p.GetStance();
        run(p, none, 0.3f);
        CHECK(mid == Stance::Sprinting && p.GetStance() == Stance::Still,
              "after letting go it takes a moment to count as still (sprinting at 0.1 s, still at 0.4 s)");
        p = fresh();
        run(p, sprint, 0.5f);
        run(p, back, 0.1f);
        CHECK(p.GetStance() == Stance::Still || p.GetStance() == Stance::Walking,
              "counter-strafing a sprint drops out of sprint spread within 0.1 s");
    }

    std::printf("Strafing\n");
    {
        World w;
        auto topSpeed = [&](PlayerInput in) {
            Player p;
            p.Init({ 0, 0, 0 }, 0.4f);
            Settle(p, w);
            for (int i = 0; i < 60; i++) p.Simulate(DT, in, w);
            return p.GetSpeed();
        };
        PlayerInput fwd, side, back, diag, sideSprint, diagSprint;
        fwd.forward = true;
        side.right = true;
        back.back = true;
        diag.forward = true; diag.left = true;
        sideSprint = side; sideSprint.sprint = true;
        diagSprint = diag; diagSprint.sprint = true;
        CHECK(fabsf(topSpeed(fwd) - 5.0f) < 1e-3f, "forward: 5.00 m/s");
        CHECK(fabsf(topSpeed(side) - 4.5f) < 1e-3f, "strafe: %.2f m/s (90%%)", topSpeed(side));
        CHECK(fabsf(topSpeed(back) - 4.0f) < 1e-3f, "backpedal: %.2f m/s (80%%)", topSpeed(back));
        CHECK(fabsf(topSpeed(diag) - 4.75f) < 1e-3f, "diagonal: %.2f m/s (no extra speed from two keys)",
              topSpeed(diag));
        CHECK(fabsf(topSpeed(sideSprint) - 4.5f) < 1e-3f, "Shift while strafing sideways: no sprint (%.2f m/s)",
              topSpeed(sideSprint));
        CHECK(fabsf(topSpeed(diagSprint) - 8.5f * 0.95f) < 1e-3f, "Shift on a forward diagonal: sprints (%.2f m/s)",
              topSpeed(diagSprint));
    }

    std::printf("Air strafing\n");
    {
        World w;
        PlayerInput none, fwd, back, sprint, rightKey;
        fwd.forward = true;
        back.back = true;
        sprint.forward = true; sprint.sprint = true;
        rightKey.right = true;
        auto heading = [](Vector3 v) { return atan2f(v.x, v.z); };
        auto yawOf = [](const Player& p) { Vector3 d = p.GetLookDirection(); return atan2f(d.x, d.z); };
        auto angleDiff = [](float a, float b) {
            float d = a - b;
            while (d > PI) d -= 2 * PI;
            while (d < -PI) d += 2 * PI;
            return fabsf(d);
        };
        // Mouse movement that turns the view to face `targetYaw` (yaw -= look.x * sensitivity)
        auto turnTo = [&](const Player& p, float targetYaw) {
            float d = yawOf(p) - targetYaw;
            while (d > PI) d -= 2 * PI;
            while (d < -PI) d += 2 * PI;
            return Vector2{ d / 0.0025f, 0 };
        };

        // Run up to walking speed, jump, then each frame let `air` choose the input until landing.
        struct Result { float speed, turnedDeg, peak, airTime, maxTurnPerTenth, speedAt07, turnAt07; };
        auto jump = [&](PlayerInput runUp, auto air, float dt) {
            Player p;
            p.Init({ 0, 0, 0 });
            for (int i = 0; i < 10; i++) p.Simulate(dt, {}, w);
            for (int i = 0; i < (int)lroundf(1.0f / dt); i++) p.Simulate(dt, runUp, w);
            float h0 = heading(p.GetVelocity());
            PlayerInput j = runUp;
            j.jump = true;
            j.look = air(p).look; // the mouse is already moving on the take-off frame
            p.Simulate(dt, j, w);
            Result r = { 0, 0, 0, dt, 0, 0, 0 };
            std::vector<float> headings;
            float prevT = dt, prevSpeed = p.GetSpeed(), prevTurn = angleDiff(heading(p.GetVelocity()), h0);
            Vector3 lastAirVel = p.GetVelocity();
            while (!p.IsGrounded()) {
                lastAirVel = p.GetVelocity();
                headings.push_back(heading(lastAirVel));
                r.peak = fmaxf(r.peak, p.GetFeet().y);
                p.Simulate(dt, air(p), w);
                r.airTime += dt;
                // Sample at exactly 0.7 s after take-off (between frames), for frame-rate comparisons
                float sp = p.GetSpeed(), tu = angleDiff(heading(p.GetVelocity()), h0);
                if (prevT < 0.7f && r.airTime >= 0.7f) {
                    float k = (0.7f - prevT) / (r.airTime - prevT);
                    r.speedAt07 = Lerp(prevSpeed, sp, k);
                    r.turnAt07 = Lerp(prevTurn, tu, k) * RAD2DEG;
                }
                prevT = r.airTime; prevSpeed = sp; prevTurn = tu;
            }
            int tenth = (int)lroundf(0.1f / dt);
            for (size_t i = tenth; i < headings.size(); i++)
                r.maxTurnPerTenth = fmaxf(r.maxTurnPerTenth, angleDiff(headings[i], headings[i - tenth]) * RAD2DEG);
            r.speed = Vector3Length(lastAirVel);
            r.turnedDeg = angleDiff(heading(lastAirVel), h0) * RAD2DEG;
            return r;
        };
        auto hold = [](PlayerInput in) { return [in](const Player&) { return in; }; };
        // A skilled player: hold D and sweep the mouse to keep facing along the
        // direction of travel, so D always pushes sideways to the motion
        auto strafer = [&](const Player& p) {
            PlayerInput in = rightKey;
            in.look = turnTo(p, heading(p.GetVelocity()));
            return in;
        };

        Result r = jump(fwd, hold(none), DT);
        CHECK(fabsf(r.speed - 5.0f) < 0.01f, "let go mid-jump: momentum carries (%.2f m/s)", r.speed);
        r = jump(fwd, hold(fwd), DT);
        CHECK(fabsf(r.speed - 5.0f) < 0.01f, "holding W in the air adds nothing (%.2f m/s)", r.speed);

        Result beginner = jump(fwd, hold(rightKey), DT);
        CHECK(beginner.turnedDeg > 2.0f && beginner.turnedDeg < 10.0f && beginner.speed < 5.1f,
              "beginner (D, no mouse): nudges the path %.1f deg, %.2f m/s", beginner.turnedDeg, beginner.speed);

        Result skilled = jump(fwd, strafer, DT);
        CHECK(skilled.turnedDeg > 60.0f && skilled.speed > 6.0f,
              "skilled (D + mouse): curves %.0f deg and builds speed to %.2f m/s in one jump", skilled.turnedDeg,
              skilled.speed);
        CHECK(skilled.maxTurnPerTenth < 50.0f, "no instant direction changes: at most %.0f deg per 0.1 s",
              skilled.maxTurnPerTenth);
        CHECK(fabsf(skilled.peak - beginner.peak) < 1e-3f && fabsf(skilled.airTime - beginner.airTime) < 1e-3f,
              "strafing doesn't change the jump's height or air time (peak %.2f m, %.2f s)", skilled.peak,
              skilled.airTime);

        r = jump(fwd, hold(back), DT);
        CHECK(r.speed < 1.0f, "holding S in the air brakes (down to %.2f m/s), but can't fly you backwards fast", r.speed);
        r = jump(none, hold(fwd), DT);
        CHECK(r.speed <= 0.57f + 1e-3f && r.speed > 0.4f, "standing jump: W only drifts %.2f m/s", r.speed);

        // Same mouse motion (a steady 6 rad/s turn to the right) + D at different frame rates
        auto steadyTurn = [&](float fps) {
            float dt = 1.0f / fps;
            return jump(fwd, [&, dt](const Player&) {
                PlayerInput in = rightKey;
                in.look = { 6.0f * dt / 0.0025f, 0 };
                return in;
            }, dt);
        };
        Result r30 = steadyTurn(30), r60 = steadyTurn(60), r144 = steadyTurn(144);
        CHECK(fabsf(r30.speedAt07 - r60.speedAt07) < 0.05f && fabsf(r144.speedAt07 - r60.speedAt07) < 0.05f &&
              fabsf(r30.turnAt07 - r60.turnAt07) < 5.0f && fabsf(r144.turnAt07 - r60.turnAt07) < 5.0f, // ~1 air tick of turn
              "same mouse motion, same result at 30/60/144 FPS (0.7 s in: %.2f / %.2f / %.2f m/s, %.0f / %.0f / %.0f deg)",
              r30.speedAt07, r60.speedAt07, r144.speedAt07, r30.turnAt07, r60.turnAt07, r144.turnAt07);

        // Long fall off a tall tower, zig-zagging A/D with the mouse like a real
        // strafer: speed keeps building but tops out at maxAirSpeed
        WorldBox tower;
        tower.position = { 0, 110, -2 };
        tower.size = { 4, 220, 4 }; // top at y = 220, edge at z = 0
        World high = { tower };
        Player p;
        p.Init({ 0, 220, -1.5f });
        for (int i = 0; i < 10; i++) p.Simulate(DT, {}, high);
        while (p.IsGrounded()) p.Simulate(DT, fwd, high); // run off the edge
        float maxSpeed = 0, t = 0;
        while (!p.IsGrounded()) {
            PlayerInput in = strafer(p);
            if (fmodf(t, 0.7f) >= 0.35f) { in.right = false; in.left = true; } // switch sides every 0.35 s
            p.Simulate(DT, in, high);
            maxSpeed = fmaxf(maxSpeed, p.GetSpeed());
            t += DT;
        }
        CHECK(maxSpeed > 10.0f && maxSpeed <= p.maxAirSpeed + 1e-3f,
              "a long strafed fall tops out at %.2f m/s (cap %.1f)", maxSpeed, p.maxAirSpeed);

        // Sprint speed is kept, not cut down
        r = jump(sprint, hold(none), DT);
        CHECK(fabsf(r.speed - 8.5f) < 0.01f, "sprint-jump keeps sprint speed (%.2f m/s)", r.speed);
    }

    std::printf("Walls stop momentum\n");
    {
        WorldBox wall;
        wall.position = { 0, 2, 3 };
        wall.size = { 10, 4, 1 };
        World w = { wall };
        Player p;
        p.Init({ 0, 0, 0 });
        Settle(p, w);
        PlayerInput fwd, back;
        fwd.forward = true;
        back.back = true;
        for (int i = 0; i < 90; i++) p.Simulate(DT, fwd, w);
        CHECK(p.GetSpeed() < 1e-3f, "running into a wall: speed drops to 0 (%.2f)", p.GetSpeed());
        p.Simulate(DT, back, w);
        CHECK(p.GetVelocity().z < 0.0f, "pulling away starts immediately (no speed stored against the wall)");
    }

    std::printf("No double jump\n");
    {
        World w;
        Player p;
        p.Init({ 0, 0, 0 });
        Settle(p, w);
        float peak = RunJump(p, w, {}, 20); // press jump again a third of a second in
        CHECK(peak < 1.7f, "pressing jump mid-air does nothing (peak %.3f)", peak);
    }

    std::printf("Jump while moving keeps horizontal speed\n");
    {
        World w;
        Player p;
        p.Init({ 0, 0, 0 });
        Settle(p, w);
        PlayerInput fwd;
        fwd.forward = true;
        for (int i = 0; i < 60; i++) p.Simulate(DT, fwd, w); // get up to speed
        float z0 = p.GetFeet().z;
        float air = 0;
        RunJump(p, w, fwd, -1, &air);
        float expected = 5.0f * air;
        CHECK(fabsf(p.GetFeet().z - z0 - expected) < 0.1f, "running jump: moved %.2f m forward in the air (expected ~%.2f)",
              p.GetFeet().z - z0, expected);
    }

    // A 1.5 m crate directly ahead (+Z), from z=2 to z=4
    WorldBox crate;
    crate.position = { 0, 0.75f, 3 };
    crate.size = { 2, 1.5f, 2 };
    crate.color = BROWN;

    std::printf("Crate blocks walking\n");
    {
        World w = { crate };
        Player p;
        p.Init({ 0, 0, 0 });
        Settle(p, w);
        PlayerInput fwd;
        fwd.forward = true;
        for (int i = 0; i < 120; i++) p.Simulate(DT, fwd, w);
        CHECK(p.GetFeet().z < 2.0f && p.GetFeet().y == 0.0f, "stopped at the crate's side (z=%.2f, y=%.2f)",
              p.GetFeet().z, p.GetFeet().y);
    }

    std::printf("Jump onto crate\n");
    {
        World w = { crate };
        Player p;
        p.Init({ 0, 0, -4 });
        Settle(p, w);
        PlayerInput fwd;
        fwd.forward = true;
        while (p.GetFeet().z < -0.3f) p.Simulate(DT, fwd, w); // run-up
        RunJump(p, w, fwd);
        CHECK(p.IsGrounded() && fabsf(p.GetFeet().y - 1.5f) < 1e-4f, "landed on top of the crate (y=%.3f, z=%.2f)",
              p.GetFeet().y, p.GetFeet().z);
    }

    std::printf("Walk off a ledge\n");
    {
        World w = { crate };
        Player p;
        p.Init({ 0, 1.5f, 3 });
        Settle(p, w);
        CHECK(p.IsGrounded() && p.GetFeet().y == 1.5f, "standing on the crate (y=%.3f)", p.GetFeet().y);
        PlayerInput fwd;
        fwd.forward = true;
        for (int i = 0; i < 120; i++) p.Simulate(DT, fwd, w);
        CHECK(p.IsGrounded() && p.GetFeet().y == 0.0f && p.GetFeet().z > 4.0f,
              "fell off the far edge to the ground (y=%.3f, z=%.2f)", p.GetFeet().y, p.GetFeet().z);
    }

    std::printf("Head hits a low ceiling\n");
    {
        WorldBox ceiling;
        ceiling.position = { 0, 2.75f, 0 }; // underside at 2.5 m
        ceiling.size = { 4, 0.5f, 4 };
        World w = { ceiling };
        Player p;
        p.Init({ 0, 0, 0 });
        Settle(p, w);
        float peak = RunJump(p, w, {});
        CHECK(peak <= 0.7f + 1e-4f, "jump capped by ceiling: head stays below 2.5 m (feet peak %.3f)", peak);
        CHECK(p.IsGrounded() && p.GetFeet().y == 0.0f, "fell back to the ground (y=%.3f)", p.GetFeet().y);
    }

    std::printf("Spawn points in the real arena\n");
    {
        World w = BuildTestWorld();
        std::vector<SpawnPoint> spawns = GetSpawnPoints();
        CHECK(spawns.size() == 2, "there are 2 spawn points (got %d)", (int)spawns.size());
        CHECK(spawns.size() == 2 && fabsf(spawns[0].position.z - spawns[1].position.z) > ARENA_LENGTH * 0.8f,
              "they are at opposite ends (z=%.1f and z=%.1f)", spawns[0].position.z, spawns[1].position.z);

        for (size_t i = 0; i < spawns.size(); i++) {
            const SpawnPoint& s = spawns[i];
            const SpawnPoint& other = spawns[(i + 1) % spawns.size()];
            Player p;
            p.Init(s.position, s.yaw);
            Settle(p, w);
            CHECK(p.IsGrounded() && p.GetFeet().y == 0.0f, "spawn %d: standing on the ground (y=%.3f)", (int)i,
                  p.GetFeet().y);

            Vector3 look = p.GetLookDirection();
            float toOther = other.position.z - s.position.z;
            CHECK(look.z * toOther > 0.0f && fabsf(look.x) < 1e-4f, "spawn %d: facing the other end (look z=%.2f)",
                  (int)i, look.z);

            // Nothing should be stuck to or right in front of the spawn
            PlayerInput fwd;
            fwd.forward = true;
            Vector3 start = p.GetFeet();
            for (int f = 0; f < 60; f++) p.Simulate(DT, fwd, w);
            float moved = fabsf(p.GetFeet().z - start.z);
            CHECK(moved > 4.6f, "spawn %d: clear path, walked %.2f m forward in 1 s", (int)i, moved);
        }
    }

    std::printf("Crouching\n");
    {
        World w;
        Player p;
        p.Init({ 0, 0, 0 });
        Settle(p, w);
        PlayerInput crouchWalk;
        crouchWalk.forward = true;
        crouchWalk.crouch = true;
        for (int i = 0; i < 60; i++) p.Simulate(DT, crouchWalk, w);
        CHECK(p.IsCrouching() && fabsf(p.GetSpeed() - 2.5f) < 0.01f, "crouch-walks at half speed (%.2f m/s)",
              p.GetSpeed());
        CHECK(fabsf(p.GetEyeHeight() - 1.1f) < 0.01f && fabsf(p.GetCamera().position.y - 1.1f) < 0.01f,
              "camera lowered to 1.1 m (eye %.3f)", p.GetEyeHeight());

        PlayerInput crouchSprint = crouchWalk;
        crouchSprint.sprint = true;
        float z0 = p.GetFeet().z;
        for (int i = 0; i < 60; i++) p.Simulate(DT, crouchSprint, w);
        CHECK(fabsf(p.GetFeet().z - z0 - 2.5f) < 0.01f, "can't sprint while crouched (%.2f m in 1 s)",
              p.GetFeet().z - z0);

        // Camera eases down rather than snapping
        Player q;
        q.Init({ 0, 0, 0 });
        Settle(q, w);
        PlayerInput c;
        c.crouch = true;
        q.Simulate(DT, c, w);
        float afterOneFrame = q.GetEyeHeight();
        for (int i = 0; i < 14; i++) q.Simulate(DT, c, w);
        float afterQuarterSecond = q.GetEyeHeight();
        CHECK(afterOneFrame > 1.55f && afterQuarterSecond < 1.15f,
              "camera eases down (1 frame: %.2f m, 0.25 s: %.2f m)", afterOneFrame, afterQuarterSecond);

        q.Simulate(DT, {}, w);
        for (int i = 0; i < 30; i++) q.Simulate(DT, {}, w);
        CHECK(!q.IsCrouching() && fabsf(q.GetEyeHeight() - 1.7f) < 0.01f, "stands back up on release (eye %.3f)",
              q.GetEyeHeight());
    }

    std::printf("Stance (drives weapon spread)\n");
    {
        World w;
        auto stanceAfter = [&](PlayerInput in, int frames) {
            Player p;
            p.Init({ 0, 0, 0 });
            Settle(p, w);
            for (int i = 0; i < frames; i++) p.Simulate(DT, in, w);
            return p.GetStance();
        };
        PlayerInput none, walk, sprint, crouch, crouchWalk, crouchSprint;
        walk.forward = true;
        sprint.forward = true; sprint.sprint = true;
        crouch.crouch = true;
        crouchWalk.forward = true; crouchWalk.crouch = true;
        crouchSprint = crouchWalk; crouchSprint.sprint = true;
        CHECK(stanceAfter(none, 10) == Stance::Still, "no input: still");
        CHECK(stanceAfter(walk, 10) == Stance::Walking, "W: walking");
        CHECK(stanceAfter(sprint, 10) == Stance::Sprinting, "W + Shift: sprinting");
        CHECK(stanceAfter(crouch, 10) == Stance::CrouchStill, "crouch: crouched still");
        CHECK(stanceAfter(crouchWalk, 10) == Stance::CrouchMoving, "crouch + W: crouch-walking");
        CHECK(stanceAfter(crouchSprint, 10) == Stance::CrouchMoving, "crouch + W + Shift: still crouch-walking");

        Player p;
        p.Init({ 0, 0, 0 });
        Settle(p, w);
        PlayerInput jump;
        jump.jump = true;
        p.Simulate(DT, jump, w);
        for (int i = 0; i < 10; i++) p.Simulate(DT, none, w);
        CHECK(p.GetStance() == Stance::Airborne, "mid-jump: airborne");
        for (int i = 0; i < 60; i++) p.Simulate(DT, none, w);
        CHECK(p.GetStance() == Stance::Still, "after landing: still");
    }

    std::printf("Health\n");
    {
        Player p;
        p.Init({ 0, 0, 0 });
        bool died = false;
        for (int i = 0; i < 3; i++) died |= p.TakeDamage(25);
        CHECK(!died && p.IsAlive() && p.GetHealth() == 25, "3 body hits: alive on %d", p.GetHealth());
        died = p.TakeDamage(50);
        CHECK(died && !p.IsAlive() && p.GetHealth() == 0, "a 4th hit (a headshot) kills, health stops at 0");
        CHECK(!p.TakeDamage(25), "can't be killed twice");
        p.Init({ 0, 0, 0 });
        CHECK(p.IsAlive() && p.GetHealth() == 100, "respawning restores full health");
    }

    std::printf("Leaning\n");
    {
        World w;
        Player p;
        p.Init({ 0, 0, 0 }, 0.7f); // facing an arbitrary direction
        Settle(p, w);
        Vector3 right = p.GetRight();
        Vector3 worldUp = { 0, 1, 0 };
        auto sideways = [&]() { return Vector3DotProduct(Vector3Subtract(p.GetCamera().position, p.GetFeet()), right); };
        auto tiltDegrees = [&]() {
            Vector3 up = Vector3Normalize(p.GetCamera().up);
            float deg = Vector3Angle(up, worldUp) * RAD2DEG;
            return Vector3DotProduct(up, right) >= 0 ? deg : -deg; // + = tilted right
        };

        PlayerInput leanR;
        leanR.leanRight = true;
        p.Simulate(DT, leanR, w);
        float oneFrame = p.GetLean();
        for (int i = 0; i < 60; i++) p.Simulate(DT, leanR, w);
        CHECK(oneFrame > 0.05f && oneFrame < 0.3f && fabsf(p.GetLean() - 1.0f) < 0.01f,
              "E eases into a full right lean (%.2f after 1 frame, %.2f after 1 s)", oneFrame, p.GetLean());
        CHECK(fabsf(sideways() - 0.45f) < 0.01f && fabsf(p.GetCamera().position.y - 1.64f) < 0.01f,
              "head is 0.45 m to the right and a bit lower (%.2f m right, eye at %.2f m)", sideways(),
              p.GetCamera().position.y);
        CHECK(fabsf(tiltDegrees() - 12.0f) < 0.2f, "view tilts 12 deg to the right (%.1f)", tiltDegrees());
        CHECK(Vector3Length(p.GetFeet()) < 1e-4f, "feet don't move while leaning");

        PlayerInput leanL;
        leanL.leanLeft = true;
        for (int i = 0; i < 90; i++) p.Simulate(DT, leanL, w);
        CHECK(fabsf(sideways() + 0.45f) < 0.01f && fabsf(tiltDegrees() + 12.0f) < 0.2f,
              "Q leans left: %.2f m, %.1f deg", sideways(), tiltDegrees());

        PlayerInput both = leanL;
        both.leanRight = true;
        for (int i = 0; i < 90; i++) p.Simulate(DT, both, w);
        CHECK(fabsf(p.GetLean()) < 0.01f, "Q and E together cancel out (lean %.2f)", p.GetLean());

        for (int i = 0; i < 90; i++) p.Simulate(DT, {}, w);
        CHECK(fabsf(sideways()) < 0.01f && fabsf(tiltDegrees()) < 0.1f, "back upright on release");

        // Crouched lean keeps the crouched height
        PlayerInput crouchLean = leanR;
        crouchLean.crouch = true;
        for (int i = 0; i < 90; i++) p.Simulate(DT, crouchLean, w);
        CHECK(fabsf(p.GetCamera().position.y - 1.04f) < 0.01f && fabsf(sideways() - 0.45f) < 0.01f,
              "crouched lean: eye at %.2f m, %.2f m right", p.GetCamera().position.y, sideways());
    }

    std::printf("Leaning into a wall\n");
    {
        // Wall on the player's right (+X is left when facing +Z, so right is -X)
        WorldBox wall;
        wall.position = { -1.0f, 2, 0 };
        wall.size = { 1, 4, 10 };   // surface at x = -0.5
        World w = { wall };
        Player p;
        p.Init({ 0, 0, 0 }, 0.0f);  // 0.5 m from the wall surface
        Settle(p, w);
        PlayerInput leanR, leanL;
        leanR.leanRight = true;
        leanL.leanLeft = true;
        for (int i = 0; i < 60; i++) p.Simulate(DT, leanR, w);
        float headX = p.GetCamera().position.x;
        CHECK(p.GetLean() > 0.2f && p.GetLean() < 0.9f && headX > -0.5f + 0.149f,
              "lean toward the wall stops short: lean %.2f, head %.2f m from the wall", p.GetLean(), headX + 0.5f);
        for (int i = 0; i < 90; i++) p.Simulate(DT, leanL, w);
        CHECK(fabsf(p.GetLean() + 1.0f) < 0.01f, "leaning away from the wall is unrestricted (lean %.2f)", p.GetLean());
    }

    std::printf("Crouching under a low obstacle\n");
    {
        // Overhang 1.5 m above the floor, from z = 2 to z = 4
        WorldBox overhang;
        overhang.position = { 0, 2.25f, 3 };
        overhang.size = { 4, 1.5f, 2 };
        World w = { overhang };
        PlayerInput fwd;
        fwd.forward = true;

        Player standing;
        standing.Init({ 0, 0, 0 });
        Settle(standing, w);
        for (int i = 0; i < 60; i++) standing.Simulate(DT, fwd, w);
        CHECK(standing.GetFeet().z < 2.0f, "standing: blocked by the overhang (z=%.2f)", standing.GetFeet().z);

        Player p;
        p.Init({ 0, 0, 0 });
        Settle(p, w);
        PlayerInput crouchFwd = fwd;
        crouchFwd.crouch = true;
        while (p.GetFeet().z < 3.0f) p.Simulate(DT, crouchFwd, w);
        CHECK(p.GetFeet().z >= 3.0f, "crouched: fits underneath (z=%.2f)", p.GetFeet().z);

        for (int i = 0; i < 30; i++) p.Simulate(DT, {}, w); // let go of crouch under it
        CHECK(p.IsCrouching() && p.GetEyeHeight() < 1.2f, "stays crouched under it after letting go (eye %.2f)",
              p.GetEyeHeight());

        PlayerInput jump;
        jump.jump = true;
        p.Simulate(DT, jump, w);
        float peak = 0;
        for (int i = 0; i < 60; i++) {
            p.Simulate(DT, {}, w);
            peak = fmaxf(peak, p.GetFeet().y);
        }
        CHECK(peak <= 0.3f + 1e-4f, "jumping under it bonks the head at 1.5 m (feet peak %.2f)", peak);

        for (int i = 0; i < 60; i++) p.Simulate(DT, fwd, w); // walk out (still crouched)
        for (int i = 0; i < 30; i++) p.Simulate(DT, {}, w);
        CHECK(!p.IsCrouching() && p.GetFeet().z > 4.0f, "stands up once clear (z=%.2f, crouching=%d)",
              p.GetFeet().z, p.IsCrouching());
    }

    std::printf("Map layout\n");
    {
        World w = BuildTestWorld();
        std::vector<SpawnPoint> spawns = GetSpawnPoints();

        // Every solid box has a twin mirrored across z = 0
        int unmatched = 0;
        for (const WorldBox& a : w) {
            bool found = false;
            for (const WorldBox& b : w) {
                if (fabsf(a.position.x - b.position.x) < 1e-3f && fabsf(a.position.y - b.position.y) < 1e-3f &&
                    fabsf(a.position.z + b.position.z) < 1e-3f && Vector3Equals(a.size, b.size)) found = true;
            }
            if (!found) unmatched++;
        }
        CHECK(unmatched == 0, "map is mirrored across the middle (%d boxes without a twin)", unmatched);

        Vector3 eyeA = Vector3Add(spawns[0].position, { 0, 1.7f, 0 });
        Vector3 eyeB = Vector3Add(spawns[1].position, { 0, 1.7f, 0 });
        CHECK(!HasLineOfSight(eyeA, eyeB, w), "the two spawns can't see each other");

        // Each lane on its own connects the spawns (walking only, 0.5 m grid)
        struct Lane { const char* name; float xMin, xMax; };
        for (Lane lane : { Lane{ "main", -20, -6.5f }, Lane{ "center", -5.5f, 11.5f }, Lane{ "flank", 12.5f, 20 } }) {
            const float cell = 0.5f;
            const int nx = (int)(ARENA_WIDTH / cell), nz = (int)(ARENA_LENGTH / cell);
            auto toWorld = [&](int i, int k) {
                return Vector3{ -ARENA_WIDTH / 2 + (i + 0.5f) * cell, 0, -ARENA_LENGTH / 2 + (k + 0.5f) * cell };
            };
            auto open = [&](int i, int k) {
                Vector3 p = toWorld(i, k);
                bool inLaneOrSpawn = fabsf(p.z) > 26.0f || (p.x > lane.xMin && p.x < lane.xMax);
                return inLaneOrSpawn && !CylinderCollides(p, 0.35f, 1.8f, w);
            };
            auto cellOf = [&](Vector3 p, int& i, int& k) {
                i = (int)((p.x + ARENA_WIDTH / 2) / cell);
                k = (int)((p.z + ARENA_LENGTH / 2) / cell);
            };
            std::vector<char> seen(nx * nz, 0);
            std::vector<int> queue;
            int si, sk, gi, gk;
            cellOf(spawns[0].position, si, sk);
            cellOf(spawns[1].position, gi, gk);
            queue.push_back(si * nz + sk);
            seen[si * nz + sk] = 1;
            for (size_t q = 0; q < queue.size(); q++) {
                int i = queue[q] / nz, k = queue[q] % nz;
                const int di[] = { 1, -1, 0, 0 }, dk[] = { 0, 0, 1, -1 };
                for (int d = 0; d < 4; d++) {
                    int a = i + di[d], b = k + dk[d];
                    if (a < 0 || b < 0 || a >= nx || b >= nz || seen[a * nz + b] || !open(a, b)) continue;
                    seen[a * nz + b] = 1;
                    queue.push_back(a * nz + b);
                }
            }
            CHECK(seen[gi * nz + gk], "%s lane alone connects spawn A to spawn B", lane.name);
        }

        // Low windows can be jumped through; tall dividers can't
        // Run toward a divider (+X) and jump about 1 m before it
        auto crossDivider = [&](Vector3 start, float jumpAtX) {
            Player p;
            p.Init(start, PI / 2); // facing +X
            Settle(p, w);
            PlayerInput in;
            in.forward = true;
            bool jumped = false;
            for (int i = 0; i < 150; i++) {
                in.jump = !jumped && p.GetFeet().x >= jumpAtX;
                jumped |= in.jump;
                p.Simulate(DT, in, w);
            }
            return p.GetFeet().x;
        };
        float x = crossDivider({ -10, 0, 0 }, -7.9f);
        CHECK(x > -5.0f, "running jump through the main/center window (x -10 -> %.1f)", x);
        x = crossDivider({ -10, 0, 0 }, 1e9f);
        CHECK(x < -6.0f, "can't walk through the window without jumping (x -10 -> %.1f)", x);
        x = crossDivider({ -10, 0, 9 }, -7.9f);
        CHECK(x < -6.0f, "can't jump the tall divider (x -10 -> %.1f)", x);
        x = crossDivider({ 7, 0, 19 }, 10.1f);
        CHECK(x > 13.0f, "running jump through the center/flank window (x 7 -> %.1f)", x);
    }

    std::printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
