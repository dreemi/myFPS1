#include "character.hpp"
#include "raymath.h"
#include <cstdio>
#include <cstring>

static const char* ANIM_NAMES[(int)CharAnim::Count] = { "idle", "walk", "run", "crouch", "crouch_walk", "fp_hold", "rest",
                                                         "fp_rifle" };

bool LoadCharacter(CharacterAssets& c, const std::string& assetDir) {
    std::string dir = assetDir + "models/character/";
    c.model = LoadModel((dir + "character.glb").c_str());
    if (c.model.meshCount == 0) {
        TraceLog(LOG_WARNING, "CHARACTER: model missing from %s", dir.c_str());
        return false;
    }
    c.anims = LoadModelAnimations((dir + "character.glb").c_str(), &c.animCount);
    for (int k = 0; k < (int)CharAnim::Count; k++) c.animIndex[k] = -1;
    for (int a = 0; a < c.animCount; a++)
        for (int k = 0; k < (int)CharAnim::Count; k++)
            if (strcmp(c.anims[a].name, ANIM_NAMES[k]) == 0) c.animIndex[k] = a;
    for (int i = 0; i < c.model.boneCount; i++)
        if (strcmp(c.model.bones[i].name, "hand_r") == 0) c.handBone = i;
        else if (strcmp(c.model.bones[i].name, "head") == 0) c.headBone = i;

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
            } else if (sscanf(line, "fp_rifle_grip %f %f %f", &v.x, &v.y, &v.z) == 3) {
                c.fpRifleGrip = v;
            } else {
                bool rifle = sscanf(line, "fp_rifle_left %d %f %f %f", &index, &v.x, &v.y, &v.z) == 4;
                if (rifle || sscanf(line, "fp_left %d %f %f %f", &index, &v.x, &v.y, &v.z) == 4) {
                    std::vector<Vector3>& track = c.leftPalm[(int)(rifle ? CharAnim::FpRifle : CharAnim::FpHold)];
                    if (index >= 0 && index < 1000) {
                        if ((int)track.size() <= index) track.resize(index + 1, v);
                        track[index] = v;
                    }
                }
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

Matrix CharacterHandTransform(const CharacterAssets& c, CharAnim anim, int frame) {
    if (!c.loaded) return MatrixIdentity();
    const ModelAnimation& a = c.anims[c.animIndex[(int)anim]];
    frame = Clamp(frame, 0, a.frameCount - 1);
    const Transform& t = a.framePoses[frame][c.handBone];
    return MatrixMultiply(MatrixMultiply(MatrixScale(t.scale.x, t.scale.y, t.scale.z), QuaternionToMatrix(t.rotation)),
                          MatrixTranslate(t.translation.x, t.translation.y, t.translation.z));
}

static Matrix TransformMatrix(const Transform& t) {
    return MatrixMultiply(MatrixMultiply(MatrixScale(t.scale.x, t.scale.y, t.scale.z), QuaternionToMatrix(t.rotation)),
                          MatrixTranslate(t.translation.x, t.translation.y, t.translation.z));
}

Matrix CharacterHeadMotion(const CharacterAssets& c, CharAnim anim, int frame) {
    if (!c.loaded || c.headBone < 0) return MatrixIdentity();
    const ModelAnimation& a = c.anims[c.animIndex[(int)anim]];
    frame = Clamp(frame, 0, a.frameCount - 1);
    return MatrixMultiply(MatrixInvert(TransformMatrix(c.model.bindPose[c.headBone])), TransformMatrix(a.framePoses[frame][c.headBone]));
}

Vector3 CharacterFpLeftPalm(const CharacterAssets& c, CharAnim anim, float blenderFrame) {
    const std::vector<Vector3>& track = c.leftPalm[(int)anim];
    if (track.empty()) return { 0, 0, 0 };
    float f = Clamp(blenderFrame, 0.0f, (float)(track.size() - 1));
    int i = (int)f;
    int j = i + 1 < (int)track.size() ? i + 1 : i;
    return Vector3Lerp(track[i], track[j], f - (float)i);
}

Vector3 CharacterGripPoint(const CharacterAssets& c, CharAnim anim, int frame) {
    if (!c.loaded) return c.aimGrip;
    // The exported grip point is for the first frame of the hold poses; other
    // frames move the hand (walk bob, reload) - follow the hand bone.
    bool fp = anim == CharAnim::FpHold || anim == CharAnim::FpRifle;
    const ModelAnimation& ref = c.anims[c.animIndex[(int)(fp ? anim : CharAnim::Idle)]];
    const ModelAnimation& cur = c.anims[c.animIndex[(int)anim]];
    frame = Clamp(frame, 0, cur.frameCount - 1);
    Vector3 delta = Vector3Subtract(cur.framePoses[frame][c.handBone].translation, ref.framePoses[0][c.handBone].translation);
    Vector3 base = anim == CharAnim::FpHold ? c.fpGrip : (anim == CharAnim::FpRifle ? c.fpRifleGrip : c.aimGrip);
    return Vector3Add(base, delta);
}
