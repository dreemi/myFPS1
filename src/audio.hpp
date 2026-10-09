#pragma once
#include "raylib.h"
#include "player.hpp"
#include <string>

// All game sound effects. Sounds are triggered by gameplay events from the
// outside (main.cpp); nothing in the gameplay code knows about audio.
// Requires InitAudioDevice() before Load.
class GameAudio {
public:
    bool Load(const std::string& assetDir);
    void Unload();

    // Gunshots: volume 0..1, pan -1 (left) .. 1 (right)
    void PlayGunshot(float volume, float pan);
    // A gunshot heard from `source` by a listener at `ear` facing with `right` as their right
    void PlayGunshotAt(Vector3 source, Vector3 ear, Vector3 right);

    void PlayReload();
    void StopReload();

    // Footsteps: walking/running loops while moving on the ground. Crouch-walking
    // plays the walking steps quieter and slower; anything else is silent.
    void UpdateFootsteps(Stance stance, bool alive);

private:
    static const int GUNSHOT_VOICES = 6; // so rapid/overlapping shots don't cut each other off
    Sound gunshot{};
    Sound gunshotVoices[GUNSHOT_VOICES]{};
    int nextVoice = 0;
    Sound reload{};
    Sound walk{};
    Sound run{};
    bool loaded = false;
};
