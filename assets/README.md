# Assets

Copied next to the game executable on every build (see CMakeLists.txt).

| File | Source | Used for |
|---|---|---|
| `textures/paving_color.jpg`, `textures/paving_ao.jpg` | ambientCG PavingStones151 (1K-JPG), CC0 | Ground and walls ("concrete") |
| `textures/steel_color.jpg`, `textures/steel_ao.jpg` | ambientCG CorrugatedSteel007B (1K-JPG), CC0 | Cover boxes |
| `models/revolver/revolver.obj`, `.mtl`, `.png` | `43-gun-_obj.rar` (Blender export, "arma.blend") | The player's gun (first person) and the enemy's gun (world model) |
| `models/character/character.glb`, `character.txt` | `civil+man+blend.rar` (Citizen.blend), converted by `tools/export_character.py` | The enemy, your own body (legs) and your first-person arms |
| `sounds/gunshot.mp3` | `freesound_community-single-pistol-gunshot-33-37187.mp3` | Every shot (yours, and the enemy's by distance and direction) |
| `sounds/reload.mp3` | `freesound_community-clean-revolver-reload-6889.mp3` | Reloading |
| `sounds/walk.mp3` | `freesound_community-walking-96582.mp3` | Footsteps: walking (and quieter/slower crouch-walking) |
| `sounds/run.mp3` | `freeeverythingxx-running-on-concrete-268478.mp3` | Footsteps: sprinting |

The game uses only the color and ambient-occlusion maps (the scene is unlit; AO is
multiplied into the color at load). The revolver's `.mtl` was edited to point at
`revolver.png` instead of paths on the original author's machine. Sounds are trimmed
of leading (and, for the footstep loops, trailing) silence and level-matched at load
(see `src/audio.cpp`); the files themselves are unedited.

The character came without animations; `tools/export_character.py` authors them
(idle, walk, run, crouch, crouch_walk, and the first-person fp_hold) and splits the
mesh into parts. To regenerate after editing the script (Blender 4.x):

    blender --background --disable-autoexec Citizen.blend --python tools/export_character.py -- <Citizen textures dir> assets/models/character
