# Dustyard

A raylib (C++) FPS: two classes (an FN FAL, or an L42A1 sniper rifle with a telescope, each with a revolver), AI opponents, and three maps: **Redoubt**, a long woodland battlefield with a stone fort in the middle; **Old Town**, an old quarter built from photogrammetry scans round a King of the Hill plaza; and **Dustyard**, a small sun-baked town. Play offline, or host a game for up to 8 players over LAN or the internet.

## Build

    cmake -S . -B build
    cmake --build build --config Release

Run `build/fps.exe` (assets are copied next to it on every build). CMake downloads raylib 5.5 and ENet 1.3.18 the first time.

## Playing

The main menu has three choices, a **map** picker (it applies to playing offline and hosting; joining plays the host's map), a **class** picker (see Classes) and a **look** picker: **Civilian** or **Soldier** (woodland fatigues, load vest, gloves). The look is how everyone, bots and players, is drawn on your screen; it doesn't change the game for anyone else.

- **Play Offline**: you and the bots, nothing on the network.
- **Host Game**: starts a game on this PC that you play in. Friends join it. Pick the UDP port (default **27015**) and the number of bots (0–6). While hosting, the top-left corner shows the port and this PC's LAN addresses.
- **Join Game**: enter the host's address (IP or host name) and the port. The menu shows *Connecting…*, then puts you in the game, or says why it couldn't (no answer, game full, different version).

Esc opens the pause menu: **Leave game** takes you back to the main menu. If you're the host, everyone who joined you is returned to their menus. A shared game keeps running while you're paused.

From the command line:

    fps.exe --host [port] --bots 2 --name Ann --map redoubt
    fps.exe --join 192.168.1.20:27015 --name Bob
    fps.exe --offline --map oldtown --class sniper
    fps.exe --offline --map parkside --look soldier

The name, address, port, bots, map, class and look you last used are saved in `dustyard.cfg` next to the exe.

## Classes

Pick one in the main menu before you play. Both carry the revolver as a sidearm.

- **Standard**: the FN FAL, automatic, 20 rounds. Three body shots or one to the head.
- **Sniper**: the Enfield L42A1, a bolt-action 7.62 rifle with a 10-round magazine and a telescope. A shot to the body does 75 damage; a shot to the head kills from any side, helmet or not. Aimed all the way, you look through the telescope (about four times magnification, a pointed post and side bars). After each shot your eye comes off it while you work the bolt (about a second: handle up, back, the case flies out, forward, down). From the hip it's wild. The reload opens the bolt, changes the magazine and closes the bolt again.

## Maps

### Redoubt (240 × 64 m)

Long and wide: team A spawns in a log palisade at one end, team B at the other, and the two halves are the same map turned half a circle.

- **The fort**: a round stone redoubt in the exact centre, the objective. A chest-high wall with a two-step fire step inside (stand on it to see and shoot over; step down to hide), two gates set off the axis, a loophole and a breach knocked down to waist height. In the middle is a raised platform with parapets facing the gates, so the gates can't see through to each other.
- **The forests**: in front of each spawn and across the far corners. Tall trunks with the crowns high overhead and nothing underneath: you can see a long way through them, but there's always a trunk to get behind. Fallen logs and boulders give low cover, and dirt roads wind through so no lane runs straight.
- **The farmland flanks**: wide, open, plowed fields along both long edges. They're fast and exposed, with a rail fence and a few hay bales. A log **checkpoint hut** with sandbags and a barrier sits where each flank meets the middle.
- **The houses**: a two-storey stone house for each team near the middle, with doors toward the woods and the fort, windows on both floors, and stairs inside.
- **The lookout towers**: a stair tower in each team's back corner. The stairs wind up inside a closed log shaft, so you're safe on the way up, to an open lookout 15 m up, above the treetops. From there you can see the other tower across the whole map and straight down the tree-free lane along your edge of the map. It's far from the action, but safe.

### Old Town (113 × 41 m)

A walled-in old quarter built entirely from photogrammetry scans of real buildings (`assets/models/scans`), round a **King of the Hill** plaza. Team A spawns in a yard in one corner and team B in the opposite one; both halves are the same, turned half a circle.

- **The hill**: a paved circle in the middle, marked by a low stone kerb, with a fountain basin and a plinth for cover. Standing in it **alone** scores a second of hill time for every second; two or more in it and it's contested, so nobody scores. A banner at the top of the screen says who holds it, and the scoreboard ranks everyone by time on the hill.
- **The streets**: alleys about 4 m wall to wall between rows of old houses all round the edge, an L-shaped block of houses on each side, and a storage building; only the square round the hill opens up, closed in by a damaged plaster house standing out of each long row. A junk-filled nook in a ruined room, and a ruined wall in a little yard.
- **Sidewalks**: every building stands on the paving with a raised sidewalk round it (16 cm, stepped onto).
- The buildings are shells with no insides; invisible boxes stop people and bullets.

### Dustyard (40 × 80 m)

A sun-baked town with three routes (long, mid through arched doors with a catwalk, and a covered apartment corridor with a terrace on top) meeting in a central plaza with a well.

### Hosting on a LAN

Join using one of the LAN addresses the host shows (e.g. `192.168.1.20`). The first time you host, Windows asks whether `fps.exe` may use the network. Allow it on **private** networks, or nobody can reach you. If you dismissed the prompt, go to Windows Security → Firewall & network protection → Allow an app through firewall, find `fps.exe`, and tick Private.

### Hosting over the internet

1. Allow `fps.exe` through the firewall, as above.
2. On your router, **forward UDP port 27015** (or whichever port you host on) to the LAN address of the hosting PC. It's usually under "Port forwarding" or "Virtual server". The protocol is UDP only; TCP isn't used. Giving the PC a fixed LAN address (a DHCP reservation) keeps the rule working.
3. Friends join using your **public** IP, which you can look up with any "what is my IP" site, plus the port.

Some connections can't accept incoming traffic, whatever you forward. These include carrier-grade NAT (common on mobile and some fibre ISPs; a WAN address of 100.64.x.x–100.127.x.x on the router is the giveaway) and some university or office networks. In that case a VPN-style LAN tool such as Tailscale, ZeroTier or Radmin VPN works: everyone joins the host's address on that virtual network.

## Helmets

Everyone, players and bots, wears a steel helmet. A shot to the **back** of the head glances off: the helmet flies off, the wearer takes no damage, and the HUD shows **NO HELMET** until the next respawn. A shot to the head from any other direction goes straight through it and counts as a normal headshot. The L42A1 goes through a helmet from any side.

## Controls

| Key | Action |
|---|---|
| WASD | Move (Shift sprint, Ctrl or C crouch, Space jump) |
| Mouse | Look; left click fire (hold for automatic with the FAL), right click aim down sights (or through the L42A1's telescope) |
| R | Reload |
| 1 / 2 / mouse wheel | Switch between your rifle and the revolver |
| Q / E | Lean |
| Tab (hold) | Scoreboard (players and bots) |
| Esc | Pause menu (resume, leave game, quit) |
| F3 | FPS counter |
| Enter | Respawn at the other spawn (offline only) |

## How multiplayer works

- **Listen server.** The host's game is the authority. It runs the simulation (`GameSim`) for every player and bot: movement, weapons, ammo, fire rate, hits, damage, deaths and respawns. Clients send only their *inputs*: buttons, view angles and the frame time. The host checks each command and rejects any that is malformed, non-finite, too long or a duplicate. A per-player time budget stops anyone running more game time than real time, which defeats speed hacks.
- **Prediction.** Each client runs the same movement and weapon code for its own player straight away, so controls feel local. Every snapshot from the host says which command it last applied. The client rewinds to the host's state and replays the commands it has sent since, and small corrections are smoothed out of the camera.
- **Interpolation.** Other players and bots are drawn 100 ms in the past, blended between the host's snapshots (sent 30 times a second), so they move smoothly despite packet timing.
- **Lag compensation.** When a client fires, the host rewinds everyone's hitboxes to what that client saw, up to 300 ms back, before tracing the bullet. You hit what was under your crosshair.
- **Transport.** UDP via [ENet](http://enet.bespin.org/). Snapshots and inputs are unreliable (the newest replaces the last; inputs repeat the last 4 commands in case one is lost). Joins, rosters and game events (shots, hits, kills, joins, leaves) are reliable and ordered. A peer that stays silent for a few seconds is treated as disconnected.

## Layout

- `src/main.cpp`: menu, game loop, presentation of events, render passes
- `src/world.*`: the map container (boxes plus size, spawns, scenery), a grid that speeds up collision and line-of-sight queries, Dustyard
- `src/map_redoubt.cpp`: Redoubt: its layout, trees, roads and the painted ground
- `src/map_oldtown.cpp`: Old Town: where each scanned building stands, its collision, the hill
- `src/scenery_mesh.*`: meshes for what isn't boxes (trees, the fort's curved walls, roofs, beams)
- `src/game/session.*`: a game being played (offline, hosted or joined): host and client logic, prediction, reconciliation, interpolation
- `src/game/sim.*`: the authoritative simulation (players, bots, shots, damage, respawns, lag compensation); no rendering or networking
- `src/game/types.hpp`: commands, events and views shared by the simulation, the network and the renderer
- `src/net/protocol.*`: message encoding and decoding (bounds-checked)
- `src/net/transport.*`: the only code that touches ENet and sockets
- `src/soldier.*`: third-person characters with their weapons (other players and bots)
- `src/render.*`: lit shader (with GPU skinning for characters and a blended terrain mode), shadows (following the player on big maps), sky, merged level meshes, culling
- `src/effects.*`: particles, decals, tracers, muzzle flashes
- `src/hud.*`: HUD, main menu, scoreboard, nameplates, kill feed
- `src/audio.*`: sound playback and synthesized effects
- `src/viewmodel.*`, `src/rifle_view.*`, `src/sniper_view.*`: first-person weapons (shared motion and arms, the revolver, the FAL, the L42A1)
- `src/player.*`, `weapon.*`, `enemy.*`, `weapon_models.*`, `character.*`: gameplay and models
- `tests/`: headless tests. `net_test` runs a real host and client over loopback. `player_test` checks both maps: Redoubt is symmetric, everything is reachable on foot, and a simulated player climbs the tower and the house stairs.
- `tools/make_textures.py`: generates the textures (run with Blender's Python).
- `tools/scan_facade.py`, `scan_object.py`, `scan_repack.py`, `scan_colliders.py`: turn photogrammetry scans into game models (see `assets/models/scans/README.md`).

For performance checks, `fps.exe --offline --map redoubt --bots 4 --bench` prints the average and worst frame time over 300 frames with vsync off.

To check animations without playing, a scripted run takes screenshots (`demo_N.png`) at the given seconds and quits; it doesn't touch your saved settings:

    fps.exe --offline --class sniper --demo-aim 1 2.5 --demo-fire 2.2 --demo-reload 4 --shots 2.6,2.9,4.5,6
