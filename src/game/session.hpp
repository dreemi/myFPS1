#pragma once
#include "game/sim.hpp"
#include "net/protocol.hpp"
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace Net { class Transport; }

// A game being played: offline, hosted (this machine runs the game and plays
// in it, others join), or joined (someone else's game). The rest of the
// program feeds it the local player's input once a frame and reads back what
// to show; it never needs to know which kind of session it is.
//
// Host: runs the GameSim, applies its own player's commands immediately and
// clients' commands as they arrive (validated), and sends each client a
// snapshot 30 times a second plus every event.
// Client: sends its commands, predicts its own player with the same
// simulation code, rewinds to each snapshot and replays the commands the host
// hasn't seen yet; other players and bots are shown 100 ms in the past,
// interpolated between snapshots.

enum class SessionMode { Offline, Host, Client };
enum class SessionState { Connecting, Playing, Failed, Ended };

struct SessionConfig {
    std::string name = "Player";
    std::string address = "127.0.0.1";   // joining
    uint16_t port = 27015;
    int bots = 1;                         // hosting / offline
    MapId map = MapId::Redoubt;           // hosting / offline (a client gets the host's)
};

// One frame of the local player's intent, from the keyboard and mouse
struct LocalInput {
    PlayerInput move;          // movement keys and the mouse delta
    WeaponInput weapon;
    int switchTo = -1;         // -1 no change, 0 rifle, 1 revolver, 2 the other one
    bool switchSpawn = false;  // offline only: jump to the other spawn
};

class Session {
public:
    Session(SessionMode mode, const SessionConfig& config);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    SessionMode Mode() const { return mode; }
    SessionState State() const { return state; }
    // The map being played; null until a client hears which one from the host
    const World* GetWorld() const { return haveWorld ? &world : nullptr; }
    const std::string& StatusText() const { return status; }

    void Update(float dt, const LocalInput& input);
    void Leave(); // disconnect / stop hosting

    // The local player (predicted, on a client); null until joined
    SimPlayer* Local() { return local; }
    const SimPlayer* Local() const { return local; }
    float RespawnTimer() const;

    std::vector<GameEvent> TakeEvents();           // to present (effects, sounds, HUD)
    bool TakeDryFire() { bool d = dryFire; dryFire = false; return d; } // the local trigger clicked on empty
    const std::vector<SoldierView>& Others() const { return others; } // other players, ready to draw
    const std::vector<SoldierView>& Bots() const { return botViews; }
    std::vector<ScoreRow> Scoreboard() const;
    std::string NameOf(uint8_t id) const;
    int PlayerCount() const;

    // Hosting: where friends connect
    uint16_t Port() const { return config.port; }
    std::vector<std::string> Addresses() const;
    // Client: the camera's offset that smooths over prediction corrections
    Vector3 ViewOffset() const { return viewOffset; }

private:
    PlayerCommand BuildCommand(float dt, const LocalInput& in, float viewTime);
    void HostReceive();
    void HostSend(float dt);
    void ClientReceive();
    void ClientPredict(float dt, const LocalInput& in);
    void Reconcile(const Proto::Snapshot& snap);
    void Interpolate(float dt);
    Proto::Snapshot BuildSnapshot(const SimPlayer& forPlayer) const;
    void BroadcastRoster();

    SessionMode mode;
    SessionState state = SessionState::Connecting;
    std::string status;
    SessionConfig config;
    World world;
    bool haveWorld = false;
    std::unique_ptr<Net::Transport> transport;
    std::vector<GameEvent> pending;   // events waiting to be presented
    std::vector<SoldierView> others, botViews;
    SimPlayer* local = nullptr;
    uint8_t localId = NO_ID;
    uint32_t nextSeq = 0;
    int desiredWeapon = WEAPON_RIFLE;
    bool dryFire = false;

    // Host / offline
    std::unique_ptr<GameSim> sim;
    std::map<uint32_t, uint8_t> peerPlayer;   // connection -> player id (after its hello)
    float snapshotTimer = 0.0f;

    // Client
    std::unique_ptr<SimPlayer> predicted;
    std::deque<PlayerCommand> unacked;        // sent, not yet applied by the host
    std::deque<Proto::Snapshot> snapshots;    // recent, oldest first
    std::map<uint8_t, std::string> roster;
    float clock = 0.0f;                       // estimate of the host's clock
    bool clockSynced = false;
    float connectTime = 0.0f;
    float respawnTimer = 0.0f;
    Vector3 viewOffset{};
    std::map<int, float> animPhase;           // per soldier (id), for interpolated views
    std::map<int, Vector3> lastFeet;
    float interpDelay = 0.1f;
};
