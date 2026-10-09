#pragma once
#include "raylib.h"

// ---------------------------------------------------------------------------
// The two visual representations of a weapon. Neither affects gameplay; they
// only draw. "Gun space" is meters, origin at the middle of the gun, -Z out of
// the barrel, +Y up. Callers set up the transform before drawing.
// ---------------------------------------------------------------------------

// The moving parts a first-person model animates itself
struct ViewModelPose {
    float slideBack = 0.0f;  // 0..1, slide pushed back by a shot
    float reload = 0.0f;     // 0..1 progress (only meaningful while reloading)
    bool reloading = false;
    bool muzzleFlash = false;
    float aim = 0.0f;        // 0 = hip, 1 = aiming down sights
    bool boxHands = false;   // draw simple box hands (when the character's arms aren't available)
};

struct WeaponModels {
    // Detailed gun with first-person hands/arms. Only ever drawn by the local
    // player's ViewModel, in camera space.
    void (*drawFirstPerson)(const ViewModelPose& pose);
    // Simpler gun, no hands, for drawing in a character's hand in the world.
    // This is what everyone else sees.
    void (*drawWorld)(bool muzzleFlash);

    // First-person placement (camera space: +X right, +Y up, -Z ahead)
    Vector3 hipOffset;      // where the gun's origin sits at the hip
    float hipYawDegrees;    // turned slightly in toward the crosshair at the hip
    float aimDistance;      // how far ahead of the eye the gun's origin sits when aimed
    Vector3 frontSightTop;  // gun space: top of the front sight post
    Vector3 rearNotch;      // gun space: middle of the rear sight notch
    Vector3 handGrip;       // gun space: where the shooting hand's palm centre sits on the grip
    // World model
    Vector3 worldMuzzle;    // gun space: tip of the world model's barrel
};

extern const WeaponModels REVOLVER_MODELS; // looks like the revolver model in assets/models/revolver

// Load/unload the weapon meshes and textures (after InitWindow). Without them
// the weapons draw as a plain placeholder box.
bool LoadWeaponModelAssets(const char* assetDir);
void UnloadWeaponModelAssets();
