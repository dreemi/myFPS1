#pragma once
#include "raylib.h"
#include "game/types.hpp"
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
    bool helmet = true;
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
    // Multiplayer
    std::string netStatus;        // top-left line: hosting / connected / offline
    const std::vector<ScoreRow>* scores = nullptr;
    bool showScores = false;      // scoreboard (held key)
};

bool Init();
void Shutdown();
void Draw(const State& s);

// Pause menu buttons (screen space); returns 0 none, 1 resume, 2 leave the game, 3 quit to desktop
int PauseMenuClick(const State& s);

// A name over another player's head (screen position), fading with distance
void Nameplate(Vector2 at, const std::string& name, float alpha);

// The main menu: play offline, host a game or join one. Draws and handles the
// mouse and keyboard itself; the fields are edited in place.
struct MenuFields {
    std::string name = "Player";
    std::string address = "127.0.0.1";
    std::string port = "27015";
    int bots = 1;
    int focus = -1;               // which text field has the keyboard
};
enum class MenuAction { None, Offline, Host, Join, Cancel, Quit };
struct MenuStatus {
    std::string text;             // what's happening (or what went wrong)
    bool busy = false;            // connecting: offer Cancel instead of the buttons
    bool error = false;
};
MenuAction MainMenu(int screenW, int screenH, MenuFields& fields, const MenuStatus& status);

Font UiFont();

} // namespace Hud
