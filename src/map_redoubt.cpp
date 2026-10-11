// Map: "Redoubt", a long woodland map. Team A spawns at the south end (-Z),
// team B at the north (+Z); everything on team A's half has a twin turned half
// a circle about the centre, so both sides get the same map. Top view (X across,
// Z up the page; the left of the design sketch is the bottom here):
//
//   z = +120  [ B spawn compound ]                  B sniper tower (x+, z+86)
//             B forest (in front of B, and the x- corner)
//   z = +20   B mid building (x-)       A's flank hut (x+, z+25)
//             ----- ring road --  ( FORT )  -- ring road -----
//   z = -20   B's flank hut (x-, z-25)  A mid building (x+)
//             A forest (in front of A, and the x+ corner)        farmland lanes
//   z = -120  [ A spawn compound ]   A sniper tower (x-, z-86)   along both long edges
//
// - The fort is a round stone redoubt in the exact centre: a chest-high wall with a
//   fire step inside, two gates, breaches, and a raised platform in the middle.
// - Each team's forest fills the ground in front of its spawn: tall trunks, crowns
//   high overhead, nothing underneath. Roads wind through it.
// - The long edges are open farmland: the fast, exposed flanks. A checkpoint hut
//   sits where each flank meets the middle.
// - Each team has a two-storey stone house near the middle, and a stair tower in its
//   back corner whose lookout platform is above the treetops.
#include "world.hpp"
#include "raymath.h"
#include <algorithm>
#include <cmath>
#include <random>

namespace {

const float HALF_W = 32.0f, HALF_L = 120.0f;

Color PlainColor(Surface s) {
    switch (s) {
        case Surface::Stone: return { 150, 146, 136, 255 };
        case Surface::Logs: return { 130, 96, 64, 255 };
        case Surface::Shingles: return { 100, 82, 64, 255 };
        case Surface::Bark: return { 80, 68, 56, 255 };
        case Surface::Wood: return { 130, 105, 80, 255 };
        case Surface::Plaster: return { 214, 196, 162, 255 };
        case Surface::Crate: return { 180, 140, 90, 255 };
        case Surface::Sand: return { 200, 180, 120, 255 };
        default: return GRAY;
    }
}

struct Polygon { std::vector<Vector2> p; }; // (x, z)

bool Inside(const Polygon& poly, float x, float z) {
    bool in = false;
    for (size_t i = 0, j = poly.p.size() - 1; i < poly.p.size(); j = i++) {
        const Vector2 &a = poly.p[i], &b = poly.p[j];
        if ((a.y > z) != (b.y > z) && x < (b.x - a.x) * (z - a.y) / (b.y - a.y) + a.x) in = !in;
    }
    return in;
}

float SegmentDistance(Vector2 p, Vector2 a, Vector2 b) {
    Vector2 ab = Vector2Subtract(b, a);
    float t = Clamp(Vector2DotProduct(Vector2Subtract(p, a), ab) / fmaxf(Vector2DotProduct(ab, ab), 1e-6f), 0.0f, 1.0f);
    return Vector2Distance(p, Vector2Add(a, Vector2Scale(ab, t)));
}

// Signed distance to a polygon's edge: positive inside
float PolygonDistance(const Polygon& poly, float x, float z) {
    float d = 1e9f;
    for (size_t i = 0, j = poly.p.size() - 1; i < poly.p.size(); j = i++) d = fminf(d, SegmentDistance({ x, z }, poly.p[j], poly.p[i]));
    return Inside(poly, x, z) ? d : -d;
}

struct Road {
    std::vector<Vector2> points; // (x, z)
    float width;
    float Distance(float x, float z) const {
        float d = 1e9f;
        for (size_t i = 0; i + 1 < points.size(); i++) d = fminf(d, SegmentDistance({ x, z }, points[i], points[i + 1]));
        return d;
    }
};

struct Rect { float x0, x1, z0, z1; };

Rect Rot(Rect r) { return { -r.x1, -r.x0, -r.z1, -r.z0 }; }
Polygon Rot(const Polygon& poly) {
    Polygon out;
    for (Vector2 v : poly.p) out.p.push_back({ -v.x, -v.y });
    return out;
}
Road Rot(const Road& r) {
    Road out = r;
    for (Vector2& v : out.points) v = { -v.x, -v.y };
    return out;
}

// Smooth value noise for organic edges (not tileable; deterministic)
float Hash(int x, int z) {
    unsigned h = (unsigned)x * 374761393u + (unsigned)z * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return (float)((h ^ (h >> 16)) & 0xffff) / 65535.0f;
}
float Noise(float x, float z) {
    int ix = (int)floorf(x), iz = (int)floorf(z);
    float fx = x - ix, fz = z - iz;
    fx = fx * fx * (3 - 2 * fx);
    fz = fz * fz * (3 - 2 * fz);
    float a = Lerp(Hash(ix, iz), Hash(ix + 1, iz), fx), b = Lerp(Hash(ix, iz + 1), Hash(ix + 1, iz + 1), fx);
    return Lerp(a, b, fz);
}
float Fbm(float x, float z) { return 0.5f * Noise(x, z) + 0.3f * Noise(x * 2.1f, z * 2.1f) + 0.2f * Noise(x * 4.3f, z * 4.3f); }

struct Builder {
    World w;
    std::shared_ptr<Scenery> sc = std::make_shared<Scenery>();
    std::mt19937 rng{ 1234 };
    float Rand(float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(rng); }

    // --- Boxes ---
    void Add(float x0, float x1, float y0, float y1, float z0, float z1, Surface s, uint8_t flags = 0) {
        if (x1 - x0 < 1e-4f || y1 - y0 < 1e-4f || z1 - z0 < 1e-4f) return;
        WorldBox b;
        b.position = { (x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2 };
        b.size = { x1 - x0, y1 - y0, z1 - z0 };
        b.color = PlainColor(s);
        b.surface = s;
        b.flags = flags;
        w.push_back(b);
    }
    void Twin(float x0, float x1, float y0, float y1, float z0, float z1, Surface s, uint8_t flags = 0) {
        Add(x0, x1, y0, y1, z0, z1, s, flags);
        Add(-x1, -x0, y0, y1, -z1, -z0, s, flags);
    }
    // A wall slab minus rectangular openings. alongX: the wall runs along X (a = x, the
    // slab is [t0, t1] in z); otherwise it runs along Z (a = z, slab [t0, t1] in x).
    struct Hole { float a0, a1, y0, y1; };
    void Wall(bool alongX, float t0, float t1, float a0, float a1, float y0, float y1, std::vector<Hole> holes, Surface s) {
        std::vector<float> as = { a0, a1 }, ys = { y0, y1 };
        for (const Hole& h : holes) {
            as.push_back(Clamp(h.a0, a0, a1)); as.push_back(Clamp(h.a1, a0, a1));
            ys.push_back(Clamp(h.y0, y0, y1)); ys.push_back(Clamp(h.y1, y0, y1));
        }
        std::sort(as.begin(), as.end()); as.erase(std::unique(as.begin(), as.end()), as.end());
        std::sort(ys.begin(), ys.end()); ys.erase(std::unique(ys.begin(), ys.end()), ys.end());
        for (size_t i = 0; i + 1 < as.size(); i++) {
            float am = (as[i] + as[i + 1]) / 2;
            // Runs of solid cells up this column become one box
            float runStart = -1;
            for (size_t k = 0; k + 1 < ys.size(); k++) {
                float ym = (ys[k] + ys[k + 1]) / 2;
                bool hole = false;
                for (const Hole& h : holes) hole |= am > h.a0 && am < h.a1 && ym > h.y0 && ym < h.y1;
                if (!hole && runStart < 0) runStart = ys[k];
                if ((hole || k + 2 == ys.size()) && runStart >= 0) {
                    float runEnd = hole ? ys[k] : ys[k + 1];
                    if (alongX) Twin(as[i], as[i + 1], runStart, runEnd, t0, t1, s);
                    else Twin(t0, t1, runStart, runEnd, as[i], as[i + 1], s);
                    runStart = -1;
                }
            }
        }
    }
    void Crate(float x, float z, float size, float top = 0.0f) {
        Twin(x - size / 2, x + size / 2, 0, size, z - size / 2, z + size / 2, Surface::Crate);
        if (top > 0.0f) Twin(x - top / 2 + 0.1f, x + top / 2 + 0.1f, size, size + top, z - top / 2, z + top / 2, Surface::Crate);
    }
    void Rock(float x, float z, float sx, float sy, float sz) {
        Twin(x - sx / 2, x + sx / 2, 0, sy, z - sz / 2, z + sz / 2, Surface::Stone);
        Twin(x - sx * 0.35f, x + sx * 0.3f, sy, sy * 1.35f, z - sz * 0.3f, z + sz * 0.35f, Surface::Stone); // a rounder top
    }

    // --- Scenery (each added with its twin) ---
    void Roof(Rect r, float base, float rise, Scenery::Roof::Shape shape, Surface s) {
        sc->roofs.push_back({ r.x0, r.x1, r.z0, r.z1, base, rise, shape, s });
        Rect t = Rot(r);
        sc->roofs.push_back({ t.x0, t.x1, t.z0, t.z1, base, rise, shape, s });
    }
    void Beam(Vector3 a, Vector3 b, float width, Surface s, bool round = false) {
        sc->beams.push_back({ a, b, width, s, round });
        sc->beams.push_back({ { -a.x, a.y, -a.z }, { -b.x, b.y, -b.z }, width, s, round });
    }
    void Ring(float inner, float outer, float bottom, float top, float from, float to, Surface s, bool collide = true) {
        sc->rings.push_back({ { 0, 0, 0 }, inner, outer, bottom, top, from, to, s });
        sc->rings.push_back({ { 0, 0, 0 }, inner, outer, bottom, top, from + PI, to + PI, s });
        if (!collide) return;
        // Collision: small square blocks along the ring's middle (hidden; the ring mesh is what's seen)
        float mid = (inner + outer) / 2, block = (outer - inner) * 0.85f;
        int count = std::max(1, (int)ceilf((to - from) * mid / (block * 0.7f)));
        for (int i = 0; i <= count; i++) {
            float a = from + (to - from) * i / count;
            float x = sinf(a) * mid, z = cosf(a) * mid;
            Twin(x - block / 2, x + block / 2, bottom, top, z - block / 2, z + block / 2, s, BOX_HIDDEN);
        }
    }
    // A fallen log lying along X or Z (collision box + a round log)
    void FallenLog(float x, float z, float length, float radius, bool alongX) {
        float hx = alongX ? length / 2 : radius, hz = alongX ? radius : length / 2;
        Twin(x - hx, x + hx, 0, radius * 1.7f, z - hz, z + hz, Surface::Bark, BOX_HIDDEN);
        Vector3 a = alongX ? Vector3{ x - hx, radius * 0.85f, z } : Vector3{ x, radius * 0.85f, z - hz };
        Vector3 b = alongX ? Vector3{ x + hx, radius * 0.85f, z } : Vector3{ x, radius * 0.85f, z + hz };
        Beam(a, b, radius * 2, Surface::Bark, true);
    }
    // A round hay bale lying on its side, axis along X
    void Bale(float x, float z) {
        const float r = 0.75f, len = 1.3f;
        Twin(x - len / 2, x + len / 2, 0, r * 2, z - r * 0.9f, z + r * 0.9f, Surface::Sand, BOX_HIDDEN);
        Beam({ x - len / 2, r, z }, { x + len / 2, r, z }, r * 2, Surface::Sand, true);
    }
};

// ---------------------------------------------------------------------------
// The layout (team A's half; twins are added for team B)
// ---------------------------------------------------------------------------

// Forest A: in front of A's spawn and the x+ corner behind it (from the sketch)
const Polygon FOREST_A = { { { -15.45f, -93.75f }, { -15.45f, -58.5f }, { -28.95f, -57.75f }, { -28.95f, -21.0f }, { 4.05f, -21.0f },
                             { 4.05f, -36.75f }, { 16.05f, -36.75f }, { 16.05f, -71.25f }, { 31.05f, -71.25f }, { 31.05f, -117.0f },
                             { 11.55f, -117.0f }, { 16.05f, -101.25f }, { -0.45f, -90.75f } } };
// The small wood in the corner behind A's sniper tower
const Polygon COPSE_A = { { { -29.7f, -117.0f }, { -28.95f, -98.25f }, { -18.45f, -99.0f }, { -12.45f, -106.5f }, { -12.45f, -117.0f } } };

const Rect FARMLAND_A = { 17.8f, 31.5f, -70.0f, 19.0f };
const Rect SPAWN_A = { -10.0f, 10.0f, -119.5f, -104.0f };
const Rect HOUSE_A = { 6.0f, 15.0f, -34.0f, -26.0f };
const Rect HUT_A = { 22.0f, 26.0f, 23.0f, 27.0f };
const Vector2 TOWER_A = { -27.5f, -86.0f };
const float FORT_R = 12.6f;      // outer face of the fort wall
const float RING_ROAD_R = 18.0f;

const std::vector<Road> ROADS_A = {
    // Spawn gate, winding through the forest to the ring road (bends so no lane sees far)
    { { { 0, -104 }, { -2, -96 }, { -6, -86 }, { -6, -75 }, { -1, -64 }, { 4, -54 }, { 3, -44 }, { -1, -35 }, { -3, -26 }, { -2.5f, -17.8f } }, 4.0f },
    // West gate, round the copse, past the tower, along the x- edge (the tower's lane) to the ring road
    { { { -10.5f, -110 }, { -11.5f, -101 }, { -15, -96 }, { -21, -93 }, { -22.5f, -85 }, { -25, -78 }, { -28.8f, -72 }, { -28.8f, -24 }, { -26, -17 }, { -20, -12 }, { -14.8f, -10.2f } }, 3.5f },
    // East gate, through the corner forest to the farmland, along its inner edge to the ring road
    { { { 10.5f, -108 }, { 14, -100 }, { 18, -92 }, { 21, -83 }, { 19.5f, -74 }, { 15.6f, -66 }, { 15.6f, 15 }, { 13, 12.45f } }, 3.5f },
    // From the farmland past the checkpoint hut to the other side's edge lane
    { { { 15.6f, 14 }, { 21.5f, 20.5f }, { 26.5f, 20.5f }, { 28.8f, 23 } }, 3.0f },
    // The house to the ring road
    { { { 10.5f, -25.5f }, { 9.5f, -20 }, { 8.1f, -16.1f } }, 3.0f },
};

void Fort(Builder& m) {
    // The wall: chest high with a two-level fire step inside, two gates (A's, a little off
    // the axis, and its twin), a narrow gap, and a breach knocked down to waist height
    const float in = 11.4f, out = FORT_R, wallH = 2.4f;
    const float h = PI / 2;
    struct Arc { float from, to; float height; bool step; };
    const Arc arcs[] = {
        { h, h + 0.1f, wallH, true },
        { h + 0.1f, h + 0.6f, 1.0f, false },          // breach (toward A's farmland)
        { h + 0.6f, PI - 0.61f, wallH, true },
        { PI - 0.49f, PI - 0.07f, wallH, true },      // (gap between: a narrow loophole)
        { PI + 0.31f, 3 * h, wallH, true },           // (gate between: A's road comes in here)
    };
    for (const Arc& a : arcs) {
        m.Ring(in, out, 0, a.height, a.from, a.to, Surface::Stone);
        if (a.step) {
            // Stepped back a little at openings; continuous where the two halves meet (at h and 3h)
            auto inset = [&](float angle, float d) { return (fabsf(angle - h) < 1e-4f || fabsf(angle - 3 * h) < 1e-4f) ? 0.0f : d; };
            m.Ring(10.6f, in, 0, 0.9f, a.from + inset(a.from, 0.02f), a.to - inset(a.to, 0.02f), Surface::Stone);
            m.Ring(9.9f, 10.6f, 0, 0.45f, a.from + inset(a.from, 0.03f), a.to - inset(a.to, 0.03f), Surface::Stone);
        }
    }
    // Rubble from the breach, and from the gate
    m.Rock(13.6f, -2.4f, 1.2f, 0.6f, 0.9f);
    m.Rock(11.0f, -4.6f, 0.9f, 0.45f, 1.1f);
    m.Rock(14.2f, -5.2f, 0.8f, 0.4f, 0.7f);
    m.Rock(-3.8f, -14.0f, 0.9f, 0.5f, 0.8f);
    // Gate posts: heavier stone either side of the gate
    for (float a : { PI - 0.07f, PI + 0.31f }) {
        float x = sinf(a) * (in + out) / 2, z = cosf(a) * (in + out) / 2;
        m.Twin(x - 0.55f, x + 0.55f, 0, 2.8f, z - 0.55f, z + 0.55f, Surface::Stone);
    }

    // The middle: a raised stone platform, parapets on the gate sides so the gates can't
    // see through to each other, steps up on the other two sides
    const float ph = 1.2f;
    m.Add(-3, 3, 0, ph, -3, 3, Surface::Stone);
    m.Twin(-3, 3, ph, ph + 1.05f, -3, -2.55f, Surface::Stone);
    m.Twin(3, 3.5f, 0, 0.8f, -1.6f, 1.6f, Surface::Stone);
    m.Twin(3.5f, 4.0f, 0, 0.4f, -1.6f, 1.6f, Surface::Stone);
    m.Add(-0.08f, 0.08f, ph, ph + 8.5f, -0.08f, 0.08f, Surface::Wood); // flagpole
    // Cover inside the walls
    m.Crate(-5.5f, -7.0f, 1.1f, 0.8f);
    m.Crate(6.5f, -4.5f, 1.0f);
    m.Crate(-7.6f, 3.2f, 1.1f);
    m.Twin(-1.5f, 1.5f, 0, 1.05f, -7.4f, -6.8f, Surface::Sand); // sandbag wall across the middle of the yard
    m.Twin(5.6f, 6.2f, 0, 1.05f, 2.0f, 4.6f, Surface::Sand);
}

void House(Builder& m) {
    // Two storeys: fieldstone below, plaster above, timber band between. Doors face the
    // forest road (x-) and the fort (z+); stairs inside along the z- wall.
    const Rect r = HOUSE_A;
    const float t = 0.3f, floor1 = 3.2f, top = 6.0f;
    const float x0 = r.x0, x1 = r.x1, z0 = r.z0, z1 = r.z1;
    using H = Builder::Hole;
    // Ground floor
    m.Wall(true, z0, z0 + t, x0, x1, 0, floor1, { H{ 8.0f, 9.2f, 1.0f, 2.2f }, H{ 12.6f, 13.8f, 1.0f, 2.2f } }, Surface::Stone);
    m.Wall(true, z1 - t, z1, x0, x1, 0, floor1, { H{ 9.8f, 11.2f, 0, 2.3f }, H{ 7.0f, 8.2f, 1.0f, 2.2f }, H{ 12.8f, 14.0f, 1.0f, 2.2f } }, Surface::Stone);
    m.Wall(false, x0, x0 + t, z0 + t, z1 - t, 0, floor1, { H{ -31.0f, -29.6f, 0, 2.3f } }, Surface::Stone);
    m.Wall(false, x1 - t, x1, z0 + t, z1 - t, 0, floor1, { H{ -31.6f, -30.4f, 1.0f, 2.2f }, H{ -28.8f, -27.6f, 1.0f, 2.2f } }, Surface::Stone);
    // Upper floor
    m.Wall(true, z0, z0 + t, x0, x1, floor1, top, { H{ 7.4f, 8.6f, 4.2f, 5.4f }, H{ 12.4f, 13.6f, 4.2f, 5.4f } }, Surface::Plaster);
    m.Wall(true, z1 - t, z1, x0, x1, floor1, top, { H{ 7.4f, 8.6f, 4.2f, 5.4f }, H{ 10.0f, 11.2f, 4.2f, 5.4f }, H{ 12.4f, 13.6f, 4.2f, 5.4f } }, Surface::Plaster);
    m.Wall(false, x0, x0 + t, z0 + t, z1 - t, floor1, top, { H{ -30.6f, -29.4f, 4.2f, 5.4f } }, Surface::Plaster);
    m.Wall(false, x1 - t, x1, z0 + t, z1 - t, floor1, top, { H{ -31.4f, -30.2f, 4.2f, 5.4f }, H{ -28.6f, -27.4f, 4.2f, 5.4f } }, Surface::Plaster);
    // Timber band and corner posts
    // (no two faces in one plane: the band stands a little proud of the floor line, the side
    // pieces fit between the end pieces, the posts stand on the band and stop short of the top)
    const float bandLo = floor1 - 0.23f, bandHi = floor1 + 0.03f;
    m.Twin(x0 - 0.06f, x1 + 0.06f, bandLo, bandHi, z0 - 0.06f, z0 + 0.1f, Surface::Wood);
    m.Twin(x0 - 0.06f, x1 + 0.06f, bandLo, bandHi, z1 - 0.1f, z1 + 0.06f, Surface::Wood);
    m.Twin(x0 - 0.06f, x0 + 0.1f, bandLo, bandHi, z0 + 0.1f, z1 - 0.1f, Surface::Wood);
    m.Twin(x1 - 0.1f, x1 + 0.06f, bandLo, bandHi, z0 + 0.1f, z1 - 0.1f, Surface::Wood);
    for (float x : { x0 - 0.07f, x1 - 0.13f })
        for (float z : { z0 - 0.07f, z1 - 0.13f }) m.Twin(x, x + 0.2f, bandHi, top - 0.04f, z, z + 0.2f, Surface::Wood);
    // Upper floor slab with the stairwell open, and a railing round the stairwell
    const float sx0 = 7.0f, sx1 = 12.4f, sz0 = z0 + t, sz1 = sz0 + 1.25f;
    m.Twin(x0 + t, x1 - t, floor1 - 0.2f, floor1, sz1, z1 - t, Surface::Wood);
    m.Twin(x0 + t, sx0, floor1 - 0.2f, floor1, sz0, sz1, Surface::Wood);
    m.Twin(sx1, x1 - t, floor1 - 0.2f, floor1, sz0, sz1, Surface::Wood);
    m.Twin(sx0, sx1 - 0.9f, floor1, floor1 + 0.95f, sz1, sz1 + 0.08f, Surface::Wood);
    // Stairs up along the z- wall, climbing toward x+
    const int steps = 10;
    for (int i = 0; i < steps; i++) {
        float x = sx0 + (sx1 - sx0 - 0.4f) * i / steps;
        float topY = floor1 * (i + 1) / steps;
        m.Twin(x, x + (sx1 - sx0 - 0.4f) / steps, std::max(0.0f, topY - 0.4f), topY, sz0, sz1, Surface::Wood);
    }
    // Ceiling (the roof's floor; the pitched roof above is scenery)
    m.Twin(x0, x1, top, top + 0.2f, z0, z1, Surface::Wood, BOX_HIDDEN);
    m.Roof({ x0 - 0.45f, x1 + 0.45f, z0 - 0.45f, z1 + 0.45f }, top, 2.4f, Scenery::Roof::RidgeX, Surface::Shingles);
    // Gable ends: solid under the roof (hidden: the roof mesh draws them)
    for (int i = 0; i < 4; i++) {
        float y = top + 0.2f + i * 0.5f, inset = (z1 - z0) / 2 * (i + 1) / 5.0f;
        m.Twin(x0, x1, y, y + 0.5f, z0 + inset, z1 - inset, Surface::Shingles, BOX_HIDDEN);
    }
    // Inside: a table and crates by the windows
    m.Twin(10.6f, 12.4f, 0.75f, 0.85f, -30.4f, -29.4f, Surface::Wood);
    m.Crate(13.8f, -27.2f, 0.9f);
    m.Twin(13.4f, 14.6f, floor1, floor1 + 0.9f, -27.6f, -26.6f, Surface::Crate);
    m.Twin(6.6f, 7.6f, floor1, floor1 + 0.9f, -27.6f, -26.6f, Surface::Crate);
    // Outside: a woodpile and a water trough
    m.Twin(7.5f, 10.5f, 0, 1.2f, -35.4f, -34.5f, Surface::Logs);
    m.Twin(15.4f, 16.0f, 0, 0.7f, -27.0f, -24.6f, Surface::Wood);
}

void Hut(Builder& m) {
    // A log checkpoint hut where A's flank meets the middle, sandbags at its door
    const Rect r = HUT_A;
    const float t = 0.25f, top = 2.6f;
    using H = Builder::Hole;
    m.Wall(true, r.z0, r.z0 + t, r.x0, r.x1, 0, top, { H{ 23.3f, 24.7f, 1.1f, 1.9f } }, Surface::Logs);
    m.Wall(true, r.z1 - t, r.z1, r.x0, r.x1, 0, top, { H{ 23.3f, 24.7f, 1.1f, 1.9f } }, Surface::Logs);
    m.Wall(false, r.x0, r.x0 + t, r.z0 + t, r.z1 - t, 0, top, { H{ 24.3f, 25.5f, 0, 2.2f } }, Surface::Logs);
    m.Wall(false, r.x1 - t, r.x1, r.z0 + t, r.z1 - t, 0, top, { H{ 24.4f, 25.6f, 1.1f, 1.9f } }, Surface::Logs);
    m.Twin(r.x0, r.x1, top, top + 0.15f, r.z0, r.z1, Surface::Wood, BOX_HIDDEN);
    m.Roof({ r.x0 - 0.4f, r.x1 + 0.4f, r.z0 - 0.4f, r.z1 + 0.4f }, top, 1.3f, Scenery::Roof::RidgeX, Surface::Shingles);
    m.Twin(r.x0, r.x1, top + 0.15f, top + 0.7f, r.z0 + 0.6f, r.z1 - 0.6f, Surface::Shingles, BOX_HIDDEN);
    // Sandbags: an L in front of the door
    m.Twin(20.3f, 21.0f, 0, 1.1f, 23.2f, 27.6f, Surface::Sand);
    m.Twin(20.3f, 22.0f, 0, 1.1f, 27.6f, 28.3f, Surface::Sand);
    // The barrier: two posts and the boom, raised
    m.Twin(17.15f, 17.4f, 0, 1.2f, 10.6f, 10.85f, Surface::Wood);
    m.Twin(13.6f, 13.85f, 0, 1.0f, 10.6f, 10.85f, Surface::Wood);
    m.Beam({ 17.28f, 1.15f, 10.73f }, { 15.3f, 4.6f, 10.73f }, 0.14f, Surface::Wood);
    m.Crate(25.2f, 22.0f, 0.9f);
}

void SpawnCompound(Builder& m) {
    // A log palisade with three gates, two cabins along the back
    const Rect r = SPAWN_A;
    const float t = 0.5f, h = 3.0f;
    m.Twin(r.x0, r.x1, 0, h, r.z0, r.z0 + t, Surface::Logs);                         // back
    m.Twin(r.x0, -3.0f, 0, h, r.z1 - t, r.z1, Surface::Logs);                        // front, gate in the middle
    m.Twin(3.0f, r.x1, 0, h, r.z1 - t, r.z1, Surface::Logs);
    m.Twin(r.x0, r.x0 + t, 0, h, r.z0 + t, -112.0f, Surface::Logs);                  // west side, gate (between back and front)
    m.Twin(r.x0, r.x0 + t, 0, h, -108.5f, r.z1 - t, Surface::Logs);
    m.Twin(r.x1 - t, r.x1, 0, h, r.z0 + t, -110.0f, Surface::Logs);                  // east side, gate
    m.Twin(r.x1 - t, r.x1, 0, h, -106.5f, r.z1 - t, Surface::Logs);
    for (float x : { -3.3f, 2.7f }) m.Twin(x, x + 0.6f, 0, h + 0.8f, r.z1 - 0.55f, r.z1 + 0.05f, Surface::Logs); // gate posts
    // Cabins
    for (Rect c : { Rect{ -9.5f, -5.0f, -119.0f, -114.5f }, Rect{ 5.0f, 9.5f, -119.0f, -115.0f } }) {
        m.Twin(c.x0, c.x1, 0, 2.6f, c.z0, c.z1, Surface::Logs);
        m.Roof({ c.x0 - 0.4f, c.x1 + 0.4f, c.z0 - 0.3f, c.z1 + 0.4f }, 2.6f, 1.6f, Scenery::Roof::RidgeX, Surface::Shingles);
        m.Twin(c.x0, c.x1, 2.6f, 3.6f, c.z0 + 0.8f, c.z1 - 0.8f, Surface::Shingles, BOX_HIDDEN);
        float dx = (c.x0 + c.x1) / 2;
        m.Twin(dx - 0.55f, dx + 0.55f, 0, 2.1f, c.z1, c.z1 + 0.06f, Surface::Wood); // door
    }
    m.Crate(-6.0f, -106.2f, 1.1f, 0.8f);
    m.Crate(6.5f, -107.0f, 1.0f);
    m.Crate(-1.5f, -117.5f, 1.2f);
}

void Tower(Builder& m) {
    // A stair tower: a closed log shaft with stairs winding up inside round a solid core
    // (one and a half turns), and an open lookout on top, above the treetops
    const float cx = TOWER_A.x, cz = TOWER_A.y;
    const float coreH = 1.6f, inH = 2.8f, outH = 3.0f, deckOver = 3.5f;
    const float platform = 15.0f;
    const int flights = 6, steps = 8;
    const float rise = platform / (flights * steps);
    auto B = [&](float u0, float u1, float y0, float y1, float v0, float v1, Surface s, uint8_t f = 0) {
        m.Twin(cx + u0, cx + u1, y0, y1, cz + v0, cz + v1, s, f);
    };
    using H = Builder::Hole;
    B(-coreH, coreH, 0, platform - 0.3f, -coreH, coreH, Surface::Logs); // the core
    // Flights: south side going east, east going north, north going west, west going south, ...
    // Corner landings between them. (u, v) of each flight's corridor and direction:
    struct Side { float u0, u1, v0, v1; int axis; int dir; }; // axis 0 = along u
    const Side sides[4] = {
        { -coreH, coreH, -inH, -coreH, 0, +1 },  // south, toward +u
        { coreH, inH, -coreH, coreH, 1, +1 },    // east, toward +v
        { -coreH, coreH, coreH, inH, 0, -1 },    // north, toward -u
        { -inH, -coreH, -coreH, coreH, 1, -1 },  // west, toward -v
    };
    const Vector2 corners[4] = { { coreH, -inH }, { coreH, coreH }, { -inH, coreH }, { -inH, -inH } }; // after each side (u0, v0 of the corner square)
    for (int f = 0; f < flights; f++) {
        const Side& s = sides[f % 4];
        float run = 2 * coreH / steps;
        for (int i = 0; i < steps; i++) {
            float top = (f * steps + i + 1) * rise;
            float bottom = f == 0 ? 0.0f : top - 0.45f;
            int k = s.dir > 0 ? i : steps - 1 - i;
            if (s.axis == 0) B(-coreH + run * k, -coreH + run * (k + 1), bottom, top, s.v0, s.v1, Surface::Wood);
            else B(s.u0, s.u1, bottom, top, -coreH + run * k, -coreH + run * (k + 1), Surface::Wood);
        }
        // The landing at the end of this flight
        if (f + 1 < flights) {
            Vector2 c = corners[f % 4];
            float top = (f + 1) * steps * rise;
            B(c.x, c.x + (inH - coreH), top - 0.45f, top, c.y, c.y + (inH - coreH), Surface::Wood);
        }
    }
    // The shaft's walls, with a door at the bottom and a window beside each flight
    std::vector<H> south = { H{ cx - 2.7f, cx - 1.7f, 0, 2.3f } }, north, east, west;
    for (int f = 0; f < flights; f++) {
        float mid = (f * steps + steps / 2) * rise;
        H hole{ 0, 0, mid + 1.0f, mid + 1.75f };
        switch (f % 4) {
            case 0: south.push_back({ cx - 0.6f, cx + 0.6f, hole.y0, hole.y1 }); break;
            case 1: east.push_back({ cz - 0.6f, cz + 0.6f, hole.y0, hole.y1 }); break;
            case 2: north.push_back({ cx - 0.6f, cx + 0.6f, hole.y0, hole.y1 }); break;
            case 3: west.push_back({ cz - 0.6f, cz + 0.6f, hole.y0, hole.y1 }); break;
        }
    }
    const float wallTop = platform - 0.3f;
    m.Wall(true, cz - outH, cz - inH, cx - outH, cx + outH, 0, wallTop, south, Surface::Logs);
    m.Wall(true, cz + inH, cz + outH, cx - outH, cx + outH, 0, wallTop, north, Surface::Logs);
    m.Wall(false, cx + inH, cx + outH, cz - inH, cz + inH, 0, wallTop, east, Surface::Logs);
    m.Wall(false, cx - outH, cx - inH, cz - inH, cz + inH, 0, wallTop, west, Surface::Logs);
    // Corner posts the full height
    for (float u : { -outH - 0.25f, outH - 0.05f })
        for (float v : { -outH - 0.25f, outH - 0.05f }) B(u, u + 0.3f, 0, platform + 2.6f, v, v + 0.3f, Surface::Wood);
    // The lookout: deck (open above the last flight), railing, roof on the posts
    B(-deckOver, coreH, platform - 0.3f, platform, -deckOver, deckOver, Surface::Wood);
    B(coreH, inH, platform - 0.3f, platform, -deckOver, -inH, Surface::Wood);
    B(coreH, inH, platform - 0.3f, platform, coreH, deckOver, Surface::Wood);
    B(inH, deckOver, platform - 0.3f, platform, -deckOver, deckOver, Surface::Wood);
    B(-deckOver, deckOver, platform, platform + 1.05f, -deckOver, -deckOver + 0.1f, Surface::Wood);
    B(-deckOver, deckOver, platform, platform + 1.05f, deckOver - 0.1f, deckOver, Surface::Wood);
    B(-deckOver, -deckOver + 0.1f, platform, platform + 1.05f, -deckOver + 0.1f, deckOver - 0.1f, Surface::Wood);
    B(deckOver - 0.1f, deckOver, platform, platform + 1.05f, -deckOver + 0.1f, deckOver - 0.1f, Surface::Wood);
    B(coreH + 0.02f, inH - 0.02f, platform, platform + 0.9f, -inH - 0.08f, -inH, Surface::Wood); // railing at the stairwell's far end
    const float roofBase = platform + 2.6f;
    B(-deckOver, deckOver, roofBase, roofBase + 0.15f, -deckOver, deckOver, Surface::Wood, BOX_HIDDEN);
    m.Roof({ cx - deckOver - 0.5f, cx + deckOver + 0.5f, cz - deckOver - 0.5f, cz + deckOver + 0.5f }, roofBase, 1.8f,
           Scenery::Roof::Pyramid, Surface::Shingles);
    // Braces on the outside of the shaft
    for (int level = 0; level < 2; level++) {
        float y0 = 0.5f + level * 7.0f, y1 = y0 + 6.5f;
        float o = outH + 0.12f;
        m.Beam({ cx - o, y0, cz - o + 0.3f }, { cx - o, y1, cz + o - 0.3f }, 0.18f, Surface::Wood);
        m.Beam({ cx + o, y0, cz + o - 0.3f }, { cx + o, y1, cz - o + 0.3f }, 0.18f, Surface::Wood);
        m.Beam({ cx - o + 0.3f, y0, cz + o }, { cx + o - 0.3f, y1, cz + o }, 0.18f, Surface::Wood);
        m.Beam({ cx + o - 0.3f, y0, cz - o }, { cx - o + 0.3f, y1, cz - o }, 0.18f, Surface::Wood);
    }
    // Cover round the foot of the tower
    m.Crate(cx + 4.6f, cz + 3.0f, 1.0f, 0.7f);
    m.Rock(cx + 5.5f, cz - 4.0f, 1.6f, 0.9f, 1.2f);
}

void Farmland(Builder& m) {
    const Rect f = FARMLAND_A;
    // A rail fence along the inner edge, with gaps to walk through
    const float fx = f.x0 - 0.2f;
    float gaps[][2] = { { -42.0f, -37.0f }, { -12.0f, -7.0f } };
    float z = f.z0 + 2.0f;
    std::vector<std::pair<float, float>> runs = { { z, gaps[0][0] }, { gaps[0][1], gaps[1][0] }, { gaps[1][1], f.z1 - 6.0f } };
    for (auto [a, b] : runs) {
        for (float p = a; p <= b + 0.01f; p += 3.0f) m.Twin(fx - 0.08f, fx + 0.08f, 0, 1.1f, p - 0.08f, p + 0.08f, Surface::Wood);
        m.Twin(fx - 0.04f, fx + 0.04f, 0.45f, 0.55f, a, b, Surface::Wood);
        m.Twin(fx - 0.04f, fx + 0.04f, 0.9f, 1.0f, a, b, Surface::Wood);
    }
    // Hay bales: what little cover there is out here
    for (Vector2 b : { Vector2{ 22.5f, -56.0f }, Vector2{ 27.5f, -39.0f }, Vector2{ 21.5f, -21.0f }, Vector2{ 26.0f, -3.0f }, Vector2{ 23.5f, 9.0f } })
        m.Bale(b.x, b.y);
    m.Bale(28.6f, -38.6f);
    // An abandoned hay wagon
    m.Twin(23.0f, 26.2f, 0.8f, 1.0f, -47.8f, -46.2f, Surface::Wood);
    m.Twin(23.0f, 26.2f, 1.0f, 1.6f, -46.3f, -46.2f, Surface::Wood);
    m.Twin(23.0f, 26.2f, 1.0f, 1.6f, -47.8f, -47.7f, Surface::Wood);
    for (float x : { 23.4f, 25.8f })
        for (float zz : { -48.0f, -46.0f }) m.Twin(x - 0.4f, x + 0.4f, 0, 0.8f, zz - 0.06f, zz + 0.06f, Surface::Wood);
}

void Boundary(Builder& m) {
    // Invisible walls (movement only), a rail fence marking them
    const float wallH = 60.0f;
    m.Add(-HALF_W - 1, -HALF_W, 0, wallH, -HALF_L - 1, HALF_L + 1, Surface::Wood, BOX_HIDDEN | BOX_NO_BULLETS);
    m.Add(HALF_W, HALF_W + 1, 0, wallH, -HALF_L - 1, HALF_L + 1, Surface::Wood, BOX_HIDDEN | BOX_NO_BULLETS);
    m.Add(-HALF_W, HALF_W, 0, wallH, -HALF_L - 1, -HALF_L, Surface::Wood, BOX_HIDDEN | BOX_NO_BULLETS);
    m.Add(-HALF_W, HALF_W, 0, wallH, HALF_L, HALF_L + 1, Surface::Wood, BOX_HIDDEN | BOX_NO_BULLETS);
    const float e = 0.25f;
    for (float x : { -HALF_W + e, HALF_W - e }) {
        for (float z = -HALF_L + 2; z < HALF_L - 1; z += 4.0f) m.Add(x - 0.08f, x + 0.08f, 0, 1.2f, z - 0.08f, z + 0.08f, Surface::Wood);
        m.Add(x - 0.04f, x + 0.04f, 0.5f, 0.6f, -HALF_L, HALF_L, Surface::Wood);
        m.Add(x - 0.04f, x + 0.04f, 1.0f, 1.1f, -HALF_L, HALF_L, Surface::Wood);
    }
    for (float z : { -HALF_L + e, HALF_L - e }) {
        for (float x = -HALF_W + 2; x < HALF_W - 1; x += 4.0f) m.Add(x - 0.08f, x + 0.08f, 0, 1.2f, z - 0.08f, z + 0.08f, Surface::Wood);
        const float in = HALF_W - e - 0.04f; // up to the side rails' inner faces
        m.Add(-in, in, 0.5f, 0.6f, z - 0.04f, z + 0.04f, Surface::Wood);
        m.Add(-in, in, 1.0f, 1.1f, z - 0.04f, z + 0.04f, Surface::Wood);
    }
}

// --- Trees -----------------------------------------------------------------------------

struct TreePlanter {
    Builder& m;
    std::vector<Rect> keepOut;     // buildings and the like (with margin)
    std::vector<Road> roads;       // all of them (both halves)
    std::vector<Vector2> placed;   // trunks so far (both halves)

    bool Clear(float x, float z, float crown, float trunk) const {
        if (fabsf(x) > HALF_W - 1.0f || fabsf(z) > HALF_L - 1.0f) return false;
        if (CylinderCollides({ x, 0, z }, trunk + 0.9f, 4.0f, m.w)) return false;  // rocks, logs, fences, other trunks
        if (sqrtf(x * x + z * z) < 24.0f) return false;                          // the fort's clearing
        for (const Rect& r : keepOut)
            if (x > r.x0 && x < r.x1 && z > r.z0 && z < r.z1) return false;
        for (const Road& r : roads)
            if (r.Distance(x, z) < r.width / 2 + 1.0f + trunk) return false;
        // The tower lanes along the long edges: no crowns hanging over them
        if (x - crown < -HALF_W + 6.5f && z > -82.0f && z < -10.0f) return false;
        if (x + crown > HALF_W - 6.5f && z < 82.0f && z > 10.0f) return false;
        return true;
    }
    bool Spaced(float x, float z, float spacing) const {
        for (Vector2 p : placed)
            if ((p.x - x) * (p.x - x) + (p.y - z) * (p.y - z) < spacing * spacing) return false;
        return true;
    }
    void Plant(const Polygon& area, int attempts) {
        float x0 = 1e9f, x1 = -1e9f, z0 = 1e9f, z1 = -1e9f;
        for (Vector2 v : area.p) { x0 = fminf(x0, v.x); x1 = fmaxf(x1, v.x); z0 = fminf(z0, v.y); z1 = fmaxf(z1, v.y); }
        for (int i = 0; i < attempts; i++) {
            float x = m.Rand(x0, x1), z = m.Rand(z0, z1);
            if (!Inside(area, x, z)) continue;
            Scenery::Tree t;
            t.height = m.Rand(10.5f, 14.0f);
            t.crownBottom = m.Rand(5.5f, 7.5f);
            t.crownRadius = m.Rand(2.6f, 3.8f);
            t.trunkRadius = m.Rand(0.22f, 0.36f);
            t.yaw = m.Rand(0, 2 * PI);
            t.seed = (unsigned)m.rng();
            t.background = false;
            if (!Clear(x, z, t.crownRadius, t.trunkRadius) || !Spaced(x, z, m.Rand(3.3f, 4.5f))) continue;
            t.base = { x, 0, z };
            Add(t);
        }
    }
    void Add(Scenery::Tree t) {
        Scenery::Tree twin = t;
        twin.base = { -t.base.x, 0, -t.base.z };
        twin.yaw = t.yaw + PI;
        for (const Scenery::Tree& tr : { t, twin }) {
            m.sc->trees.push_back(tr);
            placed.push_back({ tr.base.x, tr.base.z });
            // The trunk stops people and bullets up to where the crown starts
            float r = tr.trunkRadius * 0.9f;
            m.Add(tr.base.x - r, tr.base.x + r, 0, tr.crownBottom, tr.base.z - r, tr.base.z + r, Surface::Bark, BOX_HIDDEN);
        }
    }
};

// Background: woods all round the outside, to the horizon (drawn only)
void BackgroundTrees(Builder& m) {
    std::vector<Vector2> placed;
    const float outerX = 64.0f, outerZ = 154.0f;
    for (int i = 0; i < 9000; i++) {
        float x = m.Rand(-outerX, outerX), z = m.Rand(-outerZ, outerZ);
        if (fabsf(x) < HALF_W + 2.5f && fabsf(z) < HALF_L + 2.5f) continue;
        bool ok = true;
        for (Vector2 p : placed)
            if ((p.x - x) * (p.x - x) + (p.y - z) * (p.y - z) < 30.0f) { ok = false; break; }
        if (!ok) continue;
        placed.push_back({ x, z });
        Scenery::Tree t;
        t.base = { x, 0, z };
        t.height = m.Rand(11.0f, 16.0f);
        t.crownBottom = m.Rand(5.0f, 7.0f);
        t.crownRadius = m.Rand(3.0f, 4.2f);
        t.trunkRadius = m.Rand(0.25f, 0.4f);
        t.yaw = m.Rand(0, 2 * PI);
        t.seed = (unsigned)m.rng();
        t.background = true;
        m.sc->trees.push_back(t);
    }
}

// --- The ground ---------------------------------------------------------------------

void PaintGround(Builder& m, const std::vector<Road>& roads, const std::vector<Polygon>& forests, const std::vector<Rect>& fields,
                 const std::vector<std::pair<Rect, float>>& trampled) {
    Scenery::GroundMask& g = m.sc->ground;
    g.cell = 0.5f;
    g.origin = { -66.0f, -156.0f };
    g.width = (int)(132.0f / g.cell);
    g.height = (int)(312.0f / g.cell);
    g.rgba.assign((size_t)g.width * g.height * 4, 0);
    for (int k = 0; k < g.height; k++) {
        for (int i = 0; i < g.width; i++) {
            float x = g.origin.x + (i + 0.5f) * g.cell, z = g.origin.y + (k + 0.5f) * g.cell;
            float n = Fbm(x * 0.15f, z * 0.15f) - 0.5f, n2 = Fbm(x * 0.6f + 50, z * 0.6f) - 0.5f;
            bool outside = fabsf(x) > HALF_W + 1.5f || fabsf(z) > HALF_L + 1.5f;
            // Forest floor: inside the forests (edges ragged), and everywhere outside the boundary
            float forest = 0.0f;
            if (outside) // the woods round the map, meadow beyond them
                forest = Clamp((fminf(62.0f - fabsf(x), 152.0f - fabsf(z)) + n * 6.0f) / 4.0f, 0.0f, 1.0f);
            else {
                for (const Polygon& p : forests) forest = fmaxf(forest, Clamp((PolygonDistance(p, x, z) + 1.0f + n * 4.0f) / 3.0f, 0.0f, 1.0f));
                if (fabsf(x) > HALF_W - 0.5f || fabsf(z) > HALF_L - 0.5f) forest = fmaxf(forest, 0.6f);
            }
            // Plowed fields, a metre of soft edge
            float field = 0.0f;
            for (const Rect& f : fields) {
                float d = fminf(fminf(x - f.x0, f.x1 - x), fminf(z - f.z0, f.z1 - z));
                field = fmaxf(field, Clamp((d + n2 * 1.2f) / 1.2f, 0.0f, 1.0f));
            }
            // Roads (and the trodden ground of the yards)
            float road = 0.0f;
            for (const Road& r : roads) {
                float d = r.Distance(x, z) + n2 * 0.8f;
                road = fmaxf(road, Clamp((r.width / 2 + 0.4f - d) / 1.0f, 0.0f, 1.0f));
            }
            for (const auto& [r, amount] : trampled) {
                float d = fminf(fminf(x - r.x0, r.x1 - x), fminf(z - r.z0, r.z1 - z));
                road = fmaxf(road, amount * Clamp((d + n * 3.0f) / 2.0f, 0.0f, 1.0f));
            }
            float rad = sqrtf(x * x + z * z);
            road = fmaxf(road, 0.75f * Clamp((FORT_R - 1.5f - rad + n * 3.0f) / 2.0f, 0.0f, 1.0f)); // the fort's yard
            road = fmaxf(road, Clamp((1.6f - fabsf(rad - RING_ROAD_R) + n2 * 0.8f) / 1.0f, 0.0f, 1.0f)); // the ring road
            if (outside) road = 0.0f;
            uint8_t* px = &g.rgba[((size_t)k * g.width + i) * 4];
            px[0] = (uint8_t)(road * 255);
            px[1] = (uint8_t)(field * 255);
            px[2] = (uint8_t)(forest * 255);
            px[3] = (uint8_t)(Clamp(0.5f + n * 1.6f, 0.0f, 1.0f) * 255); // large-scale variation
        }
    }
    m.sc->hasGround = true;
}

} // namespace

World BuildRedoubt() {
    Builder m;
    Boundary(m);
    SpawnCompound(m);
    Tower(m);
    House(m);
    Hut(m);
    Farmland(m);
    Fort(m);

    // Cover out in the woods and the clearings: fallen logs and boulders
    m.FallenLog(-10.0f, -80.0f, 5.5f, 0.35f, false);
    m.FallenLog(8.0f, -68.0f, 5.0f, 0.32f, true);
    m.FallenLog(-18.0f, -48.0f, 6.0f, 0.38f, true);
    m.FallenLog(-8.0f, -30.0f, 4.5f, 0.3f, false);
    m.FallenLog(24.0f, -98.0f, 5.0f, 0.34f, false);
    m.FallenLog(-22.0f, -64.0f, 4.0f, 0.3f, false);
    m.Rock(-14.0f, -66.0f, 1.8f, 1.0f, 1.4f);
    m.Rock(8.0f, -46.0f, 1.5f, 0.9f, 1.7f);
    m.Rock(-20.0f, -30.0f, 2.0f, 1.1f, 1.5f);
    m.Rock(11.0f, -84.0f, 1.4f, 0.8f, 1.2f);
    m.Rock(-21.0f, -76.0f, 1.6f, 0.9f, 1.3f);   // in the tower's clearing
    m.Rock(-17.5f, -70.0f, 1.2f, 0.7f, 1.6f);
    m.Rock(1.5f, -22.5f, 1.3f, 0.8f, 1.1f);     // edge of the fort's clearing
    m.Rock(-14.0f, -19.0f, 1.6f, 1.0f, 1.2f);

    // Roads: A's, their twins, and the ring round the fort
    std::vector<Road> roads;
    for (const Road& r : ROADS_A) { roads.push_back(r); roads.push_back(Rot(r)); }

    // Trees, keeping clear of what's built
    TreePlanter planter{ m };
    planter.roads = roads;
    auto both = [&](Rect r, float margin) {
        Rect g = { r.x0 - margin, r.x1 + margin, r.z0 - margin, r.z1 + margin };
        planter.keepOut.push_back(g);
        planter.keepOut.push_back(Rot(g));
    };
    both(SPAWN_A, 3.0f);
    both(HOUSE_A, 3.0f);
    both(HUT_A, 3.0f);
    both({ TOWER_A.x - 3.5f, TOWER_A.x + 3.5f, TOWER_A.y - 3.5f, TOWER_A.y + 3.5f }, 5.0f);
    both(FARMLAND_A, 1.0f);
    planter.Plant(FOREST_A, 14000);
    planter.Plant(COPSE_A, 2500);
    BackgroundTrees(m);

    // The ground
    std::vector<Polygon> forests = { FOREST_A, Rot(FOREST_A), COPSE_A, Rot(COPSE_A) };
    std::vector<Rect> fields = { FARMLAND_A, Rot(FARMLAND_A) };
    std::vector<std::pair<Rect, float>> trampled;
    for (auto [r, a] : { std::pair<Rect, float>{ SPAWN_A, 0.75f }, { { HOUSE_A.x0 - 2, HOUSE_A.x1 + 2, HOUSE_A.z0 - 2, HOUSE_A.z1 + 2 }, 0.55f },
                         { { HUT_A.x0 - 3, HUT_A.x1 + 2, HUT_A.z0 - 2, HUT_A.z1 + 2 }, 0.6f },
                         { { TOWER_A.x - 5, TOWER_A.x + 5, TOWER_A.y - 5, TOWER_A.y + 5 }, 0.5f } }) {
        trampled.push_back({ r, a });
        trampled.push_back({ Rot(r), a });
    }
    PaintGround(m, roads, forests, fields, trampled);

    World& w = m.w;
    w.map = MapId::Redoubt;
    w.width = 2 * HALF_W;
    w.length = 2 * HALF_L;
    w.spawns = {
        { { 0, 0, -111.5f }, 0.0f, { 60, 120, 230, 255 } },  // team A, south, facing north (+Z)
        { { 0, 0, 111.5f }, PI, { 220, 60, 60, 255 } },      // team B, north, facing south
    };
    w.hotspots = { { 0, 0, -9 }, { 0, 0, 9 }, { -8, 0, 0 }, { 8, 0, 0 } };
    for (Vector3 h : { Vector3{ 10.5f, 0, -21.5f }, Vector3{ 18.0f, 0, 22.0f }, Vector3{ 24.0f, 0, -30.0f }, Vector3{ 24.0f, 0, 5.0f },
                       Vector3{ -5.0f, 0, -70.0f }, Vector3{ 3.0f, 0, -44.0f }, Vector3{ -28.0f, 0, -45.0f }, Vector3{ -16.0f, 0, -14.0f },
                       Vector3{ 14.0f, 0, -14.0f } }) {
        w.hotspots.push_back(h);
        w.hotspots.push_back({ -h.x, 0, -h.z });
    }
    w.ground = Surface::Dirt;
    w.fogDensity = 0.0032f;
    w.botSight = 55.0f;
    w.scenery = m.sc;
    return w;
}
