# Dustyard

A small raylib (C++) FPS: an FN FAL and a revolver, AI opponents, a sun-baked town with three routes, a catwalk and a balcony. Play offline, or host a game for up to 8 players over LAN or the internet.

## Build

    cmake -S . -B build
    cmake --build build --config Release

Run `build/fps.exe` (assets are copied next to it on every build). CMake downloads raylib 5.5 and ENet 1.3.18 the first time.

## Playing

The main menu has three choices:

- **Play Offline**: you and the bots, nothing on the network.
- **Host Game**: starts a game on this PC that you play in. Friends join it. Pick the UDP port (default **27015**) and the number of bots (0–4). While hosting, the top-left corner shows the port and this PC's LAN addresses.
- **Join Game**: enter the host's address (IP or host name) and the port. The menu shows *Connecting…*, then puts you in the game, or says why it couldn't (no answer, game full, different version).

Esc opens the pause menu: **Leave game** takes you back to the main menu. If you're the host, everyone who joined you is returned to their menus. A shared game keeps running while you're paused.

From the command line:

    fps.exe --host [port] --bots 2 --name Ann
    fps.exe --join 192.168.1.20:27015 --name Bob
    fps.exe --offline

The name, address, port and bots you last used are saved in `dustyard.cfg` next to the exe.

### Hosting on a LAN

Join using one of the LAN addresses the host shows (e.g. `192.168.1.20`). The first time you host, Windows asks whether `fps.exe` may use the network. Allow it on **private** networks, or nobody can reach you. If you dismissed the prompt, go to Windows Security → Firewall & network protection → Allow an app through firewall, find `fps.exe`, and tick Private.

### Hosting over the internet

1. Allow `fps.exe` through the firewall, as above.
2. On your router, **forward UDP port 27015** (or whichever port you host on) to the LAN address of the hosting PC. It's usually under "Port forwarding" or "Virtual server". The protocol is UDP only; TCP isn't used. Giving the PC a fixed LAN address (a DHCP reservation) keeps the rule working.
3. Friends join using your **public** IP, which you can look up with any "what is my IP" site, plus the port.

Some connections can't accept incoming traffic, whatever you forward. These include carrier-grade NAT (common on mobile and some fibre ISPs; a WAN address of 100.64.x.x–100.127.x.x on the router is the giveaway) and some university or office networks. In that case a VPN-style LAN tool such as Tailscale, ZeroTier or Radmin VPN works: everyone joins the host's address on that virtual network.

## Controls

| Key | Action |
|---|---|
| WASD | Move (Shift sprint, Ctrl or C crouch, Space jump) |
| Mouse | Look; left click fire (hold for automatic with the FAL), right click aim down sights |
| R | Reload |
| 1 / 2 / mouse wheel | Switch between the FAL and the revolver |
| Q / E | Lean |
| Tab (hold) | Scoreboard |
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
- `src/game/session.*`: a game being played (offline, hosted or joined): host and client logic, prediction, reconciliation, interpolation
- `src/game/sim.*`: the authoritative simulation (players, bots, shots, damage, respawns, lag compensation); no rendering or networking
- `src/game/types.hpp`: commands, events and views shared by the simulation, the network and the renderer
- `src/net/protocol.*`: message encoding and decoding (bounds-checked)
- `src/net/transport.*`: the only code that touches ENet and sockets
- `src/soldier.*`: third-person characters with their weapons (other players and bots)
- `src/render.*`: lit shader, shadows, sky, merged level meshes
- `src/effects.*`: particles, decals, tracers, muzzle flashes
- `src/hud.*`: HUD, main menu, scoreboard, nameplates, kill feed
- `src/audio.*`: sound playback and synthesized effects
- `src/viewmodel.*`, `src/rifle_view.*`: first-person weapons (shared motion and arms, the revolver, the FAL)
- `src/player.*`, `weapon.*`, `enemy.*`, `weapon_models.*`, `character.*`: gameplay and models
- `tests/`: headless tests. `net_test` runs a real host and client over loopback.
