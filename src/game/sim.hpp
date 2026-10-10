#pragma once
#include "player.hpp"
#include "weapon.hpp"
#include "enemy.hpp"
#include "world.hpp"
#include "game/types.hpp"
#include <deque>
#include <memory>
#include <string>
#include <vector>

// The shared game: every player, the bots, shooting, damage, deaths and
// respawns. On a host (or offline) this is the authority; a client runs one
// SimPlayer of its own to predict its movement and weapon between the host's
// updates. Nothing in here draws, plays sounds or touches the network.

// One player: the existing movement (Player) and weapons, driven by commands
struct SimPlayer {
    SimPlayer(uint8_t id, const std::string& name, unsigned seed);

    uint8_t id;
    std::string name;
    Player body;
    Weapon rifle, revolver;
    int current = WEAPON_RIFLE, wanted = WEAPON_RIFLE; // in hand, and asked for (switching while they differ)
    float holster = 0.0f;      // 0 = up, 1 = lowered out of sight (switching)
    int kills = 0, deaths = 0;
    float respawnTimer = 0.0f; // while dead
    float deathClock = 0.0f;   // seconds since death
    Vector3 fallDir = { 0, 0, -1 };
    float hitFlash = 0.0f, hitReact = 0.0f;
    float animPhase = 0.0f;    // stride cycles (third-person animation)
    int ping = 0;

    // Host bookkeeping for a remote player's commands
    uint32_t lastSeq = 0;      // last command applied
    float timeBudget = 0.25f;  // seconds of commands it may still run (no faster than real time)
    std::deque<PlayerCommand> queue;
    float viewTime = 0.0f;     // the last command's view of the world (lag compensation)

    Weapon& Gun(int i) { return i == WEAPON_RIFLE ? rifle : revolver; }
    const Weapon& Gun(int i) const { return i == WEAPON_RIFLE ? rifle : revolver; }
    Weapon& Current() { return Gun(current); }
    const Weapon& Current() const { return Gun(current); }
    bool Alive() const { return body.IsAlive(); }

    // One command: look, move, switch weapons, aim, reload, fire. Rays of shots
    // fired are appended to `shots`; `dryFired` says the trigger clicked on empty.
    void Step(const PlayerCommand& cmd, const World& world, std::vector<Ray>* shots, bool* dryFired);
    void Respawn(Vector3 feet, float yaw);
    SoldierView View() const;  // how others see it
};

// The movement half of a command, in the form the player controller takes
PlayerInput InputFromCommand(const PlayerCommand& cmd);

class GameSim {
public:
    GameSim(const World& world, int bots, unsigned seed);

    // Players: ids 0..MAX_PLAYERS-1. AddPlayer returns null when the game is full.
    SimPlayer* AddPlayer(const std::string& name);
    void RemovePlayer(uint8_t id);
    SimPlayer* Find(uint8_t id);
    const SimPlayer* Find(uint8_t id) const;
    std::vector<std::unique_ptr<SimPlayer>>& Players() { return players; }
    const std::vector<std::unique_ptr<SimPlayer>>& Players() const { return players; }
    std::vector<Enemy>& Bots() { return bots; }
    const std::vector<Enemy>& Bots() const { return bots; }

    // A remote player's command: checked and queued, run on the next Update
    void QueueCommand(uint8_t id, const PlayerCommand& cmd);
    // The local player's command: run now. True if the trigger clicked on an empty gun.
    bool ApplyCommand(uint8_t id, const PlayerCommand& cmd);
    // Offline nicety: jump to the other spawn
    void SwitchSpawn(uint8_t id);

    // Queued commands, bots, respawns, timers, the hitbox history
    void Update(float dt);
    std::vector<GameEvent> TakeEvents();
    float Time() const { return time; }
    const World& GetWorld() const { return world; }

    // Tuning
    float respawnDelay = 2.5f;
    float maxRewind = 0.3f;    // seconds lag compensation will look back

private:
    struct Hitbox {
        Vector3 feet{}, eye{};
        float bodyTop = 1.45f;
        bool alive = false;
    };
    struct HistoryFrame {
        float time;
        Hitbox players[MAX_PLAYERS];
        std::vector<std::pair<Vector3, bool>> bots; // feet, alive
    };

    bool Execute(SimPlayer& p, const PlayerCommand& cmd); // true: dry fire
    void ResolveShot(SimPlayer& shooter, Ray ray, int weapon, float viewTime);
    void UpdateBots(float dt);
    void KillPlayer(SimPlayer& victim, uint8_t killer, int weapon, bool headshot, Vector3 from);
    void RespawnPlayer(SimPlayer& p);
    Hitbox CurrentHitbox(const SimPlayer& p) const;
    Hitbox HitboxAt(uint8_t id, float t) const;   // rewound for lag compensation
    Vector3 BotFeetAt(int index, float t, bool* alive) const;
    void RecordHistory();

    World world;
    std::vector<std::unique_ptr<SimPlayer>> players;
    std::vector<Enemy> bots;
    std::deque<HistoryFrame> history;
    std::vector<GameEvent> events;
    std::vector<SpawnPoint> spawns;
    float time = 0.0f;
    unsigned seed;
};

// Hitboxes of a player for bullets: a body box on the feet and a head sphere at the eyes
bool RayHitsPlayer(Ray ray, Vector3 feet, Vector3 eye, float bodyTop, float* distance, bool* headshot);
