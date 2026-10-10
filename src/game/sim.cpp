#include "game/sim.hpp"
#include "soldier.hpp"
#include "raymath.h"
#include <algorithm>
#include <cmath>

// ===========================================================================
// SimPlayer
// ===========================================================================

SimPlayer::SimPlayer(uint8_t id, const std::string& name, unsigned seed)
    : id(id), name(name), rifle(FAL, seed * 2 + 1), revolver(REVOLVER, seed * 2 + 2) {}

PlayerInput InputFromCommand(const PlayerCommand& cmd) {
    PlayerInput in;
    in.forward = cmd.Has(BTN_FORWARD);
    in.back = cmd.Has(BTN_BACK);
    in.left = cmd.Has(BTN_LEFT);
    in.right = cmd.Has(BTN_RIGHT);
    in.sprint = cmd.Has(BTN_SPRINT);
    in.crouch = cmd.Has(BTN_CROUCH);
    in.leanLeft = cmd.Has(BTN_LEAN_LEFT);
    in.leanRight = cmd.Has(BTN_LEAN_RIGHT);
    in.jump = cmd.Has(BTN_JUMP);
    in.hasAim = true;
    in.aimYaw = cmd.yaw;
    in.aimPitch = cmd.pitch;
    return in;
}

void SimPlayer::Step(const PlayerCommand& cmd, const World& world, std::vector<Ray>* shots, bool* dryFired) {
    if (!body.IsAlive()) return;
    float dt = cmd.dt;
    Vector3 before = body.GetFeet();
    body.Simulate(dt, InputFromCommand(cmd), world);
    float moved = Vector2Length({ body.GetFeet().x - before.x, body.GetFeet().z - before.z });
    animPhase += moved / SoldierStride(SoldierAnim(body.GetSpeed(), body.IsCrouching(), body.IsGrounded()));

    // Switching: the gun in hand goes down (a reload in progress is abandoned), then the other comes up
    wanted = cmd.weapon == WEAPON_REVOLVER ? WEAPON_REVOLVER : WEAPON_RIFLE;
    if (wanted != current) {
        if (Current().IsReloading()) Current().CancelReload();
        holster = std::min(1.0f, holster + dt / 0.22f);
        if (holster >= 1.0f) current = wanted;
    } else {
        holster = std::max(0.0f, holster - dt / 0.30f);
    }

    Weapon& gun = Current();
    bool busy = holster > 0.0f || wanted != current;
    if (busy) {
        gun.UpdateAim(dt, false);
        gun.Tick(dt, body.GetStance());
    } else {
        WeaponInput in;
        in.fireHeld = cmd.Has(BTN_FIRE);
        in.firePressed = cmd.Has(BTN_FIRE_PRESSED);
        in.aim = cmd.Has(BTN_AIM);
        in.reload = cmd.Has(BTN_RELOAD);
        Ray ray;
        if (gun.Step(dt, in, body.GetStance(), body.GetCamera(), &ray) && shots) shots->push_back(ray);
        if (gun.TakeDryFire() && dryFired) *dryFired = true;
    }
    body.SetZoom(gun.GetAimAmount());
}

void SimPlayer::Respawn(Vector3 feet, float yaw) {
    body.Init(feet, yaw);
    rifle.Reset();
    revolver.Reset();
    current = wanted = WEAPON_RIFLE;
    holster = 0.0f;
    respawnTimer = 0.0f;
    deathClock = 0.0f;
    hitFlash = hitReact = 0.0f;
}

SoldierView SimPlayer::View() const {
    SoldierView v;
    v.id = id;
    v.feet = body.GetFeet();
    v.yaw = body.GetYaw();
    v.pitch = body.GetPitch();
    v.speed = body.GetSpeed();
    v.crouching = body.IsCrouching();
    v.grounded = body.IsGrounded();
    v.anim = SoldierAnim(v.speed, v.crouching, v.grounded);
    v.animPhase = animPhase;
    v.alive = body.IsAlive();
    v.deathTime = deathClock;
    v.fallDir = fallDir;
    v.hitFlash = hitFlash;
    v.hitReact = hitReact;
    v.weapon = current;
    v.muzzleFlash = Current().IsMuzzleFlashing();
    v.reloading = Current().IsReloading();
    v.health = body.GetHealth();
    v.maxHealth = body.GetMaxHealth();
    v.eyeHeight = body.GetEyeHeight();
    return v;
}

// ===========================================================================
// Hitboxes
// ===========================================================================

bool RayHitsPlayer(Ray ray, Vector3 feet, Vector3 eye, float bodyTop, float* distance, bool* headshot) {
    const float hw = PLAYER_BODY_HALF_WIDTH;
    BoundingBox body = { { feet.x - hw, feet.y, feet.z - hw }, { feet.x + hw, feet.y + bodyTop, feet.z + hw } };
    RayCollision b = GetRayCollisionBox(ray, body);
    RayCollision h = GetRayCollisionSphere(ray, eye, PLAYER_HEAD_RADIUS);
    bool hit = false;
    float best = *distance;
    if (b.hit && b.distance < best) { best = b.distance; *headshot = false; hit = true; }
    if (h.hit && h.distance < best) { best = h.distance; *headshot = true; hit = true; }
    if (hit) *distance = best;
    return hit;
}

GameSim::Hitbox GameSim::CurrentHitbox(const SimPlayer& p) const {
    Hitbox h;
    h.feet = p.body.GetFeet();
    h.eye = p.body.GetCamera().position;
    h.bodyTop = p.body.GetEyeHeight() - 0.25f;
    h.alive = p.body.IsAlive();
    return h;
}

void GameSim::RecordHistory() {
    HistoryFrame f;
    f.time = time;
    for (const auto& p : players) f.players[p->id] = CurrentHitbox(*p);
    for (const Enemy& e : bots) f.bots.push_back({ e.GetFeet(), e.IsAlive() });
    history.push_back(f);
    while (!history.empty() && history.front().time < time - maxRewind - 0.2f) history.pop_front();
}

GameSim::Hitbox GameSim::HitboxAt(uint8_t id, float t) const {
    const SimPlayer* p = Find(id);
    Hitbox now = p ? CurrentHitbox(*p) : Hitbox{};
    if (history.empty() || t >= history.back().time) return now;
    // The two recorded moments either side of t
    for (size_t i = history.size(); i-- > 0;) {
        if (history[i].time <= t) {
            const HistoryFrame& a = history[i];
            const HistoryFrame& b = i + 1 < history.size() ? history[i + 1] : history[i];
            float k = b.time > a.time ? (t - a.time) / (b.time - a.time) : 0.0f;
            Hitbox h = a.players[id];
            const Hitbox& n = b.players[id];
            if (!h.alive || !n.alive) return now.alive && h.alive ? h : Hitbox{};
            h.feet = Vector3Lerp(h.feet, n.feet, k);
            h.eye = Vector3Lerp(h.eye, n.eye, k);
            h.bodyTop = Lerp(h.bodyTop, n.bodyTop, k);
            h.alive = now.alive; // can't shoot the dead, whatever the client saw
            return h;
        }
    }
    Hitbox h = history.front().players[id];
    h.alive = h.alive && now.alive;
    return h;
}

Vector3 GameSim::BotFeetAt(int index, float t, bool* alive) const {
    const Enemy& e = bots[index];
    *alive = e.IsAlive();
    if (history.empty() || t >= history.back().time) return e.GetFeet();
    for (size_t i = history.size(); i-- > 0;) {
        if (history[i].time <= t) {
            const HistoryFrame& a = history[i];
            const HistoryFrame& b = i + 1 < history.size() ? history[i + 1] : history[i];
            if (index >= (int)a.bots.size() || index >= (int)b.bots.size()) return e.GetFeet();
            float k = b.time > a.time ? (t - a.time) / (b.time - a.time) : 0.0f;
            if (!a.bots[index].second) { *alive = false; return e.GetFeet(); }
            return Vector3Lerp(a.bots[index].first, b.bots[index].first, k);
        }
    }
    return e.GetFeet();
}

// ===========================================================================
// GameSim
// ===========================================================================

GameSim::GameSim(const World& w, int botCount, unsigned s) : world(w), spawns(GetSpawnPoints()), seed(s) {
    for (int i = 0; i < botCount; i++) {
        bots.emplace_back(seed * 7919u + (unsigned)i * 104729u + 1u);
        bots.back().Spawn(spawns[i % spawns.size()].position, world);
    }
}

SimPlayer* GameSim::AddPlayer(const std::string& name) {
    uint8_t id = NO_ID;
    for (uint8_t i = 0; i < MAX_PLAYERS && id == NO_ID; i++)
        if (!Find(i)) id = i;
    if (id == NO_ID) return nullptr;
    players.push_back(std::make_unique<SimPlayer>(id, name, seed + id * 31u));
    SimPlayer& p = *players.back();
    RespawnPlayer(p);
    GameEvent e;
    e.type = EventType::Joined;
    e.target = id;
    events.push_back(e);
    return &p;
}

void GameSim::RemovePlayer(uint8_t id) {
    auto it = std::find_if(players.begin(), players.end(), [&](const auto& p) { return p->id == id; });
    if (it == players.end()) return;
    players.erase(it);
    GameEvent e;
    e.type = EventType::Left;
    e.target = id;
    events.push_back(e);
}

SimPlayer* GameSim::Find(uint8_t id) {
    for (auto& p : players) if (p->id == id) return p.get();
    return nullptr;
}

const SimPlayer* GameSim::Find(uint8_t id) const {
    for (const auto& p : players) if (p->id == id) return p.get();
    return nullptr;
}

std::vector<GameEvent> GameSim::TakeEvents() {
    std::vector<GameEvent> out;
    out.swap(events);
    return out;
}

void GameSim::QueueCommand(uint8_t id, const PlayerCommand& in) {
    SimPlayer* p = Find(id);
    if (!p || in.seq <= p->lastSeq) return;
    if (!p->queue.empty() && in.seq <= p->queue.back().seq) return; // a duplicate (commands are sent more than once)
    PlayerCommand cmd = in;
    // Don't trust it: a sane frame time and view
    if (!std::isfinite(cmd.dt) || !std::isfinite(cmd.yaw) || !std::isfinite(cmd.pitch) || !std::isfinite(cmd.viewTime)) return;
    cmd.dt = Clamp(cmd.dt, 0.0f, 0.05f);
    cmd.pitch = Clamp(cmd.pitch, -1.5f, 1.5f);
    p->queue.push_back(cmd);
    // A client running far ahead (or a flood): drop the oldest
    while (p->queue.size() > 60) {
        p->lastSeq = p->queue.front().seq;
        p->queue.pop_front();
    }
}

bool GameSim::ApplyCommand(uint8_t id, const PlayerCommand& cmd) {
    SimPlayer* p = Find(id);
    if (!p) return false;
    PlayerCommand c = cmd;
    c.viewTime = time; // the host sees the world as it is
    bool dry = Execute(*p, c);
    p->lastSeq = cmd.seq;
    return dry;
}

bool GameSim::Execute(SimPlayer& p, const PlayerCommand& cmd) {
    std::vector<Ray> shots;
    bool dry = false;
    p.viewTime = cmd.viewTime;
    p.Step(cmd, world, &shots, &dry);
    for (const Ray& r : shots) ResolveShot(p, r, p.current, cmd.viewTime);
    return dry;
}

void GameSim::SwitchSpawn(uint8_t id) {
    SimPlayer* p = Find(id);
    if (!p || !p->Alive()) return;
    float best = 1e9f;
    int pick = 0;
    for (int i = 0; i < (int)spawns.size(); i++) {
        float d = Vector3Distance(spawns[i].position, p->body.GetFeet());
        if (d < best) { best = d; pick = i; }
    }
    const SpawnPoint& s = spawns[(pick + 1) % spawns.size()];
    p->Respawn(s.position, s.yaw);
}

void GameSim::ResolveShot(SimPlayer& shooter, Ray ray, int weapon, float viewTime) {
    const WeaponDef& def = weapon == WEAPON_RIFLE ? FAL : REVOLVER;
    // See the others where the shooter saw them (within reason)
    float t = Clamp(viewTime, time - maxRewind, time);

    WorldHit wall = TraceWorld(ray, world, 150.0f);
    float best = wall.distance;
    HitKind kind = wall.hit ? HitKind::World : HitKind::None;
    uint8_t target = NO_ID;
    bool headshot = false;

    for (const auto& q : players) {
        if (q->id == shooter.id) continue;
        Hitbox h = HitboxAt(q->id, t);
        if (!h.alive) continue;
        bool head = false;
        if (RayHitsPlayer(ray, h.feet, h.eye, h.bodyTop, &best, &head)) {
            kind = HitKind::Player;
            target = q->id;
            headshot = head;
        }
    }
    for (int i = 0; i < (int)bots.size(); i++) {
        bool alive;
        Vector3 feet = BotFeetAt(i, t, &alive);
        if (!alive) continue;
        EnemyRayHit hit = bots[i].RaycastAt(ray, feet);
        if (hit.hit && hit.distance < best) {
            best = hit.distance;
            kind = HitKind::Bot;
            target = (uint8_t)(BOT_ID_BASE + i);
            headshot = hit.headshot;
        }
    }

    GameEvent shot;
    shot.type = EventType::Shot;
    shot.actor = shooter.id;
    shot.target = target;
    shot.weapon = (uint8_t)weapon;
    shot.hit = kind;
    shot.from = ray.position;
    shot.to = Vector3Add(ray.position, Vector3Scale(ray.direction, best));
    shot.normal = wall.normal;
    shot.surface = (uint8_t)wall.surface;
    if (headshot) shot.flags |= EVENT_HEADSHOT;

    int damage = headshot ? def.headDamage : def.bodyDamage;
    bool killed = false;
    if (kind == HitKind::Player) {
        SimPlayer* victim = Find(target);
        GameEvent hurt;
        hurt.type = EventType::Hurt;
        hurt.actor = shooter.id;
        hurt.target = target;
        hurt.weapon = (uint8_t)weapon;
        hurt.amount = (int16_t)damage;
        hurt.from = ray.position;
        hurt.flags = headshot ? EVENT_HEADSHOT : 0;
        victim->hitFlash = 0.15f;
        victim->hitReact = 0.18f;
        killed = victim->body.TakeDamage(damage);
        events.push_back(shot); // the shot first, then what it did
        events.push_back(hurt);
        if (killed) {
            shooter.kills++;
            KillPlayer(*victim, shooter.id, weapon, headshot, ray.position);
        }
    } else if (kind == HitKind::Bot) {
        killed = bots[target - BOT_ID_BASE].TakeHit(shooter.body.GetCamera().position, damage);
        events.push_back(shot);
        if (killed) {
            shooter.kills++;
            GameEvent k;
            k.type = EventType::Killed;
            k.actor = shooter.id;
            k.target = target;
            k.weapon = (uint8_t)weapon;
            k.flags = headshot ? EVENT_HEADSHOT : 0;
            events.push_back(k);
        }
    } else {
        events.push_back(shot);
    }
    if (killed) {
        // Mark the shot itself too, so the shooter's client can show a kill confirmation with it
        for (auto it = events.rbegin(); it != events.rend(); ++it)
            if (it->type == EventType::Shot && it->actor == shooter.id) { it->flags |= EVENT_KILLED; break; }
    }
    if (kind == HitKind::Player || kind == HitKind::Bot) shooter.Gun(weapon).RegisterHit(killed, headshot);
}

void GameSim::KillPlayer(SimPlayer& victim, uint8_t killer, int weapon, bool headshot, Vector3 from) {
    victim.deaths++;
    victim.respawnTimer = respawnDelay;
    victim.deathClock = 0.0f;
    victim.rifle.CancelReload();
    victim.revolver.CancelReload();
    Vector3 away = Vector3Subtract(victim.body.GetFeet(), from);
    away.y = 0.0f;
    if (Vector3Length(away) > 0.01f) victim.fallDir = Vector3Normalize(away);
    GameEvent k;
    k.type = EventType::Killed;
    k.actor = killer;
    k.target = victim.id;
    k.weapon = (uint8_t)weapon;
    k.flags = headshot ? EVENT_HEADSHOT : 0;
    k.from = from;
    events.push_back(k);
}

void GameSim::RespawnPlayer(SimPlayer& p) {
    // The spawn farthest from anyone who might be waiting there, then a free spot near it
    int best = 0;
    float bestScore = -1.0f;
    for (int i = 0; i < (int)spawns.size(); i++) {
        float nearest = 1e9f;
        for (const Enemy& e : bots)
            if (e.IsAlive()) nearest = std::min(nearest, Vector3Distance(e.GetFeet(), spawns[i].position));
        for (const auto& q : players)
            if (q.get() != &p && q->Alive()) nearest = std::min(nearest, Vector3Distance(q->body.GetFeet(), spawns[i].position));
        nearest += (float)GetRandomValue(0, 100) * 0.001f; // break ties
        if (nearest > bestScore) { bestScore = nearest; best = i; }
    }
    const SpawnPoint& s = spawns[best];
    static const Vector2 OFFSETS[] = { { 0, 0 }, { 1.5f, 0 }, { -1.5f, 0 }, { 0, -1.5f }, { 1.5f, -1.5f }, { -1.5f, -1.5f },
                                       { 3.0f, 0 }, { -3.0f, 0 }, { 3.0f, -1.5f }, { -3.0f, -1.5f } };
    Vector3 forward = { sinf(s.yaw), 0, cosf(s.yaw) };
    Vector3 right = { -forward.z, 0, forward.x };
    Vector3 at = s.position;
    for (const Vector2& o : OFFSETS) {
        Vector3 c = Vector3Add(s.position, Vector3Add(Vector3Scale(right, o.x), Vector3Scale(forward, o.y)));
        if (CylinderCollides(c, 0.4f, 1.8f, world)) continue;
        bool taken = false;
        for (const auto& q : players)
            if (q.get() != &p && q->Alive() && Vector3Distance(q->body.GetFeet(), c) < 1.0f) taken = true;
        if (!taken) { at = c; break; }
    }
    p.Respawn(at, s.yaw);
    GameEvent e;
    e.type = EventType::Respawned;
    e.target = p.id;
    e.to = at;
    events.push_back(e);
}

void GameSim::UpdateBots(float dt) {
    for (int i = 0; i < (int)bots.size(); i++) {
        Enemy& e = bots[i];
        // Its target: the nearest player it can see, else the nearest alive one
        SimPlayer* target = nullptr;
        float bestSeen = 1e9f, bestAny = 1e9f;
        SimPlayer* nearest = nullptr;
        Vector3 eye = Vector3Add(e.GetFeet(), { 0, 1.69f, 0 });
        for (auto& p : players) {
            if (!p->Alive()) continue;
            float d = Vector3Distance(e.GetFeet(), p->body.GetFeet());
            if (d < bestAny) { bestAny = d; nearest = p.get(); }
            if (d < bestSeen && HasLineOfSight(eye, p->body.GetCamera().position, world)) { bestSeen = d; target = p.get(); }
        }
        if (!target) target = nearest;
        EnemyTarget t;
        if (target) {
            Hitbox h = CurrentHitbox(*target);
            t.feet = h.feet;
            t.eye = h.eye;
            t.bodyTop = h.bodyTop;
            t.alive = true;
        } else {
            t.feet = spawns[0].position;
            t.eye = Vector3Add(t.feet, { 0, 1.7f, 0 });
            t.alive = false;
        }
        e.Update(dt, t, world);

        // Its bullets can hit anyone in the way
        for (const Ray& shot : e.TakeShots()) {
            WorldHit wall = TraceWorld(shot, world, 100.0f);
            float best = wall.distance;
            SimPlayer* victim = nullptr;
            bool headshot = false;
            for (auto& p : players) {
                if (!p->Alive()) continue;
                Hitbox h = CurrentHitbox(*p);
                bool head = false;
                if (RayHitsPlayer(shot, h.feet, h.eye, h.bodyTop, &best, &head)) { victim = p.get(); headshot = head; }
            }
            GameEvent s;
            s.type = EventType::Shot;
            s.actor = (uint8_t)(BOT_ID_BASE + i);
            s.weapon = WEAPON_REVOLVER;
            s.hit = victim ? HitKind::Player : (wall.hit ? HitKind::World : HitKind::None);
            s.target = victim ? victim->id : NO_ID;
            s.from = shot.position;
            s.to = Vector3Add(shot.position, Vector3Scale(shot.direction, best));
            s.normal = wall.normal;
            s.surface = (uint8_t)wall.surface;
            if (headshot) s.flags |= EVENT_HEADSHOT;
            events.push_back(s);
            if (!victim) continue;
            int damage = headshot ? REVOLVER.headDamage : REVOLVER.bodyDamage;
            GameEvent hurt;
            hurt.type = EventType::Hurt;
            hurt.actor = s.actor;
            hurt.target = victim->id;
            hurt.weapon = WEAPON_REVOLVER;
            hurt.amount = (int16_t)damage;
            hurt.from = e.GetMuzzle();
            hurt.flags = headshot ? EVENT_HEADSHOT : 0;
            events.push_back(hurt);
            victim->hitFlash = 0.15f;
            victim->hitReact = 0.18f;
            if (victim->body.TakeDamage(damage)) KillPlayer(*victim, s.actor, WEAPON_REVOLVER, headshot, e.GetMuzzle());
        }
    }
}

void GameSim::Update(float dt) {
    time += dt;

    // Remote players' commands, no faster than real time
    for (auto& p : players) {
        p->timeBudget = std::min(p->timeBudget + dt, 0.25f);
        while (!p->queue.empty() && p->queue.front().dt <= p->timeBudget + 0.002f) {
            PlayerCommand cmd = p->queue.front();
            p->queue.pop_front();
            p->timeBudget -= cmd.dt;
            Execute(*p, cmd);
            p->lastSeq = cmd.seq;
        }
    }

    UpdateBots(dt);

    for (auto& p : players) {
        p->hitFlash = std::max(0.0f, p->hitFlash - dt);
        p->hitReact = std::max(0.0f, p->hitReact - dt);
        if (!p->Alive()) {
            p->deathClock += dt;
            p->respawnTimer -= dt;
            if (p->respawnTimer <= 0.0f) RespawnPlayer(*p);
        }
    }
    RecordHistory();
}
