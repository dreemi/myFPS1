#include "effects.hpp"
#include "raymath.h"
#include "rlgl.h"
#include <cmath>
#include <algorithm>
#include <vector>

namespace Fx {

enum class Tex { Soft, Flash, Hole, Spark };
enum class Blend { Alpha, Additive };

struct Particle {
    Vector3 pos, vel;
    float life, maxLife;
    float size, grow;       // grow: size change per second
    float rot, spin;
    float gravity, drag;
    Color color;
    Tex tex;
    Blend blend;
    bool streak;            // draw stretched along velocity (sparks)
};

struct Decal {
    Vector3 pos, normal;
    float size, rot, life;
};

struct TracerFx {
    Vector3 from, to;
    float life, maxLife;
    Color color;
};

static Texture2D textures[4];
static std::vector<Particle> particles;
static std::vector<Decal> decals;
static std::vector<TracerFx> tracers;
static bool ready = false;
static const size_t MAX_PARTICLES = 1500, MAX_DECALS = 96;

static float Rand(float lo, float hi) { return lo + (hi - lo) * (GetRandomValue(0, 10000) / 10000.0f); }
static Vector3 RandUnit() {
    Vector3 v;
    do { v = { Rand(-1, 1), Rand(-1, 1), Rand(-1, 1) }; } while (Vector3LengthSqr(v) > 1.0f || Vector3LengthSqr(v) < 0.01f);
    return Vector3Normalize(v);
}
// A random direction within `spread` (0..1, 1 = hemisphere) of n
static Vector3 RandAround(Vector3 n, float spread) {
    return Vector3Normalize(Vector3Add(n, Vector3Scale(RandUnit(), spread)));
}

// --- Generated textures -------------------------------------------------------

static Texture2D MakeTexture(int size, float (*alpha)(float x, float y), Color rgb) {
    Image img = GenImageColor(size, size, BLANK);
    Color* px = (Color*)img.data;
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            float u = (x + 0.5f) / size * 2 - 1, v = (y + 0.5f) / size * 2 - 1;
            float a = Clamp(alpha(u, v), 0.0f, 1.0f);
            px[y * size + x] = { rgb.r, rgb.g, rgb.b, (unsigned char)(a * 255) };
        }
    }
    Texture2D t = LoadTextureFromImage(img);
    UnloadImage(img);
    GenTextureMipmaps(&t);
    SetTextureFilter(t, TEXTURE_FILTER_TRILINEAR);
    return t;
}

static float SoftAlpha(float u, float v) {
    float r = sqrtf(u * u + v * v);
    return powf(fmaxf(0.0f, 1.0f - r), 1.8f);
}

static float FlashAlpha(float u, float v) {
    // A hot core with four-ish uneven spikes
    float r = sqrtf(u * u + v * v);
    float a = atan2f(v, u);
    float spikes = powf(fabsf(cosf(a * 2.0f)), 12.0f) * (0.75f + 0.25f * sinf(a * 5.0f));
    float core = powf(fmaxf(0.0f, 1.0f - r * 2.2f), 1.2f);
    float rays = powf(fmaxf(0.0f, 1.0f - r), 2.0f) * spikes;
    return core + rays;
}

// Bullet hole: a dark pit inside a ring of freshly chipped, paler material,
// so it reads on both sunlit and shaded walls
static Texture2D MakeHoleTexture(int size) {
    Image img = GenImageColor(size, size, BLANK);
    Color* px = (Color*)img.data;
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            float u = (x + 0.5f) / size * 2 - 1, v = (y + 0.5f) / size * 2 - 1;
            float r = sqrtf(u * u + v * v), a = atan2f(v, u);
            float ragged = 0.07f * sinf(a * 7.0f) + 0.05f * sinf(a * 13.0f + 1.3f);
            float rr = r - ragged;
            Color c = BLANK;
            if (rr < 0.20f) c = { 12, 10, 9, 255 };                               // the pit
            else if (rr < 0.30f) c = { 40, 36, 32, 235 };                         // its edge
            else if (rr < 0.75f) {                                                 // chipped ring
                float t = (rr - 0.30f) / 0.45f;
                unsigned char g = (unsigned char)Lerp(205, 175, t);
                c = { g, (unsigned char)(g - 8), (unsigned char)(g - 22), (unsigned char)(200 * (1.0f - t * t)) };
            }
            px[y * size + x] = c;
        }
    }
    Texture2D t = LoadTextureFromImage(img);
    UnloadImage(img);
    GenTextureMipmaps(&t);
    SetTextureFilter(t, TEXTURE_FILTER_TRILINEAR);
    return t;
}

static float SparkAlpha(float u, float v) {
    // Long thin glow (stretched along the texture's x axis when drawn)
    return powf(fmaxf(0.0f, 1.0f - fabsf(v) * 1.0f), 3.0f) * powf(fmaxf(0.0f, 1.0f - fabsf(u)), 1.0f);
}

bool Init() {
    textures[(int)Tex::Soft] = MakeTexture(64, SoftAlpha, WHITE);
    textures[(int)Tex::Flash] = MakeTexture(128, FlashAlpha, WHITE);
    textures[(int)Tex::Hole] = MakeHoleTexture(64);
    textures[(int)Tex::Spark] = MakeTexture(32, SparkAlpha, WHITE);
    ready = true;
    return true;
}

void Shutdown() {
    for (Texture2D& t : textures) if (t.id) UnloadTexture(t);
    Clear();
    ready = false;
}

void Clear() {
    particles.clear();
    decals.clear();
    tracers.clear();
}

// --- Spawning -------------------------------------------------------------------

static void Emit(const Particle& p) {
    if (particles.size() < MAX_PARTICLES) particles.push_back(p);
}

void Impact(Vector3 point, Vector3 normal, Surface surface) {
    if (!ready) return;
    Vector3 at = Vector3Add(point, Vector3Scale(normal, 0.02f));
    bool steel = surface == Surface::Steel;

    // Sparks: lots off steel, a few off stone
    int sparks = steel ? 14 : 4;
    for (int i = 0; i < sparks; i++) {
        Vector3 d = RandAround(normal, 0.9f);
        float s = Rand(4.0f, steel ? 11.0f : 7.0f);
        Emit({ at, Vector3Scale(d, s), 0, Rand(0.12f, 0.35f), Rand(0.025f, 0.045f), 0, 0, 0, 9.0f, 1.5f,
               { 255, (unsigned char)GetRandomValue(170, 230), 120, 255 }, Tex::Spark, Blend::Additive, true });
    }
    // Brief hot flash at the hit point
    Emit({ at, { 0, 0, 0 }, 0, 0.06f, steel ? 0.35f : 0.22f, 1.0f, Rand(0, 360), 0, 0, 0,
           { 255, 210, 150, 255 }, Tex::Flash, Blend::Additive, false });

    // Dust: puffs that drift and grow (grey-brown off stone, rusty off steel)
    Color dust = steel ? Color{ 120, 95, 80, 255 } : Color{ 150, 135, 115, 255 };
    int puffs = steel ? 4 : 8;
    for (int i = 0; i < puffs; i++) {
        Vector3 d = RandAround(normal, 0.6f);
        Emit({ at, Vector3Scale(d, Rand(0.8f, 2.8f)), 0, Rand(0.7f, 1.5f), Rand(0.12f, 0.24f), Rand(0.5f, 1.0f),
               Rand(0, 360), Rand(-90, 90), -0.3f, 2.5f, dust, Tex::Soft, Blend::Alpha, false });
    }
    // Chips: small dark bits thrown out and falling
    int chips = steel ? 2 : 6;
    for (int i = 0; i < chips; i++) {
        Vector3 d = RandAround(normal, 0.8f);
        Emit({ at, Vector3Scale(d, Rand(2.0f, 5.0f)), 0, Rand(0.4f, 0.8f), Rand(0.012f, 0.025f), 0, 0, 0, 9.8f, 0.5f,
               steel ? Color{ 60, 50, 45, 255 } : Color{ 90, 82, 72, 255 }, Tex::Soft, Blend::Alpha, false });
    }

    // Bullet hole
    if (decals.size() >= MAX_DECALS) decals.erase(decals.begin());
    decals.push_back({ Vector3Add(point, Vector3Scale(normal, 0.004f)), normal, Rand(0.09f, 0.12f), Rand(0, 360), 20.0f });
}

void Blood(Vector3 point, Vector3 bulletDir, bool headshot) {
    if (!ready) return;
    Vector3 dir = Vector3Normalize(bulletDir);
    // Mist: soft red puffs, mostly out the far side
    int mist = headshot ? 10 : 6;
    for (int i = 0; i < mist; i++) {
        Vector3 d = RandAround(i % 3 == 0 ? Vector3Negate(dir) : dir, 0.7f);
        Emit({ point, Vector3Scale(d, Rand(0.8f, 2.5f)), 0, Rand(0.35f, 0.7f), Rand(0.08f, 0.15f), Rand(0.3f, 0.6f),
               Rand(0, 360), Rand(-60, 60), 1.0f, 3.0f, { 120, 10, 12, 220 }, Tex::Soft, Blend::Alpha, false });
    }
    // Droplets: small, fast, falling
    int drops = headshot ? 16 : 9;
    for (int i = 0; i < drops; i++) {
        Vector3 d = RandAround(dir, 0.8f);
        Emit({ point, Vector3Scale(d, Rand(2.0f, 5.0f)), 0, Rand(0.3f, 0.6f), Rand(0.018f, 0.035f), 0, 0, 0, 9.8f, 0.8f,
               { 105, 6, 8, 255 }, Tex::Soft, Blend::Alpha, false });
    }
}

void MuzzleFlash(Vector3 muzzle, Vector3 dir) {
    if (!ready) return;
    Emit({ muzzle, { 0, 0, 0 }, 0, 0.05f, 0.45f, 2.0f, Rand(0, 360), 0, 0, 0, { 255, 220, 160, 255 }, Tex::Flash,
           Blend::Additive, false });
    for (int i = 0; i < 3; i++) {
        Vector3 d = RandAround(dir, 0.4f);
        Emit({ Vector3Add(muzzle, Vector3Scale(dir, 0.1f)), Vector3Scale(d, Rand(0.4f, 1.2f)), 0, Rand(0.6f, 1.1f),
               Rand(0.08f, 0.14f), Rand(0.3f, 0.5f), Rand(0, 360), Rand(-40, 40), -0.4f, 2.0f, { 190, 190, 185, 110 },
               Tex::Soft, Blend::Alpha, false });
    }
}

void Tracer(Vector3 from, Vector3 to, Color color) {
    if (!ready) return;
    tracers.push_back({ from, to, 0.0f, 0.07f, color });
}

// --- Update -----------------------------------------------------------------------

void Update(float dt) {
    for (Particle& p : particles) {
        p.life += dt;
        p.vel.y -= p.gravity * dt;
        p.vel = Vector3Scale(p.vel, 1.0f / (1.0f + p.drag * dt));
        p.pos = Vector3Add(p.pos, Vector3Scale(p.vel, dt));
        if (p.pos.y < 0.01f && p.gravity > 0.0f) { // land on the ground and stop
            p.pos.y = 0.01f;
            p.vel = Vector3Scale(p.vel, 0.2f);
            p.vel.y = 0;
        }
        p.size += p.grow * dt;
        p.rot += p.spin * dt;
    }
    particles.erase(std::remove_if(particles.begin(), particles.end(), [](const Particle& p) { return p.life >= p.maxLife; }),
                    particles.end());
    for (Decal& d : decals) d.life -= dt;
    decals.erase(std::remove_if(decals.begin(), decals.end(), [](const Decal& d) { return d.life <= 0; }), decals.end());
    for (TracerFx& t : tracers) t.life += dt;
    tracers.erase(std::remove_if(tracers.begin(), tracers.end(), [](const TracerFx& t) { return t.life >= t.maxLife; }),
                  tracers.end());
}

// --- Drawing ----------------------------------------------------------------------

// A textured quad from four corners
static void Quad(Texture2D tex, Vector3 a, Vector3 b, Vector3 c, Vector3 d, Color col) {
    rlSetTexture(tex.id);
    rlBegin(RL_QUADS);
    rlColor4ub(col.r, col.g, col.b, col.a);
    rlTexCoord2f(0, 1); rlVertex3f(a.x, a.y, a.z);
    rlTexCoord2f(1, 1); rlVertex3f(b.x, b.y, b.z);
    rlTexCoord2f(1, 0); rlVertex3f(c.x, c.y, c.z);
    rlTexCoord2f(0, 0); rlVertex3f(d.x, d.y, d.z);
    rlEnd();
    rlSetTexture(0);
}

// A quad stretched from p to q, facing the camera (sparks, tracers)
static void Streak(const Camera3D& cam, Texture2D tex, Vector3 p, Vector3 q, float width, Color col) {
    Vector3 along = Vector3Subtract(q, p);
    if (Vector3LengthSqr(along) < 1e-8f) return;
    Vector3 mid = Vector3Lerp(p, q, 0.5f);
    Vector3 side = Vector3CrossProduct(along, Vector3Subtract(cam.position, mid));
    if (Vector3LengthSqr(side) < 1e-10f) return;
    side = Vector3Scale(Vector3Normalize(side), width * 0.5f);
    Quad(tex, Vector3Subtract(p, side), Vector3Add(p, side), Vector3Add(q, side), Vector3Subtract(q, side), col);
}

static Color Faded(Color c, float a) { return { c.r, c.g, c.b, (unsigned char)(c.a * Clamp(a, 0.0f, 1.0f)) }; }

void Draw(const Camera3D& cam) {
    if (!ready) return;
    rlDrawRenderBatchActive();
    rlDisableBackfaceCulling();

    // Bullet holes: on the surface, depth-tested, alpha blended
    rlDisableDepthMask();
    BeginBlendMode(BLEND_ALPHA);
    for (const Decal& d : decals) {
        Vector3 n = d.normal;
        Vector3 t = fabsf(n.y) > 0.9f ? Vector3{ 1, 0, 0 } : Vector3Normalize(Vector3CrossProduct({ 0, 1, 0 }, n));
        Vector3 b = Vector3CrossProduct(n, t);
        float c = cosf(d.rot * DEG2RAD), s = sinf(d.rot * DEG2RAD);
        Vector3 r = Vector3Scale(Vector3Add(Vector3Scale(t, c), Vector3Scale(b, s)), d.size);
        Vector3 u = Vector3Scale(Vector3Subtract(Vector3Scale(b, c), Vector3Scale(t, s)), d.size);
        Color col = Faded(WHITE, d.life / 3.0f); // fade out over the last 3 seconds
        Quad(textures[(int)Tex::Hole], Vector3Subtract(Vector3Subtract(d.pos, r), u), Vector3Subtract(Vector3Add(d.pos, r), u),
             Vector3Add(Vector3Add(d.pos, r), u), Vector3Add(Vector3Subtract(d.pos, r), u), col);
    }

    // Soft particles (dust, smoke, blood)
    for (const Particle& p : particles) {
        if (p.blend != Blend::Alpha) continue;
        float t = p.life / p.maxLife;
        Color col = Faded(p.color, (1.0f - t) * (t < 0.1f ? t / 0.1f : 1.0f));
        DrawBillboardPro(cam, textures[(int)p.tex], { 0, 0, 64, 64 }, p.pos, { 0, 1, 0 }, { p.size, p.size },
                         { p.size / 2, p.size / 2 }, p.rot, col);
    }
    EndBlendMode();

    // Glowing things: sparks, flashes, tracers
    BeginBlendMode(BLEND_ADDITIVE);
    for (const Particle& p : particles) {
        if (p.blend != Blend::Additive) continue;
        float t = p.life / p.maxLife;
        Color col = Faded(p.color, 1.0f - t);
        if (p.streak) {
            Vector3 tail = Vector3Subtract(p.pos, Vector3Scale(p.vel, 0.025f));
            Streak(cam, textures[(int)Tex::Spark], tail, p.pos, p.size, col);
        } else {
            Texture2D tex = textures[(int)p.tex];
            DrawBillboardPro(cam, tex, { 0, 0, (float)tex.width, (float)tex.height }, p.pos, { 0, 1, 0 }, { p.size, p.size },
                             { p.size / 2, p.size / 2 }, p.rot, col);
        }
    }
    for (const TracerFx& tr : tracers) {
        float t = tr.life / tr.maxLife;
        // A short bright segment racing from muzzle to impact, fading as it goes
        Vector3 head = Vector3Lerp(tr.from, tr.to, Clamp(t * 1.6f, 0.0f, 1.0f));
        Vector3 tail = Vector3Lerp(tr.from, tr.to, Clamp(t * 1.6f - 0.35f, 0.0f, 1.0f));
        Streak(cam, textures[(int)Tex::Spark], tail, head, 0.035f, Faded(tr.color, 1.0f - t * 0.6f));
    }
    EndBlendMode();

    rlDrawRenderBatchActive();
    rlEnableDepthMask();
    rlEnableBackfaceCulling();
}

void DrawFlashSprite(const Camera3D& cam, Vector3 position, float size, float rotation, float alpha) {
    if (!ready) return;
    rlDrawRenderBatchActive();
    rlDisableDepthMask();
    BeginBlendMode(BLEND_ADDITIVE);
    Texture2D tex = textures[(int)Tex::Flash];
    DrawBillboardPro(cam, tex, { 0, 0, (float)tex.width, (float)tex.height }, position, { 0, 1, 0 }, { size, size },
                     { size / 2, size / 2 }, rotation, Faded({ 255, 215, 150, 255 }, alpha));
    DrawBillboardPro(cam, tex, { 0, 0, (float)tex.width, (float)tex.height }, position, { 0, 1, 0 }, { size * 0.5f, size * 0.5f },
                     { size / 4, size / 4 }, rotation + 45.0f, Faded(WHITE, alpha));
    EndBlendMode();
    rlDrawRenderBatchActive();
    rlEnableDepthMask();
}

} // namespace Fx
