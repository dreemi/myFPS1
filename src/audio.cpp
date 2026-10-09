#include "audio.hpp"
#include "raymath.h"
#include <cmath>

// Load a sound, cut the silence off its start (and optionally its end), and
// scale it so its loudest moment is at `peak`. The source recordings vary a
// lot in level and some have up to a second of silence around them.
static Sound LoadCleanSound(const std::string& path, bool trimEnd, float peak) {
    Wave w = LoadWave(path.c_str());
    if (w.data == nullptr) return Sound{};
    WaveFormat(&w, w.sampleRate, 32, w.channels); // 32-bit float samples
    float* s = (float*)w.data;
    int frames = (int)w.frameCount, ch = (int)w.channels;

    float loudest = 0.0f;
    for (int i = 0; i < frames * ch; i++) loudest = fmaxf(loudest, fabsf(s[i]));
    if (loudest > 0.0f) {
        const float threshold = loudest * 0.03f;
        auto loud = [&](int f) {
            for (int c = 0; c < ch; c++) if (fabsf(s[f * ch + c]) > threshold) return true;
            return false;
        };
        int first = 0, last = frames - 1;
        while (first < last && !loud(first)) first++;
        while (trimEnd && last > first && !loud(last)) last--;
        const int pad = (int)(w.sampleRate * 0.01f); // keep 10 ms so the attack isn't clipped
        first = first > pad ? first - pad : 0;
        last = trimEnd ? (last + pad < frames ? last + pad : frames - 1) : frames - 1;
        WaveCrop(&w, first, last + 1);

        s = (float*)w.data;
        float gain = peak / loudest;
        for (unsigned i = 0; i < w.frameCount * w.channels; i++) s[i] *= gain;
    }
    Sound snd = LoadSoundFromWave(w);
    UnloadWave(w);
    return snd;
}

bool GameAudio::Load(const std::string& assetDir) {
    std::string dir = assetDir + "sounds/";
    gunshot = LoadCleanSound(dir + "gunshot.mp3", false, 0.9f);
    reload = LoadCleanSound(dir + "reload.mp3", false, 0.8f);
    walk = LoadCleanSound(dir + "walk.mp3", true, 0.6f);
    run = LoadCleanSound(dir + "run.mp3", true, 0.6f);
    loaded = IsSoundValid(gunshot) && IsSoundValid(reload) && IsSoundValid(walk) && IsSoundValid(run);
    if (!loaded) {
        TraceLog(LOG_WARNING, "AUDIO: sounds missing from %s, playing silently", dir.c_str());
        Unload();
        return false;
    }
    for (Sound& v : gunshotVoices) v = LoadSoundAlias(gunshot);
    return true;
}

void GameAudio::Unload() {
    for (Sound& v : gunshotVoices) if (IsSoundValid(v)) UnloadSoundAlias(v);
    for (Sound* s : { &gunshot, &reload, &walk, &run }) if (IsSoundValid(*s)) UnloadSound(*s);
    *this = GameAudio{};
}

void GameAudio::PlayGunshot(float volume, float pan) {
    if (!loaded) return;
    Sound& v = gunshotVoices[nextVoice];
    nextVoice = (nextVoice + 1) % GUNSHOT_VOICES;
    SetSoundVolume(v, Clamp(volume, 0.0f, 1.0f));
    SetSoundPan(v, 0.5f - 0.5f * Clamp(pan, -1.0f, 1.0f)); // raylib: 1 = left, 0 = right
    SetSoundPitch(v, GetRandomValue(95, 105) / 100.0f);   // small variation so shots don't sound identical
    PlaySound(v);
}

void GameAudio::PlayGunshotAt(Vector3 source, Vector3 ear, Vector3 right) {
    Vector3 to = Vector3Subtract(source, ear);
    float dist = Vector3Length(to);
    float volume = 0.85f / (1.0f + dist / 8.0f); // half as loud at 8 m, a third at 16 m
    float pan = dist > 0.01f ? Vector3DotProduct(Vector3Scale(to, 1.0f / dist), right) * 0.8f : 0.0f;
    PlayGunshot(volume, pan);
}

void GameAudio::PlayReload() {
    if (!loaded) return;
    SetSoundVolume(reload, 0.8f);
    PlaySound(reload);
}

void GameAudio::StopReload() {
    if (loaded) StopSound(reload);
}

void GameAudio::UpdateFootsteps(Stance stance, bool alive) {
    if (!loaded) return;
    enum class Steps { None, Walk, Run } want = Steps::None;
    float volume = 0.0f, pitch = 1.0f;
    if (alive) {
        switch (stance) {
            case Stance::Walking:      want = Steps::Walk; volume = 0.55f; break;
            case Stance::CrouchMoving: want = Steps::Walk; volume = 0.22f; pitch = 0.8f; break; // sneaking
            case Stance::Sprinting:    want = Steps::Run;  volume = 0.65f; break;
            default: break; // still, crouched still, or in the air
        }
    }

    // Pause the loop we're not using; it resumes from the same spot later
    if (want != Steps::Walk && IsSoundPlaying(walk)) PauseSound(walk);
    if (want != Steps::Run && IsSoundPlaying(run)) PauseSound(run);
    if (want == Steps::None) return;

    Sound& s = want == Steps::Walk ? walk : run;
    SetSoundVolume(s, volume);
    SetSoundPitch(s, pitch);
    if (!IsSoundPlaying(s)) {
        // ResumeSound continues a paused loop; once it has played to the end
        // that does nothing, so start it again from the top
        ResumeSound(s);
        if (!IsSoundPlaying(s)) PlaySound(s);
    }
}
