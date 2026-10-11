#include "scenery_mesh.hpp"
#include "raymath.h"
#include <cmath>
#include <cstring>
#include <map>
#include <random>
#include <tuple>

namespace {

// Collects triangles; starts a new mesh before 16-bit indices run out
struct MeshBuilder {
    struct Chunk {
        std::vector<float> v, n, uv;
        std::vector<unsigned char> col;
        std::vector<unsigned short> idx;
    };
    Color tint = WHITE;   // colour given to the vertices added from now on
    std::vector<Chunk> chunks{ 1 };
    static constexpr int MAX_VERTS = 60000;

    // Room for `count` more vertices in the current chunk; returns its first index
    unsigned short Reserve(int count) {
        if ((int)(chunks.back().v.size() / 3) + count > MAX_VERTS) chunks.emplace_back();
        return (unsigned short)(chunks.back().v.size() / 3);
    }
    void Vertex(Vector3 p, Vector3 n, Vector2 uv) {
        Chunk& c = chunks.back();
        c.v.insert(c.v.end(), { p.x, p.y, p.z });
        c.n.insert(c.n.end(), { n.x, n.y, n.z });
        c.uv.insert(c.uv.end(), { uv.x, uv.y });
        c.col.insert(c.col.end(), { tint.r, tint.g, tint.b, tint.a });
    }
    void Tri(unsigned short a, unsigned short b, unsigned short c) { chunks.back().idx.insert(chunks.back().idx.end(), { a, b, c }); }
    // A quad, counter-clockwise seen from the side its normal faces: a b c d
    void Quad(Vector3 a, Vector3 b, Vector3 c, Vector3 d, Vector3 n, Vector2 ua, Vector2 ub, Vector2 uc, Vector2 ud) {
        unsigned short base = Reserve(4);
        Vertex(a, n, ua); Vertex(b, n, ub); Vertex(c, n, uc); Vertex(d, n, ud);
        Tri(base, base + 1, base + 2);
        Tri(base, base + 2, base + 3);
    }
    void Triangle(Vector3 a, Vector3 b, Vector3 c, Vector3 n, Vector2 ua, Vector2 ub, Vector2 uc) {
        unsigned short base = Reserve(3);
        Vertex(a, n, ua); Vertex(b, n, ub); Vertex(c, n, uc);
        Tri(base, base + 1, base + 2);
    }
    // A quad (corners in order round its edge, either way) facing toward `facing`;
    // its geometric normal is used for lighting
    void QuadFacing(Vector3 a, Vector3 b, Vector3 c, Vector3 d, Vector3 facing, Vector2 ua, Vector2 ub, Vector2 uc, Vector2 ud) {
        Vector3 n = Vector3Normalize(Vector3CrossProduct(Vector3Subtract(b, a), Vector3Subtract(d, a)));
        if (Vector3DotProduct(n, facing) < 0.0f) Quad(d, c, b, a, Vector3Negate(n), ud, uc, ub, ua);
        else Quad(a, b, c, d, n, ua, ub, uc, ud);
    }
    void TriFacing(Vector3 a, Vector3 b, Vector3 c, Vector3 facing, Vector2 ua, Vector2 ub, Vector2 uc) {
        Vector3 n = Vector3Normalize(Vector3CrossProduct(Vector3Subtract(b, a), Vector3Subtract(c, a)));
        if (Vector3DotProduct(n, facing) < 0.0f) Triangle(c, b, a, Vector3Negate(n), uc, ub, ua);
        else Triangle(a, b, c, n, ua, ub, uc);
    }

    std::vector<SceneryMeshes::Piece> Upload(bool background) {
        std::vector<SceneryMeshes::Piece> out;
        for (Chunk& c : chunks) {
            if (c.idx.empty()) continue;
            BoundingBox bb = { { 1e30f, 1e30f, 1e30f }, { -1e30f, -1e30f, -1e30f } };
            for (size_t i = 0; i < c.v.size(); i += 3) {
                bb.min = Vector3Min(bb.min, { c.v[i], c.v[i + 1], c.v[i + 2] });
                bb.max = Vector3Max(bb.max, { c.v[i], c.v[i + 1], c.v[i + 2] });
            }
            Mesh m{};
            m.vertexCount = (int)(c.v.size() / 3);
            m.triangleCount = (int)(c.idx.size() / 3);
            m.vertices = (float*)MemAlloc((unsigned)(c.v.size() * sizeof(float)));
            m.normals = (float*)MemAlloc((unsigned)(c.n.size() * sizeof(float)));
            m.texcoords = (float*)MemAlloc((unsigned)(c.uv.size() * sizeof(float)));
            m.indices = (unsigned short*)MemAlloc((unsigned)(c.idx.size() * sizeof(unsigned short)));
            m.colors = (unsigned char*)MemAlloc((unsigned)c.col.size());
            memcpy(m.colors, c.col.data(), c.col.size());
            memcpy(m.vertices, c.v.data(), c.v.size() * sizeof(float));
            memcpy(m.normals, c.n.data(), c.n.size() * sizeof(float));
            memcpy(m.texcoords, c.uv.data(), c.uv.size() * sizeof(float));
            memcpy(m.indices, c.idx.data(), c.idx.size() * sizeof(unsigned short));
            UploadMesh(&m, false);
            out.push_back({ m, bb, background });
        }
        return out;
    }
};

float TileOf(Surface s) {
    switch (s) {
        case Surface::Stone: return 2.5f;
        case Surface::Bark: return 2.0f;
        case Surface::Shingles: return 2.0f;
        case Surface::Sand: return 4.0f;
        default: return 2.0f;
    }
}

// A tapered tube along a path of rings (centre, radius); u around, v along. Optional caps.
void Tube(MeshBuilder& b, const std::vector<Vector3>& centres, const std::vector<float>& radii, int sides, float tile,
          float aroundRepeats, bool caps) {
    if (centres.size() < 2) return;
    // A frame that follows the path
    std::vector<Vector3> axisX(centres.size()), axisZ(centres.size());
    for (size_t i = 0; i < centres.size(); i++) {
        Vector3 dir = Vector3Normalize(Vector3Subtract(centres[std::min(i + 1, centres.size() - 1)], centres[i > 0 ? i - 1 : 0]));
        Vector3 ref = fabsf(dir.y) < 0.9f ? Vector3{ 0, 1, 0 } : Vector3{ 1, 0, 0 };
        axisX[i] = Vector3Normalize(Vector3CrossProduct(dir, ref));
        axisZ[i] = Vector3CrossProduct(axisX[i], dir);
    }
    float along = 0.0f;
    for (size_t i = 0; i + 1 < centres.size(); i++) {
        float len = Vector3Distance(centres[i], centres[i + 1]);
        unsigned short base = b.Reserve((sides + 1) * 2);
        for (int ring = 0; ring < 2; ring++) {
            size_t k = i + ring;
            float v = -(along + (ring ? len : 0.0f)) / tile;
            for (int s = 0; s <= sides; s++) {
                float a = 2.0f * PI * s / sides;
                Vector3 n = Vector3Add(Vector3Scale(axisX[k], cosf(a)), Vector3Scale(axisZ[k], sinf(a)));
                b.Vertex(Vector3Add(centres[k], Vector3Scale(n, radii[k])), n, { aroundRepeats * s / sides, v });
            }
        }
        for (int s = 0; s < sides; s++) {
            unsigned short a0 = base + s, a1 = base + s + 1, c0 = base + sides + 1 + s, c1 = c0 + 1;
            b.Tri(a0, c0, c1);
            b.Tri(a0, c1, a1);
        }
        along += len;
    }
    if (!caps) return;
    for (int end = 0; end < 2; end++) {
        size_t k = end ? centres.size() - 1 : 0;
        Vector3 dir = Vector3Normalize(Vector3Subtract(centres[end ? k : 1], centres[end ? k - 1 : 0]));
        Vector3 n = end ? dir : Vector3Negate(dir);
        unsigned short base = b.Reserve(sides + 1);
        b.Vertex(centres[k], n, { 0.5f, 0.5f });
        for (int s = 0; s < sides; s++) {
            float a = 2.0f * PI * s / sides;
            Vector3 r = Vector3Add(Vector3Scale(axisX[k], cosf(a)), Vector3Scale(axisZ[k], sinf(a)));
            b.Vertex(Vector3Add(centres[k], Vector3Scale(r, radii[k])), n, { 0.5f + 0.4f * cosf(a), 0.5f + 0.4f * sinf(a) });
        }
        for (int s = 0; s < sides; s++) {
            unsigned short p = base + 1 + s, q = base + 1 + (s + 1) % sides;
            if (end) b.Tri(base, q, p);
            else b.Tri(base, p, q);
        }
    }
}

// --- Trees -----------------------------------------------------------------------------

void Tree(MeshBuilder& bark, MeshBuilder& leaves, const Scenery::Tree& t) {
    std::mt19937 rng(t.seed);
    auto rand = [&](float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(rng); };

    // Trunk: flared at the foot, tapering, a gentle lean that wanders
    const int sides = t.background ? 6 : 8;
    std::vector<Vector3> centres;
    std::vector<float> radii;
    float lean = rand(0.0f, 0.7f), leanDir = rand(0, 2 * PI);
    float topY = t.crownBottom + (t.height - t.crownBottom) * 0.65f;
    const int segs = t.background ? 3 : 6;
    for (int i = 0; i <= segs; i++) {
        float f = (float)i / segs;
        float y = f * topY;
        float off = lean * f * f;
        float wx = i > 0 && i < segs ? rand(-0.14f, 0.14f) : 0.0f, wz = i > 0 && i < segs ? rand(-0.14f, 0.14f) : 0.0f;
        centres.push_back({ t.base.x + cosf(leanDir) * off + wx, y, t.base.z + sinf(leanDir) * off + wz });
        float r = t.trunkRadius * Lerp(1.0f, 0.42f, f);
        if (i == 0) r *= 1.35f; // root flare
        radii.push_back(r);
    }
    centres.insert(centres.begin() + 1, Vector3{ centres[0].x, 0.45f, centres[0].z });
    radii.insert(radii.begin() + 1, t.trunkRadius * 1.08f);
    Tube(bark, centres, radii, sides, 1.3f, 3.0f, false);
    Vector3 top = centres.back();

    // Branches: a few limbs out from the top of the clear trunk into the crown
    if (!t.background) {
        int limbs = 4 + (int)rand(0, 2.99f);
        for (int i = 0; i < limbs; i++) {
            float y = t.crownBottom + rand(-0.6f, 1.6f);
            float f = Clamp(y / topY, 0.0f, 1.0f);
            Vector3 from = { t.base.x + cosf(leanDir) * lean * f * f, y, t.base.z + sinf(leanDir) * lean * f * f };
            float a = t.yaw + i * 2 * PI / limbs + rand(-0.4f, 0.4f);
            float reach = t.crownRadius * rand(0.55f, 0.85f);
            Vector3 mid = Vector3Add(from, { cosf(a) * reach * 0.5f, rand(0.6f, 1.2f), sinf(a) * reach * 0.5f });
            Vector3 to = Vector3Add(from, { cosf(a) * reach, rand(1.4f, 2.6f), sinf(a) * reach });
            float r = t.trunkRadius * rand(0.28f, 0.4f);
            Tube(bark, { from, mid, to }, { r, r * 0.7f, r * 0.35f }, 5, 2.0f, 1.0f, false);
        }
    }

    // Crown: leaf cards scattered through an ellipsoid, thicker toward its surface
    Vector3 centre = { top.x, (t.crownBottom + t.height) * 0.5f + 0.5f, top.z };
    float R = t.crownRadius, V = (t.height - t.crownBottom) * 0.5f;
    int cards = t.background ? 28 : 64;
    // Each tree a slightly different green
    float hue = rand(-1, 1), bright = rand(0.82f, 1.08f);
    for (int i = 0; i < cards; i++) {
        Vector3 d;
        do { d = { rand(-1, 1), rand(-1, 1), rand(-1, 1) }; } while (Vector3LengthSqr(d) > 1.0f || Vector3LengthSqr(d) < 0.02f);
        float lenD = Vector3Length(d);
        Vector3 dir = Vector3Scale(d, 1.0f / lenD);
        float f = powf(rand(0, 1), 0.3f);
        // Lumpy: some sectors of the crown reach further than others
        float lump = 0.85f + 0.3f * sinf(dir.x * 3.1f + t.seed % 7) * cosf(dir.z * 2.7f + t.seed % 5);
        Vector3 p = { centre.x + dir.x * R * f * lump, centre.y + dir.y * V * f, centre.z + dir.z * R * f * lump };
        float size = rand(1.3f, 2.2f) * (t.background ? 1.4f : 1.0f);
        // Card facing: any way at all, a little toward the outside and the sky
        Vector3 rnd;
        do { rnd = { rand(-1, 1), rand(-1, 1), rand(-1, 1) }; } while (Vector3LengthSqr(rnd) > 1.0f || Vector3LengthSqr(rnd) < 0.05f);
        Vector3 n = Vector3Normalize(Vector3Add(Vector3Normalize(rnd), Vector3Add(Vector3Scale(dir, 0.35f), { 0, 0.25f, 0 })));
        // Deeper in the crown is darker (less sky reaches it), and greens vary card to card
        float shadeK = bright * Lerp(0.62f, 1.0f, f) * rand(0.9f, 1.05f);
        leaves.tint = { (unsigned char)Clamp(255 * shadeK * (1.0f + 0.06f * hue), 0, 255), (unsigned char)Clamp(255 * shadeK, 0, 255),
                        (unsigned char)Clamp(255 * shadeK * (1.0f - 0.1f * hue), 0, 255), 255 };
        Vector3 ref = fabsf(n.y) < 0.95f ? Vector3{ 0, 1, 0 } : Vector3{ 1, 0, 0 };
        Vector3 tx = Vector3Normalize(Vector3CrossProduct(ref, n));
        Vector3 ty = Vector3CrossProduct(n, tx);
        float spin = rand(0, 2 * PI);
        Vector3 ax = Vector3Add(Vector3Scale(tx, cosf(spin)), Vector3Scale(ty, sinf(spin)));
        Vector3 ay = Vector3Add(Vector3Scale(tx, -sinf(spin)), Vector3Scale(ty, cosf(spin)));
        ax = Vector3Scale(ax, size / 2);
        ay = Vector3Scale(ay, size / 2);
        // Light it as part of a round crown, not as a flat card
        Vector3 shade = Vector3Normalize(Vector3Add(dir, { 0, 0.35f, 0 }));
        leaves.Quad(Vector3Subtract(Vector3Subtract(p, ax), ay), Vector3Subtract(Vector3Add(p, ax), ay), Vector3Add(Vector3Add(p, ax), ay),
                    Vector3Add(Vector3Subtract(p, ax), ay), shade, { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 });
    }
}

// --- Round walls ------------------------------------------------------------------------

void RingMesh(MeshBuilder& b, const Scenery::Ring& r) {
    const float tile = TileOf(r.surface);
    int segs = std::max(2, (int)ceilf((r.to - r.from) * r.outer / 0.6f));
    auto at = [&](float a, float rad, float y) { return Vector3{ r.center.x + sinf(a) * rad, y, r.center.z + cosf(a) * rad }; };
    for (int i = 0; i < segs; i++) {
        float a0 = r.from + (r.to - r.from) * i / segs, a1 = r.from + (r.to - r.from) * (i + 1) / segs;
        Vector3 n0 = { sinf(a0), 0, cosf(a0) }, n1 = { sinf(a1), 0, cosf(a1) };
        Vector3 mid = { sinf((a0 + a1) / 2), 0, cosf((a0 + a1) / 2) };
        // Outer face, inner face, top
        b.QuadFacing(at(a0, r.outer, r.bottom), at(a1, r.outer, r.bottom), at(a1, r.outer, r.top), at(a0, r.outer, r.top), mid,
                     { a0 * r.outer / tile, -r.bottom / tile }, { a1 * r.outer / tile, -r.bottom / tile }, { a1 * r.outer / tile, -r.top / tile },
                     { a0 * r.outer / tile, -r.top / tile });
        b.QuadFacing(at(a0, r.inner, r.bottom), at(a1, r.inner, r.bottom), at(a1, r.inner, r.top), at(a0, r.inner, r.top), Vector3Negate(mid),
                     { a0 * r.inner / tile, -r.bottom / tile }, { a1 * r.inner / tile, -r.bottom / tile }, { a1 * r.inner / tile, -r.top / tile },
                     { a0 * r.inner / tile, -r.top / tile });
        b.QuadFacing(at(a0, r.inner, r.top), at(a1, r.inner, r.top), at(a1, r.outer, r.top), at(a0, r.outer, r.top), { 0, 1, 0 },
                     { a0 * r.inner / tile, r.inner / tile }, { a1 * r.inner / tile, r.inner / tile }, { a1 * r.outer / tile, r.outer / tile },
                     { a0 * r.outer / tile, r.outer / tile });
        (void)n0; (void)n1;
    }
    // Ends
    for (int end = 0; end < 2; end++) {
        float a = end ? r.to : r.from;
        Vector3 t = { cosf(a), 0, -sinf(a) }; // along the ring, toward increasing angle
        Vector3 n = end ? t : Vector3Negate(t);
        Vector3 p0 = at(a, r.inner, r.bottom), p1 = at(a, r.outer, r.bottom), p2 = at(a, r.outer, r.top), p3 = at(a, r.inner, r.top);
        Vector2 u0 = { r.inner / tile, -r.bottom / tile }, u1 = { r.outer / tile, -r.bottom / tile }, u2 = { r.outer / tile, -r.top / tile },
                u3 = { r.inner / tile, -r.top / tile };
        b.QuadFacing(p0, p1, p2, p3, n, u0, u1, u2, u3);
    }
}

// --- Roofs ------------------------------------------------------------------------------

void RoofMesh(MeshBuilder& roof, MeshBuilder& gable, const Scenery::Roof& r) {
    const float tile = TileOf(r.surface), thick = 0.14f;
    const float top = r.base + r.rise;
    auto slope = [&](Vector3 eaveA, Vector3 eaveB, Vector3 ridgeB, Vector3 ridgeA) {
        // Upper side, then the underside a little lower facing down, then the eave's edge
        float len = Vector3Distance(eaveA, eaveB), run = Vector3Distance(eaveA, ridgeA);
        Vector3 out = Vector3Subtract(Vector3Lerp(eaveA, eaveB, 0.5f), Vector3Lerp(ridgeA, ridgeB, 0.5f));
        out.y = 0.0f;
        out = Vector3Normalize(out);
        Vector3 up = Vector3Add(out, { 0, 1.5f, 0 });
        roof.QuadFacing(eaveA, eaveB, ridgeB, ridgeA, up, { 0, run / tile }, { len / tile, run / tile }, { len / tile, 0 }, { 0, 0 });
        Vector3 down = { 0, -thick, 0 };
        roof.QuadFacing(Vector3Add(ridgeA, down), Vector3Add(ridgeB, down), Vector3Add(eaveB, down), Vector3Add(eaveA, down),
                        Vector3Negate(up), { 0, 0 }, { len / tile, 0 }, { len / tile, run / tile }, { 0, run / tile });
        roof.QuadFacing(Vector3Add(eaveA, down), Vector3Add(eaveB, down), eaveB, eaveA, out, { 0, 0.1f }, { len / tile, 0.1f },
                        { len / tile, 0 }, { 0, 0 });
    };
    if (r.shape == Scenery::Roof::RidgeX) {
        float zm = (r.z0 + r.z1) / 2;
        slope({ r.x1, r.base, r.z0 }, { r.x0, r.base, r.z0 }, { r.x0, top, zm }, { r.x1, top, zm });
        slope({ r.x0, r.base, r.z1 }, { r.x1, r.base, r.z1 }, { r.x1, top, zm }, { r.x0, top, zm });
        // Gable ends, set in under the overhang
        float inset = 0.4f;
        for (int end = 0; end < 2; end++) {
            float x = end ? r.x1 - inset : r.x0 + inset;
            float zi0 = r.z0 + inset, zi1 = r.z1 - inset, yi = r.base + r.rise * inset / ((r.z1 - r.z0) / 2);
            Vector3 a = { x, r.base, zi0 }, b = { x, r.base, zi1 }, c = { x, top - (top - r.base) * inset / ((r.z1 - r.z0) / 2) * 0.0f - 0.05f, zm };
            (void)yi;
            Vector3 n = { end ? 1.0f : -1.0f, 0, 0 };
            float h = c.y - r.base;
            gable.TriFacing(a, b, c, n, { zi0 / 2, 0 }, { zi1 / 2, 0 }, { zm / 2, -h / 2 });
        }
    } else if (r.shape == Scenery::Roof::RidgeZ) {
        float xm = (r.x0 + r.x1) / 2;
        slope({ r.x0, r.base, r.z0 }, { r.x0, r.base, r.z1 }, { xm, top, r.z1 }, { xm, top, r.z0 });
        slope({ r.x1, r.base, r.z1 }, { r.x1, r.base, r.z0 }, { xm, top, r.z0 }, { xm, top, r.z1 });
        float inset = 0.4f;
        for (int end = 0; end < 2; end++) {
            float z = end ? r.z1 - inset : r.z0 + inset;
            Vector3 a = { r.x0 + inset, r.base, z }, b = { r.x1 - inset, r.base, z }, c = { xm, top - 0.05f, z };
            Vector3 n = { 0, 0, end ? 1.0f : -1.0f };
            float h = c.y - r.base;
            gable.TriFacing(a, b, c, n, { a.x / 2, 0 }, { b.x / 2, 0 }, { xm / 2, -h / 2 });
        }
    } else {
        // Pyramid: four triangles up to the apex, with undersides
        Vector3 apex = { (r.x0 + r.x1) / 2, top, (r.z0 + r.z1) / 2 };
        Vector3 c[4] = { { r.x0, r.base, r.z0 }, { r.x1, r.base, r.z0 }, { r.x1, r.base, r.z1 }, { r.x0, r.base, r.z1 } };
        for (int i = 0; i < 4; i++) {
            Vector3 a = c[(i + 1) % 4], b = c[i];
            Vector3 out = Vector3Subtract(Vector3Lerp(a, b, 0.5f), apex);
            out.y = 0.0f;
            out = Vector3Normalize(out);
            Vector3 up = Vector3Add(out, { 0, 1.5f, 0 });
            float len = Vector3Distance(a, b), run = Vector3Distance(Vector3Lerp(a, b, 0.5f), apex);
            roof.TriFacing(a, b, apex, up, { 0, run / tile }, { len / tile, run / tile }, { len / 2 / tile, 0 });
            Vector3 down = { 0, -thick, 0 };
            roof.TriFacing(Vector3Add(b, down), Vector3Add(a, down), Vector3Add(apex, down), Vector3Negate(up), { 0, run / tile },
                           { len / tile, run / tile }, { len / 2 / tile, 0 });
            roof.QuadFacing(Vector3Add(b, down), Vector3Add(a, down), a, b, out, { 0, 0.1f }, { len / tile, 0.1f }, { len / tile, 0 }, { 0, 0 });
        }
    }
}

// --- Beams ------------------------------------------------------------------------------

void BeamMesh(MeshBuilder& b, const Scenery::Beam& beam) {
    const float tile = TileOf(beam.surface);
    if (beam.round) {
        Tube(b, { beam.from, beam.to }, { beam.width / 2, beam.width / 2 }, 10, tile, 1.0f, true);
        return;
    }
    Vector3 dir = Vector3Normalize(Vector3Subtract(beam.to, beam.from));
    Vector3 ref = fabsf(dir.y) < 0.9f ? Vector3{ 0, 1, 0 } : Vector3{ 1, 0, 0 };
    Vector3 x = Vector3Scale(Vector3Normalize(Vector3CrossProduct(dir, ref)), beam.width / 2);
    Vector3 y = Vector3Scale(Vector3Normalize(Vector3CrossProduct(x, dir)), beam.width / 2);
    float len = Vector3Distance(beam.from, beam.to);
    Vector3 corners[4] = { Vector3Add(x, y), Vector3Subtract(y, x), Vector3Negate(Vector3Add(x, y)), Vector3Subtract(x, y) };
    for (int i = 0; i < 4; i++) {
        Vector3 c0 = corners[i], c1 = corners[(i + 1) % 4];
        Vector3 a = Vector3Add(beam.from, c0), bb = Vector3Add(beam.from, c1), c = Vector3Add(beam.to, c1), d = Vector3Add(beam.to, c0);
        // Grain runs along the beam
        b.QuadFacing(a, bb, c, d, Vector3Add(c0, c1), { 0, 0 }, { 0, 0.1f }, { len / tile, 0.1f }, { len / tile, 0 });
    }
    b.QuadFacing(Vector3Add(beam.from, corners[3]), Vector3Add(beam.from, corners[2]), Vector3Add(beam.from, corners[1]),
                 Vector3Add(beam.from, corners[0]), Vector3Negate(dir), { 0, 0 }, { 0.1f, 0 }, { 0.1f, 0.1f }, { 0, 0.1f });
    b.QuadFacing(Vector3Add(beam.to, corners[0]), Vector3Add(beam.to, corners[1]), Vector3Add(beam.to, corners[2]),
                 Vector3Add(beam.to, corners[3]), dir, { 0, 0 }, { 0.1f, 0 }, { 0.1f, 0.1f }, { 0, 0.1f });
}

} // namespace

SceneryMeshes BuildSceneryMeshes(const Scenery& s) {
    // Trees go into 32 m squares (and background trees into their own), everything else by surface
    struct Key {
        int surface, cx, cz, background;
        bool operator<(const Key& o) const {
            return std::tie(surface, cx, cz, background) < std::tie(o.surface, o.cx, o.cz, o.background);
        }
    };
    const int LEAVES = -1;
    std::map<Key, MeshBuilder> builders;
    const float cell = 32.0f;
    for (const Scenery::Tree& t : s.trees) {
        int cx = (int)floorf(t.base.x / cell), cz = (int)floorf(t.base.z / cell), bg = t.background ? 1 : 0;
        Tree(builders[{ (int)Surface::Bark, cx, cz, bg }], builders[{ LEAVES, cx, cz, bg }], t);
    }
    for (const Scenery::Ring& r : s.rings) RingMesh(builders[{ (int)r.surface, 0, 0, 0 }], r);
    for (const Scenery::Roof& r : s.roofs) RoofMesh(builders[{ (int)r.surface, 0, 0, 0 }], builders[{ (int)Surface::Wood, 0, 0, 0 }], r);
    for (const Scenery::Beam& b : s.beams) BeamMesh(builders[{ (int)b.surface, 0, 0, 0 }], b);
    SceneryMeshes out;
    for (auto& [key, builder] : builders) {
        std::vector<SceneryMeshes::Piece> pieces = builder.Upload(key.background != 0);
        if (key.surface == LEAVES) {
            out.leaves.insert(out.leaves.end(), pieces.begin(), pieces.end());
            continue;
        }
        SceneryMeshes::Group* g = nullptr;
        for (SceneryMeshes::Group& existing : out.solids)
            if ((int)existing.surface == key.surface) g = &existing;
        if (!g) { out.solids.push_back({ (Surface)key.surface, {} }); g = &out.solids.back(); }
        g->pieces.insert(g->pieces.end(), pieces.begin(), pieces.end());
    }
    return out;
}
