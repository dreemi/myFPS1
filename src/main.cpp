#include <cstdlib>
#include <cstring>
#include "raylib.h"
#include "player.hpp"
#include "weapon.hpp"
#include "viewmodel.hpp"
#include "rifle_view.hpp"
#include "world.hpp"
#include "enemy.hpp"
#include "materials.hpp"
#include "audio.hpp"
#include "character.hpp"
#include "render.hpp"
#include "effects.hpp"
#include "hud.hpp"
#include "soldier.hpp"
#include "helmet.hpp"
#include "game/session.hpp"
#include "raymath.h"
#include "rlgl.h"
#include <algorithm>
#include <fstream>
#include <map>
#include <memory>
#include <string>

static const int SCREEN_W = 1280;
static const int SCREEN_H = 720;

// --- Settings remembered between runs (name, last address, port, bots) --------------
static std::string ConfigPath() { return std::string(GetApplicationDirectory()) + "dustyard.cfg"; }

static void LoadSettings(Hud::MenuFields& f) {
    std::ifstream in(ConfigPath());
    std::string key, value;
    while (in >> key >> value) {
        if (key == "name") f.name = value;
        else if (key == "address") f.address = value;
        else if (key == "port") f.port = value;
        else if (key == "bots") f.bots = std::clamp(atoi(value.c_str()), 0, 4);
    }
}

static void SaveSettings(const Hud::MenuFields& f) {
    std::ofstream out(ConfigPath());
    auto safe = [](std::string s) { // one word per value
        for (char& c : s) if (c == ' ') c = '_';
        return s.empty() ? std::string("-") : s;
    };
    out << "name " << safe(f.name) << "\naddress " << safe(f.address) << "\nport " << safe(f.port) << "\nbots " << f.bots << "\n";
}

// Presentation state that belongs to one session (reset when a new one starts)
struct Feel {
    float hurt = 0.0f;         // recent damage, drives the red screen edges (0..1)
    float killBanner = 0.0f;   // "ELIMINATED" under the crosshair
    bool killBannerHeadshot = false;
    std::vector<Hud::DamageMarker> damageMarkers;
    std::vector<Hud::FeedEntry> feed;
    bool wasGrounded = true;   // for the landing sound
    float lastFallSpeed = 0.0f;
    float punchPitch = 0.0f, punchYaw = 0.0f; // degrees, camera kick when firing (visual only)
    float shake = 0.0f;        // when hit
    float muzzleSmoke = 0.0f, smokeTimer = 0.0f;
    float bodyPhase = 0.0f;    // stride cycles walked, for your own legs
    bool wasAlive = true;
    std::map<uint8_t, std::string> names; // remembered, for players who've left
    std::map<uint8_t, bool> wasReloading;
};

int main(int argc, char** argv) {
    // Command line: --overview N (debug render), --host [port], --join address[:port],
    // --offline, --name NAME, --bots N
    int overview = -1;
    Hud::MenuFields fields;
    LoadSettings(fields);
    int autoStart = -1; // a SessionMode to start straight away
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        bool hasNext = i + 1 < argc && argv[i + 1][0] != '-';
        if (a == "--overview" && hasNext) overview = atoi(argv[++i]);
        else if (a == "--host") { autoStart = (int)SessionMode::Host; if (hasNext) fields.port = argv[++i]; }
        else if (a == "--offline") autoStart = (int)SessionMode::Offline;
        else if (a == "--join" && hasNext) {
            autoStart = (int)SessionMode::Client;
            std::string addr = argv[++i];
            size_t colon = addr.rfind(':');
            if (colon != std::string::npos) { fields.port = addr.substr(colon + 1); addr = addr.substr(0, colon); }
            fields.address = addr;
        } else if (a == "--name" && hasNext) fields.name = argv[++i];
        else if (a == "--bots" && hasNext) fields.bots = std::clamp(atoi(argv[++i]), 0, 4);
    }

    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
    InitWindow(SCREEN_W, SCREEN_H, "Dustyard");
    SetTargetFPS(60);
    InitAudioDevice();

    // Textures and models live in assets/ next to the executable
    std::string assetDir = std::string(GetApplicationDirectory()) + "assets/";
    WorldMaterials materials = LoadWorldMaterials(assetDir);
    LoadWeaponModelAssets(assetDir.c_str());
    GameAudio audio;
    audio.Load(assetDir);
    CharacterAssets character; // everyone's body, your own legs, and your first-person arms
    LoadCharacter(character, assetDir);

    World world = BuildTestWorld();
    Model ground = LoadGroundModel(ARENA_WIDTH, ARENA_LENGTH, materials); // only used if lighting is unavailable

    // Lighting: one lit shader for the level, characters and guns
    Render::Init(assetDir);
    Fx::Init();
    Fx::SetWorld(&world); // dropped brass and magazines land on whatever they hit
    Hud::Init();
    Render::BuildLevel(world, materials);
    Render::ApplyLitShader(character.model);
    SetWeaponModelShader(Render::LitShader());
    InitHelmet(&character);
    float sunOnGun = 1.0f; // eases between 0 (player in shade) and 1
    std::vector<SpawnPoint> spawns = GetSpawnPoints();

    std::unique_ptr<Session> session;
    std::unique_ptr<RifleView> rifleView;
    std::unique_ptr<ViewModel> revolverView;
    Feel feel;
    Hud::MenuStatus menuStatus;
    bool paused = false;
    bool quit = false;
    bool showFps = false;
    int inputGrace = 0;        // frames to ignore the gun after a menu click

    SetExitKey(KEY_NULL); // Esc pauses instead of quitting
    EnableCursor();       // for the menu

    auto startSession = [&](SessionMode mode) {
        SessionConfig cfg;
        cfg.name = fields.name;
        cfg.address = fields.address;
        int port = atoi(fields.port.c_str());
        cfg.port = (uint16_t)(port > 0 && port < 65536 ? port : 27015);
        fields.port = std::to_string(cfg.port);
        cfg.bots = fields.bots;
        SaveSettings(fields);
        session = std::make_unique<Session>(mode, world, cfg);
        rifleView = std::make_unique<RifleView>(FAL_MODELS);
        revolverView = std::make_unique<ViewModel>(REVOLVER_MODELS);
        rifleView->arms = revolverView->arms = &character;
        feel = Feel{};
        Fx::Clear();
        paused = false;
        menuStatus = {};
        if (session->State() == SessionState::Failed) {
            menuStatus = { session->StatusText(), false, true };
            session.reset();
        } else if (session->State() == SessionState::Connecting) {
            menuStatus = { session->StatusText(), true, false };
        }
    };
    auto endSession = [&](const std::string& message, bool error) {
        if (session) session->Leave();
        session.reset();
        audio.StopReload();
        audio.StopFootstepsFrom(0);
        menuStatus = { message, false, error };
        paused = false;
        EnableCursor();
    };
    if (autoStart >= 0 && overview < 0) startSession((SessionMode)autoStart);

    bool inGame = false; // the cursor is captured while playing
    while (!WindowShouldClose() && !quit) {
        float dt = GetFrameTime();
        if (IsKeyPressed(KEY_F3)) showFps = !showFps;

        // ---------- Session lifecycle ----------
        if (session && session->State() == SessionState::Connecting) {
            session->Update(dt, LocalInput{});
            menuStatus.text = session->StatusText();
        }
        if (session && (session->State() == SessionState::Failed || session->State() == SessionState::Ended)) {
            endSession(session->StatusText(), session->State() == SessionState::Failed ||
                                                  session->Mode() == SessionMode::Client);
        }
        bool playing = session && session->State() == SessionState::Playing && session->Local();
        if (playing && !inGame) { DisableCursor(); inGame = true; inputGrace = 2; }
        if (!playing && inGame) { EnableCursor(); inGame = false; }

        SimPlayer* local = playing ? session->Local() : nullptr;
        Weapon* gunPtr = local ? &local->Current() : nullptr;
        WeaponView* viewModel = nullptr;
        if (local) viewModel = local->current == WEAPON_RIFLE ? (WeaponView*)rifleView.get() : (WeaponView*)revolverView.get();

        // ---------- Update ----------
        if (playing) {
            if (IsKeyPressed(KEY_ESCAPE)) {
                paused = !paused;
                if (paused) EnableCursor(); else DisableCursor();
            }
            if (inputGrace > 0 && !paused) inputGrace--;
            LocalInput in;
            bool controls = !paused && inputGrace == 0;
            if (controls) {
                in.move = Player::ReadInput();
                in.weapon = WeaponInput::FromKeyboardAndMouse();
                if (IsKeyPressed(KEY_ONE)) in.switchTo = WEAPON_RIFLE;
                if (IsKeyPressed(KEY_TWO)) in.switchTo = WEAPON_REVOLVER;
                if (GetMouseWheelMove() != 0.0f) in.switchTo = 2;
                in.switchSpawn = IsKeyPressed(KEY_ENTER);
            }
            // Offline, pausing stops the world; in a shared game it carries on without you
            if (!(paused && session->Mode() == SessionMode::Offline)) session->Update(dt, in);
            if (session->State() != SessionState::Playing) continue; // ended this frame: back to the menu next frame
            local = session->Local();
            gunPtr = &local->Current();
            viewModel = local->current == WEAPON_RIFLE ? (WeaponView*)rifleView.get() : (WeaponView*)revolverView.get();
        }

        // Names, remembered for the kill feed (even after someone leaves)
        std::vector<ScoreRow> scores;
        if (session && playing) {
            scores = session->Scoreboard();
            for (const ScoreRow& r : scores) feel.names[r.id] = r.name;
        }
        auto nameOf = [&](uint8_t id) -> std::string {
            if (id >= BOT_ID_BASE && id != NO_ID) return BotName(id);
            auto it = feel.names.find(id);
            return it != feel.names.end() ? it->second : std::string("?");
        };
        auto findSoldier = [&](uint8_t id) -> const SoldierView* {
            if (!session) return nullptr;
            for (const SoldierView& v : session->Others()) if (v.id == id) return &v;
            for (const SoldierView& v : session->Bots()) if (v.id == id) return &v;
            return nullptr;
        };

        Camera3D cam = {};
        if (local) {
            cam = local->body.GetCamera();
            Vector3 off = session->ViewOffset();
            cam.position = Vector3Add(cam.position, off);
            cam.target = Vector3Add(cam.target, off);
        }

        if (playing) {
            Weapon& gun = *gunPtr;
            Vector3 ear = cam.position, right = local->body.GetRight();
            uint8_t me = local->id;
            auto addFeed = [&](const std::string& text, Color color) {
                feel.feed.insert(feel.feed.begin(), { text, color, 5.0f });
                if (feel.feed.size() > 5) feel.feed.pop_back();
            };

            // What happened: effects, sounds and the HUD
            for (const GameEvent& e : session->TakeEvents()) {
                switch (e.type) {
                case EventType::Shot: {
                    bool mine = e.actor == me;
                    Vector3 dir = Vector3Normalize(Vector3Subtract(e.to, e.from));
                    Vector3 muzzle = e.from;
                    if (mine) {
                        muzzle = viewModel->CameraToWorld(viewModel->MuzzleCameraSpace(gun), cam);
                    } else if (const SoldierView* v = findSoldier(e.actor)) {
                        muzzle = SoldierMuzzle(&character, *v);
                    }
                    if (!e.Is(EVENT_ECHO)) {
                        // The shot itself: bang, flash, tracer
                        if (mine) {
                            if (e.weapon == WEAPON_RIFLE) audio.PlayRifleShot(0.85f, 0.0f);
                            else audio.PlayGunshot(0.8f, 0.0f);
                            feel.punchPitch += e.weapon == WEAPON_RIFLE ? Lerp(1.2f, 0.6f, gun.GetAimAmount()) : Lerp(2.4f, 1.1f, gun.GetAimAmount());
                            feel.punchYaw += GetRandomValue(-100, 100) / 100.0f * 0.5f;
                            Fx::GunSmoke(muzzle, local->body.GetLookDirection(), 1.0f);
                            feel.muzzleSmoke = 1.0f;
                            Fx::Tracer(muzzle, e.to, { 255, 220, 160, 140 });
                        } else {
                            if (e.weapon == WEAPON_RIFLE) audio.PlayRifleShotAt(muzzle, ear, right);
                            else audio.PlayGunshotAt(muzzle, ear, right);
                            Fx::MuzzleFlash(muzzle, dir);
                            Fx::Tracer(muzzle, e.to, { 255, 210, 140, 255 });
                            // A near miss cracks past your head
                            if (local->Alive() && !(e.hit == HitKind::Player && e.target == me)) {
                                Vector3 along = Vector3Subtract(e.to, e.from);
                                float len = Vector3Length(along);
                                float k = len > 0 ? Clamp(Vector3DotProduct(Vector3Subtract(ear, e.from), along) / (len * len), 0.0f, 1.0f) : 0.0f;
                                Vector3 closest = Vector3Add(e.from, Vector3Scale(along, k));
                                float miss = Vector3Distance(ear, closest);
                                if (miss < 2.0f && k > 0.0f && k < 1.0f) {
                                    float pan = Vector3DotProduct(Vector3Normalize(Vector3Subtract(closest, ear)), right);
                                    audio.Play(Sfx::BulletCrack, Lerp(0.9f, 0.35f, miss / 2.0f), pan);
                                }
                            }
                        }
                    }
                    if (e.Is(EVENT_PREDICTED)) break; // what it hit comes from the host
                    // What it hit
                    if (e.Is(EVENT_DEFLECTED)) {
                        Fx::Impact(e.to, Vector3Negate(dir), Surface::Steel);
                        audio.PlayImpact(Surface::Steel, e.to, ear, right);
                        if (mine) audio.Play(Sfx::HitTick, 0.4f);
                    } else if (e.hit == HitKind::World) {
                        Fx::Impact(e.to, e.normal, (Surface)e.surface);
                        audio.PlayImpact((Surface)e.surface, e.to, ear, right);
                    } else if (e.hit == HitKind::Player || e.hit == HitKind::Bot) {
                        if (!(e.hit == HitKind::Player && e.target == me)) {
                            Fx::Blood(e.to, dir, e.Is(EVENT_HEADSHOT));
                            audio.PlayAt(Sfx::FleshHit, e.to, ear, right, 0.9f);
                        }
                        if (mine) audio.Play(e.Is(EVENT_HEADSHOT) ? Sfx::Headshot : Sfx::HitTick, 0.7f);
                    }
                    break;
                }
                case EventType::Hurt:
                    if (e.target == me) {
                        audio.Play(Sfx::Hurt, e.Is(EVENT_HEADSHOT) ? 1.0f : 0.8f);
                        feel.hurt = 1.0f;
                        feel.shake = e.Is(EVENT_HEADSHOT) ? 1.0f : 0.6f;
                        // Which way it came from, relative to the way you face
                        Vector3 from = Vector3Subtract(e.from, local->body.GetFeet());
                        Vector3 fwd = local->body.GetLookDirection();
                        fwd.y = 0;
                        fwd = Vector3Normalize(fwd);
                        float angle = atan2f(Vector3DotProduct(from, right), Vector3DotProduct(from, fwd));
                        feel.damageMarkers.push_back({ angle, 1.4f });
                    }
                    break;
                case EventType::Killed: {
                    const char* weapon = e.weapon == WEAPON_RIFLE ? "FAL" : "REVOLVER";
                    std::string line = nameOf(e.actor) + "  [" + weapon + "]  " + nameOf(e.target);
                    if (e.Is(EVENT_HEADSHOT)) line += "  -  HEADSHOT";
                    Color c = { 236, 236, 230, 255 };
                    if (e.actor == me) {
                        c = { 255, 214, 92, 255 };
                        audio.Play(Sfx::KillConfirm, 0.7f);
                        feel.killBanner = 1.6f;
                        feel.killBannerHeadshot = e.Is(EVENT_HEADSHOT);
                    }
                    if (e.target == me) {
                        c = { 232, 72, 58, 255 };
                        local->body.SetZoom(0.0f);
                        audio.StopReload();
                    }
                    addFeed(line, c);
                    break;
                }
                case EventType::Joined:
                    if (e.target != me) addFeed(nameOf(e.target) + " joined", { 170, 200, 230, 255 });
                    break;
                case EventType::Left:
                    addFeed(nameOf(e.target) + " left", { 170, 172, 168, 255 });
                    break;
                case EventType::HelmetOff: {
                    Vector3 dir = Vector3Normalize(Vector3Subtract(e.to, e.from));
                    dir.y = 0.0f;
                    dir = Vector3LengthSqr(dir) > 1e-6f ? Vector3Normalize(dir) : Vector3{ 0, 0, 1 };
                    Matrix start;
                    Vector3 carried{};
                    if (e.target == me) {
                        // Yours: off the back of your head and past you
                        Vector3 head = Vector3Add(cam.position, { 0, 0.08f, 0 });
                        Vector3 look = local->body.GetLookDirection();
                        start = MatrixMultiply(MatrixRotateY(atan2f(look.x, look.z)), MatrixTranslate(head.x, head.y, head.z));
                        carried = local->body.GetVelocity();
                        audio.Play(Sfx::Hurt, 0.5f);
                        feel.shake = 0.7f;
                        addFeed("Your helmet stopped a round", { 170, 200, 140, 255 });
                    } else if (const SoldierView* v = findSoldier(e.target)) {
                        start = SoldierHelmetTransform(&character, *v);
                    } else {
                        start = MatrixTranslate(e.from.x, e.from.y, e.from.z);
                    }
                    Vector3 at = { start.m12, start.m13, start.m14 };
                    Vector3 vel = Vector3Add(Vector3Add(Vector3Scale(dir, 3.2f), { 0, 2.6f, 0 }), carried);
                    Matrix rot = start;
                    rot.m12 = rot.m13 = rot.m14 = 0.0f;
                    Fx::Tumble(at, vel, rot, DEBRIS_HELMET, HelmetRestHeight());
                    Fx::Impact(e.from, Vector3Negate(dir), Surface::Steel);
                    audio.PlayImpact(Surface::Steel, e.from, ear, right);
                    break;
                }
                case EventType::Respawned:
                    break;
                }
            }

            if (local->Alive()) {
                if (session->TakeDryFire()) {
                    audio.Play(Sfx::DryFire, 0.8f);
                    viewModel->DryFire();
                }
                // Landing thud, louder for longer falls
                if (local->body.IsGrounded() && !feel.wasGrounded && feel.lastFallSpeed > 3.0f)
                    audio.Play(Sfx::Land, Clamp(feel.lastFallSpeed / 9.0f, 0.25f, 0.9f));
                feel.wasGrounded = local->body.IsGrounded();
                feel.lastFallSpeed = -local->body.GetVerticalVelocity();

                // The first-person gun: holstering while switching, its mechanism, its sounds
                rifleView->holster = local->current == WEAPON_RIFLE ? local->holster : 1.0f;
                revolverView->holster = local->current == WEAPON_REVOLVER ? local->holster : 1.0f;
                viewModel->Update(dt, MotionOf(local->body), gun, &cam);
                ViewModelEvents vmEvents = viewModel->TakeEvents();
                for (ViewSound s : vmEvents.sounds) {
                    switch (s) {
                        case ViewSound::Latch: audio.PlayReloadStep(ReloadStep::Latch); break;
                        case ViewSound::Open: audio.PlayReloadStep(ReloadStep::Open); break;
                        case ViewSound::Eject: audio.PlayReloadStep(ReloadStep::Eject); break;
                        case ViewSound::Insert: audio.PlayReloadStep(ReloadStep::Insert); break;
                        case ViewSound::Spin: audio.PlayReloadStep(ReloadStep::Spin); break;
                        case ViewSound::Close: audio.PlayReloadStep(ReloadStep::Close); break;
                        case ViewSound::MagOut: audio.Play(Sfx::MagOut, 0.8f); break;
                        case ViewSound::MagIn: audio.Play(Sfx::MagIn, 0.85f); break;
                        case ViewSound::Slap: audio.Play(Sfx::MagSlap, 0.7f); break;
                        case ViewSound::ChargeBack: audio.Play(Sfx::ChargeBack, 0.8f); break;
                        case ViewSound::ChargeRelease: audio.Play(Sfx::ChargeRelease, 0.9f); break;
                    }
                }
                for (const ViewModelEvents::Debris& d : vmEvents.debris) {
                    bool mag = d.kind == DEBRIS_RIFLE_MAG;
                    Fx::Casing(d.position, Vector3Add(d.velocity, local->body.GetVelocity()), d.kind,
                               mag ? 0.0136f : (d.kind == DEBRIS_RIFLE_CASE ? 0.006f : 0.0055f), mag);
                }
                // Smoke keeps curling off the muzzle for a moment after a shot
                if (feel.muzzleSmoke > 0.0f) {
                    feel.smokeTimer -= dt;
                    if (feel.smokeTimer <= 0.0f) {
                        feel.smokeTimer = 0.05f;
                        Fx::GunSmoke(viewModel->CameraToWorld(viewModel->MuzzleCameraSpace(gun), cam), { 0, 1, 0 }, feel.muzzleSmoke * 0.3f);
                    }
                    feel.muzzleSmoke -= dt * 1.1f;
                }
                Stance st = local->body.GetStance();
                float stride = st == Stance::Sprinting ? 2.2f : (st == Stance::CrouchMoving ? 1.0f : 1.4f);
                feel.bodyPhase += local->body.GetSpeed() * dt / stride;
            }
            feel.wasAlive = local->Alive();

            // Other people's reloads, heard from where they are
            for (const SoldierView& v : session->Others()) {
                bool was = feel.wasReloading[v.id];
                if (v.reloading && !was) audio.PlayAt(Sfx::MagOut, v.feet, ear, right, 0.6f);
                feel.wasReloading[v.id] = v.reloading;
            }

            Fx::Update(dt);
            for (const Fx::CasingBounce& b : Fx::TakeCasingBounces()) {
                if (b.kind == DEBRIS_RIFLE_MAG || b.kind == DEBRIS_HELMET)
                    audio.PlayAt(Sfx::MagDrop, b.position, ear, right, 0.4f + 0.5f * b.strength);
                else
                    audio.PlayAt(Sfx::CasingTink, b.position, ear, right, 0.06f + 0.14f * b.strength); // brass: a faint tinkle
            }
            feel.hurt = std::max(0.0f, feel.hurt - dt * 1.2f);
            feel.shake = std::max(0.0f, feel.shake - dt * 3.0f);
            feel.killBanner = std::max(0.0f, feel.killBanner - dt);
            feel.punchPitch -= feel.punchPitch * std::min(1.0f, dt * 11.0f);
            feel.punchYaw -= feel.punchYaw * std::min(1.0f, dt * 11.0f);
            for (Hud::DamageMarker& d : feel.damageMarkers) d.life -= dt;
            feel.damageMarkers.erase(std::remove_if(feel.damageMarkers.begin(), feel.damageMarkers.end(),
                                                    [](const Hud::DamageMarker& d) { return d.life <= 0; }),
                                     feel.damageMarkers.end());
            for (Hud::FeedEntry& f : feel.feed) f.life -= dt;
            feel.feed.erase(std::remove_if(feel.feed.begin(), feel.feed.end(), [](const Hud::FeedEntry& f) { return f.life <= 0; }),
                            feel.feed.end());

            // Footsteps: yours, and everyone else's from where they are
            audio.UpdateFootsteps(local->body.GetStance(), local->Alive());
            int slot = 0;
            auto steps = [&](const SoldierView& v) {
                int moving = 0;
                if (v.alive && v.grounded) {
                    if (v.anim == CharAnim::Run) moving = 2;
                    else if (v.anim == CharAnim::Walk || (v.bot && v.speed > 0.3f)) moving = 1;
                }
                audio.UpdateFootstepsAt(slot++, moving, v.feet, ear, right);
            };
            for (const SoldierView& v : session->Others()) steps(v);
            for (const SoldierView& v : session->Bots()) steps(v);
            audio.StopFootstepsFrom(slot);
        } else {
            audio.UpdateFootsteps(Stance::Still, false);
        }
        audio.Update();

        // ---------- Draw ----------
        if (local) {
            // The camera we render from: the player's, plus the visual kick and shake
            Vector3 f = Vector3Normalize(Vector3Subtract(cam.target, cam.position));
            Vector3 r = Vector3Normalize(Vector3CrossProduct(f, { 0, 1, 0 }));
            f = Vector3RotateByAxisAngle(f, r, feel.punchPitch * DEG2RAD);
            f = Vector3RotateByAxisAngle(f, { 0, 1, 0 }, -feel.punchYaw * DEG2RAD);
            if (feel.shake > 0.0f) {
                float k = feel.shake * feel.shake * 0.025f;
                cam.position = Vector3Add(cam.position, { GetRandomValue(-100, 100) / 100.0f * k, GetRandomValue(-100, 100) / 100.0f * k, 0 });
            }
            cam.target = Vector3Add(cam.position, f);
        } else {
            // The menu: a slow flight around the town
            float a = (float)GetTime() * 0.05f;
            cam.position = { sinf(a) * 34.0f, 16.0f, cosf(a) * 34.0f };
            cam.target = { 0, 1.5f, 0 };
            cam.up = { 0, 1, 0 };
            cam.fovy = 60.0f;
            cam.projection = CAMERA_PERSPECTIVE;
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
            cam.projection = CAMERA_PERSPECTIVE;
        }

        // The player's own body (no head or arms: the camera is in the head and the
        // first-person arms are drawn with the gun), so the legs show when looking
        // down. The whole body still casts a shadow.
        auto drawPlayerBody = [&](int parts) {
            if (local && local->Alive() && character.loaded) {
                CharAnim anim = CharAnim::Idle;
                switch (local->body.GetStance()) {
                    case Stance::Walking:      anim = CharAnim::Walk; break;
                    case Stance::Sprinting:    anim = CharAnim::Run; break;
                    case Stance::CrouchStill:  anim = CharAnim::Crouch; break;
                    case Stance::CrouchMoving: anim = CharAnim::CrouchWalk; break;
                    default: break;
                }
                Vector3 look = local->body.GetLookDirection();
                float yaw = atan2f(look.x, look.z);
                // The model's eyes are 14 cm in front of its feet; shift it back so they
                // sit under the camera, plus a little more to stay out of the collar
                Vector3 fw = { sinf(yaw), 0, cosf(yaw) };
                Vector3 at = Vector3Subtract(local->body.GetFeet(), Vector3Scale(fw, 0.18f));
                Matrix toWorld = MatrixMultiply(MatrixRotateY(yaw), MatrixTranslate(at.x, at.y, at.z));
                DrawCharacter(character, anim, CharacterFrame(character, anim, feel.bodyPhase), toWorld, parts, WHITE);
            }
        };
        auto drawSoldiers = [&]() {
            if (!session) return;
            for (const SoldierView& v : session->Others()) DrawSoldier(&character, v);
            for (const SoldierView& v : session->Bots()) DrawSoldier(&character, v);
        };

        // Muzzle flashes light up their surroundings: yours, and the latest of anyone else's
        if (local) {
            Vector3 look = local->body.GetLookDirection();
            Render::SetFlashLight(0, Vector3Add(cam.position, Vector3Add(Vector3Scale(look, 0.8f), Vector3Scale(local->body.GetRight(), 0.15f))),
                                  { 7.0f, 4.5f, 1.8f }, gunPtr->IsMuzzleFlashing() ? 7.0f : 0.0f);
        } else {
            Render::SetFlashLight(0, {}, {}, 0.0f);
        }
        Render::SetFlashLight(1, {}, {}, 0.0f);
        if (session) {
            for (const auto* list : { &session->Others(), &session->Bots() })
                for (const SoldierView& v : *list)
                    if (v.alive && v.muzzleFlash) Render::SetFlashLight(1, SoldierMuzzle(&character, v), { 7.0f, 4.5f, 1.8f }, 7.0f);
        }

        // 1. Shadows from the sun
        Render::BeginShadowPass();
            Render::DrawLevel();
            drawSoldiers();
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
                for (const WorldBox& b : world) DrawWorldBox(b, materials);
            }
            drawSoldiers();
            drawPlayerBody(PART_LEGS);
            for (const SpawnPoint& s : spawns)
                Fx::DrawGroundMarker(s.position, 1.3f, s.yaw, { s.color.r, s.color.g, s.color.b, 170 });
            Fx::DrawCasings(DrawDebris);
            Render::SurfaceObject(0.25f);
            Fx::Draw(cam);
        Render::EndWorldPass();

        // 3. First-person weapon: its own pass on top of the world, lit like the
        //    spot the player stands in (darker in shadow)
        if (local && local->Alive() && overview < 0) {
            sunOnGun += ((Render::SunVisible(cam.position, world) ? 1.0f : 0.0f) - sunOnGun) * (1.0f - expf(-8.0f * dt));
            Render::SetViewModelLighting(cam, sunOnGun);
            Render::SetFlashLight(0, { 0.15f, -0.08f, -0.9f }, { 7.0f, 4.5f, 1.8f }, gunPtr->IsMuzzleFlashing() ? 1.5f : 0.0f);
            Render::SetFlashLight(1, {}, {}, 0.0f);
            viewModel->Draw(*gunPtr);
        }

        if (local && overview < 0) {
            Vector3 look = local->body.GetLookDirection();
            auto onScreen = [&](Vector3 p) {
                return Vector3DotProduct(Vector3Subtract(p, cam.position), look) > 0.0f && HasLineOfSight(cam.position, p, world);
            };
            // Bots: a slim health bar over the head once hurt
            for (const SoldierView& v : session->Bots()) {
                if (!v.alive || v.health >= v.maxHealth) continue;
                Vector3 top = Vector3Add(v.feet, { 0, 2.1f, 0 });
                if (!onScreen(top)) continue;
                Vector2 s = GetWorldToScreen(top, cam);
                const float bw = 54, bh = 5;
                DrawRectangleRec({ s.x - bw / 2 - 1, s.y - 1, bw + 2, bh + 2 }, { 0, 0, 0, 160 });
                DrawRectangleRec({ s.x - bw / 2, s.y, bw * v.health / v.maxHealth, bh }, { 232, 72, 58, 255 });
            }
            // Other players: their names
            for (const SoldierView& v : session->Others()) {
                if (!v.alive) continue;
                Vector3 top = Vector3Add(v.feet, { 0, v.eyeHeight + 0.45f, 0 });
                if (!onScreen(top)) continue;
                float dist = Vector3Distance(cam.position, top);
                Hud::Nameplate(GetWorldToScreen(top, cam), nameOf(v.id), Clamp(1.4f - dist / 40.0f, 0.35f, 1.0f));
            }
        }

        // 4. HUD and menus
        if (overview < 0) {
            if (local) {
                Weapon& gun = *gunPtr;
                Hud::State hs;
                hs.screenW = SCREEN_W;
                hs.screenH = SCREEN_H;
                hs.health = local->body.GetHealth();
                hs.helmet = local->helmet;
                hs.maxHealth = local->body.GetMaxHealth();
                hs.alive = local->Alive();
                hs.deathTimer = session->RespawnTimer();
                hs.hurt = feel.hurt;
                hs.weapon = local->current == WEAPON_RIFLE ? 0 : 1;
                hs.ammo = gun.GetAmmo();
                hs.magazine = gun.GetDef().magazineSize;
                hs.reloading = gun.IsReloading();
                hs.reloadProgress = gun.GetReloadProgress();
                hs.spreadPx = tanf(gun.GetSpread() * DEG2RAD) / tanf(local->body.GetCamera().fovy * 0.5f * DEG2RAD) * (SCREEN_H * 0.5f);
                hs.recoil = gun.GetRecoil();
                hs.aim = gun.GetAimAmount();
                hs.hitMarker = gun.GetHitMarker();
                hs.hitKilled = gun.LastHitKilled();
                hs.hitHeadshot = gun.LastHitHeadshot();
                hs.killBanner = feel.killBanner;
                hs.killBannerHeadshot = feel.killBannerHeadshot;
                for (const ScoreRow& r : scores)
                    if (r.local) { hs.kills = r.kills; hs.deaths = r.deaths; }
                hs.damage = &feel.damageMarkers;
                hs.feed = &feel.feed;
                hs.paused = paused;
                hs.showFps = showFps;
                hs.scores = &scores;
                hs.showScores = IsKeyDown(KEY_TAB);
                if (session->Mode() == SessionMode::Host) {
                    std::string addrs;
                    for (const std::string& a : session->Addresses()) addrs += (addrs.empty() ? "" : ", ") + a;
                    hs.netStatus = "HOSTING  -  port " + std::to_string(session->Port()) + (addrs.empty() ? "" : "  -  LAN " + addrs) +
                                   "  -  " + std::to_string(session->PlayerCount()) + " playing";
                } else if (session->Mode() == SessionMode::Client) {
                    int ping = 0;
                    for (const ScoreRow& r : scores) if (r.local) ping = r.ping;
                    hs.netStatus = session->StatusText() + "  -  ping " + std::to_string(ping) + " ms";
                }
                Hud::Draw(hs);
                if (paused) {
                    int click = Hud::PauseMenuClick(hs);
                    if (click == 1) { paused = false; DisableCursor(); inputGrace = 2; }
                    if (click == 2) endSession("", false);
                    if (click == 3) quit = true;
                }
            } else {
                Hud::MenuAction action = Hud::MainMenu(SCREEN_W, SCREEN_H, fields, menuStatus);
                switch (action) {
                    case Hud::MenuAction::Offline: startSession(SessionMode::Offline); break;
                    case Hud::MenuAction::Host: startSession(SessionMode::Host); break;
                    case Hud::MenuAction::Join: startSession(SessionMode::Client); break;
                    case Hud::MenuAction::Cancel: endSession("Cancelled", false); break;
                    case Hud::MenuAction::Quit: quit = true; break;
                    default: break;
                }
                if (showFps) DrawText(TextFormat("%d FPS", GetFPS()), 24, 18, 18, { 120, 230, 120, 255 });
            }
        }

        EndDrawing();
        static int frames = 0;
        if (overview >= 0 && ++frames == 30) {
            TakeScreenshot(TextFormat("overview_%d.png", overview));
            quit = true;
        }
    }

    if (session) session->Leave();
    session.reset();
    // Models share the lit shader: give them back the default before unloading
    Render::DetachLitShader(character.model);
    SetWeaponModelShader(Shader{ rlGetShaderIdDefault(), rlGetShaderLocsDefault() });
    UnloadHelmet();
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
