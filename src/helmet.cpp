#include "helmet.hpp"
#include "render.hpp"
#include "raymath.h"
#include "rlgl.h"
#include <cmath>
#include <vector>

static Mesh mesh{};
static Material material{};
static bool ready = false;
static Vector3 seat = { 0.0f, 1.71f, 0.03f }; // dome centre on the head, bind-pose model space
static Vector3 radii = { 0.115f, 0.11f, 0.135f };
static float restHeight = 0.04f;

static const Color OLIVE_DRAB = { 76, 80, 56, 255 };

// Fit the shell to the head: the top of the head-part vertices, and how wide and
// deep the skull is in its top 15 cm
static void FitToHead(const CharacterAssets* c) {
    if (!c || !c->loaded) return;
    float top = -1e9f;
    for (int m = 0; m < c->model.meshCount; m++) {
        if (c->meshPart[m] != PART_HEAD || !c->model.meshes[m].vertices) continue;
        const Mesh& me = c->model.meshes[m];
        for (int i = 0; i < me.vertexCount; i++) top = fmaxf(top, me.vertices[i * 3 + 1]);
    }
    if (top < 0.5f) return;
    Vector3 lo = { 1e9f, 1e9f, 1e9f }, hi = { -1e9f, -1e9f, -1e9f };
    for (int m = 0; m < c->model.meshCount; m++) {
        if (c->meshPart[m] != PART_HEAD || !c->model.meshes[m].vertices) continue;
        const Mesh& me = c->model.meshes[m];
        for (int i = 0; i < me.vertexCount; i++) {
            Vector3 v = { me.vertices[i * 3], me.vertices[i * 3 + 1], me.vertices[i * 3 + 2] };
            if (v.y < top - 0.15f) continue;
            lo = Vector3Min(lo, v);
            hi = Vector3Max(hi, v);
        }
    }
    // The rim sits a little above the brow at the front and lower at the back and sides
    seat = { (lo.x + hi.x) * 0.5f, top - 0.085f, (lo.z + hi.z) * 0.5f - 0.005f };
    radii = { (hi.x - lo.x) * 0.5f + 0.024f, 0.085f + 0.03f, (hi.z - lo.z) * 0.5f + 0.022f };
}

// The shell as one strip of rings: outer dome from the crown down to the rim, the
// flared lip, then the inside back up to the crown
static Mesh BuildMesh() {
    const int SLICES = 36, DOME_RINGS = 14;
    const float THICK = 0.007f;
    struct Ring { std::vector<Vector3> p, n; };
    std::vector<Ring> rings;
    auto rimTheta = [](float phi) { return (102.0f - 12.0f * cosf(phi)) * DEG2RAD; }; // deeper at the back
    auto ellipsoid = [&](float theta, float phi, Vector3 r) {
        return Vector3{ r.x * sinf(theta) * sinf(phi), r.y * cosf(theta), r.z * sinf(theta) * cosf(phi) };
    };
    auto normalOf = [](Vector3 p, Vector3 r) {
        return Vector3Normalize({ p.x / (r.x * r.x), p.y / (r.y * r.y), p.z / (r.z * r.z) });
    };
    Vector3 inner = { radii.x - THICK, radii.y - THICK, radii.z - THICK };
    // Outer dome
    for (int k = 0; k <= DOME_RINGS; k++) {
        Ring ring;
        for (int i = 0; i <= SLICES; i++) {
            float phi = 2.0f * PI * i / SLICES;
            float theta = rimTheta(phi) * k / DOME_RINGS;
            Vector3 p = ellipsoid(theta, phi, radii);
            ring.p.push_back(p);
            ring.n.push_back(k == 0 ? Vector3{ 0, 1, 0 } : normalOf(p, radii));
        }
        rings.push_back(ring);
    }
    // The lip: out and slightly down, then under and back in
    Ring lip, lipUnder;
    for (int i = 0; i <= SLICES; i++) {
        float phi = 2.0f * PI * i / SLICES;
        Vector3 rim = ellipsoid(rimTheta(phi), phi, radii);
        Vector3 out = { sinf(phi), 0.0f, cosf(phi) };
        Vector3 p = Vector3Add(rim, Vector3Add(Vector3Scale(out, 0.011f), { 0, -0.005f, 0 }));
        lip.p.push_back(p);
        lip.n.push_back(Vector3Normalize(Vector3Add(out, { 0, -0.6f, 0 })));
        lipUnder.p.push_back(Vector3Add(p, { 0, -0.003f, 0 }));
        lipUnder.n.push_back({ 0, -1, 0 });
    }
    rings.push_back(lip);
    rings.push_back(lipUnder);
    // Inside, from the rim back up to the crown, facing in
    for (int k = DOME_RINGS; k >= 0; k--) {
        Ring ring;
        for (int i = 0; i <= SLICES; i++) {
            float phi = 2.0f * PI * i / SLICES;
            float theta = rimTheta(phi) * k / DOME_RINGS;
            Vector3 p = ellipsoid(theta, phi, inner);
            ring.p.push_back(p);
            ring.n.push_back(Vector3Negate(k == 0 ? Vector3{ 0, 1, 0 } : normalOf(p, inner)));
        }
        rings.push_back(ring);
    }

    int cols = SLICES + 1, rows = (int)rings.size();
    Mesh m{};
    m.vertexCount = rows * cols;
    m.triangleCount = (rows - 1) * SLICES * 2;
    m.vertices = (float*)MemAlloc(m.vertexCount * 3 * sizeof(float));
    m.normals = (float*)MemAlloc(m.vertexCount * 3 * sizeof(float));
    m.texcoords = (float*)MemAlloc(m.vertexCount * 2 * sizeof(float));
    m.indices = (unsigned short*)MemAlloc(m.triangleCount * 3 * sizeof(unsigned short));
    float lowest = 0.0f;
    for (int r = 0; r < rows; r++) {
        for (int i = 0; i < cols; i++) {
            int v = r * cols + i;
            Vector3 p = rings[r].p[i], n = rings[r].n[i];
            m.vertices[v * 3] = p.x; m.vertices[v * 3 + 1] = p.y; m.vertices[v * 3 + 2] = p.z;
            m.normals[v * 3] = n.x; m.normals[v * 3 + 1] = n.y; m.normals[v * 3 + 2] = n.z;
            m.texcoords[v * 2] = (float)i / SLICES;
            m.texcoords[v * 2 + 1] = (float)r / (rows - 1);
            lowest = fminf(lowest, p.y);
        }
    }
    int t = 0;
    for (int r = 0; r + 1 < rows; r++) {
        for (int i = 0; i < SLICES; i++) {
            unsigned short a = (unsigned short)(r * cols + i), b = (unsigned short)(a + 1);
            unsigned short c = (unsigned short)(a + cols), d = (unsigned short)(c + 1);
            // Counter-clockwise seen from outside for the outer rows; the inner rows
            // run back up, so the same order faces inward there
            m.indices[t++] = a; m.indices[t++] = c; m.indices[t++] = b;
            m.indices[t++] = b; m.indices[t++] = c; m.indices[t++] = d;
        }
    }
    restHeight = -lowest;
    UploadMesh(&m, false);
    return m;
}

void InitHelmet(const CharacterAssets* character) {
    if (ready) UnloadHelmet();
    FitToHead(character);
    mesh = BuildMesh();
    material = LoadMaterialDefault();
    if (Render::Ready()) material.shader = Render::LitShader();
    material.maps[MATERIAL_MAP_DIFFUSE].color = OLIVE_DRAB;
    ready = true;
}

void UnloadHelmet() {
    if (!ready) return;
    UnloadMesh(mesh);
    material.shader = Shader{ rlGetShaderIdDefault(), rlGetShaderLocsDefault() }; // the lit shader isn't ours to free
    UnloadMaterial(material);
    ready = false;
}

void DrawHelmet(Matrix transform) {
    if (!ready) return;
    Render::SurfaceObject(0.35f); // painted steel: a soft sheen
    DrawMesh(mesh, material, transform);
}

Matrix HelmetOnHead(const CharacterAssets& character, CharAnim anim, int frame) {
    return MatrixMultiply(MatrixTranslate(seat.x, seat.y, seat.z), CharacterHeadMotion(character, anim, frame));
}

float HelmetRestHeight() { return restHeight; }
