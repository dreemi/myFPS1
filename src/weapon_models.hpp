#pragma once
#include "raylib.h"

// ---------------------------------------------------------------------------
// The two visual representations of a weapon. Neither affects gameplay; they
// only draw. "Gun space" is meters, origin at the web of the shooting hand on
// top of the grip, -Z out of the barrel, +Y up. Callers set up the transform
// before drawing.
// ---------------------------------------------------------------------------

// The revolver's moving parts (assets/models/revolver, made by tools/make_revolver.py).
// Everything here is visual state; the viewmodel animates it from the weapon's gameplay state.
enum Chamber : unsigned char { CHAMBER_EMPTY = 0, CHAMBER_LIVE = 1, CHAMBER_FIRED = 2 };

struct RevolverPose {
    float hammer = 0.0f;    // 0 = down, 1 = fully cocked
    float trigger = 0.0f;   // 0 = forward, 1 = pulled through
    float cylinder = 0.0f;  // rotation in chambers (1 = 60 deg, counter-clockwise seen from behind)
    float crane = 0.0f;     // 0 = closed, 1 = swung fully out to the left
    float ejector = 0.0f;   // 0 = home, 1 = rod pushed all the way (cases thrown clear)
    float latch = 0.0f;     // 0 = rest, 1 = thumbed forward
    float caseLift = 0.0f;  // cases ride out of the chambers with the extractor (0..1 of its stroke)
    unsigned char chambers[6] = { CHAMBER_LIVE, CHAMBER_LIVE, CHAMBER_LIVE, CHAMBER_LIVE, CHAMBER_LIVE, CHAMBER_LIVE };
    bool speedloader = false;      // drawn at speedloaderTransform (gun space)
    bool speedloaderFull = false;  // still holding its six rounds
    Matrix speedloaderTransform{};
};

// Fixed geometry of the revolver, read from revolver.txt (gun space)
struct RevolverRig {
    Vector3 cylinderAxis = { 0.0f, 0.0345f, 0.0f };  // a point on the cylinder's axis (which runs along Z)
    float chamberRadius = 0.0135f;
    float cylinderRearZ = -0.0263f;                  // the rear face, where the cases' rims sit
    Vector3 cylinderGap = { 0.0f, 0.048f, -0.07185f };
    Vector3 cranePivot = { -0.009f, 0.0125f, 0.0f };  // hinge axis (along Z) the cylinder swings out on
    Vector3 hammerPivot = { 0.0f, 0.030f, 0.002f };
    Vector3 triggerPivot = { 0.0f, 0.004f, -0.036f };
    Vector3 muzzle = { 0.0f, 0.048f, -0.245f };
    Vector3 frontSight = { 0.0f, 0.0665f, -0.2405f };
    Vector3 rearNotch = { 0.0f, 0.0665f, -0.0205f };
    float caseLength = 0.033f;
    // How far things move
    float craneOpenDegrees = 100.0f;
    float hammerCockDegrees = 40.0f;
    float triggerPullDegrees = 24.0f;
    float ejectorStroke = 0.017f;
    float latchTravel = 0.0035f;
};
const RevolverRig& GetRevolverRig();

// Gun-space transforms of the moving assemblies for a pose
Matrix RevolverCraneTransform(const RevolverPose& pose);
Matrix RevolverCylinderTransform(const RevolverPose& pose);   // crane swing, then the cylinder's turn
// Gun-space position of chamber k's rear (rim face centre); the chamber runs along -Z of the cylinder
Vector3 RevolverChamberRear(const RevolverPose& pose, int k);
// Which chamber is lined up with the barrel when the cylinder has turned `cylinder` chambers
int RevolverChamberAtBarrel(float cylinder);

// Loose cartridges (the viewmodel's falling cases, world brass): origin at the rim face, bullet along -Z
void DrawCartridge(Matrix transform, bool fired);

// What the first-person model needs besides the gun's own parts
struct ViewModelPose {
    RevolverPose revolver;
    float aim = 0.0f;        // 0 = hip, 1 = aiming down sights
    bool boxHands = false;   // draw simple box hands (when the character's arms aren't available)
    float reload = 0.0f;     // 0..1 progress (only meaningful while reloading), for the box hands
    bool reloading = false;
};

struct WeaponModels {
    // Detailed gun with first-person hands/arms. Only ever drawn by the local
    // player's ViewModel, in camera space.
    void (*drawFirstPerson)(const ViewModelPose& pose);
    // The same gun at rest, no hands, for drawing in a character's hand in the
    // world. This is what everyone else sees.
    void (*drawWorld)(bool muzzleFlash);

    // First-person placement (camera space: +X right, +Y up, -Z ahead)
    float fpScale;          // the first-person gun is drawn this much larger than life so it reads at arm's length
    Vector3 hipOffset;      // where the gun's origin sits at the hip
    float hipYawDegrees;    // turned slightly in toward the crosshair at the hip
    float aimDistance;      // how far ahead of the eye the gun's origin sits when aimed
    Vector3 frontSightTop;  // gun space: top of the front sight blade
    Vector3 rearNotch;      // gun space: middle of the rear sight notch
    Vector3 handGrip;       // gun space: where the shooting hand's palm centre sits on the grip
    Vector3 fpMuzzle;       // gun space: tip of the first-person model's barrel
    // World model
    float worldScale;       // drawn a little larger so it reads at range
    Vector3 worldMuzzle;    // gun space: tip of the world model's barrel (scale included)
};

// The revolver. Sight and muzzle points are refreshed from revolver.txt when the assets load.
extern WeaponModels REVOLVER_MODELS;

// Load/unload the weapon meshes and textures (after InitWindow). Without them
// the weapons draw as a plain placeholder box.
bool LoadWeaponModelAssets(const char* assetDir);
void UnloadWeaponModelAssets();
void SetWeaponModelShader(Shader shader); // e.g. the lit shader
