#include "weapon_models.hpp"
#include "raymath.h"
#include "rlgl.h"
#include <string>

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
// Revolver (assets/models/revolver). In the .obj the barrel points along -X,
// +Y is up, and it is about 0.68 m long. These place it in gun space.
// ---------------------------------------------------------------------------

static const Vector3 REV_GRIP_TOP = { 0.150f, -0.020f, 0.0f };      // where the hand's web sits
static const Vector3 REV_FRONT_SIGHT = { -0.397f, 0.1505f, 0.0f };  // top of the front sight blade
static const Vector3 REV_REAR_NOTCH = { 0.045f, 0.1436f, 0.0f };    // rear of the top strap (the notch)
static const Vector3 REV_MUZZLE = { -0.412f, 0.107f, 0.0f };        // center of the bore
static const float REV_GRIP_RAKE = 18.0f;                           // grip leans back this far (degrees)
static const float REV_FIRST_PERSON_SCALE = 0.50f;                  // ~34 cm long, oversized a bit to read well at arm's length
static const float REV_WORLD_SCALE = 0.50f;                         // a bit chunkier so it reads at range

// Model space -> gun space: put the grip top at the origin, tilt so the sights
// are level (the front sight is a little higher than the rear notch), turn the
// barrel from -X to -Z, and scale.
static Matrix RevolverToGun(float scale) {
    float level = atan2f(REV_FRONT_SIGHT.y - REV_REAR_NOTCH.y, REV_REAR_NOTCH.x - REV_FRONT_SIGHT.x);
    Matrix m = MatrixTranslate(-REV_GRIP_TOP.x, -REV_GRIP_TOP.y, -REV_GRIP_TOP.z);
    m = MatrixMultiply(m, MatrixRotateZ(level));
    m = MatrixMultiply(m, MatrixRotateY(-90.0f * DEG2RAD));
    return MatrixMultiply(m, MatrixScale(scale, scale, scale));
}

static Model revolver = { 0 };
static bool revolverLoaded = false;

bool LoadWeaponModelAssets(const char* assetDir) {
    std::string dir = assetDir;
    revolver = LoadModel((dir + "models/revolver/revolver.obj").c_str());
    revolverLoaded = revolver.meshCount > 0;
    if (!revolverLoaded) {
        TraceLog(LOG_WARNING, "WEAPONS: revolver model missing from %s, using a placeholder", assetDir);
        return false;
    }
    for (int i = 0; i < revolver.materialCount; i++) {
        Texture2D t = revolver.materials[i].maps[MATERIAL_MAP_DIFFUSE].texture;
        GenTextureMipmaps(&t);
        SetTextureFilter(t, TEXTURE_FILTER_TRILINEAR);
        revolver.materials[i].maps[MATERIAL_MAP_DIFFUSE].texture = t;
        revolver.materials[i].maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
    }
    return true;
}

void UnloadWeaponModelAssets() {
    if (revolverLoaded) UnloadModel(revolver);
    revolverLoaded = false;
}

static void DrawRevolverMesh(float scale) {
    if (revolverLoaded) {
        revolver.transform = RevolverToGun(scale);
        DrawModel(revolver, { 0, 0, 0 }, 1.0f, WHITE);
    } else {
        // Placeholder: a dark box along the barrel and one for the grip
        Vector3 muzzle = Vector3Transform(REV_MUZZLE, RevolverToGun(scale));
        DrawCubeV({ 0, muzzle.y, muzzle.z / 2 }, { 0.04f, 0.05f, fabsf(muzzle.z) }, DARKGRAY);
        DrawCubeV({ 0, -0.05f, 0.02f }, { 0.035f, 0.1f, 0.05f }, DARKGRAY);
    }
}

// ---------------------------------------------------------------------------
// First person: the revolver plus hands and forearms
// ---------------------------------------------------------------------------

static const Color SKIN = { 214, 168, 135, 255 };
static const Color SLEEVE = { 62, 74, 60, 255 };

static void DrawRevolverFirstPerson(const ViewModelPose& pose) {
    DrawRevolverMesh(REV_FIRST_PERSON_SCALE);

    if (pose.muzzleFlash) {
        // Smaller when aiming so it doesn't hide what you're shooting at
        Vector3 muzzle = Vector3Transform(REV_MUZZLE, RevolverToGun(REV_FIRST_PERSON_SCALE));
        float s = Lerp(1.0f, 0.5f, pose.aim);
        DrawSphere(Vector3Add(muzzle, { 0, 0, -0.03f }), 0.035f * s, Fade(YELLOW, 0.9f));
        DrawSphere(Vector3Add(muzzle, { 0, 0, -0.025f }), 0.020f * s, WHITE);
    }
    if (!pose.boxHands) return; // the ViewModel draws the character's real arms

    // Fallback box hands. Reload: the support hand drops away and comes back
    float reloadArc = pose.reloading ? sinf(PI * pose.reload) : 0.0f;
    Vector3 supportOffset = { -0.02f * reloadArc, -0.13f * reloadArc, 0.05f * reloadArc };

    // Shooting hand around the grip (grip space: origin at the grip top, raked back)
    rlPushMatrix();
        rlRotatef(-REV_GRIP_RAKE, 1, 0, 0);
        DrawPart({ 0.004f, -0.048f, 0.006f }, { 0.040f, 0.082f, 0.056f }, SKIN); // palm and fingers
        DrawPart({ -0.019f, -0.006f, -0.014f }, { 0.011f, 0.015f, 0.046f }, SKIN); // thumb, left side
    rlPopMatrix();
    DrawPart({ 0.016f, -0.016f, -0.050f }, { 0.010f, 0.011f, 0.042f }, SKIN); // trigger finger along the frame
    DrawLimb({ 0.010f, -0.105f, 0.060f }, { 0.30f, -0.55f, 1.0f }, 0.45f, 0.070f, 0.07f, SKIN, SLEEVE);

    // Support hand: cups the shooting hand from the left
    rlPushMatrix();
        rlTranslatef(supportOffset.x, supportOffset.y, supportOffset.z);
        rlPushMatrix();
            rlRotatef(-REV_GRIP_RAKE, 1, 0, 0);
            DrawPart({ -0.027f, -0.054f, -0.004f }, { 0.026f, 0.080f, 0.066f }, SKIN);
            DrawPart({ -0.002f, -0.066f, -0.034f }, { 0.046f, 0.054f, 0.015f }, SKIN); // fingers round the front
        rlPopMatrix();
        DrawLimb({ -0.030f, -0.110f, 0.040f }, { -0.45f, -0.55f, 1.0f }, 0.45f, 0.068f, 0.07f, SKIN, SLEEVE);
    rlPopMatrix();
}

// ---------------------------------------------------------------------------
// World model: the same revolver, no hands
// ---------------------------------------------------------------------------

static void DrawRevolverWorld(bool muzzleFlash) {
    DrawRevolverMesh(REV_WORLD_SCALE);
    if (muzzleFlash) {
        Vector3 muzzle = Vector3Transform(REV_MUZZLE, RevolverToGun(REV_WORLD_SCALE));
        DrawSphere(muzzle, 0.12f, Fade(YELLOW, 0.9f));
        DrawSphere(muzzle, 0.06f, WHITE);
    }
}

const WeaponModels REVOLVER_MODELS = {
    DrawRevolverFirstPerson,
    DrawRevolverWorld,
    { 0.19f, -0.17f, -0.48f },                                         // hip offset (camera space): at arm's length
    -4.0f,                                                             // hip yaw: angled in toward the crosshair
    0.42f,                                                             // aim distance: arms extended
    Vector3Transform(REV_FRONT_SIGHT, RevolverToGun(REV_FIRST_PERSON_SCALE)), // front sight (gun space)
    Vector3Transform(REV_REAR_NOTCH, RevolverToGun(REV_FIRST_PERSON_SCALE)),  // rear notch (gun space)
    { 0.0f, -0.050f, 0.016f },                                         // hand grip: palm centre, down the grip
    Vector3Transform(REV_MUZZLE, RevolverToGun(REV_WORLD_SCALE)),            // world model muzzle (gun space)
};
