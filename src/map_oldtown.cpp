// Map: "Old Town", a walled-in old quarter built from photogrammetry scans
// (assets/models/scans), around a King of the Hill plaza. Team A spawns in the corner at
// -Z, +X; team B in the opposite corner. Everything on team A's half has a twin turned half
// a circle about the centre. Top view (X up the page, Z to the right, as the design sketch):
//
//   [A spawn]  ======== rows of old houses (the north edge) =========================
//   |          L-block of houses ......    [winery nook]   ...  [plaster house]   |
//   |          |        [storage bldg]      ( THE HILL )    [storage bldg]        |
//   |   [ruin]                    [plaster house]   [winery nook]        L-block   |
//   ======================== rows of old houses (the south edge) ======  [B spawn]
//
// - The buildings are shells: facades (a wall and its roof) and whole scanned buildings.
//   Nothing has an inside; each has invisible boxes for collision.
// - Facades stand 5 cm apart, and back-to-back ones 2 cm apart at the ridge, so their
//   end caps never share a plane (AGENTS.md: no z-fighting).
#include "world.hpp"
#include "raymath.h"
#include <cmath>
#include <string>
#include <vector>

namespace {

const float HALF_W = 27.0f, HALF_L = 80.75f;
const float GAP = 0.05f;              // between facades in a row

// What the scanned models measure (model space: Y up; a facade's face looks toward +Z)
struct ModelInfo {
    const char* name;
    float x0, x1;      // along the facade
    float back;        // how far the roof reaches behind the wall (z of its back, negative)
    float height;
};
const ModelInfo M_HOUSE_A = { "house_a", -4.80f, 4.80f, -3.08f, 5.13f };
const ModelInfo M_HOUSE_B = { "house_b", -3.90f, 3.90f, -3.14f, 4.80f };
const ModelInfo M_HOUSE_C = { "house_c", -4.06f, 4.06f, -3.31f, 5.39f };
const ModelInfo M_YELLOW = { "yellow_house", -5.91f, 5.91f, -2.78f, 5.55f };
const ModelInfo M_DOOR = { "door_wall", -1.91f, 1.89f, 0.0f, 3.51f };
const ModelInfo M_LONG = { "long_house", -14.75f, 14.76f, 0.0f, 3.69f };
const ModelInfo M_WINERY = { "winery_corner", -3.55f, 3.54f, -0.93f, 3.67f };
const ModelInfo M_PLASTER = { "plaster_house", -8.93f, 9.08f, -2.82f, 9.66f };

// Collision of the whole scanned objects, in their model space: x0, x1, z0, z1, height
struct Col { float x0, x1, z0, z1, h; };
const Col STORAGE_COL[] = {
    { -9.43f, 9.57f, -4.24f, -3.24f, 4.0f },
    { -9.43f, -7.43f, -3.24f, -1.24f, 4.5f },
    { -9.43f, -8.43f, -1.24f, -0.24f, 6.5f },
    { -9.43f, -7.43f, -0.24f, 1.76f, 5.0f },
    { -9.43f, -5.43f, 1.76f, 2.76f, 5.5f },
    { -9.43f, -5.43f, 2.76f, 5.76f, 3.5f },
    { -9.43f, -2.43f, 5.76f, 7.76f, 3.0f },
    { -8.43f, 3.57f, -1.24f, -0.24f, 4.5f },
    { -7.43f, -6.43f, -7.24f, -5.24f, 3.0f },
    { -7.43f, -5.43f, -5.24f, -4.24f, 1.0f },
    { -7.43f, -0.43f, -3.24f, -2.24f, 4.0f },
    { -7.43f, 3.57f, -2.24f, -1.24f, 4.5f },
    { -7.43f, -2.43f, -0.24f, 0.76f, 4.5f },
    { -7.43f, -5.43f, 0.76f, 1.76f, 5.0f },
    { -6.43f, -3.43f, -7.24f, -6.24f, 3.0f },
    { -6.43f, -5.43f, -6.24f, -5.24f, 3.5f },
    { -5.43f, -4.43f, -6.24f, -5.24f, 3.0f },
    { -5.43f, -3.43f, -5.24f, -4.24f, 2.0f },
    { -5.43f, -3.43f, 0.76f, 1.76f, 6.0f },
    { -5.43f, -3.43f, 1.76f, 2.76f, 7.0f },
    { -5.43f, -4.43f, 2.76f, 3.76f, 4.5f },
    { -5.43f, -2.43f, 3.76f, 5.76f, 3.5f },
    { -4.43f, -3.43f, -6.24f, -5.24f, 3.5f },
    { -4.43f, -3.43f, 2.76f, 3.76f, 6.0f },
    { -3.43f, -2.43f, -7.24f, -5.24f, 1.5f },
    { -3.43f, -0.43f, -5.24f, -4.24f, 2.5f },
    { -3.43f, 3.57f, 0.76f, 1.76f, 5.0f },
    { -3.43f, 3.57f, 1.76f, 2.76f, 5.5f },
    { -3.43f, -2.43f, 2.76f, 3.76f, 7.0f },
    { -2.43f, -1.43f, -6.24f, -5.24f, 2.0f },
    { -2.43f, 3.57f, -0.24f, 0.76f, 5.0f },
    { -0.43f, 3.57f, -3.24f, -2.24f, 4.5f },
    { 2.57f, 9.57f, 2.76f, 3.76f, 3.5f },
    { 3.57f, 9.57f, -3.24f, -2.24f, 4.0f },
    { 3.57f, 6.57f, -2.24f, 2.76f, 3.5f },
    { 6.57f, 9.57f, -2.24f, -1.24f, 4.0f },
    { 6.57f, 9.57f, -1.24f, 2.76f, 3.5f },
};
const Col PLASTER_COL[] = {
    { -8.93f, -7.93f, -0.82f, 1.18f, 6.5f },
    { -8.93f, -5.93f, 1.18f, 3.18f, 6.0f },
    { -7.93f, -6.93f, -1.82f, 0.18f, 8.0f },
    { -7.93f, -6.93f, 0.18f, 1.18f, 6.5f },
    { -6.93f, -5.93f, -2.82f, -0.82f, 9.5f },
    { -6.93f, -2.93f, -0.82f, 0.18f, 8.5f },
    { -6.93f, -3.93f, 0.18f, 1.18f, 7.5f },
    { -5.93f, -0.93f, -1.82f, -0.82f, 9.5f },
    { -5.93f, -3.93f, 1.18f, 2.18f, 6.5f },
    { -5.93f, -4.93f, 2.18f, 3.18f, 6.0f },
    { -4.93f, -3.93f, 2.18f, 3.18f, 7.0f },
    { -3.93f, -1.93f, 0.18f, 1.18f, 9.5f },
    { -3.93f, -2.93f, 1.18f, 2.18f, 7.0f },
    { -3.93f, 1.07f, 2.18f, 3.18f, 6.0f },
    { -2.93f, 1.07f, -0.82f, 0.18f, 9.5f },
    { -2.93f, -1.93f, 1.18f, 2.18f, 7.5f },
    { -1.93f, -0.93f, 0.18f, 1.18f, 9.0f },
    { -1.93f, 8.07f, 1.18f, 2.18f, 7.0f },
    { -0.93f, 0.07f, -1.82f, -0.82f, 9.0f },
    { -0.93f, 0.07f, 0.18f, 1.18f, 8.5f },
    { 0.07f, 6.07f, -1.82f, -0.82f, 9.5f },
    { 0.07f, 6.07f, 0.18f, 1.18f, 8.0f },
    { 1.07f, 3.07f, -0.82f, 0.18f, 9.0f },
    { 1.07f, 2.07f, 2.18f, 3.18f, 6.5f },
    { 2.07f, 6.07f, -2.82f, -1.82f, 10.0f },
    { 2.07f, 9.07f, 2.18f, 3.18f, 6.0f },
    { 3.07f, 7.07f, -0.82f, 0.18f, 8.5f },
    { 6.07f, 7.07f, -2.82f, -0.82f, 9.0f },
    { 6.07f, 7.07f, 0.18f, 1.18f, 7.5f },
    { 7.07f, 8.07f, -2.82f, -1.82f, 7.5f },
    { 7.07f, 8.07f, -1.82f, 1.18f, 7.0f },
    { 8.07f, 9.07f, -2.82f, 1.18f, 6.0f },
    { 8.07f, 9.07f, 1.18f, 2.18f, 6.5f },
};
const Col RUIN_COL[] = {
    { -2.96f, -1.96f, -1.13f, -0.63f, 3.5f },
    { -2.96f, -1.46f, -0.63f, 0.37f, 4.5f },
    { -1.46f, 0.04f, -0.63f, -0.13f, 4.5f },
    { -0.46f, 2.54f, -1.13f, -0.63f, 4.5f },
    { 0.04f, 0.54f, -0.63f, -0.13f, 3.0f },
    { 0.04f, 2.04f, -0.13f, 0.37f, 1.0f },
    { 0.54f, 1.04f, -0.63f, -0.13f, 3.5f },
    { 1.04f, 1.54f, -0.63f, -0.13f, 4.0f },
    { 1.54f, 2.54f, -0.63f, -0.13f, 4.5f },
    { 1.54f, 2.04f, 0.37f, 0.87f, 1.0f },
    { 2.04f, 2.54f, -0.13f, 0.87f, 3.5f },
    { 2.04f, 2.54f, 0.87f, 1.37f, 2.0f },
    { 2.54f, 3.04f, -0.63f, 0.87f, 4.0f },
    { 2.54f, 3.04f, 0.87f, 1.37f, 1.5f },
};
const Col WINERY_COL[] = {
    { -3.55f, -3.05f, -0.93f, 0.07f, 4.0f },
    { -3.05f, 1.45f, -0.93f, -0.43f, 4.0f },
    { -1.05f, -0.05f, -0.43f, 0.07f, 1.0f },
    { -0.05f, 1.95f, -0.43f, 0.07f, 1.5f },
    { 1.45f, 2.45f, -0.93f, -0.43f, 3.5f },
    { 2.45f, 2.95f, -0.93f, -0.43f, 4.0f },
    { 2.95f, 3.95f, -0.93f, -0.43f, 3.5f },
};

struct Builder {
    World w;
    std::shared_ptr<Scenery> sc = std::make_shared<Scenery>();

    void Add(float x0, float x1, float y0, float y1, float z0, float z1, Surface s, uint8_t flags = 0) {
        if (x1 - x0 < 1e-4f || y1 - y0 < 1e-4f || z1 - z0 < 1e-4f) return;
        WorldBox b;
        b.position = { (x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2 };
        b.size = { x1 - x0, y1 - y0, z1 - z0 };
        b.color = GRAY;
        b.surface = s;
        b.flags = flags;
        w.push_back(b);
    }
    void Twin(float x0, float x1, float y0, float y1, float z0, float z1, Surface s, uint8_t flags = 0) {
        Add(x0, x1, y0, y1, z0, z1, s, flags);
        Add(-x1, -x0, y0, y1, -z1, -z0, s, flags);
    }
    void Solid(float x0, float x1, float z0, float z1, float h) {   // invisible collision
        Twin(fminf(x0, x1), fmaxf(x0, x1), 0, h, fminf(z0, z1), fmaxf(z0, z1), Surface::Plaster, BOX_HIDDEN);
    }
    // A scanned model (and its twin), turned by yaw (a multiple of 90 degrees)
    void Prop(const char* model, float x, float y, float z, float yaw) {
        sc->props.push_back({ model, { x, y, z }, yaw, 1.0f });
        sc->props.push_back({ model, { -x, y, -z }, yaw + PI, 1.0f });
    }
    // Model-space boxes placed with a prop (yaw a multiple of 90 degrees keeps them boxes)
    void Colliders(const Col* cols, int count, float x, float z, float yaw) {
        float c = roundf(cosf(yaw)), s = roundf(sinf(yaw));
        for (int i = 0; i < count; i++) {
            const Col& k = cols[i];
            float ax = x + k.x0 * c + k.z0 * s, az = z - k.x0 * s + k.z0 * c;
            float bx = x + k.x1 * c + k.z1 * s, bz = z - k.x1 * s + k.z1 * c;
            Solid(ax, bx, az, bz, k.h);
        }
    }
    void Ring(float inner, float outer, float bottom, float top, float from, float to, Surface s) {
        sc->rings.push_back({ { 0, 0, 0 }, inner, outer, bottom, top, from, to, s });
        sc->rings.push_back({ { 0, 0, 0 }, inner, outer, bottom, top, from + PI, to + PI, s });
        float mid = (inner + outer) / 2, block = (outer - inner) * 0.85f;
        int count = std::max(1, (int)ceilf((to - from) * mid / (block * 0.7f)));
        for (int i = 0; i <= count; i++) {
            float a = from + (to - from) * i / count;
            float x = sinf(a) * mid, z = cosf(a) * mid;
            Twin(x - block / 2, x + block / 2, bottom, top, z - block / 2, z + block / 2, s, BOX_HIDDEN);
        }
    }
    void Crate(float x, float z, float size, float top = 0.0f) {
        Twin(x - size / 2, x + size / 2, 0, size, z - size / 2, z + size / 2, Surface::Crate);
        if (top > 0.0f) Twin(x - top / 2 + 0.1f, x + top / 2 + 0.1f, size, size + top, z - top / 2, z + top / 2, Surface::Crate);
    }

    // A row of facades standing on a line, faces toward `facing` (one of +-X, +-Z), laid
    // from `start` along the line in the direction the models' +x turns to. Returns where
    // it ends. Each facade gets a collision box from its wall back to `backTo` (or its own
    // depth when backTo is NAN).
    float Row(const std::vector<const ModelInfo*>& models, char axis, float line, int facing, float start, float backTo) {
        // yaw so the model's +Z looks along `facing` on the given axis
        float yaw = axis == 'x' ? (facing > 0 ? PI / 2 : -PI / 2) : (facing > 0 ? 0.0f : PI);
        // the model's +x then runs along: (cos yaw, -sin yaw)
        float runX = roundf(cosf(yaw)), runZ = roundf(-sinf(yaw));
        float along = runX != 0 ? runX : runZ;   // +1 or -1 along the row's axis
        float pos = start;
        for (const ModelInfo* m : models) {
            float len = m->x1 - m->x0;
            float centre = pos + along * (-m->x0);   // where the model's origin goes along the row
            float back = std::isnan(backTo) ? line - facing * (fabsf(m->back) + 0.3f) : backTo;
            if (axis == 'x') {
                Prop(m->name, line, 0.0f, centre, yaw);
                Solid(line, back, pos, pos + along * len, fmaxf(m->height, 4.0f));
            } else {
                Prop(m->name, centre, 0.0f, line, yaw);
                Solid(pos, pos + along * len, line, back, fmaxf(m->height, 4.0f));
            }
            pos += along * (len + GAP);
        }
        return pos;
    }
};

} // namespace

World BuildOldTown() {
    Builder m;
    const float NaN = NAN;

    // --- The north edge (its twin is the south edge): houses facing into the town, roofs
    //     sloping away. Two pieces stand out of the line: the winery nook (an alcove with
    //     junk in it) and the plaster house.
    const float northLine = 20.7f;
    float z = m.Row({ &M_LONG, &M_HOUSE_A, &M_HOUSE_B }, 'x', northLine, -1, -63.25f, HALF_W + 0.5f);
    // Winery nook: its back wall on the line, its floor 3 cm off the paving
    {
        float zc = z - M_WINERY.x0;                     // the model's +x runs along +Z here
        float x = northLine - 0.93f - 0.02f;          // its back wall just in front of the line
        m.Prop(M_WINERY.name, x, 0.03f, zc, -PI / 2);
        m.Colliders(WINERY_COL, sizeof(WINERY_COL) / sizeof(Col), x, zc, -PI / 2);
        m.Solid(northLine, HALF_W + 0.5f, z, z + (M_WINERY.x1 - M_WINERY.x0), 4.0f);
        z += M_WINERY.x1 - M_WINERY.x0 + GAP;
    }
    z = m.Row({ &M_YELLOW, &M_HOUSE_C, &M_DOOR, &M_HOUSE_B }, 'x', northLine, -1, z, HALF_W + 0.5f);
    // The plaster house: a whole building standing forward of the line, its back on it
    {
        float zc = z - M_PLASTER.x0;
        float x = northLine + 0.8f + M_PLASTER.back;    // back face 0.8 m behind the line, front out in the street
        m.Prop(M_PLASTER.name, x, -0.06f, zc, -PI / 2);
        m.Colliders(PLASTER_COL, sizeof(PLASTER_COL) / sizeof(Col), x, zc, -PI / 2);
        m.Solid(northLine, HALF_W + 0.5f, z, z + (M_PLASTER.x1 - M_PLASTER.x0), 6.0f);
        z += M_PLASTER.x1 - M_PLASTER.x0 + GAP;
    }
    z = m.Row({ &M_HOUSE_A, &M_YELLOW, &M_HOUSE_C, &M_DOOR }, 'x', northLine, -1, z, HALF_W + 0.5f);
    // A plastered pier closes the corner where this row meets the east end's row
    const float eastLine = 74.95f;
    m.Twin(northLine, northLine + 3.5f, 0, 3.6f, z, eastLine - 0.01f, Surface::Plaster);
    m.Solid(northLine, HALF_W + 0.5f, z, eastLine, 4.0f);

    // --- The west end (twin: the east end): houses facing +Z, from the south edge's line
    //     to where team A's spawn opens out
    const float westLine = -eastLine;
    float x = m.Row({ &M_HOUSE_C, &M_YELLOW, &M_HOUSE_B, &M_DOOR }, 'z', westLine, +1, -northLine + GAP, -HALF_L - 0.5f);

    // --- Team A's spawn: a corner yard at the far west, backed by more houses
    {
        const float backLine = -80.1f;
        m.Row({ &M_HOUSE_C, &M_HOUSE_B }, 'z', backLine, +1, x + GAP, -HALF_L - 0.5f);
        // Along the north boundary: facing -X, laid from the north row's start back to the west
        // (the model's +x runs along +Z for this facing, so lay them from the west end)
        m.Row({ &M_HOUSE_B, &M_HOUSE_A }, 'x', 26.4f, -1, backLine - 0.6f, HALF_W + 0.5f);
        // A plastered wall closes the step back from the spawn yard to the north row
        m.Twin(northLine + 0.01f, 26.4f - 0.01f, 0, 3.6f, -63.25f - 0.3f, -63.25f - GAP, Surface::Plaster);
        m.Solid(northLine, 26.4f, -63.6f, -63.25f, 4.0f);
    }

    // --- The L-shaped block of houses north-west of the middle: back-to-back rows meeting at
    //     a ridge, and a wing reaching south from its west end
    {
        const float ridge = 13.8f;
        // north side (facing +X): each wall as far from the ridge as its roof is deep
        float zz = -63.45f;
        for (const ModelInfo* mi : { &M_HOUSE_A, &M_HOUSE_B, &M_HOUSE_C, &M_YELLOW }) {
            float line = ridge + 0.01f - mi->back;    // back (negative) -> wall north of the ridge
            m.Row({ mi }, 'x', line, +1, zz + (mi->x1 - mi->x0), ridge);
            zz += mi->x1 - mi->x0 + GAP;
        }
        // south side (facing -X)
        zz = -63.45f;
        for (const ModelInfo* mi : { &M_YELLOW, &M_HOUSE_C, &M_HOUSE_A, &M_HOUSE_B }) {
            float line = ridge - 0.01f + mi->back;
            m.Row({ mi }, 'x', line, -1, zz, ridge);
            zz += mi->x1 - mi->x0 + GAP;
        }
        // the wing: back to back along X, ridge at z = -59.9, from the block's south side west
        const float wingRidge = -59.9f;
        const float wingEnd = ridge + M_YELLOW.back - 0.01f + 0.1f;     // tucked against the south side's wall
        m.Row({ &M_YELLOW }, 'z', wingRidge + 0.01f - M_YELLOW.back, +1, wingEnd - (M_YELLOW.x1 - M_YELLOW.x0), wingRidge);
        m.Row({ &M_HOUSE_A }, 'z', wingRidge - 0.01f + M_HOUSE_A.back, -1, wingEnd, wingRidge);
    }

    // --- A ruined wall out in the open south-west
    {
        float rx = -11.0f, rz = -59.9f;
        m.Prop("ruined_wall", rx, 0.0f, rz, 0.0f);
        m.Colliders(RUIN_COL, sizeof(RUIN_COL) / sizeof(Col), rx, rz, 0.0f);
    }

    // --- The storage building between the L-block and the plaza
    {
        float sx = 0.0f, sz = -33.6f;
        m.Prop("storage_building", sx, 0.0f, sz, PI / 2);
        m.Colliders(STORAGE_COL, sizeof(STORAGE_COL) / sizeof(Col), sx, sz, PI / 2);
    }

    // --- The hill: a paved circle marked by a low stone kerb, a fountain basin in the middle
    //     with a plinth, and a little cover round it
    const float hillR = 10.0f;
    m.Ring(hillR + 0.2f, hillR + 0.6f, 0, 0.18f, PI / 2, 3 * PI / 2, Surface::Stone);
    m.Ring(2.0f, 2.6f, 0, 0.85f, PI / 2, 3 * PI / 2, Surface::Stone);
    m.Add(-0.45f, 0.45f, 0, 1.7f, -0.45f, 0.45f, Surface::Stone);       // plinth (its own twin)
    m.Crate(4.6f, -6.0f, 1.1f, 0.8f);
    m.Crate(-6.8f, -3.2f, 1.0f);
    m.Twin(5.5f, 8.0f, 0, 1.05f, 3.0f, 3.6f, Surface::Sand);           // sandbags
    m.Twin(-2.0f, 0.6f, 0, 1.05f, -8.4f, -7.8f, Surface::Sand);
    // Cover in the streets
    m.Crate(-17.5f, -12.0f, 1.2f, 0.9f);
    m.Crate(9.0f, -50.0f, 1.1f);
    m.Crate(-4.0f, -68.0f, 1.2f, 0.8f);
    m.Crate(-18.5f, -48.0f, 1.0f);
    m.Crate(16.5f, -70.5f, 1.1f, 0.8f);                                 // in the spawn yard

    // --- The boundary: invisible walls (movement only)
    const float wallH = 60.0f;
    m.Add(-HALF_W - 1, -HALF_W, 0, wallH, -HALF_L - 1, HALF_L + 1, Surface::Plaster, BOX_HIDDEN | BOX_NO_BULLETS);
    m.Add(HALF_W, HALF_W + 1, 0, wallH, -HALF_L - 1, HALF_L + 1, Surface::Plaster, BOX_HIDDEN | BOX_NO_BULLETS);
    m.Add(-HALF_W, HALF_W, 0, wallH, -HALF_L - 1, -HALF_L, Surface::Plaster, BOX_HIDDEN | BOX_NO_BULLETS);
    m.Add(-HALF_W, HALF_W, 0, wallH, HALF_L, HALF_L + 1, Surface::Plaster, BOX_HIDDEN | BOX_NO_BULLETS);

    World& w = m.w;
    w.map = MapId::OldTown;
    w.width = 2 * HALF_W;
    w.length = 2 * HALF_L;
    w.spawns = {
        { { 18.5f, 0, -74.0f }, atan2f(-18.5f, 74.0f), { 60, 120, 230, 255 } },   // team A, the corner yard
        { { -18.5f, 0, 74.0f }, atan2f(18.5f, -74.0f), { 220, 60, 60, 255 } },    // team B
    };
    w.hotspots = { { 6, 0, 6 }, { -6, 0, -6 }, { 6, 0, -6 }, { -6, 0, 6 }, { 0, 0, 7 }, { 0, 0, -7 } };
    for (Vector3 h : { Vector3{ 9.0f, 0, -20.0f }, Vector3{ -12.0f, 0, -40.0f }, Vector3{ 4.0f, 0, -55.0f },
                       Vector3{ 18.9f, 0, -40.0f }, Vector3{ -14.0f, 0, -20.0f } }) {
        w.hotspots.push_back(h);
        w.hotspots.push_back({ -h.x, 0, -h.z });
    }
    w.ground = Surface::Concrete;
    w.fogDensity = 0.004f;
    w.botSight = 45.0f;
    w.hillCenter = { 0, 0, 0 };
    w.hillRadius = hillR;
    w.scenery = m.sc;
    return w;
}
