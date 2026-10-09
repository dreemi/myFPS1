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
#include "hud.hpp"
#include "raymath.h"
#include "rlgl.h"
#include <algorithm>
#include <string>

static const int SCREEN_W = 1280;
static const int SCREEN_H = 720;

int main() {
    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
    InitWindow(SCREEN_W, SCREEN_H, "Dustyard");
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
    Hud::Init();
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

    bool started = false;      // title screen until the first click
    bool paused = false;
    bool quit = false;
    bool showFps = false;
    int inputGrace = 0;        // frames to ignore the gun after a menu click
    int deaths = 0;
    float hurt = 0.0f;         // recent damage, drives the red screen edges (0..1)
    float deathTimer = 0.0f;   // > 0 while dead
    float killBanner = 0.0f;   // "ELIMINATED" under the crosshair
    bool killBannerHeadshot = false;
    std::vector<Hud::DamageMarker> damageMarkers;
    std::vector<Hud::FeedEntry> feed;
    auto addFeed = [&](const std::string& text, Color color) {
        feed.insert(feed.begin(), { text, color, 5.0f });
        if (feed.size() > 4) feed.pop_back();
    };

    // Camera feel (visual only: the real aim is never moved)
    float punchPitch = 0.0f, punchYaw = 0.0f; // degrees, kick when firing
    float shake = 0.0f;                         // when hit

    SetExitKey(KEY_NULL); // Esc pauses instead of quitting
    EnableCursor();       // for the title screen

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

    while (!WindowShouldClose() && !quit) {
        float dt = GetFrameTime();

        // ---------- Menus ----------
        if (IsKeyPressed(KEY_F3)) showFps = !showFps;
        if (!started) {
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) { started = true; DisableCursor(); inputGrace = 2; }
        } else {
            if (IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_TAB)) {
                paused = !paused;
                if (paused) EnableCursor(); else DisableCursor();
            }
        }
        bool playing = started && !paused;
        if (inputGrace > 0 && playing) inputGrace--;

        // Enter respawns at the next spawn point (the other end of the arena)
        if (playing && player.IsAlive() && IsKeyPressed(KEY_ENTER)) {
            currentSpawn = (currentSpawn + 1) % (int)spawns.size();
            player.Init(spawns[currentSpawn].position, spawns[currentSpawn].yaw);
        }

        // ---------- Update ----------
        if (playing) {
            if (player.IsAlive()) {
                player.Update(dt, world);
                bool wasReloading = gun.IsReloading();
                bool fired = inputGrace > 0 ? (gun.Tick(dt, player.GetStance()), false)
                                            : gun.Update(dt, player.GetCamera(), world, enemies, player.GetStance());
                if (fired) {
                    audio.PlayGunshot(0.8f, 0.0f);
                    punchPitch += Lerp(2.4f, 1.1f, gun.GetAimAmount());
                    punchYaw += GetRandomValue(-100, 100) / 100.0f * 0.5f;
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
                    if (shot.killed) {
                        killBanner = 1.6f;
                        killBannerHeadshot = shot.headshot;
                        addFeed(shot.headshot ? "RIVAL ELIMINATED  -  HEADSHOT" : "RIVAL ELIMINATED", { 255, 214, 92, 255 });
                    }
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
                        hurt = 1.0f;
                        shake = t.headshot ? 1.0f : 0.6f;
                        // Which way the shot came from, relative to the way the player faces
                        Vector3 from = Vector3Subtract(e.GetMuzzle(), player.GetFeet());
                        Vector3 fwd = player.GetLookDirection();
                        fwd.y = 0;
                        fwd = Vector3Normalize(fwd);
                        float angle = atan2f(Vector3DotProduct(from, player.GetRight()), Vector3DotProduct(from, fwd));
                        damageMarkers.push_back({ angle, 1.4f });
                        if (player.TakeDamage(t.headshot ? REVOLVER.headDamage : REVOLVER.bodyDamage)) {
                            deaths++;
                            deathTimer = 2.5f;
                            player.SetZoom(0.0f);
                            audio.StopReload();
                            addFeed(t.headshot ? "YOU WERE ELIMINATED  -  HEADSHOT" : "YOU WERE ELIMINATED", { 232, 72, 58, 255 });
                        }
                    }
                }
            }
            Fx::Update(dt);
            hurt = std::max(0.0f, hurt - dt * 1.2f);
            shake = std::max(0.0f, shake - dt * 3.0f);
            killBanner = std::max(0.0f, killBanner - dt);
            punchPitch -= punchPitch * std::min(1.0f, dt * 11.0f);
            punchYaw -= punchYaw * std::min(1.0f, dt * 11.0f);
            for (Hud::DamageMarker& d : damageMarkers) d.life -= dt;
            damageMarkers.erase(std::remove_if(damageMarkers.begin(), damageMarkers.end(), [](const Hud::DamageMarker& d) { return d.life <= 0; }),
                                damageMarkers.end());
            for (Hud::FeedEntry& f : feed) f.life -= dt;
            feed.erase(std::remove_if(feed.begin(), feed.end(), [](const Hud::FeedEntry& f) { return f.life <= 0; }), feed.end());
        }
        for (WorldBox& b : world) b.hitFlash = std::max(0.0f, b.hitFlash - dt);
        audio.UpdateFootsteps(player.GetStance(), player.IsAlive() && playing);

        // ---------- Draw ----------
        // The camera we render from: the player's, plus the visual kick and shake
        Camera3D cam = player.GetCamera();
        {
            Vector3 f = Vector3Normalize(Vector3Subtract(cam.target, cam.position));
            Vector3 r = Vector3Normalize(Vector3CrossProduct(f, { 0, 1, 0 }));
            f = Vector3RotateByAxisAngle(f, r, punchPitch * DEG2RAD);
            f = Vector3RotateByAxisAngle(f, { 0, 1, 0 }, -punchYaw * DEG2RAD);
            if (shake > 0.0f) {
                float k = shake * shake * 0.025f;
                cam.position = Vector3Add(cam.position, { GetRandomValue(-100, 100) / 100.0f * k, GetRandomValue(-100, 100) / 100.0f * k, 0 });
            }
            cam.target = Vector3Add(cam.position, f);
        }

        // The player's own body (no head or arms: the camera is in the head and the
        // first-person arms are drawn with the gun), so the legs show when looking
        // down. The whole body still casts a shadow.
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
                // sit under the camera, plus a little more to stay out of the collar
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
        //    spot the player stands in (darker in shadow)
        if (player.IsAlive() && started) {
            sunOnGun += ((Render::SunVisible(cam.position, world) ? 1.0f : 0.0f) - sunOnGun) * (1.0f - expf(-8.0f * dt));
            Render::SetViewModelLighting(cam, sunOnGun);
            Render::SetFlashLight(0, { 0.15f, -0.08f, -0.9f }, { 7.0f, 4.5f, 1.8f }, gun.IsMuzzleFlashing() ? 1.5f : 0.0f);
            Render::SetFlashLight(1, {}, {}, 0.0f);
            viewModel.Draw(gun);
        }

        // Enemy health: a slim bar over the head once it has been hurt
        for (const Enemy& e : enemies) {
            if (!e.IsAlive() || e.GetHealth() >= e.GetMaxHealth() || !started) continue;
            Vector3 top = Vector3Add(e.GetFeet(), { 0, e.GetHeight() + 0.3f, 0 });
            if (Vector3DotProduct(Vector3Subtract(top, cam.position), player.GetLookDirection()) <= 0.0f) continue; // behind
            if (!HasLineOfSight(cam.position, top, world)) continue;
            Vector2 s = GetWorldToScreen(top, cam);
            const float bw = 54, bh = 5;
            DrawRectangleRec({ s.x - bw / 2 - 1, s.y - 1, bw + 2, bh + 2 }, { 0, 0, 0, 160 });
            DrawRectangleRec({ s.x - bw / 2, s.y, bw * e.GetHealth() / e.GetMaxHealth(), bh }, { 232, 72, 58, 255 });
        }

        // 4. HUD and menus
        Hud::State hs;
        hs.screenW = SCREEN_W;
        hs.screenH = SCREEN_H;
        hs.health = player.GetHealth();
        hs.maxHealth = player.GetMaxHealth();
        hs.alive = player.IsAlive();
        hs.deathTimer = deathTimer;
        hs.hurt = hurt;
        hs.ammo = gun.GetAmmo();
        hs.magazine = gun.GetDef().magazineSize;
        hs.reloading = gun.IsReloading();
        hs.reloadProgress = gun.GetReloadProgress();
        hs.spreadPx = tanf(gun.GetSpread() * DEG2RAD) / tanf(player.GetCamera().fovy * 0.5f * DEG2RAD) * (SCREEN_H * 0.5f);
        hs.recoil = gun.GetRecoil();
        hs.aim = gun.GetAimAmount();
        hs.hitMarker = gun.GetHitMarker();
        hs.hitKilled = gun.LastHitKilled();
        hs.hitHeadshot = gun.LastHitHeadshot();
        hs.killBanner = killBanner;
        hs.killBannerHeadshot = killBannerHeadshot;
        hs.kills = gun.GetKills();
        hs.deaths = deaths;
        hs.damage = &damageMarkers;
        hs.feed = &feed;
        hs.started = started;
        hs.paused = paused;
        hs.showFps = showFps;
        Hud::Draw(hs);
        if (paused) {
            int click = Hud::PauseMenuClick(hs);
            if (click == 1) { paused = false; DisableCursor(); inputGrace = 2; }
            if (click == 2) quit = true;
        }

        EndDrawing();
    }

    // Models share the lit shader: give them back the default before unloading
    for (Model* m : { &unitCube, &spawnPad, &character.model }) Render::DetachLitShader(*m);
    SetWeaponModelShader(Shader{ rlGetShaderIdDefault(), rlGetShaderLocsDefault() });
    UnloadModel(unitCube);
    UnloadModel(spawnPad);
    Render::Shutdown();
    Fx::Shutdown();
    Hud::Shutdown();
    UnloadGroundModel(ground);
    UnloadWeaponModelAssets();
    UnloadWorldMaterials(materials);
    audio.Unload();
    UnloadCharacter(character);
    CloseAudioDevice();
    CloseWindow();
    return 0;
}
