#include "audio.hpp"
#include "raymath.h"
#include <cmath>
#include <vector>

// --- Synthesis ------------------------------------------------------------------
// Small helpers for building effects out of decaying tones and filtered noise.
namespace {
const int RATE = 44100;

struct Synth {
    std::vector<float> s;
    unsigned seed = 12345;
    explicit Synth(float seconds) : s((size_t)(seconds * RATE), 0.0f) {}
    float Noise() { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return (seed & 0xFFFFFF) / 8388608.0f - 1.0f; }
    float T(size_t i) const { return (float)i / RATE; }

    // A decaying sine, optionally sliding in pitch (freq -> freq * slide over the sound)
    void Tone(float freq, float amp, float decay, float start = 0.0f, float slide = 1.0f) {
        double phase = 0;
        float length = s.size() / (float)RATE;
        for (size_t i = (size_t)(start * RATE); i < s.size(); i++) {
            float t = T(i) - start;
            float f = freq * powf(slide, t / length);
            phase += 2.0 * PI * f / RATE;
            s[i] += amp * expf(-t * decay) * (float)sin(phase);
        }
    }
    // A burst of noise through a simple low-pass (cut 0..1) and optional high-pass
    void NoiseBurst(float amp, float decay, float lowCut, float highCut = 0.0f, float start = 0.0f) {
        float lp = 0, hp = 0, prev = 0;
        for (size_t i = (size_t)(start * RATE); i < s.size(); i++) {
            float t = T(i) - start;
            lp += (Noise() - lp) * lowCut;
            float v = lp;
            if (highCut > 0.0f) { hp = highCut * (hp + v - prev); prev = v; v = hp; }
            s[i] += amp * expf(-t * decay) * v;
        }
    }
    Sound Make(float peak) {
        float loudest = 0.0f;
        for (float v : s) loudest = fmaxf(loudest, fabsf(v));
        if (loudest > 0.0f) for (float& v : s) v *= peak / loudest;
        // short fade-in/out so nothing clicks
        for (size_t i = 0; i < 64 && i < s.size(); i++) { s[i] *= i / 64.0f; s[s.size() - 1 - i] *= i / 64.0f; }
        Wave w{};
        w.frameCount = (unsigned)s.size();
        w.sampleRate = RATE;
        w.sampleSize = 32;
        w.channels = 1;
        w.data = s.data();
        return LoadSoundFromWave(w); // copies the data
    }
};

Sound MakeSfx(Sfx kind) {
    switch (kind) {
    case Sfx::ImpactConcrete: { // a dry crack and a puff of grit
        Synth x(0.22f);
        x.NoiseBurst(1.0f, 55.0f, 0.55f, 0.85f);
        x.NoiseBurst(0.6f, 25.0f, 0.12f);
        x.Tone(140.0f, 0.35f, 35.0f, 0.0f, 0.6f);
        return x.Make(0.85f);
    }
    case Sfx::ImpactSteel: {    // a bright, ringing ping off sheet metal
        Synth x(0.6f);
        x.NoiseBurst(0.5f, 90.0f, 0.8f, 0.9f);
        const float base = 1180.0f;
        x.Tone(base, 0.55f, 9.0f);
        x.Tone(base * 2.76f, 0.35f, 13.0f);
        x.Tone(base * 5.40f, 0.20f, 20.0f);
        x.Tone(base * 8.93f, 0.12f, 30.0f);
        return x.Make(0.75f);
    }
    case Sfx::ImpactWood: {     // a hollow knock and a splintery crackle
        Synth x(0.25f);
        x.Tone(210.0f, 0.9f, 30.0f, 0.0f, 0.8f);
        x.Tone(470.0f, 0.4f, 45.0f);
        x.NoiseBurst(0.7f, 60.0f, 0.5f, 0.7f);
        x.NoiseBurst(0.3f, 25.0f, 0.9f, 0.9f, 0.02f);
        return x.Make(0.8f);
    }
    case Sfx::FleshHit: {       // a heavy, muffled thwack
        Synth x(0.18f);
        x.Tone(110.0f, 1.0f, 28.0f, 0.0f, 0.5f);
        x.NoiseBurst(0.8f, 40.0f, 0.18f);
        return x.Make(0.9f);
    }
    case Sfx::HitTick: {        // the hit marker: a crisp, short tick
        Synth x(0.07f);
        x.Tone(2900.0f, 1.0f, 85.0f);
        x.NoiseBurst(0.5f, 160.0f, 0.9f, 0.9f);
        return x.Make(0.6f);
    }
    case Sfx::Headshot: {       // a bright ding on top of the tick
        Synth x(0.45f);
        x.Tone(2900.0f, 0.6f, 85.0f);
        x.Tone(1960.0f, 0.8f, 9.0f);
        x.Tone(2940.0f, 0.45f, 12.0f);
        x.Tone(3920.0f, 0.25f, 16.0f);
        return x.Make(0.6f);
    }
    case Sfx::KillConfirm: {    // two quick rising notes
        Synth x(0.45f);
        x.Tone(660.0f, 0.7f, 14.0f);
        x.Tone(1320.0f, 0.25f, 18.0f);
        x.Tone(990.0f, 0.8f, 9.0f, 0.09f);
        x.Tone(1980.0f, 0.25f, 12.0f, 0.09f);
        return x.Make(0.55f);
    }
    case Sfx::BulletCrack: {    // a supersonic snap as a round passes close by
        Synth x(0.16f);
        x.NoiseBurst(1.0f, 70.0f, 0.95f, 0.95f);
        x.Tone(2400.0f, 0.35f, 30.0f, 0.0f, 0.3f);
        return x.Make(0.8f);
    }
    case Sfx::Hurt: {           // a dull body blow
        Synth x(0.35f);
        x.Tone(75.0f, 1.0f, 12.0f, 0.0f, 0.7f);
        x.NoiseBurst(0.7f, 18.0f, 0.06f);
        return x.Make(0.95f);
    }
    case Sfx::DryFire: {        // the hammer falling on an empty chamber
        Synth x(0.12f);
        x.NoiseBurst(0.7f, 140.0f, 0.9f, 0.8f);
        x.Tone(3400.0f, 0.5f, 120.0f);
        x.NoiseBurst(0.5f, 160.0f, 0.9f, 0.8f, 0.035f);
        x.Tone(2600.0f, 0.4f, 120.0f, 0.035f);
        return x.Make(0.5f);
    }
    case Sfx::CasingTink: {     // a brass case bouncing on stone: bright, short, inharmonic
        Synth x(0.25f);
        x.NoiseBurst(0.35f, 220.0f, 0.95f, 0.9f);
        const float base = 3150.0f;
        x.Tone(base, 0.55f, 26.0f);
        x.Tone(base * 1.47f, 0.40f, 32.0f);
        x.Tone(base * 2.27f, 0.30f, 40.0f);
        x.Tone(base * 3.11f, 0.18f, 55.0f);
        x.Tone(base * 0.52f, 0.15f, 30.0f, 0.0f, 0.9f);
        return x.Make(0.45f);
    }
    case Sfx::RifleBoom: {      // the body of a full-power rifle round: a deep thump and a sharp crack
        Synth x(0.7f);
        x.NoiseBurst(1.0f, 20.0f, 0.30f);
        x.Tone(68.0f, 0.9f, 8.0f, 0.0f, 0.6f);
        x.NoiseBurst(0.6f, 70.0f, 0.95f, 0.9f);
        x.NoiseBurst(0.25f, 5.0f, 0.12f, 0.0f, 0.05f); // the rumble rolling off the walls
        return x.Make(0.95f);
    }
    case Sfx::MagOut: {         // the release clicks, the magazine scrapes out of the well
        Synth x(0.25f);
        x.NoiseBurst(0.6f, 130.0f, 0.9f, 0.85f);
        x.Tone(2300.0f, 0.4f, 90.0f);
        x.NoiseBurst(0.35f, 16.0f, 0.5f, 0.6f, 0.04f);
        return x.Make(0.6f);
    }
    case Sfx::MagIn: {          // steel on steel, locking home
        Synth x(0.2f);
        x.Tone(320.0f, 0.5f, 40.0f);
        x.Tone(1600.0f, 0.7f, 60.0f);
        x.Tone(2850.0f, 0.4f, 80.0f);
        x.NoiseBurst(0.8f, 90.0f, 0.85f, 0.8f);
        return x.Make(0.7f);
    }
    case Sfx::MagSlap: {        // a palm smacking the floorplate
        Synth x(0.15f);
        x.NoiseBurst(0.9f, 45.0f, 0.2f);
        x.Tone(180.0f, 0.6f, 35.0f);
        return x.Make(0.55f);
    }
    case Sfx::ChargeBack: {     // the charging handle dragged back: a gritty slide and a stop
        Synth x(0.3f);
        x.NoiseBurst(0.5f, 7.0f, 0.6f, 0.7f);
        x.Tone(900.0f, 0.3f, 30.0f);
        x.NoiseBurst(0.6f, 100.0f, 0.9f, 0.8f, 0.2f);
        x.Tone(1800.0f, 0.3f, 80.0f, 0.2f);
        return x.Make(0.55f);
    }
    case Sfx::ChargeRelease: {  // the bolt slamming forward and locking on a round
        Synth x(0.25f);
        x.Tone(140.0f, 0.6f, 30.0f);
        x.Tone(700.0f, 0.8f, 50.0f);
        x.Tone(1900.0f, 0.5f, 70.0f);
        x.NoiseBurst(1.0f, 70.0f, 0.8f, 0.7f);
        return x.Make(0.8f);
    }
    case Sfx::MagDrop: {        // an empty magazine clattering onto stone
        Synth x(0.35f);
        x.Tone(420.0f, 0.7f, 35.0f);
        x.Tone(1250.0f, 0.35f, 45.0f);
        x.Tone(2600.0f, 0.2f, 60.0f, 0.05f);
        x.NoiseBurst(0.6f, 50.0f, 0.4f);
        return x.Make(0.6f);
    }
    case Sfx::WeaponDraw: {     // sling and metal shifting as a gun comes up
        Synth x(0.3f);
        x.NoiseBurst(0.4f, 14.0f, 0.3f, 0.4f);
        x.Tone(2500.0f, 0.15f, 40.0f, 0.08f);
        x.NoiseBurst(0.3f, 60.0f, 0.8f, 0.8f, 0.12f);
        return x.Make(0.4f);
    }
    case Sfx::Land: {           // boots hitting the ground
        Synth x(0.2f);
        x.NoiseBurst(1.0f, 30.0f, 0.09f);
        x.Tone(60.0f, 0.8f, 22.0f);
        return x.Make(0.8f);
    }
    default: return Sound{};
    }
}
} // namespace

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

// The reload recording, cut into its separate mechanical sounds. Times are
// seconds into reload.mp3; each cut starts just before its click so playing it
// at the animation's moment puts the click on the beat.
static void LoadReloadSteps(const std::string& path, Sound out[(int)ReloadStep::Count]) {
    static const float CUTS[(int)ReloadStep::Count][2] = {
        { 0.29f, 0.43f },  // Latch: the thumb-piece click
        { 0.43f, 0.72f },  // Open: the cylinder rattling out on the crane
        { 0.76f, 1.20f },  // Eject: the rod stroke and cases clattering out
        { 1.33f, 1.72f },  // Insert: rounds seating, the speedloader letting go
        { 2.88f, 3.36f },  // Spin: the cylinder's ratchet whirring down
        { 2.04f, 2.46f },  // Close: the crane slapping shut and locking
    };
    Wave w = LoadWave(path.c_str());
    if (w.data == nullptr) return;
    WaveFormat(&w, w.sampleRate, 32, w.channels);
    const float* s = (const float*)w.data;
    float loudest = 0.0f;
    for (unsigned i = 0; i < w.frameCount * w.channels; i++) loudest = fmaxf(loudest, fabsf(s[i]));
    float gain = loudest > 0.0f ? 0.8f / loudest : 1.0f;
    for (int k = 0; k < (int)ReloadStep::Count; k++) {
        Wave cut = WaveCopy(w);
        int from = (int)(CUTS[k][0] * w.sampleRate), to = (int)(CUTS[k][1] * w.sampleRate);
        if (to > (int)w.frameCount) to = (int)w.frameCount;
        if (from >= to) { UnloadWave(cut); continue; }
        WaveCrop(&cut, from, to);
        float* c = (float*)cut.data;
        int frames = (int)cut.frameCount, ch = (int)cut.channels;
        int fadeIn = (int)(0.004f * cut.sampleRate), fadeOut = (int)(0.04f * cut.sampleRate);
        for (int f = 0; f < frames; f++) {
            float g = gain;
            if (f < fadeIn) g *= (float)f / fadeIn;
            if (f > frames - fadeOut) g *= (float)(frames - f) / fadeOut;
            for (int j = 0; j < ch; j++) c[f * ch + j] *= g;
        }
        out[k] = LoadSoundFromWave(cut);
        UnloadWave(cut);
    }
    UnloadWave(w);
}

bool GameAudio::Load(const std::string& assetDir) {
    std::string dir = assetDir + "sounds/";
    gunshot = LoadCleanSound(dir + "gunshot.mp3", false, 0.9f);
    LoadReloadSteps(dir + "reload.mp3", reloadSteps);
    walk = LoadCleanSound(dir + "walk.mp3", true, 0.6f);
    run = LoadCleanSound(dir + "run.mp3", true, 0.6f);
    loaded = IsSoundValid(gunshot) && IsSoundValid(reloadSteps[0]) && IsSoundValid(walk) && IsSoundValid(run);
    if (!loaded) {
        TraceLog(LOG_WARNING, "AUDIO: sounds missing from %s, playing silently", dir.c_str());
        Unload();
        return false;
    }
    for (Sound& v : gunshotVoices) v = LoadSoundAlias(gunshot);
    for (int i = 0; i < FOOTSTEP_SLOTS; i++) {
        otherWalk[i] = LoadSoundAlias(walk);
        otherRun[i] = LoadSoundAlias(run);
    }
    for (int k = 0; k < (int)Sfx::Count; k++) {
        sfx[k] = MakeSfx((Sfx)k);
        for (Sound& v : sfxVoices[k]) v = LoadSoundAlias(sfx[k]);
    }

    // Ambience: a quiet wind bed, streamed and looping
    wind = LoadMusicStream((dir + "wind.wav").c_str());
    windLoaded = IsMusicValid(wind);
    if (windLoaded) {
        SetMusicVolume(wind, 0.22f);
        PlayMusicStream(wind);
    }
    return true;
}

void GameAudio::Unload() {
    for (Sound& v : gunshotVoices) if (IsSoundValid(v)) UnloadSoundAlias(v);
    for (int i = 0; i < FOOTSTEP_SLOTS; i++)
        for (Sound* a : { &otherWalk[i], &otherRun[i] }) if (IsSoundValid(*a)) UnloadSoundAlias(*a);
    for (int k = 0; k < (int)Sfx::Count; k++) {
        for (Sound& v : sfxVoices[k]) if (IsSoundValid(v)) UnloadSoundAlias(v);
        if (IsSoundValid(sfx[k])) UnloadSound(sfx[k]);
    }
    for (Sound* s : { &gunshot, &walk, &run }) if (IsSoundValid(*s)) UnloadSound(*s);
    for (Sound& s : reloadSteps) if (IsSoundValid(s)) UnloadSound(s);
    if (windLoaded) UnloadMusicStream(wind);
    *this = GameAudio{};
}

void GameAudio::Update() {
    if (windLoaded) UpdateMusicStream(wind);
}

void GameAudio::Play(Sfx s, float volume, float pan) {
    if (!loaded) return;
    int k = (int)s;
    Sound& v = sfxVoices[k][sfxNext[k]];
    sfxNext[k] = (sfxNext[k] + 1) % SFX_VOICES;
    SetSoundVolume(v, Clamp(volume, 0.0f, 1.0f));
    SetSoundPan(v, 0.5f - 0.5f * Clamp(pan, -1.0f, 1.0f));
    SetSoundPitch(v, GetRandomValue(94, 106) / 100.0f);
    PlaySound(v);
}

// Volume falls off with distance (half at 6 m); pan from the listener's right
static void Spatial(Vector3 source, Vector3 ear, Vector3 right, float& volume, float& pan) {
    Vector3 to = Vector3Subtract(source, ear);
    float dist = Vector3Length(to);
    volume /= 1.0f + dist / 6.0f;
    pan = dist > 0.01f ? Vector3DotProduct(Vector3Scale(to, 1.0f / dist), right) * 0.8f : 0.0f;
}

void GameAudio::PlayAt(Sfx s, Vector3 source, Vector3 ear, Vector3 right, float volume) {
    float pan;
    Spatial(source, ear, right, volume, pan);
    Play(s, volume, pan);
}

void GameAudio::PlayImpact(Surface surface, Vector3 at, Vector3 ear, Vector3 right) {
    Sfx s = Sfx::ImpactConcrete;
    if (surface == Surface::Steel) s = Sfx::ImpactSteel;
    else if (surface == Surface::Wood || surface == Surface::Crate || surface == Surface::Painted) s = Sfx::ImpactWood;
    PlayAt(s, at, ear, right, 0.9f);
}

void GameAudio::StopFootstepsFrom(int firstSlot) {
    if (!loaded) return;
    for (int i = firstSlot < 0 ? 0 : firstSlot; i < FOOTSTEP_SLOTS; i++)
        for (Sound* s : { &otherWalk[i], &otherRun[i] }) if (IsSoundPlaying(*s)) PauseSound(*s);
}

void GameAudio::PlayRifleShotAt(Vector3 source, Vector3 ear, Vector3 right) {
    Vector3 to = Vector3Subtract(source, ear);
    float dist = Vector3Length(to);
    float volume = 0.9f / (1.0f + dist / 9.0f); // a rifle carries a little further
    float pan = dist > 0.01f ? Vector3DotProduct(Vector3Scale(to, 1.0f / dist), right) * 0.8f : 0.0f;
    PlayRifleShot(volume, pan);
}

void GameAudio::UpdateFootstepsAt(int slot, int moving, Vector3 source, Vector3 ear, Vector3 right) {
    if (!loaded || slot < 0 || slot >= FOOTSTEP_SLOTS) return;
    Sound& walkS = otherWalk[slot];
    Sound& runS = otherRun[slot];
    if (moving != 1 && IsSoundPlaying(walkS)) PauseSound(walkS);
    if (moving != 2 && IsSoundPlaying(runS)) PauseSound(runS);
    if (moving == 0) return;
    Sound& s = moving == 1 ? walkS : runS;
    float volume = 0.9f, pan;
    Spatial(source, ear, right, volume, pan);
    SetSoundVolume(s, volume);
    SetSoundPan(s, 0.5f - 0.5f * pan);
    if (!IsSoundPlaying(s)) {
        ResumeSound(s);
        if (!IsSoundPlaying(s)) PlaySound(s);
    }
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

void GameAudio::PlayRifleShot(float volume, float pan) {
    if (!loaded) return;
    Sound& v = gunshotVoices[nextVoice];
    nextVoice = (nextVoice + 1) % GUNSHOT_VOICES;
    SetSoundVolume(v, Clamp(volume, 0.0f, 1.0f));
    SetSoundPan(v, 0.5f - 0.5f * Clamp(pan, -1.0f, 1.0f));
    SetSoundPitch(v, GetRandomValue(72, 78) / 100.0f); // a bigger cartridge: deeper and longer
    PlaySound(v);
    Play(Sfx::RifleBoom, volume * 0.8f, pan);
}

void GameAudio::PlayGunshotAt(Vector3 source, Vector3 ear, Vector3 right) {
    Vector3 to = Vector3Subtract(source, ear);
    float dist = Vector3Length(to);
    float volume = 0.85f / (1.0f + dist / 8.0f); // half as loud at 8 m, a third at 16 m
    float pan = dist > 0.01f ? Vector3DotProduct(Vector3Scale(to, 1.0f / dist), right) * 0.8f : 0.0f;
    PlayGunshot(volume, pan);
}

void GameAudio::PlayReloadStep(ReloadStep step) {
    if (!loaded) return;
    Sound& s = reloadSteps[(int)step];
    if (!IsSoundValid(s)) return;
    SetSoundVolume(s, 0.85f);
    SetSoundPitch(s, GetRandomValue(97, 103) / 100.0f);
    PlaySound(s);
}

void GameAudio::StopReload() {
    if (!loaded) return;
    for (Sound& s : reloadSteps) if (IsSoundValid(s)) StopSound(s);
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
