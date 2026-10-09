#pragma once
#include "raylib.h"
#include <string>
#include <vector>

// Everything drawn on top of the 3D view: crosshair, hit markers, health,
// ammo, kill feed, damage indicators, and the start / pause / death screens.
namespace Hud {

struct DamageMarker {
    float angle;   // where the shot came from, relative to where you face (radians, 0 = ahead, + = right)
    float life;    // seconds left (fades out)
};

struct FeedEntry {
    std::string text;
    Color color;
    float life;
};

struct State {
    int screenW = 1280, screenH = 720;
    // Player
    int health = 100, maxHealth = 100;
    bool alive = true;
    float deathTimer = 0.0f;      // seconds until respawn
    float hurt = 0.0f;            // recent damage (0..1), red screen edges
    // Weapon
    int weapon = 1;               // 0 = rifle (magazine panel), 1 = revolver (cylinder panel)
    const char* weaponNames[2] = { "FAL", "REVOLVER" };
    int ammo = 6, magazine = 6;
    bool reloading = false;
    float reloadProgress = 0.0f;
    float spreadPx = 0.0f;        // crosshair gap from the spread cone
    float recoil = 0.0f;
    float aim = 0.0f;             // aiming down sights hides the crosshair
    float hitMarker = 0.0f;       // seconds left
    bool hitKilled = false, hitHeadshot = false;
    float killBanner = 0.0f;      // seconds left on "ELIMINATED"
    bool killBannerHeadshot = false;
    // Score
    int kills = 0, deaths = 0;
    const std::vector<DamageMarker>* damage = nullptr;
    const std::vector<FeedEntry>* feed = nullptr;
    // Screens
    bool started = true;          // false: title screen
    bool paused = false;
    bool showFps = false;
};

bool Init();
void Shutdown();
void Draw(const State& s);

// Pause menu buttons (screen space); returns 0 none, 1 resume, 2 quit
int PauseMenuClick(const State& s);

Font UiFont();

} // namespace Hud
