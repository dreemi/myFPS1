#include "weapon_models.hpp"
#include "render.hpp"
#include "helmet.hpp"
#include "raymath.h"
#include "rlgl.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <map>
#include <vector>

// Draws one box with dark edges so the flat-colored faces read as 3D
static void DrawPart(Vector3 center, Vector3 size, Color color) {
    DrawCubeV(center, size, color);
    DrawCubeWiresV(center, size, Fade(BLACK, 0.6f));
}

// A forearm: a box from `start` along `dir`, skin for the first `skinLength`
// (the wrist) and sleeve after that
static void DrawLimb(Vector3 start, Vector3 dir, float length, float thickness, float skinLength, Color skin,
                     Color sleeve) {
    dir = Vector3Normalize(dir);
    float yaw = atan2f(dir.x, dir.z) * RAD2DEG;
    float pitch = -asinf(dir.y) * RAD2DEG;
    rlPushMatrix();
        rlTranslatef(start.x, start.y, start.z);
        rlRotatef(yaw, 0, 1, 0);   // the box runs along +Z, turned to point along dir
        rlRotatef(pitch, 1, 0, 0);
        DrawPart({ 0, 0, skinLength / 2 }, { thickness * 0.85f, thickness * 0.85f, skinLength }, skin);
        float sleeveLen = length - skinLength;
        DrawPart({ 0, 0, skinLength + sleeveLen / 2 }, { thickness, thickness, sleeveLen }, sleeve);
    rlPopMatrix();
}

// ---------------------------------------------------------------------------
// Gun assets: a .glb with one mesh per part and material, and a .txt listing
// which mesh is which plus the points the game animates about (revolver.txt,
// fal.txt, written by tools/make_revolver.py and tools/make_fal.py)
// ---------------------------------------------------------------------------

// How each material looks. Textures are neutral; the tint gives the colour.
struct MaterialLook {
    const char* name;
    Color tint;
    float metallic, roughness;
    bool roughMap;
    int albedo;       // 0 = steel texture, 1 = revolver walnut, 2 = FAL wood
    bool normalMap;   // revolver walnut only
    float occlusion;
};
static const MaterialLook LOOKS[] = {
    { "steel",      { 140, 143, 152, 255 }, 1.0f, 0.80f, true,  0, false, 1.0f },  // deep blued steel: dark, but a strong mirror
    { "bright",     { 215, 215, 220, 255 }, 1.0f, 0.80f, true,  0, false, 1.0f },  // polished: extractor, screws, bolt
    { "hardened",   { 128, 122, 116, 255 }, 1.0f, 0.85f, true,  0, false, 1.0f },  // case-hardened hammer and trigger
    { "wood",       { 255, 255, 255, 255 }, 0.0f, 1.00f, true,  1, true,  1.0f },  // oiled walnut, checkered
    { "gold",       { 235, 190, 105, 255 }, 1.0f, 0.70f, true,  0, false, 1.0f },  // grip medallions
    { "brass",      { 232, 182, 102, 255 }, 1.0f, 1.10f, true,  0, false, 1.0f },
    { "primer",     { 205, 200, 195, 255 }, 1.0f, 0.90f, true,  0, false, 1.0f },
    { "lead",       { 128, 128, 134, 255 }, 0.5f, 0.65f, false, 0, false, 1.0f },
    { "bore",       {  24,  24,  24, 255 }, 0.0f, 0.90f, false, 0, false, 0.15f }, // inside barrels and chambers
    { "red",        { 230,  45,  30, 255 }, 0.0f, 0.35f, false, 0, false, 1.0f },  // front sight insert
    { "black",      {  45,  45,  48, 255 }, 0.0f, 1.40f, true,  0, false, 1.0f },  // anodized speedloader
    { "parkerized", {  92,  92,  96, 255 }, 1.0f, 1.85f, true,  0, false, 1.0f },  // the FAL's dull phosphate finish
    { "falwood",    { 255, 255, 255, 255 }, 0.0f, 1.00f, true,  2, false, 1.0f },  // the FAL's furniture
    { "copper",     { 222, 140, 100, 255 }, 1.0f, 1.00f, true,  0, false, 1.0f },  // bullet jackets
};
static const int LOOK_COUNT = sizeof(LOOKS) / sizeof(LOOKS[0]);

struct GunAsset {
    Model model{};
    bool loaded = false;
    std::vector<std::vector<int>> partMeshes; // per part (in the order of the part name list)
    std::vector<int> meshLook;                // LOOKS index per mesh
    std::map<std::string, Vector3> points;    // "key x y z" lines
    std::map<std::string, float> values;      // "key v" lines
};

static GunAsset revolver, fal;
static Texture2D steelAlbedo{}, steelRough{}, woodAlbedo{}, woodRough{}, woodNormal{}, falWoodAlbedo{}, falWoodRough{};
static RevolverRig rig;
static FalRig falRig;

const RevolverRig& GetRevolverRig() { return rig; }
const FalRig& GetFalRig() { return falRig; }

static Texture2D LoadTex(const std::string& path) {
    Texture2D t = LoadTexture(path.c_str());
    if (t.id == 0) return t;
    GenTextureMipmaps(&t);
    SetTextureFilter(t, TEXTURE_FILTER_TRILINEAR);
    return t;
}

static bool LoadGun(GunAsset& g, const std::string& glb, const std::string& txt, const std::vector<const char*>& partNames) {
    g = GunAsset{};
    g.partMeshes.resize(partNames.size());
    FILE* f = fopen(txt.c_str(), "r");
    if (!f) return false;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char key[64], a[64], b[64];
        int index;
        Vector3 v;
        if (sscanf(line, "mesh %d %63s %63s", &index, a, b) == 3) {
            for (size_t p = 0; p < partNames.size(); p++)
                if (strcmp(a, partNames[p]) == 0) g.partMeshes[p].push_back(index);
            if ((int)g.meshLook.size() <= index) g.meshLook.resize(index + 1, 0);
            for (int l = 0; l < LOOK_COUNT; l++)
                if (strcmp(b, LOOKS[l].name) == 0) g.meshLook[index] = l;
        } else if (sscanf(line, "%63s %f %f %f", key, &v.x, &v.y, &v.z) == 4) {
            g.points[key] = v;
        } else if (sscanf(line, "%63s %f", key, &v.x) == 2) {
            g.values[key] = v.x;
        }
    }
    fclose(f);
    if (g.partMeshes[0].empty()) return false;
    g.model = LoadModel(glb.c_str());
    if (g.model.meshCount == 0) return false;
    // Every mesh using a material shares it, so set each material up once from its look
    for (int i = 0; i < g.model.meshCount && i < (int)g.meshLook.size(); i++) {
        const MaterialLook& look = LOOKS[g.meshLook[i]];
        Material& m = g.model.materials[g.model.meshMaterial[i]];
        for (int k = 0; k < 12; k++) m.maps[k].texture = Texture2D{};
        Texture2D albedo = look.albedo == 1 ? woodAlbedo : (look.albedo == 2 ? falWoodAlbedo : steelAlbedo);
        Texture2D rough = look.albedo == 1 ? woodRough : (look.albedo == 2 ? falWoodRough : steelRough);
        m.maps[MATERIAL_MAP_ALBEDO].texture = albedo;
        m.maps[MATERIAL_MAP_ALBEDO].color = look.tint;
        if (look.roughMap) m.maps[MATERIAL_MAP_METALNESS].texture = rough; // texture1
        if (look.normalMap) m.maps[MATERIAL_MAP_NORMAL].texture = woodNormal; // texture2
    }
    g.loaded = true;
    return true;
}

static Vector3 Point(const GunAsset& g, const char* key, Vector3 fallback) {
    auto it = g.points.find(key);
    return it == g.points.end() ? fallback : it->second;
}
static float Value(const GunAsset& g, const char* key, float fallback) {
    auto it = g.values.find(key);
    return it == g.values.end() ? fallback : it->second;
}

enum RevPart { RP_FRAME, RP_GRIP, RP_CYLINDER, RP_CRANE, RP_EJECTOR, RP_HAMMER, RP_TRIGGER, RP_LATCH, RP_ROUND, RP_CASE,
               RP_SPEEDLOADER, RP_COUNT };
enum FalPart { FP_RECEIVER, FP_BOLT, FP_CHARGER, FP_TRIGGER, FP_MAG, FP_ROUND, FP_CASE, FP_COUNT };

bool LoadWeaponModelAssets(const char* assetDir) {
    std::string rdir = std::string(assetDir) + "models/revolver/";
    std::string fdir = std::string(assetDir) + "models/fal/";
    steelAlbedo = LoadTex(rdir + "steel_albedo.png");
    steelRough = LoadTex(rdir + "steel_rough.png");
    woodAlbedo = LoadTex(rdir + "wood_albedo.png");
    woodRough = LoadTex(rdir + "wood_rough.png");
    woodNormal = LoadTex(rdir + "wood_normal.png");
    falWoodAlbedo = LoadTex(fdir + "fal_wood_albedo.png");
    falWoodRough = LoadTex(fdir + "fal_wood_rough.png");

    if (LoadGun(revolver, rdir + "revolver.glb", rdir + "revolver.txt",
                { "frame", "grip", "cylinder", "crane", "ejector", "hammer", "trigger", "latch", "round", "case", "speedloader" })) {
        rig.cylinderAxis = Point(revolver, "cylinder_axis", rig.cylinderAxis);
        rig.cylinderRearZ = Point(revolver, "cylinder_rear", { 0, 0, rig.cylinderRearZ }).z;
        rig.cylinderGap = Point(revolver, "cylinder_gap", rig.cylinderGap);
        rig.cranePivot = Point(revolver, "crane_pivot", rig.cranePivot);
        rig.hammerPivot = Point(revolver, "hammer_pivot", rig.hammerPivot);
        rig.triggerPivot = Point(revolver, "trigger_pivot", rig.triggerPivot);
        rig.muzzle = Point(revolver, "muzzle", rig.muzzle);
        rig.frontSight = Point(revolver, "front_sight", rig.frontSight);
        rig.rearNotch = Point(revolver, "rear_notch", rig.rearNotch);
        rig.chamberRadius = Value(revolver, "chamber_radius", rig.chamberRadius);
        rig.caseLength = Value(revolver, "case_length", rig.caseLength);
        // The points the first-person view lines up with
        REVOLVER_MODELS.frontSightTop = rig.frontSight;
        REVOLVER_MODELS.rearNotch = rig.rearNotch;
        REVOLVER_MODELS.fpMuzzle = rig.muzzle;
        REVOLVER_MODELS.worldMuzzle = Vector3Scale(rig.muzzle, REVOLVER_MODELS.worldScale);
        TraceLog(LOG_INFO, "WEAPONS: revolver loaded (%d meshes)", revolver.model.meshCount);
    } else {
        TraceLog(LOG_WARNING, "WEAPONS: revolver model missing from %s, using a placeholder", rdir.c_str());
    }

    if (LoadGun(fal, fdir + "fal.glb", fdir + "fal.txt", { "receiver", "bolt", "charger", "trigger", "mag", "round", "case" })) {
        falRig.muzzle = Point(fal, "muzzle", falRig.muzzle);
        falRig.frontSight = Point(fal, "front_sight", falRig.frontSight);
        falRig.rearSight = Point(fal, "rear_sight", falRig.rearSight);
        falRig.ejectionPort = Point(fal, "ejection_port", falRig.ejectionPort);
        falRig.chamber = Point(fal, "chamber", falRig.chamber);
        falRig.magPivot = Point(fal, "mag_pivot", falRig.magPivot);
        falRig.magTop = Point(fal, "mag_top", falRig.magTop);
        falRig.triggerPivot = Point(fal, "trigger_pivot", falRig.triggerPivot);
        falRig.chargerKnob = Point(fal, "charger_knob", falRig.chargerKnob);
        falRig.handGrip = Point(fal, "hand_grip", falRig.handGrip);
        falRig.handguard = Point(fal, "handguard", falRig.handguard);
        falRig.boltTravel = Value(fal, "bolt_travel", falRig.boltTravel);
        falRig.chargerTravel = Value(fal, "charger_travel", falRig.chargerTravel);
        falRig.caseLength = Value(fal, "case_length", falRig.caseLength);
        FAL_MODELS.frontSightTop = falRig.frontSight;
        FAL_MODELS.rearNotch = falRig.rearSight;
        FAL_MODELS.handGrip = falRig.handGrip;
        FAL_MODELS.fpMuzzle = falRig.muzzle;
        FAL_MODELS.worldMuzzle = falRig.muzzle;
        TraceLog(LOG_INFO, "WEAPONS: FAL loaded (%d meshes)", fal.model.meshCount);
    } else {
        TraceLog(LOG_WARNING, "WEAPONS: FAL model missing from %s, using a placeholder", fdir.c_str());
    }
    return revolver.loaded && fal.loaded;
}

void SetWeaponModelShader(Shader shader) {
    for (GunAsset* g : { &revolver, &fal })
        if (g->loaded)
            for (int i = 0; i < g->model.materialCount; i++) g->model.materials[i].shader = shader;
}

void UnloadWeaponModelAssets() {
    for (GunAsset* g : { &revolver, &fal }) {
        if (!g->loaded) continue;
        // The textures are shared between materials: free them once, below
        for (int i = 0; i < g->model.materialCount; i++)
            for (int k = 0; k < 12; k++) g->model.materials[i].maps[k].texture = Texture2D{};
        UnloadModel(g->model);
        *g = GunAsset{};
    }
    for (Texture2D* t : { &steelAlbedo, &steelRough, &woodAlbedo, &woodRough, &woodNormal, &falWoodAlbedo, &falWoodRough })
        if (t->id) { UnloadTexture(*t); *t = Texture2D{}; }
}

static void DrawGunPart(const GunAsset& g, int part, Matrix transform) {
    for (int i : g.partMeshes[part]) {
        if (i >= g.model.meshCount) continue;
        const MaterialLook& look = LOOKS[i < (int)g.meshLook.size() ? g.meshLook[i] : 0];
        Render::SurfacePbr(look.metallic, look.roughness, look.roughMap, look.normalMap, look.occlusion);
        DrawMesh(g.model.meshes[i], g.model.materials[g.model.meshMaterial[i]], transform);
    }
}

// ---------------------------------------------------------------------------
// Posing
// ---------------------------------------------------------------------------

static Matrix AboutPoint(Vector3 p, Matrix rotation) {
    return MatrixMultiply(MatrixMultiply(MatrixTranslate(-p.x, -p.y, -p.z), rotation), MatrixTranslate(p.x, p.y, p.z));
}

Matrix RevolverCraneTransform(const RevolverPose& pose) {
    return AboutPoint(rig.cranePivot, MatrixRotateZ(pose.crane * rig.craneOpenDegrees * DEG2RAD));
}

Matrix RevolverCylinderTransform(const RevolverPose& pose) {
    Matrix turn = AboutPoint(rig.cylinderAxis, MatrixRotateZ(pose.cylinder * 60.0f * DEG2RAD));
    return MatrixMultiply(turn, RevolverCraneTransform(pose));
}

// Chamber k's centre on the cylinder's rear face, before the cylinder moves (chamber 0 at the top)
static Vector3 ChamberRest(int k) {
    float a = (90.0f + 60.0f * k) * DEG2RAD;
    return { rig.cylinderAxis.x + rig.chamberRadius * cosf(a), rig.cylinderAxis.y + rig.chamberRadius * sinf(a),
             rig.cylinderRearZ };
}

Vector3 RevolverChamberRear(const RevolverPose& pose, int k) {
    return Vector3Transform(ChamberRest(k), RevolverCylinderTransform(pose));
}

int RevolverChamberAtBarrel(float cylinder) {
    int turns = (int)lroundf(cylinder);
    return ((-turns) % 6 + 6) % 6;
}

static void DrawPartMeshes(int part, Matrix transform) { DrawGunPart(revolver, part, transform); }

void DrawCartridge(Matrix transform, bool fired) {
    if (!revolver.loaded) {
        rlPushMatrix();
            rlMultMatrixf(MatrixToFloat(transform));
            DrawCubeV({ 0, 0, -0.016f }, { 0.009f, 0.009f, 0.033f }, fired ? Color{ 200, 160, 80, 255 } : GOLD);
        rlPopMatrix();
        return;
    }
    DrawPartMeshes(fired ? RP_CASE : RP_ROUND, transform);
}

static void DrawRevolver(const RevolverPose& pose) {
    if (!revolver.loaded) {
        // Placeholder: a dark box along the barrel and one for the grip
        DrawCubeV({ 0, rig.muzzle.y, rig.muzzle.z / 2 }, { 0.025f, 0.035f, fabsf(rig.muzzle.z) }, DARKGRAY);
        DrawCubeV({ 0, -0.05f, 0.02f }, { 0.03f, 0.1f, 0.04f }, DARKGRAY);
        return;
    }
    Matrix id = MatrixIdentity();
    DrawPartMeshes(RP_FRAME, id);
    DrawPartMeshes(RP_GRIP, id);
    DrawPartMeshes(RP_HAMMER, AboutPoint(rig.hammerPivot, MatrixRotateX(pose.hammer * rig.hammerCockDegrees * DEG2RAD)));
    DrawPartMeshes(RP_TRIGGER, AboutPoint(rig.triggerPivot, MatrixRotateX(-pose.trigger * rig.triggerPullDegrees * DEG2RAD)));
    DrawPartMeshes(RP_LATCH, MatrixTranslate(0, 0, -pose.latch * rig.latchTravel));

    Matrix crane = RevolverCraneTransform(pose);
    Matrix cylinder = RevolverCylinderTransform(pose);
    DrawPartMeshes(RP_CRANE, crane);
    DrawPartMeshes(RP_CYLINDER, cylinder);
    float push = pose.ejector * rig.ejectorStroke; // toward the shooter (+Z)
    DrawPartMeshes(RP_EJECTOR, MatrixMultiply(MatrixTranslate(0, 0, push), cylinder));

    // What's in the chambers, riding out with the extractor
    float lift = pose.caseLift * rig.ejectorStroke;
    for (int k = 0; k < 6; k++) {
        if (pose.chambers[k] == CHAMBER_EMPTY) continue;
        Vector3 c = ChamberRest(k);
        DrawCartridge(MatrixMultiply(MatrixTranslate(c.x, c.y, c.z + lift), cylinder), pose.chambers[k] == CHAMBER_FIRED);
    }

    if (pose.speedloader) {
        DrawPartMeshes(RP_SPEEDLOADER, pose.speedloaderTransform);
        if (pose.speedloaderFull) {
            for (int k = 0; k < 6; k++) {
                float a = (90.0f + 60.0f * k) * DEG2RAD;
                Matrix at = MatrixTranslate(rig.chamberRadius * cosf(a), rig.chamberRadius * sinf(a), 0.0f);
                DrawCartridge(MatrixMultiply(at, pose.speedloaderTransform), false);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// First person: the revolver plus (fallback) box hands
// ---------------------------------------------------------------------------

static const Color SKIN = { 214, 168, 135, 255 };
static const Color SLEEVE = { 62, 74, 60, 255 };
static const float GRIP_RAKE = 18.0f;

static void DrawRevolverFirstPerson(const ViewModelPose& pose) {
    DrawRevolver(pose.revolver);
    Render::SurfaceObject(0.35f);

    if (!pose.boxHands) return; // the ViewModel draws the character's real arms (and the flash)

    // Fallback box hands. Reload: the support hand drops away and comes back
    float reloadArc = pose.reloading ? sinf(PI * pose.reload) : 0.0f;
    Vector3 supportOffset = { -0.02f * reloadArc, -0.13f * reloadArc, 0.05f * reloadArc };
    rlPushMatrix();
        rlRotatef(-GRIP_RAKE, 1, 0, 0);
        DrawPart({ 0.004f, -0.048f, 0.010f }, { 0.040f, 0.082f, 0.056f }, SKIN); // palm and fingers
    rlPopMatrix();
    DrawLimb({ 0.010f, -0.105f, 0.060f }, { 0.30f, -0.55f, 1.0f }, 0.45f, 0.070f, 0.07f, SKIN, SLEEVE);
    rlPushMatrix();
        rlTranslatef(supportOffset.x, supportOffset.y, supportOffset.z);
        rlPushMatrix();
            rlRotatef(-GRIP_RAKE, 1, 0, 0);
            DrawPart({ -0.030f, -0.054f, 0.000f }, { 0.026f, 0.080f, 0.066f }, SKIN);
        rlPopMatrix();
        DrawLimb({ -0.030f, -0.110f, 0.040f }, { -0.45f, -0.55f, 1.0f }, 0.45f, 0.068f, 0.07f, SKIN, SLEEVE);
    rlPopMatrix();
}

// ---------------------------------------------------------------------------
// World model: the same revolver at rest, no hands
// ---------------------------------------------------------------------------

static void DrawRevolverWorld(bool muzzleFlash) {
    (void)muzzleFlash; // the flash is an effect spawned when it fires (see Fx::MuzzleFlash)
    static const RevolverPose atRest;
    rlPushMatrix();
        rlScalef(REVOLVER_MODELS.worldScale, REVOLVER_MODELS.worldScale, REVOLVER_MODELS.worldScale);
        DrawRevolver(atRest);
    rlPopMatrix();
    Render::SurfaceObject(0.25f);
}

WeaponModels REVOLVER_MODELS = {
    DrawRevolverFirstPerson,
    DrawRevolverWorld,
    1.3f,                            // first-person scale
    { 0.19f, -0.17f, -0.48f },       // hip offset (camera space): at arm's length
    -4.0f,                           // hip yaw: angled in toward the crosshair
    0.42f,                           // aim distance: arms extended
    { 0.0f, 0.0665f, -0.2405f },     // front sight (gun space; refreshed from revolver.txt)
    { 0.0f, 0.0665f, -0.0205f },     // rear notch
    { 0.0f, -0.050f, 0.020f },       // hand grip: palm centre, down the grip
    { 0.0f, 0.048f, -0.245f },       // first-person muzzle
    1.15f,                           // world scale
    { 0.0f, 0.0552f, -0.2818f },     // world model muzzle (scaled)
};

// ---------------------------------------------------------------------------
// The FAL
// ---------------------------------------------------------------------------

void DrawRifleCartridge(Matrix transform, bool fired) {
    if (!fal.loaded) {
        rlPushMatrix();
            rlMultMatrixf(MatrixToFloat(transform));
            DrawCubeV({ 0, 0, -0.025f }, { 0.012f, 0.012f, 0.051f }, fired ? Color{ 200, 160, 80, 255 } : GOLD);
        rlPopMatrix();
        return;
    }
    DrawGunPart(fal, fired ? FP_CASE : FP_ROUND, transform);
}

// A magazine on its own (gun space at the magazine's seated position, then `transform`)
static void DrawFalMagazine(Matrix transform, bool loaded) {
    if (!fal.loaded) {
        rlPushMatrix();
            rlMultMatrixf(MatrixToFloat(transform));
            DrawCubeV({ 0, -0.08f, -0.076f }, { 0.025f, 0.14f, 0.07f }, DARKGRAY);
        rlPopMatrix();
        return;
    }
    DrawGunPart(fal, FP_MAG, transform);
    if (loaded) {
        // The top round, held in the lips, pointing up the gun
        Vector3 t = falRig.magTop;
        Matrix round = MatrixMultiply(MatrixRotateX(-4.0f * DEG2RAD), MatrixTranslate(t.x, t.y - 0.002f, t.z + 0.030f));
        DrawRifleCartridge(MatrixMultiply(round, transform), false);
    }
}

void DrawFal(const FalPose& pose) {
    if (!fal.loaded) {
        DrawCubeV({ 0, 0.03f, -0.20f }, { 0.035f, 0.06f, 0.80f }, DARKGRAY);
        DrawCubeV({ 0, -0.05f, 0.03f }, { 0.03f, 0.10f, 0.04f }, DARKGRAY);
        return;
    }
    Matrix id = MatrixIdentity();
    DrawGunPart(fal, FP_RECEIVER, id);
    DrawGunPart(fal, FP_BOLT, MatrixTranslate(0, 0, pose.bolt * falRig.boltTravel));          // back = toward the shooter
    DrawGunPart(fal, FP_CHARGER, MatrixTranslate(0, 0, pose.charger * falRig.chargerTravel));
    DrawGunPart(fal, FP_TRIGGER, AboutPoint(falRig.triggerPivot, MatrixRotateX(-pose.trigger * 14.0f * DEG2RAD)));
    if (pose.magInGun) DrawFalMagazine(pose.magTransform, pose.magLoaded);
}

// The magazine rocks out (and in) about the hook at its front
Matrix FalMagazineRock(float degrees, float drop) {
    return MatrixMultiply(AboutPoint(falRig.magPivot, MatrixRotateX(degrees * DEG2RAD)), MatrixTranslate(0, -drop, 0));
}

void DrawDebris(Matrix transform, int kind) {
    switch (kind) {
        case DEBRIS_REVOLVER_ROUND: DrawCartridge(transform, false); break;
        case DEBRIS_REVOLVER_CASE: DrawCartridge(transform, true); break;
        case DEBRIS_RIFLE_ROUND: DrawRifleCartridge(transform, false); break;
        case DEBRIS_RIFLE_CASE: DrawRifleCartridge(transform, true); break;
        case DEBRIS_HELMET: DrawHelmet(transform); break;
        case DEBRIS_RIFLE_MAG: {
            // A loose magazine's origin is its centre; the part is modelled in place under the gun
            Vector3 c = { 0.0f, -0.08f, -0.0765f };
            DrawFalMagazine(MatrixMultiply(MatrixTranslate(-c.x, -c.y, -c.z), transform), false);
            break;
        }
    }
    Render::SurfaceObject(0.25f);
}

// The FAL at rest in someone's hands (other players)
static void DrawFalWorld(bool muzzleFlash) {
    (void)muzzleFlash; // the flash is an effect spawned when it fires
    static const FalPose atRest;
    DrawFal(atRest);
    Render::SurfaceObject(0.25f);
}

WeaponModels FAL_MODELS = {
    nullptr,                         // drawn by the rifle's own view (RifleView)
    DrawFalWorld,
    1.0f,                            // life size
    { 0.10f, -0.19f, -0.10f },       // hip offset (camera space): stock tucked under the arm, close to the chest
    -2.5f,                           // hip yaw
    0.19f,                           // aim distance: cheek on the stock, the eye behind the rear ring
    { 0.0f, 0.098f, -0.562f },       // front sight (gun space; refreshed from fal.txt)
    { 0.0f, 0.098f, 0.056f },        // rear sight
    { 0.0f, -0.050f, 0.031f },       // hand grip
    { 0.0f, 0.045f, -0.775f },       // muzzle
    1.0f,
    { 0.0f, 0.045f, -0.775f },
};
