#include "character.hpp"
#include "raymath.h"
#include <cstdio>
#include <cstring>

static const char* ANIM_NAMES[(int)CharAnim::Count] = { "idle", "walk", "run", "crouch", "crouch_walk", "fp_hold" };

bool LoadCharacter(CharacterAssets& c, const std::string& assetDir) {
    std::string dir = assetDir + "models/character/";
    c.model = LoadModel((dir + "character.glb").c_str());
    if (c.model.meshCount == 0) {
        TraceLog(LOG_WARNING, "CHARACTER: model missing from %s", dir.c_str());
        return false;
    }
    c.anims = LoadModelAnimations((dir + "character.glb").c_str(), &c.animCount);
    for (int a = 0; a < c.animCount; a++)
        for (int k = 0; k < (int)CharAnim::Count; k++)
            if (strcmp(c.anims[a].name, ANIM_NAMES[k]) == 0) c.animIndex[k] = a;
    for (int i = 0; i < c.model.boneCount; i++)
        if (strcmp(c.model.bones[i].name, "hand_r") == 0) c.handBone = i;

    // character.txt: which mesh is which part, and the grip points
    c.meshPart.assign(c.model.meshCount, PART_LEGS);
    char* text = LoadFileText((dir + "character.txt").c_str());
    if (text) {
        for (char* line = strtok(text, "\n"); line; line = strtok(nullptr, "\n")) {
            int index;
            char part[32];
            Vector3 v;
            if (sscanf(line, "mesh %d %31s", &index, part) == 2 && index >= 0 && index < c.model.meshCount) {
                static const struct { const char* name; int part; } PARTS[] = {
                    { "head", PART_HEAD }, { "upperarms", PART_UPPERARMS }, { "forearms", PART_FOREARMS },
                    { "torso", PART_TORSO }, { "legs", PART_LEGS } };
                for (const auto& p : PARTS) if (strcmp(part, p.name) == 0) c.meshPart[index] = p.part;
            } else if (sscanf(line, "aim_grip %f %f %f", &v.x, &v.y, &v.z) == 3) {
                c.aimGrip = v;
            } else if (sscanf(line, "fp_grip %f %f %f", &v.x, &v.y, &v.z) == 3) {
                c.fpGrip = v;
            }
        }
        UnloadFileText(text);
    }

    c.loaded = c.handBone >= 0 && text != nullptr;
    for (int k = 0; k < (int)CharAnim::Count; k++) {
        if (c.animIndex[k] < 0) {
            TraceLog(LOG_WARNING, "CHARACTER: animation '%s' missing", ANIM_NAMES[k]);
            c.loaded = false;
        }
    }
    if (!c.loaded) UnloadCharacter(c);
    return c.loaded;
}

void UnloadCharacter(CharacterAssets& c) {
    if (c.anims) UnloadModelAnimations(c.anims, c.animCount);
    if (c.model.meshCount > 0) UnloadModel(c.model);
    c = CharacterAssets{};
}

int CharacterFrame(const CharacterAssets& c, CharAnim anim, float phase) {
    int ai = c.animIndex[(int)anim];
    if (ai < 0) return 0;
    int n = c.anims[ai].frameCount;
    phase -= floorf(phase);
    return Clamp((int)(phase * n), 0, n - 1);
}

int CharacterFrameAt(const CharacterAssets& c, CharAnim anim, float fraction) {
    int ai = c.animIndex[(int)anim];
    if (ai < 0) return 0;
    int n = c.anims[ai].frameCount;
    return Clamp((int)lroundf(Clamp(fraction, 0.0f, 1.0f) * (n - 1)), 0, n - 1);
}

void DrawCharacter(CharacterAssets& c, CharAnim anim, int frame, Matrix transform, int parts, Color tint) {
    if (!c.loaded) return;
    int ai = c.animIndex[(int)anim];
    UpdateModelAnimation(c.model, c.anims[ai], frame);
    Matrix m = MatrixMultiply(c.model.transform, transform);
    for (int i = 0; i < c.model.meshCount; i++) {
        if (!(parts & c.meshPart[i])) continue;
        Material& mat = c.model.materials[c.model.meshMaterial[i]];
        Color old = mat.maps[MATERIAL_MAP_DIFFUSE].color;
        mat.maps[MATERIAL_MAP_DIFFUSE].color = tint;
        DrawMesh(c.model.meshes[i], mat, m);
        mat.maps[MATERIAL_MAP_DIFFUSE].color = old;
    }
}

Vector3 CharacterGripPoint(const CharacterAssets& c, CharAnim anim, int frame) {
    if (!c.loaded) return c.aimGrip;
    // The exported grip point is for the first frame of the hold poses; other
    // frames move the hand (walk bob, reload) - follow the hand bone.
    bool fp = anim == CharAnim::FpHold;
    const ModelAnimation& ref = c.anims[c.animIndex[(int)(fp ? CharAnim::FpHold : CharAnim::Idle)]];
    const ModelAnimation& cur = c.anims[c.animIndex[(int)anim]];
    frame = Clamp(frame, 0, cur.frameCount - 1);
    Vector3 delta = Vector3Subtract(cur.framePoses[frame][c.handBone].translation, ref.framePoses[0][c.handBone].translation);
    return Vector3Add(fp ? c.fpGrip : c.aimGrip, delta);
}
