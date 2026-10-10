#include "game/session.hpp"
#include "net/transport.hpp"
#include "soldier.hpp"
#include "raymath.h"
#include <algorithm>
#include <cmath>

using namespace Proto;

static const float SNAPSHOT_INTERVAL = 1.0f / 30.0f;
static const float CONNECT_TIMEOUT = 6.0f;

Session::Session(SessionMode m, const World& w, const SessionConfig& cfg) : mode(m), config(cfg), world(w) {
    if (config.name.empty()) config.name = "Player";
    if (mode == SessionMode::Client) {
        transport = std::make_unique<Net::Transport>();
        if (!transport->Connect(config.address, config.port)) {
            state = SessionState::Failed;
            status = "Couldn't find " + config.address;
            return;
        }
        status = "Connecting to " + config.address + ":" + std::to_string(config.port) + "...";
        return;
    }
    sim = std::make_unique<GameSim>(world, config.bots, (unsigned)GetRandomValue(1, 1 << 30));
    local = sim->AddPlayer(config.name);
    localId = local->id;
    if (mode == SessionMode::Host) {
        transport = std::make_unique<Net::Transport>();
        if (!transport->Host(config.port, MAX_PLAYERS - 1)) {
            state = SessionState::Failed;
            status = "Couldn't open port " + std::to_string(config.port) + " (is something else using it?)";
            local = nullptr;
            return;
        }
        status = "Hosting on port " + std::to_string(config.port);
    } else {
        status = "Offline";
    }
    state = SessionState::Playing;
}

Session::~Session() { Leave(); }

void Session::Leave() {
    if (transport) transport->Close();
    if (state == SessionState::Playing || state == SessionState::Connecting) state = SessionState::Ended;
}

std::vector<std::string> Session::Addresses() const { return Net::LocalAddresses(); }

std::vector<GameEvent> Session::TakeEvents() {
    std::vector<GameEvent> out;
    out.swap(pending);
    return out;
}

float Session::RespawnTimer() const {
    if (sim && local) return local->respawnTimer;
    return respawnTimer;
}

int Session::PlayerCount() const {
    if (sim) return (int)sim->Players().size();
    return snapshots.empty() ? 1 : (int)snapshots.back().players.size();
}

std::string Session::NameOf(uint8_t id) const {
    if (id >= BOT_ID_BASE && id != NO_ID) return "RIVAL";
    if (sim) {
        if (const SimPlayer* p = sim->Find(id)) return p->name;
    } else {
        auto it = roster.find(id);
        if (it != roster.end()) return it->second;
    }
    return "?";
}

std::vector<ScoreRow> Session::Scoreboard() const {
    std::vector<ScoreRow> rows;
    if (sim) {
        for (const auto& p : sim->Players())
            rows.push_back({ p->id, p->name, p->kills, p->deaths, p->ping, p->id == localId, p->Alive() });
    } else if (!snapshots.empty()) {
        for (const Snapshot::PlayerEntry& p : snapshots.back().players)
            rows.push_back({ p.view.id, NameOf(p.view.id), p.kills, p.deaths, p.ping, p.view.id == localId, p.view.alive });
    }
    std::sort(rows.begin(), rows.end(), [](const ScoreRow& a, const ScoreRow& b) {
        return a.kills != b.kills ? a.kills > b.kills : a.deaths < b.deaths;
    });
    return rows;
}

// ---------------------------------------------------------------------------
// The local player's command
// ---------------------------------------------------------------------------

PlayerCommand Session::BuildCommand(float dt, const LocalInput& in, float viewTime) {
    PlayerCommand c;
    c.seq = ++nextSeq;
    c.dt = std::min(dt, 0.05f);
    local->body.LookAngles(in.move.look, c.yaw, c.pitch);
    const PlayerInput& m = in.move;
    if (m.forward) c.buttons |= BTN_FORWARD;
    if (m.back) c.buttons |= BTN_BACK;
    if (m.left) c.buttons |= BTN_LEFT;
    if (m.right) c.buttons |= BTN_RIGHT;
    if (m.jump) c.buttons |= BTN_JUMP;
    if (m.crouch) c.buttons |= BTN_CROUCH;
    if (m.sprint) c.buttons |= BTN_SPRINT;
    if (m.leanLeft) c.buttons |= BTN_LEAN_LEFT;
    if (m.leanRight) c.buttons |= BTN_LEAN_RIGHT;
    if (in.weapon.fireHeld) c.buttons |= BTN_FIRE;
    if (in.weapon.firePressed) c.buttons |= BTN_FIRE_PRESSED;
    if (in.weapon.aim) c.buttons |= BTN_AIM;
    if (in.weapon.reload) c.buttons |= BTN_RELOAD;
    if (in.switchTo == 0 || in.switchTo == 1) desiredWeapon = in.switchTo;
    else if (in.switchTo == 2) desiredWeapon = 1 - desiredWeapon;
    c.weapon = (uint8_t)desiredWeapon;
    c.viewTime = viewTime;
    return c;
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

void Session::Update(float dt, const LocalInput& in) {
    if (state == SessionState::Failed || state == SessionState::Ended) return;

    if (mode == SessionMode::Client) {
        ClientReceive();
        if (state == SessionState::Connecting) {
            connectTime += dt;
            if (connectTime > CONNECT_TIMEOUT) {
                state = SessionState::Failed;
                status = "Couldn't connect to " + config.address + ":" + std::to_string(config.port) + " (timed out)";
                transport->Close();
            }
            return;
        }
        if (state != SessionState::Playing) return;
        ClientPredict(dt, in);
        Interpolate(dt);
        transport->Flush();
        return;
    }

    // Host / offline: the authoritative game
    if (mode == SessionMode::Host) HostReceive();
    if (local && local->Alive()) {
        if (in.switchSpawn && mode == SessionMode::Offline) sim->SwitchSpawn(localId);
        if (sim->ApplyCommand(localId, BuildCommand(dt, in, sim->Time()))) dryFire = true;
    }
    sim->Update(dt);
    std::vector<GameEvent> ev = sim->TakeEvents();
    pending.insert(pending.end(), ev.begin(), ev.end());
    if (mode == SessionMode::Host) {
        for (size_t i = 0; i < ev.size(); i += 200) {
            std::vector<GameEvent> chunk(ev.begin() + i, ev.begin() + std::min(ev.size(), i + 200));
            transport->Broadcast(EncodeEvents(chunk), true);
        }
        HostSend(dt);
    }

    others.clear();
    for (const auto& p : sim->Players())
        if (p->id != localId) others.push_back(p->View());
    botViews.clear();
    for (int i = 0; i < (int)sim->Bots().size(); i++) {
        SoldierView v = sim->Bots()[i].View();
        v.id = (uint8_t)(BOT_ID_BASE + i);
        botViews.push_back(v);
    }
}

// ---------------------------------------------------------------------------
// Host
// ---------------------------------------------------------------------------

void Session::BroadcastRoster() {
    std::vector<RosterEntry> r;
    for (const auto& p : sim->Players()) r.push_back({ p->id, p->name });
    transport->Broadcast(EncodeRoster(r), true);
}

void Session::HostReceive() {
    std::vector<Net::TransportEvent> evs;
    transport->Poll(evs);
    for (Net::TransportEvent& e : evs) {
        auto known = peerPlayer.find(e.peer);
        if (e.type == Net::TransportEvent::Disconnected) {
            if (known != peerPlayer.end()) {
                sim->RemovePlayer(known->second);
                peerPlayer.erase(known);
                BroadcastRoster();
            }
            continue;
        }
        if (e.type != Net::TransportEvent::Received) continue; // a new connection says hello next
        uint8_t type = TypeOf(e.data);
        if (type == MSG_HELLO && known == peerPlayer.end()) {
            Hello h;
            if (!DecodeHello(e.data, h)) { transport->Disconnect(e.peer); continue; }
            if (h.version != VERSION) {
                transport->Send(e.peer, EncodeReject("The host is running a different version of the game"), true);
                transport->Disconnect(e.peer);
                continue;
            }
            std::string name = h.name.empty() ? "Player" : h.name.substr(0, 16);
            SimPlayer* p = sim->AddPlayer(name);
            if (!p) {
                transport->Send(e.peer, EncodeReject("The game is full"), true);
                transport->Disconnect(e.peer);
                continue;
            }
            peerPlayer[e.peer] = p->id;
            transport->Send(e.peer, EncodeWelcome({ p->id, sim->Time() }), true);
            BroadcastRoster();
        } else if (type == MSG_INPUT && known != peerPlayer.end()) {
            std::vector<PlayerCommand> cmds;
            if (!DecodeInput(e.data, cmds)) continue;
            for (const PlayerCommand& c : cmds) sim->QueueCommand(known->second, c);
        }
    }
}

Snapshot Session::BuildSnapshot(const SimPlayer& forPlayer) const {
    Snapshot s;
    s.time = sim->Time();
    s.hasOwn = true;
    OwnState& o = s.own;
    o.ackSeq = forPlayer.lastSeq;
    o.body = forPlayer.body.GetState();
    o.weapons[WEAPON_RIFLE] = forPlayer.rifle.GetState();
    o.weapons[WEAPON_REVOLVER] = forPlayer.revolver.GetState();
    o.current = (uint8_t)forPlayer.current;
    o.wanted = (uint8_t)forPlayer.wanted;
    o.holster = forPlayer.holster;
    o.respawnTimer = forPlayer.respawnTimer;
    for (const auto& p : sim->Players()) s.players.push_back({ p->View(), p->kills, p->deaths, p->ping });
    for (int i = 0; i < (int)sim->Bots().size(); i++) {
        SoldierView v = sim->Bots()[i].View();
        v.id = (uint8_t)i;
        s.bots.push_back(v);
    }
    return s;
}

void Session::HostSend(float dt) {
    snapshotTimer += dt;
    if (snapshotTimer < SNAPSHOT_INTERVAL) {
        transport->Flush();
        return;
    }
    snapshotTimer = fmodf(snapshotTimer, SNAPSHOT_INTERVAL);
    for (const auto& pp : peerPlayer) {
        SimPlayer* p = sim->Find(pp.second);
        if (!p) continue;
        p->ping = transport->PingMs(pp.first);
        transport->Send(pp.first, EncodeSnapshot(BuildSnapshot(*p)), false);
    }
    transport->Flush();
}

// ---------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------

void Session::ClientReceive() {
    std::vector<Net::TransportEvent> evs;
    transport->Poll(evs);
    for (Net::TransportEvent& e : evs) {
        if (e.type == Net::TransportEvent::Connected) {
            status = "Connected, joining...";
            transport->Send(0, EncodeHello({ VERSION, config.name }), true);
            continue;
        }
        if (e.type == Net::TransportEvent::Disconnected) {
            if (state == SessionState::Connecting) {
                state = SessionState::Failed;
                if (status.rfind("Rejected", 0) != 0)
                    status = "Couldn't connect to " + config.address + ":" + std::to_string(config.port);
            } else if (state == SessionState::Playing) {
                state = SessionState::Ended;
                status = "Disconnected from the host";
            }
            transport->Close();
            return;
        }
        switch (TypeOf(e.data)) {
        case MSG_WELCOME: {
            Welcome w;
            if (!DecodeWelcome(e.data, w) || state != SessionState::Connecting) break;
            localId = w.id;
            predicted = std::make_unique<SimPlayer>(localId, config.name, (unsigned)GetRandomValue(1, 1 << 30));
            local = predicted.get();
            clock = w.time;
            state = SessionState::Playing;
            status = "Connected to " + config.address + ":" + std::to_string(config.port);
            break;
        }
        case MSG_REJECT: {
            std::string reason;
            DecodeReject(e.data, reason);
            state = SessionState::Failed;
            status = "Rejected: " + reason;
            break;
        }
        case MSG_ROSTER: {
            std::vector<RosterEntry> r;
            if (!DecodeRoster(e.data, r)) break;
            roster.clear();
            for (const RosterEntry& x : r) roster[x.id] = x.name;
            break;
        }
        case MSG_SNAPSHOT: {
            Snapshot s;
            if (!DecodeSnapshot(e.data, s) || !local) break;
            if (!snapshots.empty() && s.time <= snapshots.back().time) break; // arrived out of order
            // Keep a clock close to the host's (snapshots come in steadily)
            if (!clockSynced || fabsf(s.time - clock) > 0.5f) {
                clock = s.time;
                clockSynced = true;
            } else {
                clock += (s.time - clock) * 0.1f;
            }
            if (s.hasOwn) Reconcile(s);
            snapshots.push_back(std::move(s));
            while (snapshots.size() > 40) snapshots.pop_front();
            break;
        }
        case MSG_EVENTS: {
            std::vector<GameEvent> ev;
            if (!DecodeEvents(e.data, ev) || !local) break;
            for (GameEvent& g : ev) {
                if (g.type == EventType::Shot && g.actor == localId) {
                    // The answer to a shot we've already shown: only what it hit is news
                    g.flags |= EVENT_ECHO;
                    if (g.hit == HitKind::Player || g.hit == HitKind::Bot)
                        local->Gun(g.weapon).RegisterHit(g.Is(EVENT_KILLED), g.Is(EVENT_HEADSHOT));
                }
                pending.push_back(g);
            }
            break;
        }
        default: break;
        }
    }
}

void Session::ClientPredict(float dt, const LocalInput& in) {
    clock += dt;
    float renderTime = clock - interpDelay;
    viewOffset = Vector3Scale(viewOffset, expf(-12.0f * dt));
    if (!local->Alive()) {
        respawnTimer = std::max(0.0f, respawnTimer - dt);
        return;
    }
    PlayerCommand cmd = BuildCommand(dt, in, renderTime);
    std::vector<Ray> shots;
    bool dry = false;
    local->Step(cmd, world, &shots, &dry);
    if (dry) dryFire = true;
    // Show our own shots straight away; the host decides what they hit
    for (const Ray& r : shots) {
        WorldHit h = TraceWorld(r, world, 150.0f);
        GameEvent g;
        g.type = EventType::Shot;
        g.actor = localId;
        g.weapon = (uint8_t)local->current;
        g.hit = h.hit ? HitKind::World : HitKind::None;
        g.from = r.position;
        g.to = Vector3Add(r.position, Vector3Scale(r.direction, h.distance > 149.0f ? 80.0f : h.distance));
        g.flags = EVENT_PREDICTED;
        pending.push_back(g);
    }
    unacked.push_back(cmd);
    while (unacked.size() > 120) unacked.pop_front();
    // Each packet carries the last few commands too, in case one went missing
    std::vector<PlayerCommand> send;
    for (size_t i = unacked.size() > 4 ? unacked.size() - 4 : 0; i < unacked.size(); i++) send.push_back(unacked[i]);
    transport->Send(0, EncodeInput(send), false);
}

void Session::Reconcile(const Snapshot& snap) {
    const OwnState& o = snap.own;
    bool wasAlive = local->Alive();
    Vector3 before = local->body.GetCamera().position;
    while (!unacked.empty() && unacked.front().seq <= o.ackSeq) unacked.pop_front();

    if (o.body.health > 0 && !wasAlive) local->Respawn(o.body.feet, o.body.yaw);
    // Where the host says we were, then the commands it hasn't seen yet, again
    local->body.SetState(o.body);
    if (local->Alive()) {
        for (const PlayerCommand& c : unacked) local->body.Simulate(c.dt, InputFromCommand(c), world);
    }
    Vector3 after = local->body.GetCamera().position;
    Vector3 error = Vector3Subtract(before, after);
    if (wasAlive && local->Alive() && Vector3Length(error) < 1.0f) viewOffset = Vector3Add(viewOffset, error);
    else viewOffset = { 0, 0, 0 }; // a respawn or a big correction: just go there

    respawnTimer = o.respawnTimer;
    if (!local->Alive()) {
        if (wasAlive) local->deathClock = 0.0f;
        return;
    }
    // Weapons: our predictions stand while we're still mid-action; otherwise take the host's word
    bool pendingWeaponInput = false, pendingSwitch = false;
    for (const PlayerCommand& c : unacked) {
        if (c.buttons & (BTN_FIRE | BTN_FIRE_PRESSED | BTN_RELOAD)) pendingWeaponInput = true;
        if (c.weapon != o.wanted) pendingSwitch = true;
    }
    if (!pendingWeaponInput) {
        for (int i = 0; i < 2; i++) {
            Weapon& w = local->Gun(i);
            WeaponState mine = w.GetState(), host = o.weapons[i];
            bool reloadDiffers = (mine.reloadTimer > 0.0f) != (host.reloadTimer > 0.0f) ||
                                 fabsf(mine.reloadTimer - host.reloadTimer) > 0.3f;
            if (mine.ammo != host.ammo || reloadDiffers) w.SetState(host);
        }
    }
    if (!pendingSwitch && unacked.empty()) {
        local->current = o.current;
        local->wanted = o.wanted;
        local->holster = o.holster;
    }
}

// Other players and bots, between the two snapshots either side of 100 ms ago
void Session::Interpolate(float dt) {
    others.clear();
    botViews.clear();
    if (snapshots.empty()) return;
    float t = clock - interpDelay;
    const Snapshot* a = &snapshots.front();
    const Snapshot* b = &snapshots.back();
    for (size_t i = 0; i + 1 < snapshots.size(); i++) {
        if (snapshots[i].time <= t && snapshots[i + 1].time >= t) {
            a = &snapshots[i];
            b = &snapshots[i + 1];
            break;
        }
    }
    if (t >= snapshots.back().time) a = b = &snapshots.back();
    float k = b->time > a->time ? Clamp((t - a->time) / (b->time - a->time), 0.0f, 1.0f) : 1.0f;

    auto blend = [&](const SoldierView* from, const SoldierView& to) {
        SoldierView v = to; // discrete things from the newer snapshot
        if (from && from->alive == to.alive) {
            v.feet = Vector3Lerp(from->feet, to.feet, k);
            float dy = atan2f(sinf(to.yaw - from->yaw), cosf(to.yaw - from->yaw));
            v.yaw = from->yaw + dy * k;
            v.pitch = Lerp(from->pitch, to.pitch, k);
            v.speed = Lerp(from->speed, to.speed, k);
            v.eyeHeight = Lerp(from->eyeHeight, to.eyeHeight, k);
            v.deathTime = Lerp(from->deathTime, to.deathTime, k);
            v.hitReact = Lerp(from->hitReact, to.hitReact, k);
            v.hitFlash = Lerp(from->hitFlash, to.hitFlash, k);
        }
        return v;
    };
    auto stride = [&](SoldierView& v, int key, bool bot) {
        // Walk the animation by how far it actually moved, so feet don't slide
        auto last = lastFeet.find(key);
        float moved = last == lastFeet.end() ? 0.0f : Vector2Length({ v.feet.x - last->second.x, v.feet.z - last->second.z });
        if (moved > 2.0f) moved = 0.0f; // teleported (respawn)
        lastFeet[key] = v.feet;
        if (!bot) v.anim = SoldierAnim(v.speed, v.crouching, v.grounded);
        animPhase[key] += moved / SoldierStride(v.anim);
        v.animPhase = animPhase[key];
    };
    for (const Snapshot::PlayerEntry& p : b->players) {
        if (p.view.id == localId) continue;
        const SoldierView* from = nullptr;
        for (const Snapshot::PlayerEntry& q : a->players) if (q.view.id == p.view.id) from = &q.view;
        SoldierView v = blend(from, p.view);
        stride(v, p.view.id, false);
        others.push_back(v);
    }
    for (const SoldierView& bv : b->bots) {
        const SoldierView* from = nullptr;
        for (const SoldierView& q : a->bots) if (q.id == bv.id) from = &q;
        SoldierView v = blend(from, bv);
        v.id = (uint8_t)(BOT_ID_BASE + bv.id);
        stride(v, v.id, true);
        botViews.push_back(v);
    }
    (void)dt;
}
