#pragma once
#include "game/types.hpp"
#include "player.hpp"
#include "weapon.hpp"
#include <cstdint>
#include <string>
#include <vector>

// The wire format between a host and its clients. Messages start with a type
// byte. Reliable (ordered) messages: hello, welcome, reject, roster, events.
// Unreliable ones (newest wins): input, snapshot. Floats that decide movement
// are sent exactly so the host replays the client's frames bit for bit;
// presentation-only values are quantized.
namespace Proto {

constexpr uint16_t VERSION = 4;

enum MsgType : uint8_t {
    MSG_HELLO = 1,    // client -> host: version, name
    MSG_WELCOME,      // host -> client: your id, the host's clock
    MSG_REJECT,       // host -> client: why not (then disconnect)
    MSG_ROSTER,       // host -> clients: who's playing (id, name)
    MSG_INPUT,        // client -> host: the last few commands (sent again in case one is lost)
    MSG_SNAPSHOT,     // host -> client: the state of the game, and of you
    MSG_EVENTS,       // host -> clients: shots, hits, deaths, joins...
};

class Writer {
public:
    std::vector<uint8_t> data;
    void U8(uint8_t v) { data.push_back(v); }
    void U16(uint16_t v) { Raw(&v, 2); }
    void I16(int16_t v) { Raw(&v, 2); }
    void U32(uint32_t v) { Raw(&v, 4); }
    void F32(float v) { Raw(&v, 4); }
    void Vec3(Vector3 v) { F32(v.x); F32(v.y); F32(v.z); }
    void Str(const std::string& s);
    void Angle(float radians);             // 16 bits around the circle
    void Unit(float v, float max);         // 0..max in 8 bits
private:
    void Raw(const void* p, size_t n);     // little-endian machines only (x86, ARM)
};

class Reader {
public:
    Reader(const std::vector<uint8_t>& d) : p(d.data()), end(d.data() + d.size()) {}
    bool ok = true;                        // false once anything ran past the end
    uint8_t U8();
    uint16_t U16();
    int16_t I16();
    uint32_t U32();
    float F32();
    Vector3 Vec3();
    std::string Str();
    float Angle();
    float Unit(float max);
    bool Done() const { return p >= end; }
private:
    bool Raw(void* out, size_t n);
    const uint8_t* p;
    const uint8_t* end;
};

// The receiving client's own player, in full (for prediction)
struct OwnState {
    uint32_t ackSeq = 0;           // the last of its commands the host has applied
    PlayerState body;
    WeaponState weapons[2];
    uint8_t current = 0, wanted = 0;
    float holster = 0.0f;
    float respawnTimer = 0.0f;
    bool helmet = true;
};

struct Snapshot {
    float time = 0.0f;             // the host's clock
    bool hasOwn = false;
    OwnState own;
    struct PlayerEntry {
        SoldierView view;
        int kills = 0, deaths = 0, ping = 0, hill = 0;
    };
    std::vector<PlayerEntry> players;
    std::vector<SoldierView> bots;
    struct BotScore { int kills = 0, deaths = 0, hill = 0; };
    HillState hill;
    std::vector<BotScore> botScores;   // one per bot, for the scoreboard
};

struct Hello { uint16_t version = VERSION; std::string name; };
struct Welcome { uint8_t id = NO_ID; float time = 0.0f; uint8_t map = 0; };
struct RosterEntry { uint8_t id; std::string name; };

std::vector<uint8_t> EncodeHello(const Hello& h);
std::vector<uint8_t> EncodeWelcome(const Welcome& w);
std::vector<uint8_t> EncodeReject(const std::string& reason);
std::vector<uint8_t> EncodeRoster(const std::vector<RosterEntry>& roster);
std::vector<uint8_t> EncodeInput(const std::vector<PlayerCommand>& commands);
std::vector<uint8_t> EncodeSnapshot(const Snapshot& s);
std::vector<uint8_t> EncodeEvents(const std::vector<GameEvent>& events);

// Decoding: the message's type, then the matching Decode (false if malformed)
uint8_t TypeOf(const std::vector<uint8_t>& msg);
bool DecodeHello(const std::vector<uint8_t>& msg, Hello& out);
bool DecodeWelcome(const std::vector<uint8_t>& msg, Welcome& out);
bool DecodeReject(const std::vector<uint8_t>& msg, std::string& reason);
bool DecodeRoster(const std::vector<uint8_t>& msg, std::vector<RosterEntry>& out);
bool DecodeInput(const std::vector<uint8_t>& msg, std::vector<PlayerCommand>& out);
bool DecodeSnapshot(const std::vector<uint8_t>& msg, Snapshot& out);
bool DecodeEvents(const std::vector<uint8_t>& msg, std::vector<GameEvent>& out);

} // namespace Proto
