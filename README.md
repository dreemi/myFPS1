# Dustyard

A small raylib (C++) FPS vertical slice: one revolver, one AI opponent, a three-route yard.

## Build

    cmake -S . -B build
    cmake --build build --config Release

Run `build/fps.exe` (assets are copied next to it on every build).

## Controls

| Key | Action |
|---|---|
| WASD | Move (Shift sprint, Ctrl crouch, Space jump) |
| Mouse | Look; left click fire, right click aim down sights |
| R | Reload |
| Q / E | Lean |
| Esc / Tab | Pause menu |
| F3 | FPS counter |

## Layout

- `src/main.cpp` – game loop, states, render passes
- `src/render.*` – lit shader, shadows, sky, merged level meshes
- `src/effects.*` – particles, decals, tracers, muzzle flashes
- `src/hud.*` – HUD, menus, damage indicators, kill feed
- `src/audio.*` – sound playback and synthesized effects
- `src/player.*`, `weapon.*`, `enemy.*`, `viewmodel.*`, `character.*` – gameplay and models
- `tests/` – headless gameplay tests
