#include "weapon_models.hpp"
#include "render.hpp"
#include "raymath.h"
#include "rlgl.h"
#include <cstdio>
#include <cstring>
#include <string>
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
// Revolver assets: one mesh per part and material (see revolver.txt)
// ---------------------------------------------------------------------------

enum RevPart { RP_FRAME, RP_GRIP, RP_CYLINDER, RP_CRANE, RP_EJECTOR, RP_HAMMER, RP_TRIGGER, RP_LATCH, RP_ROUND, RP_CASE,
               RP_SPEEDLOADER, RP_COUNT };
static const char* PART_NAMES[RP_COUNT] = { "frame", "grip", "cylinder", "crane", "ejector", "hammer", "trigger", "latch",
                                            "round", "case", "speedloader" };

// How each material looks. Textures are neutral; the tint gives the colour.
struct MaterialLook {
    const char* name;
    Color tint;
    float metallic, roughness;
    bool roughMap;
    int albedo;       // 0 = steel texture, 1 = wood texture
    bool normalMap;   // wood only
    float occlusion;
};
static const MaterialLook LOOKS[] = {
    { "steel",    { 140, 143, 152, 255 }, 1.0f, 0.80f, true,  0, false, 1.0f },  // deep blued steel: dark, but a strong mirror
    { "bright",   { 215, 215, 220, 255 }, 1.0f, 0.80f, true,  0, false, 1.0f },  // polished: extractor, screws
    { "hardened", { 128, 122, 116, 255 }, 1.0f, 0.85f, true,  0, false, 1.0f },  // case-hardened hammer and trigger
    { "wood",     { 255, 255, 255, 255 }, 0.0f, 1.00f, true,  1, true,  1.0f },  // oiled walnut, checkered
    { "gold",     { 235, 190, 105, 255 }, 1.0f, 0.70f, true,  0, false, 1.0f },  // grip medallions
    { "brass",    { 232, 182, 102, 255 }, 1.0f, 1.10f, true,  0, false, 1.0f },
    { "primer",   { 205, 200, 195, 255 }, 1.0f, 0.90f, true,  0, false, 1.0f },
    { "lead",     { 128, 128, 134, 255 }, 0.5f, 0.65f, false, 0, false, 1.0f },
    { "bore",     {  24,  24,  24, 255 }, 0.0f, 0.90f, false, 0, false, 0.15f }, // inside the barrel and chambers
    { "red",      { 230,  45,  30, 255 }, 0.0f, 0.35f, false, 0, false, 1.0f },  // front sight insert
    { "black",    {  45,  45,  48, 255 }, 0.0f, 1.40f, true,  0, false, 1.0f },  // anodized speedloader
};
static const int LOOK_COUNT = sizeof(LOOKS) / sizeof(LOOKS[0]);

static Model revolver = { 0 };
static bool revolverLoaded = false;
static std::vector<int> partMeshes[RP_COUNT];
static std::vector<int> meshLook;   // LOOKS index per mesh
static Texture2D steelAlbedo{}, steelRough{}, woodAlbedo{}, woodRough{}, woodNormal{};
static RevolverRig rig;

const RevolverRig& GetRevolverRig() { return rig; }

static Texture2D LoadTex(const std::string& path, bool color) {
    Texture2D t = LoadTexture(path.c_str());
    if (t.id == 0) return t;
    GenTextureMipmaps(&t);
    SetTextureFilter(t, TEXTURE_FILTER_TRILINEAR);
    (void)color;
    return t;
}

static bool ReadRig(const std::string& path) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return false;
    for (int p = 0; p < RP_COUNT; p++) partMeshes[p].clear();
    meshLook.clear();
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char key[64], a[64], b[64];
        int index;
        Vector3 v;
        if (sscanf(line, "mesh %d %63s %63s", &index, a, b) == 3) {
            for (int p = 0; p < RP_COUNT; p++)
                if (strcmp(a, PART_NAMES[p]) == 0) partMeshes[p].push_back(index);
            if ((int)meshLook.size() <= index) meshLook.resize(index + 1, 0);
            for (int l = 0; l < LOOK_COUNT; l++)
                if (strcmp(b, LOOKS[l].name) == 0) meshLook[index] = l;
        } else if (sscanf(line, "%63s %f %f %f", key, &v.x, &v.y, &v.z) == 4) {
            std::string k = key;
            if (k == "cylinder_axis") rig.cylinderAxis = v;
            else if (k == "cylinder_rear") rig.cylinderRearZ = v.z;
            else if (k == "cylinder_gap") rig.cylinderGap = v;
            else if (k == "crane_pivot") rig.cranePivot = v;
            else if (k == "hammer_pivot") rig.hammerPivot = v;
            else if (k == "trigger_pivot") rig.triggerPivot = v;
            else if (k == "muzzle") rig.muzzle = v;
            else if (k == "front_sight") rig.frontSight = v;
            else if (k == "rear_notch") rig.rearNotch = v;
        } else if (sscanf(line, "%63s %f", key, &v.x) == 2) {
            std::string k = key;
            if (k == "chamber_radius") rig.chamberRadius = v.x;
            else if (k == "case_length") rig.caseLength = v.x;
        }
    }
    fclose(f);
    return !partMeshes[RP_FRAME].empty();
}

bool LoadWeaponModelAssets(const char* assetDir) {
    std::string dir = std::string(assetDir) + "models/revolver/";
    revolver = LoadModel((dir + "revolver.glb").c_str());
    revolverLoaded = revolver.meshCount > 0 && ReadRig(dir + "revolver.txt");
    if (!revolverLoaded) {
        TraceLog(LOG_WARNING, "WEAPONS: revolver model missing from %s, using a placeholder", dir.c_str());
        if (revolver.meshCount > 0) UnloadModel(revolver);
        revolver = Model{};
        return false;
    }
    steelAlbedo = LoadTex(dir + "steel_albedo.png", true);
    steelRough = LoadTex(dir + "steel_rough.png", false);
    woodAlbedo = LoadTex(dir + "wood_albedo.png", true);
    woodRough = LoadTex(dir + "wood_rough.png", false);
    woodNormal = LoadTex(dir + "wood_normal.png", false);
    // Every mesh using a material shares it, so set each material up once from its look
    for (int i = 0; i < revolver.meshCount && i < (int)meshLook.size(); i++) {
        const MaterialLook& look = LOOKS[meshLook[i]];
        Material& m = revolver.materials[revolver.meshMaterial[i]];
        for (int k = 0; k < 12; k++) m.maps[k].texture = Texture2D{};
        m.maps[MATERIAL_MAP_ALBEDO].texture = look.albedo == 1 ? woodAlbedo : steelAlbedo;
        m.maps[MATERIAL_MAP_ALBEDO].color = look.tint;
        if (look.roughMap) m.maps[MATERIAL_MAP_METALNESS].texture = look.albedo == 1 ? woodRough : steelRough; // texture1
        if (look.normalMap) m.maps[MATERIAL_MAP_NORMAL].texture = woodNormal;                                   // texture2
    }
    // The points the first-person view lines up with
    REVOLVER_MODELS.frontSightTop = rig.frontSight;
    REVOLVER_MODELS.rearNotch = rig.rearNotch;
    REVOLVER_MODELS.fpMuzzle = rig.muzzle;
    REVOLVER_MODELS.worldMuzzle = Vector3Scale(rig.muzzle, REVOLVER_MODELS.worldScale);
    TraceLog(LOG_INFO, "WEAPONS: revolver loaded (%d meshes)", revolver.meshCount);
    return true;
}

void SetWeaponModelShader(Shader shader) {
    if (!revolverLoaded) return;
    for (int i = 0; i < revolver.materialCount; i++) revolver.materials[i].shader = shader;
}

void UnloadWeaponModelAssets() {
    if (revolverLoaded) {
        // The textures are shared between materials: free them once, here
        for (int i = 0; i < revolver.materialCount; i++)
            for (int k = 0; k < 12; k++) revolver.materials[i].maps[k].texture = Texture2D{};
        UnloadModel(revolver);
        for (Texture2D* t : { &steelAlbedo, &steelRough, &woodAlbedo, &woodRough, &woodNormal })
            if (t->id) UnloadTexture(*t);
    }
    revolver = Model{};
    revolverLoaded = false;
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

static void DrawPartMeshes(int part, Matrix transform) {
    for (int i : partMeshes[part]) {
        if (i >= revolver.meshCount) continue;
        const MaterialLook& look = LOOKS[i < (int)meshLook.size() ? meshLook[i] : 0];
        Render::SurfacePbr(look.metallic, look.roughness, look.roughMap, look.normalMap, look.occlusion);
        DrawMesh(revolver.meshes[i], revolver.materials[revolver.meshMaterial[i]], transform);
    }
}

void DrawCartridge(Matrix transform, bool fired) {
    if (!revolverLoaded) {
        rlPushMatrix();
            rlMultMatrixf(MatrixToFloat(transform));
            DrawCubeV({ 0, 0, -0.016f }, { 0.009f, 0.009f, 0.033f }, fired ? Color{ 200, 160, 80, 255 } : GOLD);
        rlPopMatrix();
        return;
    }
    DrawPartMeshes(fired ? RP_CASE : RP_ROUND, transform);
}

static void DrawRevolver(const RevolverPose& pose) {
    if (!revolverLoaded) {
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
