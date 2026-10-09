#pragma once
#include "raylib.h"
#include "player.hpp"
#include "world.hpp"
#include <string>

// Short effects, synthesized at startup (no files)
enum class Sfx {
    ImpactConcrete, ImpactSteel, ImpactWood, FleshHit, HitTick, Headshot, KillConfirm,
    BulletCrack, Hurt, DryFire, Land, CasingTink, Count
};

// The revolver's reload, step by step (cut from the reload recording so each
// sound lands on its moment in the animation)
enum class ReloadStep { Latch, Open, Eject, Insert, Spin, Close, Count };

// All game sound. Sounds are triggered by gameplay events from the outside
// (main.cpp); nothing in the gameplay code knows about audio.
// Requires InitAudioDevice() before Load.
class GameAudio {
public:
    bool Load(const std::string& assetDir);
    void Unload();
    void Update(); // keeps the ambience streaming

    // Gunshots: volume 0..1, pan -1 (left) .. 1 (right)
    void PlayGunshot(float volume, float pan);
    // A gunshot heard from `source` by a listener at `ear` facing with `right` as their right
    void PlayGunshotAt(Vector3 source, Vector3 ear, Vector3 right);

    void PlayReloadStep(ReloadStep step);
    void StopReload();

    // Effects: flat (UI-like) or positioned in the world
    void Play(Sfx s, float volume = 1.0f, float pan = 0.0f);
    void PlayAt(Sfx s, Vector3 source, Vector3 ear, Vector3 right, float volume = 1.0f);
    void PlayImpact(Surface surface, Vector3 at, Vector3 ear, Vector3 right);

    // Footsteps: walking/running loops while moving on the ground. Crouch-walking
    // plays the walking steps quieter and slower; anything else is silent.
    void UpdateFootsteps(Stance stance, bool alive);
    // The enemy's footsteps, positioned: moving = 0 none, 1 walk, 2 run
    void UpdateEnemyFootsteps(int moving, Vector3 source, Vector3 ear, Vector3 right);

private:
    static const int GUNSHOT_VOICES = 6; // so rapid/overlapping shots don't cut each other off
    static const int SFX_VOICES = 4;
    Sound gunshot{};
    Sound gunshotVoices[GUNSHOT_VOICES]{};
    int nextVoice = 0;
    Sound reloadSteps[(int)ReloadStep::Count]{};
    Sound walk{};
    Sound run{};
    Sound enemyWalk{}, enemyRun{};      // aliases: their own playback state
    Sound sfx[(int)Sfx::Count]{};
    Sound sfxVoices[(int)Sfx::Count][SFX_VOICES]{};
    int sfxNext[(int)Sfx::Count]{};
    Music wind{};
    bool windLoaded = false;
    bool loaded = false;
};
