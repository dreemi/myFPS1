# Dustyard

A small raylib (C++) FPS vertical slice: an FN FAL and a revolver, one AI opponent, a sun-baked town with three routes, a catwalk and a balcony.

## Build

    cmake -S . -B build
    cmake --build build --config Release

Run `build/fps.exe` (assets are copied next to it on every build).

## Controls

| Key | Action |
|---|---|
| WASD | Move (Shift sprint, Ctrl or C crouch, Space jump) |
| Mouse | Look; left click fire (hold for automatic with the FAL), right click aim down sights |
| R | Reload |
| 1 / 2 / mouse wheel | Switch between the FAL and the revolver |
| Q / E | Lean |
| Esc / Tab | Pause menu |
| F3 | FPS counter |
| Enter | Respawn at the other spawn |

## Layout

- `src/main.cpp` – game loop, states, render passes
- `src/render.*` – lit shader, shadows, sky, merged level meshes
- `src/effects.*` – particles, decals, tracers, muzzle flashes
- `src/hud.*` – HUD, menus, damage indicators, kill feed
- `src/audio.*` – sound playback and synthesized effects
- `src/viewmodel.*`, `src/rifle_view.*` – first-person weapons: shared motion and arms, the revolver, the FAL
- `src/player.*`, `weapon.*`, `enemy.*`, `weapon_models.*`, `character.*` – gameplay and models
- `tests/` – headless gameplay tests
