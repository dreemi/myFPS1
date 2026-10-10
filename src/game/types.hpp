#pragma once
#include "raylib.h"
#include "character.hpp"
#include <cstdint>
#include <string>

// The vocabulary shared by the game simulation, the network protocol and the
// presentation (rendering, sound, HUD). Nothing here knows how it is drawn or sent.

constexpr int MAX_PLAYERS = 8;
constexpr uint8_t NO_ID = 255;
constexpr uint8_t BOT_ID_BASE = 100;   // ids from here up are bots (BOT_ID_BASE + index)
inline std::string BotName(uint8_t id) { return "RIVAL " + std::to_string(id - BOT_ID_BASE + 1); }
constexpr int WEAPON_RIFLE = 0, WEAPON_REVOLVER = 1;

// One frame of a player's intent, as sent from a client to the host
enum CommandButton : uint16_t {
    BTN_FORWARD = 1 << 0, BTN_BACK = 1 << 1, BTN_LEFT = 1 << 2, BTN_RIGHT = 1 << 3,
    BTN_JUMP = 1 << 4,         // pressed this frame
    BTN_CROUCH = 1 << 5, BTN_SPRINT = 1 << 6, BTN_LEAN_LEFT = 1 << 7, BTN_LEAN_RIGHT = 1 << 8,
    BTN_FIRE = 1 << 9,         // held
    BTN_FIRE_PRESSED = 1 << 10,
    BTN_AIM = 1 << 11,
    BTN_RELOAD = 1 << 12,      // pressed this frame
};
struct PlayerCommand {
    uint32_t seq = 0;          // increases by one per command
    float dt = 0.0f;           // how long this command lasts (the client's frame time)
    float yaw = 0.0f, pitch = 0.0f; // where the player is looking, radians
    uint16_t buttons = 0;
    uint8_t weapon = WEAPON_RIFLE; // the weapon the player wants in hand
    float viewTime = 0.0f;     // the host's clock as the client saw the world (for lag compensation)
    bool Has(uint16_t b) const { return (buttons & b) != 0; }
};

// Something that happened in the game, for effects, sounds, the HUD and the kill feed
enum class EventType : uint8_t {
    Shot,       // a gun fired: actor, weapon, from, to, hit (what it hit), headshot, killed
    Hurt,       // target took `amount` damage from actor (at `from`)
    Killed,     // target died to actor (weapon, headshot)
    Joined,     // target joined (name in the roster)
    Left,       // target left
    Respawned,  // target respawned at `to`
};
enum class HitKind : uint8_t { None, World, Player, Bot };
enum EventFlag : uint8_t {
    EVENT_HEADSHOT = 1,
    EVENT_KILLED = 2,
    EVENT_PREDICTED = 4,       // the local client's own guess (its shot, before the host answers)
    EVENT_ECHO = 8,            // the host's answer to a shot the local client already showed
};
struct GameEvent {
    EventType type = EventType::Shot;
    uint8_t actor = NO_ID;     // who did it (player id, or BOT_ID_BASE + bot)
    uint8_t target = NO_ID;    // who it was done to
    uint8_t weapon = WEAPON_RIFLE;
    HitKind hit = HitKind::None;
    uint8_t flags = 0;
    uint8_t surface = 0;       // Surface, for impacts
    int16_t amount = 0;
    Vector3 from{}, to{}, normal{};
    bool Is(uint8_t f) const { return (flags & f) != 0; }
};

// How a character (another player, or a bot) looks this frame: enough to draw
// it in the world, play its footsteps and label it
struct SoldierView {
    uint8_t id = NO_ID;
    bool bot = false;
    Vector3 feet{};
    float yaw = 0.0f;          // radians, 0 = facing +Z
    float pitch = 0.0f;        // where it's aiming, radians
    float speed = 0.0f;        // horizontal m/s
    bool crouching = false, grounded = true;
    CharAnim anim = CharAnim::Idle;
    float animPhase = 0.0f;    // stride cycles walked
    bool alive = true;
    float deathTime = 0.0f;    // seconds since it died
    Vector3 fallDir = { 0, 0, -1 }; // which way it falls (away from the killing shot)
    float hitFlash = 0.0f, hitReact = 0.0f; // red flash and flinch, seconds left
    int weapon = WEAPON_REVOLVER;
    bool muzzleFlash = false;
    bool reloading = false;
    int health = 100, maxHealth = 100;
    float eyeHeight = 1.7f;
};

// One line of the scoreboard
struct ScoreRow {
    uint8_t id = NO_ID;
    std::string name;
    int kills = 0, deaths = 0, ping = 0;
    bool local = false, alive = true;
};
