#include <cstdlib>
#include <cstring>
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

int main(int argc, char** argv) {
    // Debug: "--overview N" renders the map from fixed camera N, saves overview_N.png and exits
    int overview = -1;
    for (int i = 1; i + 1 < argc; i++)
        if (strcmp(argv[i], "--overview") == 0) overview = atoi(argv[i + 1]);
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
    Model spawnPad = LoadModelFromMesh(GenMeshCylinder(1.2f, 0.02f, 32)); // spawn markers
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

    bool wasGrounded = true;   // for the landing sound
    float lastFallSpeed = 0.0f;

    // Camera feel (visual only: the real aim is never moved)
    float punchPitch = 0.0f, punchYaw = 0.0f; // degrees, kick when firing
    float shake = 0.0f;                         // when hit
    float muzzleSmoke = 0.0f, smokeTimer = 0.0f;   // wisps off the muzzle after a shot

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
        gun.Reset(); // a fresh life starts with a full cylinder
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
            gun.Reset();
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
                    // Effects: a tracer and smoke from where the gun's muzzle appears, then whatever it hit
                    const ShotResult& shot = gun.LastShot();
                    const Camera3D& c = player.GetCamera();
                    Vector3 fwd = player.GetLookDirection();
                    Vector3 gunTip = viewModel.CameraToWorld(viewModel.MuzzleCameraSpace(gun), c);
                    Vector3 end = shot.hit ? shot.point : Vector3Add(c.position, Vector3Scale(shot.direction, 80.0f));
                    Fx::Tracer(gunTip, end, { 255, 220, 160, 140 });
                    Fx::GunSmoke(gunTip, fwd, 1.0f);
                    muzzleSmoke = 1.0f;
                    Vector3 ear = c.position, right = player.GetRight();
                    if (shot.hitEnemy) {
                        Fx::Blood(shot.point, shot.direction, shot.headshot);
                        audio.PlayAt(Sfx::FleshHit, shot.point, ear, right, 0.9f);
                        audio.Play(shot.headshot ? Sfx::Headshot : Sfx::HitTick, 0.7f);
                    } else if (shot.hit) {
                        Fx::Impact(shot.point, shot.normal, shot.surface);
                        audio.PlayImpact(shot.surface, shot.point, ear, right);
                        if (shot.hitTarget) audio.Play(Sfx::HitTick, 0.5f);
                    }
                    if (shot.killed) {
                        audio.Play(Sfx::KillConfirm, 0.7f);
                        killBanner = 1.6f;
                        killBannerHeadshot = shot.headshot;
                        addFeed(shot.headshot ? "RIVAL ELIMINATED  -  HEADSHOT" : "RIVAL ELIMINATED", { 255, 214, 92, 255 });
                    }
                }
                (void)wasReloading;
                if (gun.TakeDryFire()) {
                    audio.Play(Sfx::DryFire, 0.8f);
                    viewModel.DryFire();
                }
                // Landing thud, louder for longer falls
                if (player.IsGrounded() && !wasGrounded && lastFallSpeed > 3.0f)
                    audio.Play(Sfx::Land, Clamp(lastFallSpeed / 9.0f, 0.25f, 0.9f));
                wasGrounded = player.IsGrounded();
                lastFallSpeed = -player.GetVerticalVelocity();
                player.SetZoom(gun.GetAimAmount());
                viewModel.Update(dt, MotionOf(player), gun, &player.GetCamera());
                // The revolver's mechanism: reload sounds on their beats, cases dropping into the world
                ViewModelEvents vmEvents = viewModel.TakeEvents();
                if (vmEvents.latch) audio.PlayReloadStep(ReloadStep::Latch);
                if (vmEvents.open) audio.PlayReloadStep(ReloadStep::Open);
                if (vmEvents.eject) audio.PlayReloadStep(ReloadStep::Eject);
                if (vmEvents.insert) audio.PlayReloadStep(ReloadStep::Insert);
                if (vmEvents.spin) audio.PlayReloadStep(ReloadStep::Spin);
                if (vmEvents.close) audio.PlayReloadStep(ReloadStep::Close);
                for (const ViewModelEvents::Casing& cs : vmEvents.casings)
                    Fx::Casing(cs.position, Vector3Add(cs.velocity, player.GetVelocity()), cs.fired);
                // Smoke keeps curling off the muzzle for a moment after a shot
                if (muzzleSmoke > 0.0f) {
                    smokeTimer -= dt;
                    if (smokeTimer <= 0.0f) {
                        smokeTimer = 0.05f;
                        Vector3 tip = viewModel.CameraToWorld(viewModel.MuzzleCameraSpace(gun), player.GetCamera());
                        Fx::GunSmoke(tip, { 0, 1, 0 }, muzzleSmoke * 0.3f);
                    }
                    muzzleSmoke -= dt * 1.1f;
                }
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
                    if (t.hitWorld) {
                        Fx::Impact(t.end, t.normal, t.surface);
                        audio.PlayImpact(t.surface, t.end, player.GetCamera().position, player.GetRight());
                    }
                    // A near miss cracks past the player's head
                    if (!t.hitPlayer && player.IsAlive()) {
                        Vector3 head = player.GetCamera().position;
                        Vector3 along = Vector3Subtract(t.end, shot.position);
                        float len = Vector3Length(along);
                        float k = len > 0 ? Clamp(Vector3DotProduct(Vector3Subtract(head, shot.position), along) / (len * len), 0.0f, 1.0f) : 0.0f;
                        Vector3 closest = Vector3Add(shot.position, Vector3Scale(along, k));
                        float miss = Vector3Distance(head, closest);
                        if (miss < 2.0f && k > 0.0f && k < 1.0f) {
                            float pan = Vector3DotProduct(Vector3Normalize(Vector3Subtract(closest, head)), player.GetRight());
                            audio.Play(Sfx::BulletCrack, Lerp(0.9f, 0.35f, miss / 2.0f), pan);
                        }
                    }
                    if (t.hitPlayer && player.IsAlive()) {
                        audio.Play(Sfx::Hurt, t.headshot ? 1.0f : 0.8f);
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
            for (const Fx::CasingBounce& b : Fx::TakeCasingBounces())
                audio.PlayAt(Sfx::CasingTink, b.position, player.GetCamera().position, player.GetRight(), 0.25f + 0.5f * b.strength);
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
        for (const Enemy& e : enemies) {
            CharAnim a = e.GetAnim();
            int moving = (!playing || !e.IsAlive()) ? 0 : (a == CharAnim::Run ? 2 : (a == CharAnim::Walk ? 1 : 0));
            audio.UpdateEnemyFootsteps(moving, e.GetFeet(), player.GetCamera().position, player.GetRight());
        }
        audio.Update();

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
        if (overview >= 0) {
            static const Vector3 VIEWS[][2] = {
                { { 0.0f, 95.0f, -0.01f }, { 0, 0, 0 } },          // straight down
                { { -26.0f, 26.0f, -52.0f }, { 0, 0, -12 } },      // over team A's spawn
                { { 30.0f, 22.0f, 4.0f }, { 0, 0, -8 } },          // the plaza from the east
                { { 2.0f, 3.5f, -34.0f }, { 2, 2.5f, -12 } },      // mid from spawn A, eye height
                { { 17.5f, 5.1f, -22.0f }, { 14, 2.5f, -4 } },     // standing on the apartments terrace
                { { -16.0f, 1.7f, -34.0f }, { -15, 1.6f, -18 } },  // long from spawn
            };
            int v = overview % (int)(sizeof(VIEWS) / sizeof(VIEWS[0]));
            cam.position = VIEWS[v][0];
            cam.target = VIEWS[v][1];
            cam.up = { 0, 1, 0 };
            cam.fovy = v == 0 ? 50.0f : 70.0f;
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

        // Muzzle flashes light up their surroundings
        Vector3 look = player.GetLookDirection();
        Render::SetFlashLight(0, Vector3Add(cam.position, Vector3Add(Vector3Scale(look, 0.8f), Vector3Scale(player.GetRight(), 0.15f))),
                              { 7.0f, 4.5f, 1.8f }, gun.IsMuzzleFlashing() ? 7.0f : 0.0f);
        Render::SetFlashLight(1, enemies.empty() ? Vector3{} : enemies[0].GetMuzzle(), { 7.0f, 4.5f, 1.8f },
                              !enemies.empty() && enemies[0].IsMuzzleFlashing() ? 7.0f : 0.0f);

        // 1. Shadows from the sun
        Render::BeginShadowPass();
            Render::DrawLevel();
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
            for (const Enemy& e : enemies) e.Draw(&character);
            drawPlayerBody(PART_LEGS);
            for (const SpawnPoint& s : spawns)
                Fx::DrawGroundMarker(s.position, 1.3f, s.yaw, { s.color.r, s.color.g, s.color.b, 170 });
            Fx::DrawCasings(DrawCartridge);
            Render::SurfaceObject(0.25f);
            Fx::Draw(cam);
        Render::EndWorldPass();

        // 3. First-person weapon: its own pass on top of the world, lit like the
        //    spot the player stands in (darker in shadow)
        if (player.IsAlive() && started && overview < 0) {
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
        if (overview < 0) Hud::Draw(hs);
        if (paused) {
            int click = Hud::PauseMenuClick(hs);
            if (click == 1) { paused = false; DisableCursor(); inputGrace = 2; }
            if (click == 2) quit = true;
        }

        EndDrawing();
        static int frames = 0;
        if (overview >= 0 && ++frames == 30) {
            TakeScreenshot(TextFormat("overview_%d.png", overview));
            quit = true;
        }
    }

    // Models share the lit shader: give them back the default before unloading
    for (Model* m : { &spawnPad, &character.model }) Render::DetachLitShader(*m);
    SetWeaponModelShader(Shader{ rlGetShaderIdDefault(), rlGetShaderLocsDefault() });
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
