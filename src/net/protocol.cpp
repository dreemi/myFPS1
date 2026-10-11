#include "net/protocol.hpp"
#include <cmath>
#include <cstring>

namespace Proto {

// --- Writer / Reader ------------------------------------------------------------------

void Writer::Raw(const void* src, size_t n) {
    const uint8_t* b = (const uint8_t*)src;
    data.insert(data.end(), b, b + n);
}

void Writer::Str(const std::string& s) {
    size_t n = s.size() < 255 ? s.size() : 255;
    U8((uint8_t)n);
    Raw(s.data(), n);
}

void Writer::Angle(float a) {
    float turns = a / (2.0f * 3.14159265f);
    turns -= floorf(turns);
    U16((uint16_t)lroundf(turns * 65535.0f));
}

void Writer::Unit(float v, float max) {
    float k = max > 0.0f ? v / max : 0.0f;
    k = k < 0.0f ? 0.0f : (k > 1.0f ? 1.0f : k);
    U8((uint8_t)lroundf(k * 255.0f));
}

bool Reader::Raw(void* out, size_t n) {
    if (!ok || (size_t)(end - p) < n) {
        ok = false;
        memset(out, 0, n);
        return false;
    }
    memcpy(out, p, n);
    p += n;
    return true;
}

uint8_t Reader::U8() { uint8_t v; Raw(&v, 1); return v; }
uint16_t Reader::U16() { uint16_t v; Raw(&v, 2); return v; }
int16_t Reader::I16() { int16_t v; Raw(&v, 2); return v; }
uint32_t Reader::U32() { uint32_t v; Raw(&v, 4); return v; }
float Reader::F32() {
    float v;
    Raw(&v, 4);
    if (!std::isfinite(v)) { ok = false; return 0.0f; }
    return v;
}
Vector3 Reader::Vec3() { Vector3 v; v.x = F32(); v.y = F32(); v.z = F32(); return v; }
std::string Reader::Str() {
    uint8_t n = U8();
    std::string s(n, '\0');
    if (n) Raw(&s[0], n);
    return s;
}
float Reader::Angle() {
    float turns = U16() / 65535.0f;
    float a = turns * 2.0f * 3.14159265f;
    return a > 3.14159265f ? a - 2.0f * 3.14159265f : a;
}
float Reader::Unit(float max) { return U8() / 255.0f * max; }

uint8_t TypeOf(const std::vector<uint8_t>& msg) { return msg.empty() ? 0 : msg[0]; }

// --- Handshake ------------------------------------------------------------------------

std::vector<uint8_t> EncodeHello(const Hello& h) {
    Writer w;
    w.U8(MSG_HELLO);
    w.U16(h.version);
    w.Str(h.name);
    w.U8(h.playerClass);
    return w.data;
}

bool DecodeHello(const std::vector<uint8_t>& msg, Hello& out) {
    Reader r(msg);
    if (r.U8() != MSG_HELLO) return false;
    out.version = r.U16();
    out.name = r.Str();
    out.playerClass = r.U8() == CLASS_SNIPER ? CLASS_SNIPER : CLASS_STANDARD;
    return r.ok;
}

std::vector<uint8_t> EncodeWelcome(const Welcome& wel) {
    Writer w;
    w.U8(MSG_WELCOME);
    w.U8(wel.id);
    w.F32(wel.time);
    w.U8(wel.map);
    return w.data;
}

bool DecodeWelcome(const std::vector<uint8_t>& msg, Welcome& out) {
    Reader r(msg);
    if (r.U8() != MSG_WELCOME) return false;
    out.id = r.U8();
    out.time = r.F32();
    out.map = r.U8();
    return r.ok && out.id < MAX_PLAYERS && out.map < (uint8_t)MapId::Count;
}

std::vector<uint8_t> EncodeReject(const std::string& reason) {
    Writer w;
    w.U8(MSG_REJECT);
    w.Str(reason);
    return w.data;
}

bool DecodeReject(const std::vector<uint8_t>& msg, std::string& reason) {
    Reader r(msg);
    if (r.U8() != MSG_REJECT) return false;
    reason = r.Str();
    return r.ok;
}

std::vector<uint8_t> EncodeRoster(const std::vector<RosterEntry>& roster) {
    Writer w;
    w.U8(MSG_ROSTER);
    w.U8((uint8_t)roster.size());
    for (const RosterEntry& e : roster) {
        w.U8(e.id);
        w.Str(e.name);
    }
    return w.data;
}

bool DecodeRoster(const std::vector<uint8_t>& msg, std::vector<RosterEntry>& out) {
    Reader r(msg);
    if (r.U8() != MSG_ROSTER) return false;
    int n = r.U8();
    out.clear();
    for (int i = 0; i < n && r.ok; i++) {
        RosterEntry e;
        e.id = r.U8();
        e.name = r.Str();
        out.push_back(e);
    }
    return r.ok;
}

// --- Input ------------------------------------------------------------------------------

std::vector<uint8_t> EncodeInput(const std::vector<PlayerCommand>& commands) {
    Writer w;
    w.U8(MSG_INPUT);
    w.U8((uint8_t)commands.size());
    for (const PlayerCommand& c : commands) {
        w.U32(c.seq);
        w.F32(c.dt);
        w.F32(c.yaw);
        w.F32(c.pitch);
        w.U16(c.buttons);
        w.U8(c.weapon);
        w.F32(c.viewTime);
    }
    return w.data;
}

bool DecodeInput(const std::vector<uint8_t>& msg, std::vector<PlayerCommand>& out) {
    Reader r(msg);
    if (r.U8() != MSG_INPUT) return false;
    int n = r.U8();
    if (n > 16) return false;
    out.clear();
    for (int i = 0; i < n && r.ok; i++) {
        PlayerCommand c;
        c.seq = r.U32();
        c.dt = r.F32();
        c.yaw = r.F32();
        c.pitch = r.F32();
        c.buttons = r.U16();
        c.weapon = r.U8();
        c.viewTime = r.F32();
        out.push_back(c);
    }
    return r.ok;
}

// --- Snapshot ---------------------------------------------------------------------------

enum SoldierFlag : uint8_t {
    SF_ALIVE = 1, SF_CROUCH = 2, SF_GROUNDED = 4, SF_RELOADING = 8, SF_MUZZLE = 16, SF_RIFLE = 32, SF_HELMET = 64, SF_SNIPER = 128,
};

static void WriteSoldier(Writer& w, const SoldierView& v) {
    uint8_t f = (v.alive ? SF_ALIVE : 0) | (v.crouching ? SF_CROUCH : 0) | (v.grounded ? SF_GROUNDED : 0) |
                (v.reloading ? SF_RELOADING : 0) | (v.muzzleFlash ? SF_MUZZLE : 0) | (v.weapon == WEAPON_RIFLE ? SF_RIFLE : 0) |
                (v.helmet ? SF_HELMET : 0) | (v.weapon == WEAPON_SNIPER ? SF_SNIPER : 0);
    w.U8(v.id);
    w.U8(f);
    w.Vec3(v.feet);
    w.Angle(v.yaw);
    w.Angle(v.pitch);
    w.Unit(v.speed, 12.0f);
    w.U8((uint8_t)v.anim);
    w.Unit(v.eyeHeight, 2.0f);
    w.U8((uint8_t)(v.health < 0 ? 0 : (v.health > 255 ? 255 : v.health)));
    w.Unit(v.deathTime, 5.0f);
    w.Angle(atan2f(v.fallDir.x, v.fallDir.z));
    w.Unit(v.hitFlash, 0.5f);
    w.Unit(v.hitReact, 0.5f);
}

static SoldierView ReadSoldier(Reader& r, bool bot) {
    SoldierView v;
    v.bot = bot;
    v.id = r.U8();
    uint8_t f = r.U8();
    v.alive = f & SF_ALIVE;
    v.crouching = f & SF_CROUCH;
    v.grounded = f & SF_GROUNDED;
    v.reloading = f & SF_RELOADING;
    v.muzzleFlash = f & SF_MUZZLE;
    v.weapon = (f & SF_SNIPER) ? WEAPON_SNIPER : ((f & SF_RIFLE) ? WEAPON_RIFLE : WEAPON_REVOLVER);
    v.helmet = f & SF_HELMET;
    v.feet = r.Vec3();
    v.yaw = r.Angle();
    v.pitch = r.Angle();
    v.speed = r.Unit(12.0f);
    uint8_t anim = r.U8();
    v.anim = anim < (uint8_t)CharAnim::Count ? (CharAnim)anim : CharAnim::Idle;
    v.eyeHeight = r.Unit(2.0f);
    v.health = r.U8();
    v.deathTime = r.Unit(5.0f);
    float fall = r.Angle();
    v.fallDir = { sinf(fall), 0.0f, cosf(fall) };
    v.hitFlash = r.Unit(0.5f);
    v.hitReact = r.Unit(0.5f);
    return v;
}

static void WriteOwn(Writer& w, const OwnState& o) {
    const PlayerState& b = o.body;
    w.U32(o.ackSeq);
    w.Vec3(b.feet);
    w.F32(b.yaw);
    w.F32(b.pitch);
    w.Vec3(b.velocity);
    w.F32(b.velocityY);
    w.F32(b.airTickTimer);
    w.U8((uint8_t)((b.grounded ? 1 : 0) | (b.crouching ? 2 : 0)));
    w.F32(b.leanAmount);
    w.F32(b.eyeHeight);
    w.I16((int16_t)b.health);
    w.U8((uint8_t)b.stance);
    for (const WeaponState& ws : o.weapons) {
        w.U8((uint8_t)ws.ammo);
        w.F32(ws.reloadTimer);
        w.F32(ws.reloadDuration);
        w.U8(ws.emptyReload ? 1 : 0);
    }
    w.U8(o.current);
    w.U8(o.wanted);
    w.F32(o.holster);
    w.F32(o.respawnTimer);
    w.U8(o.helmet ? 1 : 0);
}

static void ReadOwn(Reader& r, OwnState& o) {
    PlayerState& b = o.body;
    o.ackSeq = r.U32();
    b.feet = r.Vec3();
    b.yaw = r.F32();
    b.pitch = r.F32();
    b.velocity = r.Vec3();
    b.velocityY = r.F32();
    b.airTickTimer = r.F32();
    uint8_t f = r.U8();
    b.grounded = f & 1;
    b.crouching = f & 2;
    b.leanAmount = r.F32();
    b.eyeHeight = r.F32();
    b.health = r.I16();
    uint8_t stance = r.U8();
    b.stance = stance <= (uint8_t)Stance::Airborne ? (Stance)stance : Stance::Still;
    for (WeaponState& ws : o.weapons) {
        ws.ammo = r.U8();
        ws.reloadTimer = r.F32();
        ws.reloadDuration = r.F32();
        ws.emptyReload = r.U8() != 0;
    }
    o.current = r.U8() == WEAPON_REVOLVER ? WEAPON_REVOLVER : WEAPON_RIFLE;
    o.wanted = r.U8() == WEAPON_REVOLVER ? WEAPON_REVOLVER : WEAPON_RIFLE;
    o.holster = r.F32();
    o.respawnTimer = r.F32();
    o.helmet = r.U8() != 0;
}

std::vector<uint8_t> EncodeSnapshot(const Snapshot& s) {
    Writer w;
    w.U8(MSG_SNAPSHOT);
    w.F32(s.time);
    w.U8(s.hasOwn ? 1 : 0);
    if (s.hasOwn) WriteOwn(w, s.own);
    w.U8((uint8_t)s.players.size());
    for (const Snapshot::PlayerEntry& p : s.players) {
        WriteSoldier(w, p.view);
        w.I16((int16_t)p.kills);
        w.I16((int16_t)p.deaths);
        w.U16((uint16_t)(p.ping < 0 ? 0 : (p.ping > 65535 ? 65535 : p.ping)));
        w.U16((uint16_t)(p.hill < 0 ? 0 : (p.hill > 65535 ? 65535 : p.hill)));
    }
    w.U8((uint8_t)s.bots.size());
    for (const SoldierView& b : s.bots) WriteSoldier(w, b);
    for (size_t i = 0; i < s.bots.size(); i++) {
        Snapshot::BotScore sc = i < s.botScores.size() ? s.botScores[i] : Snapshot::BotScore{};
        w.I16((int16_t)sc.kills);
        w.I16((int16_t)sc.deaths);
        w.U16((uint16_t)(sc.hill < 0 ? 0 : (sc.hill > 65535 ? 65535 : sc.hill)));
    }
    w.U8((uint8_t)((s.hill.active ? 1 : 0) | (s.hill.contested ? 2 : 0)));
    w.U8(s.hill.holder);
    return w.data;
}

bool DecodeSnapshot(const std::vector<uint8_t>& msg, Snapshot& out) {
    Reader r(msg);
    if (r.U8() != MSG_SNAPSHOT) return false;
    out = Snapshot{};
    out.time = r.F32();
    out.hasOwn = r.U8() != 0;
    if (out.hasOwn) ReadOwn(r, out.own);
    int n = r.U8();
    if (n > MAX_PLAYERS) return false;
    for (int i = 0; i < n && r.ok; i++) {
        Snapshot::PlayerEntry p;
        p.view = ReadSoldier(r, false);
        p.kills = r.I16();
        p.deaths = r.I16();
        p.ping = r.U16();
        p.hill = r.U16();
        out.players.push_back(p);
    }
    int b = r.U8();
    for (int i = 0; i < b && r.ok; i++) out.bots.push_back(ReadSoldier(r, true));
    for (int i = 0; i < b && r.ok; i++) {
        Snapshot::BotScore sc;
        sc.kills = r.I16();
        sc.deaths = r.I16();
        sc.hill = r.U16();
        out.botScores.push_back(sc);
    }
    uint8_t hf = r.U8();
    out.hill.active = hf & 1;
    out.hill.contested = hf & 2;
    out.hill.holder = r.U8();
    return r.ok;
}

// --- Events -----------------------------------------------------------------------------

std::vector<uint8_t> EncodeEvents(const std::vector<GameEvent>& events) {
    Writer w;
    w.U8(MSG_EVENTS);
    w.U8((uint8_t)events.size());
    for (const GameEvent& e : events) {
        w.U8((uint8_t)e.type);
        w.U8(e.actor);
        w.U8(e.target);
        w.U8(e.weapon);
        w.U8((uint8_t)e.hit);
        w.U8(e.flags);
        w.U8(e.surface);
        w.I16(e.amount);
        w.Vec3(e.from);
        w.Vec3(e.to);
        w.Vec3(e.normal);
    }
    return w.data;
}

bool DecodeEvents(const std::vector<uint8_t>& msg, std::vector<GameEvent>& out) {
    Reader r(msg);
    if (r.U8() != MSG_EVENTS) return false;
    int n = r.U8();
    out.clear();
    for (int i = 0; i < n && r.ok; i++) {
        GameEvent e;
        uint8_t type = r.U8();
        if (type > (uint8_t)EventType::HelmetOff) return false;
        e.type = (EventType)type;
        e.actor = r.U8();
        e.target = r.U8();
        uint8_t weapon = r.U8();
        e.weapon = weapon == WEAPON_REVOLVER || weapon == WEAPON_SNIPER ? weapon : WEAPON_RIFLE;
        uint8_t hit = r.U8();
        e.hit = hit <= (uint8_t)HitKind::Bot ? (HitKind)hit : HitKind::None;
        e.flags = r.U8();
        e.surface = r.U8();
        e.amount = r.I16();
        e.from = r.Vec3();
        e.to = r.Vec3();
        e.normal = r.Vec3();
        out.push_back(e);
    }
    return r.ok;
}

} // namespace Proto
