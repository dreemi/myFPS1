// Multiplayer tests: the wire format, the authoritative simulation, and a real
// host and client talking over loopback UDP. No window needed.
#include "../src/game/session.hpp"
#include "../src/game/sim.hpp"
#include "../src/net/protocol.hpp"
#include "raymath.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

static int failures = 0;

#define CHECK(cond, ...)                                   \
    do {                                                   \
        if (cond) { std::printf("  pass: "); }             \
        else { std::printf("  FAIL: "); failures++; }      \
        std::printf(__VA_ARGS__);                          \
        std::printf("\n");                                 \
    } while (0)

static const float DT = 1.0f / 60.0f;

static PlayerCommand Command(uint32_t seq, float yaw, float pitch, uint16_t buttons, float viewTime = 0.0f) {
    PlayerCommand c;
    c.seq = seq;
    c.dt = DT;
    c.yaw = yaw;
    c.pitch = pitch;
    c.buttons = buttons;
    c.weapon = WEAPON_RIFLE;
    c.viewTime = viewTime;
    return c;
}

// Yaw and pitch that look from one point at another, in the player's convention
static void LookAt(const SimPlayer& p, Vector3 target, float& yaw, float& pitch) {
    Camera3D cam = p.body.GetCamera();
    Vector3 d = Vector3Normalize(Vector3Subtract(target, cam.position));
    // Find the yaw that the player maps to this direction by search (keeps the test
    // independent of the controller's angle convention)
    float bestYaw = 0, best = -2;
    for (int i = 0; i < 3600; i++) {
        float y = i * (2 * PI / 3600);
        Player probe = p.body;
        PlayerInput in;
        in.hasAim = true;
        in.aimYaw = y;
        in.aimPitch = 0;
        PlayerState s = probe.GetState();
        s.yaw = y;
        s.pitch = 0;
        probe.SetState(s);
        Vector3 f = probe.GetLookDirection();
        float dot = f.x * d.x + f.z * d.z;
        if (dot > best) { best = dot; bestYaw = y; }
    }
    yaw = bestYaw;
    float bestPitch = 0;
    best = -2;
    for (int i = -890; i <= 890; i++) {
        float pt = i * 0.1f * DEG2RAD;
        Player probe = p.body;
        PlayerState s = probe.GetState();
        s.yaw = yaw;
        s.pitch = pt;
        probe.SetState(s);
        float dot = Vector3DotProduct(probe.GetLookDirection(), d);
        if (dot > best) { best = dot; bestPitch = pt; }
    }
    pitch = bestPitch;
}

static void Pump(Session& a, Session* b, float seconds) {
    int frames = (int)(seconds / DT);
    for (int i = 0; i < frames; i++) {
        a.Update(DT, LocalInput{});
        if (b) b->Update(DT, LocalInput{});
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
}

int main() {
    const World world = BuildTestWorld();

    std::printf("Protocol\n");
    {
        std::vector<PlayerCommand> cmds = { Command(7, 1.25f, -0.3f, BTN_FORWARD | BTN_FIRE, 3.5f), Command(8, 1.3f, -0.2f, BTN_JUMP) };
        std::vector<PlayerCommand> back;
        bool ok = Proto::DecodeInput(Proto::EncodeInput(cmds), back);
        CHECK(ok && back.size() == 2 && back[0].seq == 7 && back[1].buttons == BTN_JUMP, "input round-trips");
        CHECK(ok && fabsf(back[0].yaw - 1.25f) < 1e-3f && fabsf(back[0].viewTime - 3.5f) < 1e-4f, "angles and view time survive");

        std::vector<uint8_t> msg = Proto::EncodeInput(cmds);
        msg.resize(msg.size() / 2);
        CHECK(!Proto::DecodeInput(msg, back), "a truncated message is rejected");
        std::vector<uint8_t> junk = { Proto::MSG_INPUT, 255, 1, 2 };
        CHECK(!Proto::DecodeInput(junk, back), "junk is rejected");

        Proto::Hello h;
        h.name = "Ann";
        Proto::Hello hb;
        CHECK(Proto::DecodeHello(Proto::EncodeHello(h), hb) && hb.name == "Ann" && hb.version == Proto::VERSION, "hello round-trips");

        GameEvent e;
        e.type = EventType::Killed;
        e.actor = 2;
        e.target = BOT_ID_BASE;
        e.flags = EVENT_HEADSHOT;
        e.from = { 1, 2, 3 };
        std::vector<GameEvent> eb;
        CHECK(Proto::DecodeEvents(Proto::EncodeEvents({ e }), eb) && eb.size() == 1 && eb[0].type == EventType::Killed &&
                  eb[0].target == BOT_ID_BASE && eb[0].Is(EVENT_HEADSHOT) && Vector3Distance(eb[0].from, e.from) < 0.01f,
              "events round-trip");
    }

    std::printf("Authoritative simulation\n");
    {
        GameSim sim(world, 0, 1);
        SimPlayer* a = sim.AddPlayer("A");
        SimPlayer* b = sim.AddPlayer("B");
        CHECK(a && b && a->id != b->id, "two players join");
        for (int i = 0; i < MAX_PLAYERS - 2; i++) sim.AddPlayer("X");
        CHECK(sim.AddPlayer("full") == nullptr, "a full game refuses more players");
        for (int id = 2; id < MAX_PLAYERS; id++) sim.RemovePlayer((uint8_t)id);
        CHECK(sim.Players().size() == 2, "players leave cleanly");

        // Face each other in the open and fire: B dies, A scores, B respawns with full ammo
        PlayerState sa = a->body.GetState(), sb = b->body.GetState();
        sa.feet = { 0, 0, -6 };
        sb.feet = { 0, 0, 6 };
        a->body.SetState(sa);
        b->body.SetState(sb);
        b->rifle.SetState({ 3, 0.0f, 0.0f, false });
        sim.Update(DT);
        float yaw, pitch;
        LookAt(*a, Vector3Add(b->body.GetFeet(), { 0, 1.0f, 0 }), yaw, pitch);
        uint32_t seq = 1;
        bool killed = false;
        for (int i = 0; i < 120 && !killed; i++) {
            sim.ApplyCommand(a->id, Command(seq++, yaw, pitch, BTN_FIRE | (i == 0 ? BTN_FIRE_PRESSED : 0), sim.Time()));
            sim.Update(DT);
            for (const GameEvent& e : sim.TakeEvents())
                if (e.type == EventType::Killed && e.target == b->id && e.actor == a->id) killed = true;
        }
        CHECK(killed, "the host resolves hits and kills");
        CHECK(a->kills == 1 && b->deaths == 1, "kills and deaths are counted (%d, %d)", a->kills, b->deaths);
        for (int i = 0; i < 300 && !b->Alive(); i++) sim.Update(DT);
        CHECK(b->Alive() && b->body.GetHealth() == b->body.GetMaxHealth(), "the victim respawns at full health");
        CHECK(b->rifle.GetAmmo() == b->rifle.GetDef().magazineSize, "and with a full magazine");

        // A client can't move faster than real time by sending extra commands
        Vector3 start = b->body.GetFeet();
        // (10 s of sprinting sent at once, over one second of real time)
        for (int i = 0; i < 600; i++) sim.QueueCommand(b->id, Command(1000 + i, 0, 0, BTN_FORWARD | BTN_SPRINT));
        for (int i = 0; i < 60; i++) sim.Update(DT);
        float moved = Vector3Distance(start, b->body.GetFeet());
        CHECK(moved < 9.0f, "a flood of commands is limited to real time (moved %.2f m in 1 s)", moved);

        // Nonsense input is dropped rather than trusted
        PlayerCommand bad = Command(5000, NAN, 0, BTN_FORWARD);
        Vector3 before = b->body.GetFeet();
        sim.QueueCommand(b->id, bad);
        PlayerCommand huge = Command(5001, 0, 0, BTN_FORWARD);
        huge.dt = 10.0f;
        sim.QueueCommand(b->id, huge);
        sim.Update(DT);
        moved = Vector3Distance(before, b->body.GetFeet());
        CHECK(std::isfinite(b->body.GetFeet().x) && moved < 0.5f, "NaN and oversized commands do no harm (moved %.2f m)", moved);
    }

    std::printf("Sniper class\n");
    {
        GameSim sim(world, 0, 1);
        SimPlayer* a = sim.AddPlayer("A", CLASS_SNIPER);
        SimPlayer* b = sim.AddPlayer("B");
        CHECK(a->PrimaryWeapon() == WEAPON_SNIPER && &a->rifle.GetDef() == &L42A1 && a->rifle.GetAmmo() == 10,
              "a sniper carries the L42A1, ten rounds");
        CHECK(b->PrimaryWeapon() == WEAPON_RIFLE && &b->rifle.GetDef() == &FAL, "the standard class keeps the FAL");
        CHECK(a->View().weapon == WEAPON_SNIPER, "and others see the L42A1 in a sniper's hands");

        PlayerState sa = a->body.GetState(), sb = b->body.GetState();
        sa.feet = { 0, 0, -9 };
        sb.feet = { 0, 0, 9 };
        a->body.SetState(sa);
        b->body.SetState(sb);
        sim.Update(DT);
        float yaw, pitch;
        LookAt(*a, Vector3Add(b->body.GetFeet(), { 0, 1.0f, 0 }), yaw, pitch);
        uint32_t seq = 1;
        // Raise the telescope: the view narrows to its field
        for (int i = 0; i < 40; i++) { sim.ApplyCommand(a->id, Command(seq++, yaw, pitch, BTN_AIM, sim.Time())); sim.Update(DT); }
        CHECK(a->rifle.IsScoped() && fabsf(a->body.GetCamera().fovy - L42A1.scopeFovy) < 0.01f,
              "aimed, it's through the telescope (field of view %.1f)", a->body.GetCamera().fovy);
        // A shot to the body takes 75; holding the trigger doesn't fire again until the bolt's worked
        int shots = 0, kills = 0;
        bool sniperShot = false;
        float aimDuringBolt = 1.0f;
        for (int i = 0; i < 60; i++) {
            uint16_t buttons = BTN_AIM | BTN_FIRE | (i == 0 ? BTN_FIRE_PRESSED : 0) | (i == 10 ? BTN_RELOAD : 0);
            sim.ApplyCommand(a->id, Command(seq++, yaw, pitch, buttons, sim.Time()));
            sim.Update(DT);
            if (i == 40) aimDuringBolt = a->rifle.GetAimAmount();
            for (const GameEvent& e : sim.TakeEvents()) {
                if (e.type == EventType::Shot && e.actor == a->id) { shots++; sniperShot = e.weapon == WEAPON_SNIPER; }
                if (e.type == EventType::Killed && e.actor == a->id && e.target == b->id && e.weapon == WEAPON_SNIPER) kills++;
            }
        }
        CHECK(kills == 0 && b->body.GetHealth() == 25, "a body shot does 75 (health left %d)", b->body.GetHealth());
        CHECK(shots == 1 && sniperShot, "a second shot waits for the bolt (%d shot%s in a second)", shots, shots == 1 ? "" : "s");
        CHECK(aimDuringBolt < 0.1f, "working the bolt takes the eye off the telescope (aim %.2f)", aimDuringBolt);
        CHECK(!a->rifle.IsReloading(), "a reload asked for mid-bolt waits...");
        for (int i = 0; i < 20; i++) { sim.ApplyCommand(a->id, Command(seq++, yaw, pitch, 0, sim.Time())); sim.Update(DT); }
        CHECK(a->rifle.IsReloading(), "...and starts once the bolt's closed");

        // A shot to the head kills, even from behind through a helmet (which stays on: it didn't stop it)
        {
            GameSim hs(world, 0, 2);
            SimPlayer* s = hs.AddPlayer("S", CLASS_SNIPER);
            SimPlayer* v = hs.AddPlayer("V");
            PlayerState ss = s->body.GetState(), vs = v->body.GetState();
            ss.feet = { 0, 0, -9 };
            vs.feet = { 0, 0, 9 };
            s->body.SetState(ss);
            for (float yaw0 : { 0.0f, PI / 2, PI, -PI / 2 }) {   // V faces away from S (down +Z)
                vs.yaw = yaw0;
                v->body.SetState(vs);
                if (v->body.GetLookDirection().z > 0.9f) break;
            }
            hs.Update(DT);
            float hy, hp;
            LookAt(*s, v->body.GetCamera().position, hy, hp);
            uint32_t q = 1;
            for (int i = 0; i < 40; i++) { hs.ApplyCommand(s->id, Command(q++, hy, hp, BTN_AIM, hs.Time())); hs.Update(DT); }
            bool headKill = false, helmetOff = false;
            for (int i = 0; i < 5; i++) {
                hs.ApplyCommand(s->id, Command(q++, hy, hp, BTN_AIM | (i == 0 ? BTN_FIRE | BTN_FIRE_PRESSED : 0), hs.Time()));
                hs.Update(DT);
                for (const GameEvent& e : hs.TakeEvents()) {
                    if (e.type == EventType::Killed && e.target == v->id && e.Is(EVENT_HEADSHOT)) headKill = true;
                    if (e.type == EventType::HelmetOff) helmetOff = true;
                }
            }
            CHECK(headKill && !helmetOff, "a head shot from behind kills straight through the helmet");
        }

        // On the wire: the class in the hello, the rifle in snapshots and events
        Proto::Hello h{ Proto::VERSION, "S", CLASS_SNIPER }, hb;
        CHECK(Proto::DecodeHello(Proto::EncodeHello(h), hb) && hb.playerClass == CLASS_SNIPER, "the class travels in the hello");
        Proto::Snapshot snap;
        snap.players.push_back({ a->View(), 0, 0, 0, 0 });
        Proto::Snapshot back;
        CHECK(Proto::DecodeSnapshot(Proto::EncodeSnapshot(snap), back) && back.players[0].view.weapon == WEAPON_SNIPER,
              "and the rifle in snapshots");
        GameEvent ev;
        ev.type = EventType::Shot;
        ev.weapon = WEAPON_SNIPER;
        std::vector<GameEvent> evs;
        CHECK(Proto::DecodeEvents(Proto::EncodeEvents({ ev }), evs) && evs.size() == 1 && evs[0].weapon == WEAPON_SNIPER,
              "and in events");
    }

    std::printf("Helmets\n");
    {
        CHECK(ShotFromBehind({ 0, 0, 1 }, { 0, 0, 1 }), "a shot travelling the way the head faces comes from behind");
        CHECK(!ShotFromBehind({ 0, 0, -1 }, { 0, 0, 1 }) && !ShotFromBehind({ 1, 0, 0 }, { 0, 0, 1 }),
              "from the front or the side it doesn't");

        GameSim sim(world, 0, 5);
        SimPlayer* a = sim.AddPlayer("A");
        SimPlayer* b = sim.AddPlayer("B");
        // Faces a player along a direction (searching the controller's own yaw convention)
        auto face = [](SimPlayer* p, Vector3 dir) {
            PlayerState st = p->body.GetState();
            float bestYaw = 0, best = -2;
            for (int k = 0; k < 720; k++) {
                st.yaw = k * (2 * PI / 720);
                st.pitch = 0;
                p->body.SetState(st);
                float d = Vector3DotProduct(p->body.GetLookDirection(), dir);
                if (d > best) { best = d; bestYaw = st.yaw; }
            }
            st.yaw = bestYaw;
            p->body.SetState(st);
        };
        auto place = [&](Vector3 at, SimPlayer* p) {
            PlayerState st = p->body.GetState();
            st.feet = at;
            p->body.SetState(st);
        };
        uint32_t seq = 1;
        // One aimed shot from A at B's head; returns the events
        auto shootHead = [&]() {
            float yaw, pitch;
            LookAt(*a, b->body.GetCamera().position, yaw, pitch);
            for (int k = 0; k < 40; k++) { sim.ApplyCommand(a->id, Command(seq++, yaw, pitch, BTN_AIM)); sim.Update(DT); }
            sim.TakeEvents();
            sim.ApplyCommand(a->id, Command(seq++, yaw, pitch, BTN_AIM | BTN_FIRE | BTN_FIRE_PRESSED, sim.Time()));
            sim.Update(DT);
            std::vector<GameEvent> ev = sim.TakeEvents();
            for (int k = 0; k < 30; k++) { sim.ApplyCommand(a->id, Command(seq++, yaw, pitch, BTN_AIM)); sim.Update(DT); }
            return ev;
        };
        auto has = [](const std::vector<GameEvent>& ev, EventType t) {
            for (const GameEvent& e : ev) if (e.type == t) return true;
            return false;
        };

        // From behind: the helmet pops off, B is unhurt; the next one is a normal headshot
        place({ 0, 0, -6 }, a);
        place({ 0, 0, -3 }, b);
        face(b, { 0, 0, 1 });
        sim.Update(DT);
        std::vector<GameEvent> ev = shootHead();
        bool deflected = false;
        for (const GameEvent& e : ev) if (e.type == EventType::Shot && e.Is(EVENT_DEFLECTED)) deflected = true;
        CHECK(deflected && has(ev, EventType::HelmetOff), "a shot to the back of the head knocks the helmet off");
        CHECK(b->Alive() && b->body.GetHealth() == b->body.GetMaxHealth() && !b->helmet, "and the wearer is unhurt, now bareheaded");
        ev = shootHead();
        CHECK(!b->Alive() && has(ev, EventType::Killed), "the next shot to the head is a normal headshot");
        for (int k = 0; k < 400 && !b->Alive(); k++) sim.Update(DT);
        CHECK(b->Alive() && b->helmet, "a respawn comes with a new helmet");

        // From the front: pierced, a normal headshot, and it stays on
        place({ 0, 0, -6 }, a);
        place({ 0, 0, -3 }, b);
        face(b, { 0, 0, -1 });
        sim.Update(DT);
        ev = shootHead();
        CHECK(!has(ev, EventType::HelmetOff) && !b->Alive(), "from the front the helmet is pierced: a normal headshot");

        // A bot's helmet works the same way
        GameSim botSim(world, 1, 9);
        CHECK(botSim.BotView(0).helmet && botSim.BotView(0).id == BOT_ID_BASE, "bots wear helmets too");
    }

    std::printf("Bots on the scoreboard\n");
    {
        Proto::Snapshot snap;
        SoldierView v;
        v.id = 0;
        v.helmet = false;
        snap.bots.push_back(v);
        snap.botScores.push_back({ 4, 2 });
        Proto::Snapshot back;
        bool ok = Proto::DecodeSnapshot(Proto::EncodeSnapshot(snap), back);
        CHECK(ok && back.botScores.size() == 1 && back.botScores[0].kills == 4 && back.botScores[0].deaths == 2,
              "bot scores travel in snapshots");
        CHECK(ok && !back.bots[0].helmet, "and so do helmets");

        SessionConfig oc;
        oc.bots = 2;
        oc.map = MapId::Dustyard;
        Session offline(SessionMode::Offline, oc);
        int botRows = 0;
        for (const ScoreRow& r : offline.Scoreboard()) if (r.bot) botRows++;
        CHECK(botRows == 2, "the scoreboard lists the bots (%d)", botRows);
    }

    std::printf("King of the hill\n");
    {
        World town = BuildOldTown();
        GameSim sim(town, 0, 4);
        SimPlayer* a = sim.AddPlayer("A");
        SimPlayer* b = sim.AddPlayer("B");
        auto place = [](SimPlayer* p, Vector3 at) { PlayerState st = p->body.GetState(); st.feet = at; p->body.SetState(st); };
        place(a, { 5.0f, 0, 5.0f });
        place(b, { 0, 0, -60.0f });
        for (int i = 0; i < 120; i++) sim.Update(DT);
        CHECK(sim.Hill().active && sim.Hill().holder == a->id && !sim.Hill().contested, "alone on the hill, A holds it");
        CHECK(fabsf(a->hillTime - 2.0f) < 0.05f && b->hillTime == 0.0f, "and scores the time held (%.2f s)", a->hillTime);
        place(b, { -5.0f, 0, -4.0f });
        for (int i = 0; i < 60; i++) sim.Update(DT);
        CHECK(sim.Hill().contested && sim.Hill().holder == NO_ID, "two on it: contested, nobody scores");
        CHECK(fabsf(a->hillTime - 2.0f) < 0.05f, "(A's time stays at %.2f s)", a->hillTime);

        Proto::Snapshot snap;
        snap.hill = sim.Hill();
        snap.players.push_back({ a->View(), 1, 2, 30, 17 });
        Proto::Snapshot back;
        bool ok = Proto::DecodeSnapshot(Proto::EncodeSnapshot(snap), back);
        CHECK(ok && back.hill.active && back.hill.contested && back.players[0].hill == 17, "the hill travels in snapshots");

        World plain = BuildTestWorld();
        GameSim none(plain, 0, 1);
        CHECK(!none.Hill().active, "maps without a hill have none");
    }

    std::printf("Bots fight each other\n");
    {
        GameSim sim(world, 2, 3); // no players: the bots only have each other
        int botKills = 0, botHits = 0, selfHits = 0;
        for (int i = 0; i < 60 * 180 && botKills == 0; i++) {
            sim.Update(DT);
            for (const GameEvent& e : sim.TakeEvents()) {
                bool byBot = e.actor >= BOT_ID_BASE && e.actor != NO_ID;
                if (byBot && e.type == EventType::Shot && e.hit == HitKind::Bot) {
                    botHits++;
                    if (e.target == e.actor) selfHits++;
                }
                if (byBot && e.type == EventType::Killed && e.target >= BOT_ID_BASE && e.target != e.actor) botKills++;
            }
        }
        CHECK(botHits > 0 && selfHits == 0, "bots shoot and hit each other (%d hits)", botHits);
        CHECK(botKills > 0, "and kill each other");
        CHECK(BotName(BOT_ID_BASE + 1) == "RIVAL 2", "bots have their own names in the kill feed");
    }

    std::printf("Host and client over loopback\n");
    {
        const uint16_t port = 27391;
        SessionConfig hc;
        hc.name = "Host";
        hc.port = port;
        hc.bots = 1;
        hc.map = MapId::Dustyard;
        Session host(SessionMode::Host, hc);
        CHECK(host.State() == SessionState::Playing && host.Local(), "the host is playing straight away");

        SessionConfig taken = hc;
        Session second(SessionMode::Host, taken);
        CHECK(second.State() == SessionState::Failed, "a second host on the same port fails cleanly: %s", second.StatusText().c_str());

        SessionConfig cc;
        cc.name = "Guest";
        cc.address = "127.0.0.1";
        cc.port = port;
        cc.map = MapId::Dustyard;
        cc.playerClass = CLASS_SNIPER;
        Session client(SessionMode::Client, cc);
        for (int i = 0; i < 300 && client.State() == SessionState::Connecting; i++) Pump(host, &client, DT);
        CHECK(client.State() == SessionState::Playing && client.Local(), "the client connects: %s", client.StatusText().c_str());
        Pump(host, &client, 0.5f);
        CHECK(host.PlayerCount() == 2, "the host sees two players");
        CHECK(client.Others().size() == 1, "the client sees the host's player");
        CHECK(client.Bots().size() == 1, "and the bot");
        bool named = false;
        for (const ScoreRow& r : client.Scoreboard()) if (r.name == "Host") named = true;
        CHECK(named, "names arrive with the roster");
        bool sniper = !host.Others().empty() && host.Others()[0].weapon == WEAPON_SNIPER;
        CHECK(sniper && client.Local() && client.Local()->PrimaryWeapon() == WEAPON_SNIPER, "the guest plays the class they picked");

        // The client walks: predicted locally at once, and the host agrees
        if (client.Local()) {
            Vector3 start = client.Local()->body.GetFeet();
            LocalInput walk;
            walk.move.forward = true;
            for (int i = 0; i < 60; i++) {
                host.Update(DT, LocalInput{});
                client.Update(DT, walk);
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }
            Pump(host, &client, 0.4f);
            Vector3 predictedFeet = client.Local()->body.GetFeet();
            Vector3 hostFeet{};
            for (const SoldierView& v : host.Others()) hostFeet = v.feet;
            float walked = Vector3Distance(start, predictedFeet);
            float disagree = Vector3Distance(predictedFeet, hostFeet);
            CHECK(walked > 2.0f, "the client moves (%.2f m)", walked);
            CHECK(disagree < 0.15f, "the host's copy matches the prediction (off by %.3f m)", disagree);
        }

        // The client leaves: the host notices, carries on
        client.Leave();
        CHECK(client.State() == SessionState::Ended, "leaving ends the client's session");
        Pump(host, nullptr, 0.5f);
        CHECK(host.PlayerCount() == 1 && host.Others().empty(), "the host drops the player who left");

        // Join a port nobody's hosting on: fails after the timeout, without crashing
        SessionConfig nobody = cc;
        nobody.port = port + 1;
        Session lost(SessionMode::Client, nobody);
        for (int i = 0; i < 500 && lost.State() == SessionState::Connecting; i++) Pump(lost, nullptr, DT);
        CHECK(lost.State() == SessionState::Failed, "joining nobody fails: %s", lost.StatusText().c_str());

        // The host stops: a connected client is told
        Session again(SessionMode::Client, cc);
        for (int i = 0; i < 300 && again.State() == SessionState::Connecting; i++) Pump(host, &again, DT);
        CHECK(again.State() == SessionState::Playing, "a client can join again");
        host.Leave();
        for (int i = 0; i < 400 && again.State() == SessionState::Playing; i++) Pump(again, nullptr, DT);
        CHECK(again.State() == SessionState::Ended, "when the host quits, the client returns: %s", again.StatusText().c_str());
    }

    std::printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
    return failures ? 1 : 0;
}
