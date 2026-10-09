#include "raylib.h"
#include "player.hpp"
#include "weapon.hpp"
#include "viewmodel.hpp"
#include "world.hpp"
#include "enemy.hpp"
#include "materials.hpp"
#include "audio.hpp"
#include "character.hpp"
#include "render.hpp"
#include "effects.hpp"
#include "raymath.h"
#include "rlgl.h"
#include <algorithm>
#include <string>

static const int SCREEN_W = 1280;
static const int SCREEN_H = 720;

int main() {
    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
    InitWindow(SCREEN_W, SCREEN_H, "Simple FPS - raylib");
    SetTargetFPS(60);
    DisableCursor(); // lock + hide the mouse for free-look
    InitAudioDevice();

    // Textures and models live in assets/ next to the executable
    std::string assetDir = std::string(GetApplicationDirectory()) + "assets/";
    WorldMaterials materials = LoadWorldMaterials(assetDir);
    LoadWeaponModelAssets(assetDir.c_str());
    GameAudio audio;
    audio.Load(assetDir);
    CharacterAssets character; // the enemy, your own body, and your first-person arms
    LoadCharacter(character, assetDir);

    World world = BuildTestWorld();
    Model ground = LoadGroundModel(ARENA_WIDTH, ARENA_LENGTH, materials); // only used if lighting is unavailable

    // Lighting: one lit shader for the level, characters and guns
    Render::Init(assetDir);
    Fx::Init();
    Render::BuildLevel(world, materials);
    Render::ApplyLitShader(character.model);
    SetWeaponModelShader(Render::LitShader());
    Model unitCube = LoadModelFromMesh(GenMeshCube(1, 1, 1));            // practice targets
    Model spawnPad = LoadModelFromMesh(GenMeshCylinder(1.2f, 0.02f, 32)); // spawn markers
    Render::ApplyLitShader(unitCube);
    Render::ApplyLitShader(spawnPad);
    float sunOnGun = 1.0f; // eases between 0 (player in shade) and 1
    std::vector<SpawnPoint> spawns = GetSpawnPoints();
    int currentSpawn = GetRandomValue(0, (int)spawns.size() - 1);

    Player player;
    player.Init(spawns[currentSpawn].position, spawns[currentSpawn].yaw);
    Weapon gun(REVOLVER, (unsigned)GetRandomValue(0, 1 << 30)); // gameplay
    ViewModel viewModel(REVOLVER_MODELS);                           // how the local player sees it
    viewModel.arms = &character;
    float bodyPhase = 0.0f; // stride cycles walked, for your own legs

    std::vector<Enemy> enemies;
    enemies.emplace_back((unsigned)GetRandomValue(0, 1 << 30));
    for (Enemy& e : enemies) e.Spawn(player.GetFeet(), world);

    bool paused = false;
    int deaths = 0;
    float damageFlash = 0.0f;  // red screen tint after being hit
    float deathTimer = 0.0f;   // > 0 while showing "YOU DIED"


    // Respawn at whichever spawn is farther from the enemies
    auto respawnPlayer = [&]() {
        float bestDist = -1.0f;
        for (int i = 0; i < (int)spawns.size(); i++) {
            float nearest = 1e9f;
            for (const Enemy& e : enemies)
                if (e.IsAlive()) nearest = std::min(nearest, Vector3Distance(e.GetFeet(), spawns[i].position));
            if (nearest > bestDist) { bestDist = nearest; currentSpawn = i; }
        }
        player.Init(spawns[currentSpawn].position, spawns[currentSpawn].yaw);
    };

    while (!WindowShouldClose()) {
        float dt = GetFrameTime();

        // ---------- Update ----------
        // Tab toggles mouse capture so you can click out of the window
        if (IsKeyPressed(KEY_TAB)) {
            paused = !paused;
            if (paused) EnableCursor(); else DisableCursor();
        }

        // Enter respawns at the next spawn point (the other end of the arena)
        if (!paused && player.IsAlive() && IsKeyPressed(KEY_ENTER)) {
            currentSpawn = (currentSpawn + 1) % (int)spawns.size();
            player.Init(spawns[currentSpawn].position, spawns[currentSpawn].yaw);
        }

        if (!paused) {
            if (player.IsAlive()) {
                player.Update(dt, world);
                bool wasReloading = gun.IsReloading();
                if (gun.Update(dt, player.GetCamera(), world, enemies, player.GetStance())) {
                    audio.PlayGunshot(0.8f, 0.0f);
                    // Effects: a tracer from roughly where the gun is, then whatever it hit
                    const ShotResult& shot = gun.LastShot();
                    const Camera3D& c = player.GetCamera();
                    Vector3 fwd = player.GetLookDirection();
                    Vector3 gunTip = Vector3Add(c.position, Vector3Add(Vector3Scale(fwd, 0.7f),
                                        Vector3Add(Vector3Scale(player.GetRight(), 0.17f * (1.0f - gun.GetAimAmount())), { 0, -0.12f, 0 })));
                    Vector3 end = shot.hit ? shot.point : Vector3Add(c.position, Vector3Scale(shot.direction, 80.0f));
                    Fx::Tracer(gunTip, end, { 255, 220, 160, 140 });
                    if (shot.hitEnemy) Fx::Blood(shot.point, shot.direction, shot.headshot);
                    else if (shot.hit) Fx::Impact(shot.point, shot.normal, shot.surface);
                }
                if (!wasReloading && gun.IsReloading()) audio.PlayReload();
                player.SetZoom(gun.GetAimAmount());
                viewModel.Update(dt, MotionOf(player), gun);
                Stance st = player.GetStance();
                float stride = st == Stance::Sprinting ? 2.2f : (st == Stance::CrouchMoving ? 1.0f : 1.4f);
                bodyPhase += player.GetSpeed() * dt / stride;
            } else {
                deathTimer -= dt;
                if (deathTimer <= 0.0f) respawnPlayer();
            }

            // What the enemies can see of the player: real eye position and hitboxes
            EnemyTarget target;
            target.feet = player.GetFeet();
            target.eye = player.GetCamera().position;
            target.bodyTop = player.GetEyeHeight() - 0.25f;
            target.alive = player.IsAlive();

            for (Enemy& e : enemies) {
                e.Update(dt, target, world);
                for (const Ray& shot : e.TakeShots()) {
                    audio.PlayGunshotAt(e.GetMuzzle(), player.GetCamera().position, player.GetRight());
                    ShotTrace t = TraceShotAtPlayer(shot, target, world);
                    Fx::MuzzleFlash(e.GetMuzzle(), shot.direction);
                    Fx::Tracer(e.GetMuzzle(), t.end, { 255, 210, 140, 255 });
                    if (t.hitWorld) Fx::Impact(t.end, t.normal, t.surface);
                    if (t.hitPlayer && player.IsAlive()) {
                        damageFlash = t.headshot ? 0.6f : 0.4f;
                        if (player.TakeDamage(t.headshot ? REVOLVER.headDamage : REVOLVER.bodyDamage)) {
                            deaths++;
                            deathTimer = 2.5f;
                            player.SetZoom(0.0f);
                            audio.StopReload();
                        }
                    }
                }
            }
            Fx::Update(dt);
            damageFlash = std::max(0.0f, damageFlash - dt);
        }
        for (WorldBox& b : world) b.hitFlash = std::max(0.0f, b.hitFlash - dt);
        audio.UpdateFootsteps(player.GetStance(), player.IsAlive() && !paused);

        // ---------- Draw ----------
        const Camera3D& cam = player.GetCamera();

        // Your own body (no head or arms: the camera is in the head and the
        // first-person arms are drawn with the gun), so you see your legs when you
        // look down. The whole body still casts a shadow.
        auto drawPlayerBody = [&](int parts) {
            if (player.IsAlive() && character.loaded) {
                CharAnim anim = CharAnim::Idle;
                switch (player.GetStance()) {
                    case Stance::Walking:      anim = CharAnim::Walk; break;
                    case Stance::Sprinting:    anim = CharAnim::Run; break;
                    case Stance::CrouchStill:  anim = CharAnim::Crouch; break;
                    case Stance::CrouchMoving: anim = CharAnim::CrouchWalk; break;
                    default: break;
                }
                Vector3 look = player.GetLookDirection();
                float yaw = atan2f(look.x, look.z);
                // The model's eyes are 14 cm in front of its feet; shift it back so they
                // sit under the camera, plus a little more so you don't see inside the collar
                Vector3 f = { sinf(yaw), 0, cosf(yaw) };
                Vector3 at = Vector3Subtract(player.GetFeet(), Vector3Scale(f, 0.18f));
                Matrix toWorld = MatrixMultiply(MatrixRotateY(yaw), MatrixTranslate(at.x, at.y, at.z));
                DrawCharacter(character, anim, CharacterFrame(character, anim, bodyPhase), toWorld, parts, WHITE);
            }
        };
        auto drawTargets = [&]() {
            for (const WorldBox& b : world)
                if (b.isTarget) DrawModelEx(unitCube, b.position, { 0, 1, 0 }, 0, b.size, b.hitFlash > 0.0f ? RED : b.color);
        };

        // Muzzle flashes light up their surroundings
        Vector3 look = player.GetLookDirection();
        Render::SetFlashLight(0, Vector3Add(cam.position, Vector3Add(Vector3Scale(look, 0.8f), Vector3Scale(player.GetRight(), 0.15f))),
                              { 7.0f, 4.5f, 1.8f }, gun.IsMuzzleFlashing() ? 7.0f : 0.0f);
        Render::SetFlashLight(1, enemies.empty() ? Vector3{} : enemies[0].GetMuzzle(), { 7.0f, 4.5f, 1.8f },
                              !enemies.empty() && enemies[0].IsMuzzleFlashing() ? 7.0f : 0.0f);

        // 1. Shadows from the sun
        Render::BeginShadowPass();
            Render::DrawLevel();
            drawTargets();
            for (const Enemy& e : enemies) e.Draw(&character);
            drawPlayerBody(PART_ALL & ~PART_HEAD);
        Render::EndShadowPass();

        BeginDrawing();
        ClearBackground(BLACK);
        Render::DrawSky(cam, SCREEN_W, SCREEN_H);

        // 2. The world, lit
        Render::BeginWorldPass(cam);
            if (Render::Ready()) {
                Render::DrawLevel();
            } else {
                DrawModel(ground, { 0, 0, 0 }, 1.0f, WHITE);
                for (const WorldBox& b : world) if (!b.isTarget) DrawWorldBox(b, materials);
            }
            for (const SpawnPoint& s : spawns) DrawModel(spawnPad, { s.position.x, 0.0f, s.position.z }, 1.0f, s.color);
            drawTargets();
            for (const Enemy& e : enemies) e.Draw(&character);
            drawPlayerBody(PART_LEGS);
            Fx::Draw(cam);
        Render::EndWorldPass();

        // 3. First-person weapon: its own pass on top of the world, lit like the
        //    spot you're standing in (darker in shadow)
        if (player.IsAlive()) {
            sunOnGun += ((Render::SunVisible(cam.position, world) ? 1.0f : 0.0f) - sunOnGun) * (1.0f - expf(-8.0f * dt));
            Render::SetViewModelLighting(cam, sunOnGun);
            Render::SetFlashLight(0, { 0.15f, -0.08f, -0.9f }, { 7.0f, 4.5f, 1.8f }, gun.IsMuzzleFlashing() ? 1.5f : 0.0f);
            Render::SetFlashLight(1, {}, {}, 0.0f);
            viewModel.Draw(gun);
        }

        // Enemy health bars, drawn in screen space above their heads
        for (const Enemy& e : enemies) {
            if (!e.IsAlive()) continue;
            Vector3 top = Vector3Add(e.GetFeet(), { 0, e.GetHeight() + 0.35f, 0 });
            Vector3 toTop = Vector3Subtract(top, cam.position);
            if (Vector3DotProduct(toTop, player.GetLookDirection()) <= 0.0f) continue; // behind us
            if (!HasLineOfSight(cam.position, top, world)) continue;
            Vector2 s = GetWorldToScreen(top, cam);
            const int bw = 50, bh = 6;
            int filled = bw * e.GetHealth() / e.GetMaxHealth();
            DrawRectangle((int)s.x - bw / 2 - 1, (int)s.y - 1, bw + 2, bh + 2, BLACK);
            DrawRectangle((int)s.x - bw / 2, (int)s.y, filled, bh, { 80, 220, 90, 255 });
        }

        // Red tint when hit
        if (damageFlash > 0.0f) DrawRectangle(0, 0, SCREEN_W, SCREEN_H, Fade(RED, std::min(0.45f, damageFlash)));

        if (player.IsAlive()) {
            gun.DrawHUD(SCREEN_W, SCREEN_H, player.GetCamera().fovy);
        } else {
            DrawRectangle(0, 0, SCREEN_W, SCREEN_H, Fade(BLACK, 0.5f));
            const char* msg = "YOU DIED";
            DrawText(msg, SCREEN_W / 2 - MeasureText(msg, 60) / 2, SCREEN_H / 2 - 40, 60, RED);
            const char* sub = TextFormat("Respawning in %.1f", std::max(0.0f, deathTimer));
            DrawText(sub, SCREEN_W / 2 - MeasureText(sub, 22) / 2, SCREEN_H / 2 + 30, 22, RAYWHITE);
        }

        // Health and score
        int hp = player.GetHealth();
        Color hpColor = hp > 50 ? RAYWHITE : (hp > 25 ? ORANGE : RED);
        DrawText(TextFormat("HP %d", hp), 20, SCREEN_H - 50, 30, hpColor);
        DrawRectangle(20, SCREEN_H - 16, 150, 6, Fade(BLACK, 0.6f));
        DrawRectangle(20, SCREEN_H - 16, 150 * hp / player.GetMaxHealth(), 6, hpColor);
        DrawText(TextFormat("Deaths: %d", deaths), SCREEN_W - 260, 14, 20, RAYWHITE);

        DrawFPS(10, 10);
        DrawText("WASD move | Space jump | Shift sprint | Ctrl/C crouch | Q/E lean | LMB shoot | RMB aim | R reload | Enter respawn | Tab release mouse | Esc quit",
                 10, 36, 18, RAYWHITE);
        if (paused) DrawText("PAUSED - press Tab to resume", SCREEN_W / 2 - 180, SCREEN_H / 2 - 60, 24, YELLOW);

        EndDrawing();
    }

    // Models share the lit shader: give them back the default before unloading
    for (Model* m : { &unitCube, &spawnPad, &character.model }) Render::DetachLitShader(*m);
    SetWeaponModelShader(Shader{ rlGetShaderIdDefault(), rlGetShaderLocsDefault() });
    UnloadModel(unitCube);
    UnloadModel(spawnPad);
    Render::Shutdown();
    Fx::Shutdown();
    UnloadGroundModel(ground);
    UnloadWeaponModelAssets();
    UnloadWorldMaterials(materials);
    audio.Unload();
    UnloadCharacter(character);
    CloseAudioDevice();
    CloseWindow();
    return 0;
}
