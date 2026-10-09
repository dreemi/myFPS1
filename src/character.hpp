#pragma once
#include "raylib.h"
#include <string>
#include <vector>

// The human character model (assets/models/character, made by
// tools/export_character.py). Used for the enemy, for the player's own body
// (hips and legs, seen when looking down), and for the first-person arms.
// Model space: meters, Y up, facing +Z, the character's right is -X.

enum class CharAnim { Idle, Walk, Run, Crouch, CrouchWalk, FpHold, Rest, FpRifle, Count };

// Which parts of the mesh to draw (see tools/export_character.py)
enum CharPart {
    PART_HEAD = 1,       // head and neck
    PART_UPPERARMS = 2,
    PART_FOREARMS = 4,   // forearms and hands: the first-person arms
    PART_TORSO = 8,      // chest and shoulders
    PART_LEGS = 16,      // hips, legs, feet: your own body when you look down
    PART_ALL = 31,
};

struct CharacterAssets {
    Model model{};
    ModelAnimation* anims = nullptr;
    int animCount = 0;
    int animIndex[(int)CharAnim::Count];
    std::vector<int> meshPart;   // CharPart of each mesh
    Vector3 aimGrip{};           // right palm centre in the idle (aiming) pose
    Vector3 fpGrip{};            // right palm centre in the first-person revolver hold
    Vector3 fpRifleGrip{};       // right palm centre in the first-person rifle hold
    std::vector<Vector3> leftPalm[(int)CharAnim::Count]; // left palm centre per Blender frame (fp_hold, fp_rifle)
    int handBone = -1;           // hand_r
    bool loaded = false;
};

bool LoadCharacter(CharacterAssets& c, const std::string& assetDir);
void UnloadCharacter(CharacterAssets& c);

// Animation frame for a looping phase (0..1)
int CharacterFrame(const CharacterAssets& c, CharAnim anim, float phase);
// Animation frame at a fraction (0..1) of a non-looping animation, 1 = the last frame
int CharacterFrameAt(const CharacterAssets& c, CharAnim anim, float fraction);

// Pose the model and draw the chosen parts. `transform` goes from model space
// to wherever it's drawn (world, or gun space for the first-person arms).
void DrawCharacter(CharacterAssets& c, CharAnim anim, int frame, Matrix transform, int parts, Color tint);

// Model-space position of the right palm (where a gun is held) in that frame
Vector3 CharacterGripPoint(const CharacterAssets& c, CharAnim anim, int frame);
// Model-space transform of the right hand bone in that frame
Matrix CharacterHandTransform(const CharacterAssets& c, CharAnim anim, int frame);

// fp_hold's frames, as authored in Blender (tools/export_character.py)
const float FP_RELOAD_FIRST = 1.0f, FP_RELOAD_LAST = 90.0f;  // the revolver's reload
const float FP_ADS_FIRST = 91.0f, FP_LAST_FRAME = 100.0f;    // hip -> aiming down sights
// fp_rifle's frames: the magazine change (shared), then either a plain finish or
// charging the rifle after an empty magazine, then aiming
const float FPR_SWAP_FIRST = 1.0f, FPR_SWAP_LAST = 60.0f;        // 0 .. 2.0 s
const float FPR_TACTICAL_FIRST = 61.0f, FPR_TACTICAL_LAST = 78.0f; // 2.0 .. 2.6 s
const float FPR_EMPTY_FIRST = 79.0f, FPR_EMPTY_LAST = 120.0f;      // 2.0 .. 3.4 s
const float FPR_ADS_FIRST = 121.0f, FPR_LAST_FRAME = 130.0f;
// Left palm centre (model space) at a Blender frame of a first-person animation, interpolated
Vector3 CharacterFpLeftPalm(const CharacterAssets& c, CharAnim anim, float blenderFrame);
