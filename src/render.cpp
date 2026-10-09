#include "render.hpp"
#include "raymath.h"
#include "rlgl.h"
#include <cstring>
#include <vector>

namespace Render {

// --- Look of the scene ---------------------------------------------------------
// Late-afternoon sun from the +X/+Z side, so the lanes' walls cast long shadows
static const Vector3 SUN_DIR = Vector3Normalize({ -0.42f, -0.70f, -0.58f });
static const Vector3 SUN_COLOR = { 2.15f, 1.9f, 1.6f };       // linear
static const Vector3 SKY_AMBIENT = { 0.26f, 0.31f, 0.40f };   // linear, light from above
static const Vector3 GROUND_AMBIENT = { 0.16f, 0.14f, 0.12f };// linear, bounce from below
static const Vector3 HORIZON = { 0.74f, 0.77f, 0.80f };       // sRGB; also the fog colour
static const Vector3 ZENITH = { 0.27f, 0.45f, 0.74f };        // sRGB
static const Vector3 SUN_TINT = { 1.00f, 0.90f, 0.75f };      // sRGB
static const Vector3 ENV_GROUND = { 0.26f, 0.22f, 0.17f };    // linear: sunlit paving, as reflections see it
static const float FOG_DENSITY = 0.006f;
static const int SHADOW_SIZE = 4096;

static bool ready = false;
static Shader lit{}, sky{};
static RenderTexture2D shadowMap{};
static Matrix lightVP = MatrixIdentity();
// The level: one merged model per surface kind
struct LevelPart { Model model; SurfaceLook look; };
static std::vector<LevelPart> level;

static int Loc(Shader s, const char* name) { return GetShaderLocation(s, name); }
static void SetVec3(Shader s, const char* name, Vector3 v) { SetShaderValue(s, Loc(s, name), &v, SHADER_UNIFORM_VEC3); }
static void SetFloat(Shader s, const char* name, float v) { SetShaderValue(s, Loc(s, name), &v, SHADER_UNIFORM_FLOAT); }
static void SetInt(Shader s, const char* name, int v) { SetShaderValue(s, Loc(s, name), &v, SHADER_UNIFORM_INT); }

static RenderTexture2D LoadDepthTarget(int size) {
    RenderTexture2D t{};
    t.id = rlLoadFramebuffer();
    t.texture.width = t.texture.height = size;
    if (t.id == 0) return t;
    rlEnableFramebuffer(t.id);
    t.depth.id = rlLoadTextureDepth(size, size, false);
    t.depth.width = t.depth.height = size;
    t.depth.format = 19; // DEPTH_COMPONENT_24BIT
    t.depth.mipmaps = 1;
    rlFramebufferAttach(t.id, t.depth.id, RL_ATTACHMENT_DEPTH, RL_ATTACHMENT_TEXTURE2D, 0);
    if (!rlFramebufferComplete(t.id)) TraceLog(LOG_WARNING, "RENDER: shadow map framebuffer incomplete");
    rlDisableFramebuffer();
    return t;
}

bool Init(const std::string& assetDir) {
    std::string dir = assetDir + "shaders/";
    lit = LoadShader((dir + "lit.vs").c_str(), (dir + "lit.fs").c_str());
    sky = LoadShader(nullptr, (dir + "sky.fs").c_str());
    if (!IsShaderValid(lit) || !IsShaderValid(sky)) {
        TraceLog(LOG_WARNING, "RENDER: shaders failed to load from %s, falling back to unlit", dir.c_str());
        return false;
    }
    // raylib only looks these up for its own names; make sure DrawMesh feeds them
    lit.locs[SHADER_LOC_MATRIX_MODEL] = Loc(lit, "matModel");
    lit.locs[SHADER_LOC_MATRIX_NORMAL] = Loc(lit, "matNormal");
    shadowMap = LoadDepthTarget(SHADOW_SIZE);
    SetFloat(lit, "shadowTexel", 1.0f / SHADOW_SIZE);
    for (int i = 0; i < 2; i++) SetFlashLight(i, { 0, 0, 0 }, { 0, 0, 0 }, 0.0f);
    SurfaceObject(0.25f);
    ready = shadowMap.id != 0;
    return ready;
}

static void UnloadKeepTextures(Model& m) {
    // The level models borrow WorldMaterials' textures; don't let UnloadModel free them
    for (int i = 0; i < m.materialCount; i++)
        for (int k = 0; k < 12; k++) // raylib's MAX_MATERIAL_MAPS m.materials[i].maps[k].texture = Texture2D{};
    for (int i = 0; i < m.materialCount; i++) m.materials[i].shader = Shader{ rlGetShaderIdDefault(), rlGetShaderLocsDefault() };
    UnloadModel(m);
    m = Model{};
}

void Shutdown() {
    for (LevelPart& p : level) UnloadKeepTextures(p.model);
    level.clear();
    if (shadowMap.id) rlUnloadFramebuffer(shadowMap.id);
    if (shadowMap.depth.id) rlUnloadTexture(shadowMap.depth.id);
    if (IsShaderValid(lit)) UnloadShader(lit);
    if (IsShaderValid(sky)) UnloadShader(sky);
    ready = false;
}

bool Ready() { return ready; }
Shader LitShader() { return lit; }
Vector3 SunDirection() { return SUN_DIR; }

void ApplyLitShader(Model& model) {
    if (!ready) return;
    for (int i = 0; i < model.materialCount; i++) model.materials[i].shader = lit;
}

void DetachLitShader(Model& model) {
    for (int i = 0; i < model.materialCount; i++)
        if (model.materials[i].shader.id == lit.id) model.materials[i].shader = Shader{ rlGetShaderIdDefault(), rlGetShaderLocsDefault() };
}

// --- Level geometry ----------------------------------------------------------

static void AddQuad(std::vector<float>& v, std::vector<float>& n, std::vector<float>& uv, std::vector<unsigned short>& idx,
                    Vector3 c[4], Vector3 normal) {
    unsigned short base = (unsigned short)(v.size() / 3);
    // Corners go bottom-left, bottom-right, top-right, top-left: the face's own UVs (top of the texture up)
    static const float FACE_UV[4][2] = { { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 } };
    for (int i = 0; i < 4; i++) {
        v.insert(v.end(), { c[i].x, c[i].y, c[i].z });
        n.insert(n.end(), { normal.x, normal.y, normal.z });
        uv.insert(uv.end(), { FACE_UV[i][0], FACE_UV[i][1] });
    }
    idx.insert(idx.end(), { base, (unsigned short)(base + 1), (unsigned short)(base + 2), base, (unsigned short)(base + 2), (unsigned short)(base + 3) });
}

static Model BuildModel(const std::vector<const WorldBox*>& boxes, bool withGround, Texture2D albedo, Texture2D rough,
                        Texture2D normal) {
    std::vector<float> v, n, uv;
    std::vector<unsigned short> idx;
    struct Face { Vector3 n, r, u; };
    static const Face faces[6] = { // the bottom face only for boxes that don't sit on the ground
        { { 1, 0, 0 }, { 0, 0, -1 }, { 0, 1, 0 } }, { { -1, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 } },
        { { 0, 0, 1 }, { 1, 0, 0 }, { 0, 1, 0 } },  { { 0, 0, -1 }, { -1, 0, 0 }, { 0, 1, 0 } },
        { { 0, 1, 0 }, { 1, 0, 0 }, { 0, 0, -1 } }, { { 0, -1, 0 }, { 1, 0, 0 }, { 0, 0, 1 } },
    };
    for (const WorldBox* b : boxes) {
        Vector3 half = Vector3Scale(b->size, 0.5f);
        auto extent = [&](Vector3 a) { return fabsf(a.x) * half.x + fabsf(a.y) * half.y + fabsf(a.z) * half.z; };
        bool raised = b->position.y - half.y > 0.01f;
        for (const Face& f : faces) {
            if (f.n.y < 0.0f && !raised) continue;
            Vector3 base = Vector3Add(b->position, Vector3Scale(f.n, extent(f.n)));
            Vector3 r = Vector3Scale(f.r, extent(f.r)), u = Vector3Scale(f.u, extent(f.u));
            Vector3 c[4] = { Vector3Subtract(Vector3Subtract(base, r), u), Vector3Subtract(Vector3Add(base, r), u),
                             Vector3Add(Vector3Add(base, r), u), Vector3Add(Vector3Subtract(base, r), u) };
            AddQuad(v, n, uv, idx, c, f.n);
        }
    }
    if (withGround) {
        float hw = ARENA_WIDTH / 2, hl = ARENA_LENGTH / 2;
        Vector3 c[4] = { { -hw, 0, hl }, { hw, 0, hl }, { hw, 0, -hl }, { -hw, 0, -hl } };
        AddQuad(v, n, uv, idx, c, { 0, 1, 0 });
    }

    Mesh mesh{};
    mesh.vertexCount = (int)(v.size() / 3);
    mesh.triangleCount = (int)(idx.size() / 3);
    mesh.vertices = (float*)MemAlloc((unsigned)(v.size() * sizeof(float)));
    mesh.normals = (float*)MemAlloc((unsigned)(n.size() * sizeof(float)));
    mesh.texcoords = (float*)MemAlloc((unsigned)(uv.size() * sizeof(float))); // per face (most surfaces map in world space instead)
    memcpy(mesh.texcoords, uv.data(), uv.size() * sizeof(float));
    mesh.indices = (unsigned short*)MemAlloc((unsigned)(idx.size() * sizeof(unsigned short)));
    memcpy(mesh.vertices, v.data(), v.size() * sizeof(float));
    memcpy(mesh.normals, n.data(), n.size() * sizeof(float));
    memcpy(mesh.indices, idx.data(), idx.size() * sizeof(unsigned short));
    UploadMesh(&mesh, false);

    Model m = LoadModelFromMesh(mesh);
    m.materials[0].shader = lit;
    m.materials[0].maps[MATERIAL_MAP_ALBEDO].texture = albedo;
    m.materials[0].maps[MATERIAL_MAP_METALNESS].texture = rough;  // shader's texture1
    m.materials[0].maps[MATERIAL_MAP_NORMAL].texture = normal;    // shader's texture2
    return m;
}

void BuildLevel(const World& world, const WorldMaterials& mat) {
    if (!ready || !mat.loaded) return;
    for (int s = 1; s < (int)Surface::Count; s++) {
        std::vector<const WorldBox*> boxes;
        for (const WorldBox& b : world)
            if ((int)b.surface == s && !b.isTarget) boxes.push_back(&b);
        bool ground = (Surface)s == Surface::Concrete; // the ground is paved
        if (boxes.empty() && !ground) continue;
        const SurfaceLook& look = mat.Of((Surface)s);
        level.push_back({ BuildModel(boxes, ground, look.albedo, look.rough, look.normal), look });
    }
}

void DrawLevel() {
    for (const LevelPart& p : level) {
        SurfaceWorldMapped(p.look.tile, p.look.specular, p.look.faceMapped);
        DrawModel(p.model, { 0, 0, 0 }, 1.0f, p.look.tint);
    }
    SurfaceObject(0.25f);
}

// --- Passes --------------------------------------------------------------------

void BeginShadowPass() {
    if (!ready) return;
    // An orthographic "camera" at the sun, framing the whole arena
    Camera3D sun{};
    sun.target = { 0, 0, 0 };
    sun.position = Vector3Scale(SUN_DIR, -80.0f);
    sun.up = { 0, 1, 0 };
    sun.fovy = 96.0f; // ortho height in meters: covers the 40 x 80 m arena from this angle
    sun.projection = CAMERA_ORTHOGRAPHIC;
    BeginTextureMode(shadowMap);
    ClearBackground(WHITE);
    BeginMode3D(sun);
    lightVP = MatrixMultiply(rlGetMatrixModelview(), rlGetMatrixProjection());
}

void EndShadowPass() {
    if (!ready) return;
    EndMode3D();
    EndTextureMode();
}

void DrawSky(const Camera3D& cam, int w, int h) {
    if (!ready) { ClearBackground({ 135, 190, 230, 255 }); return; }
    Vector3 f = Vector3Normalize(Vector3Subtract(cam.target, cam.position));
    Vector3 r = Vector3Normalize(Vector3CrossProduct(f, cam.up));
    Vector3 u = Vector3CrossProduct(r, f);
    Vector2 res = { (float)w, (float)h };
    SetShaderValue(sky, Loc(sky, "resolution"), &res, SHADER_UNIFORM_VEC2);
    SetVec3(sky, "camForward", f);
    SetVec3(sky, "camRight", r);
    SetVec3(sky, "camUp", u);
    SetFloat(sky, "tanHalfFov", tanf(cam.fovy * 0.5f * DEG2RAD));
    SetVec3(sky, "sunDir", SUN_DIR);
    SetVec3(sky, "horizon", HORIZON);
    SetVec3(sky, "zenith", ZENITH);
    SetVec3(sky, "sunTint", SUN_TINT);
    BeginShaderMode(sky);
    DrawRectangle(0, 0, w, h, WHITE);
    EndShaderMode();
}

static void SetCommonLighting(Vector3 sunDir, Vector3 viewPos, float fog, int shadows, float sunVisibility, Vector3 up) {
    SetVec3(lit, "envUp", up);
    SetVec3(lit, "horizonColor", HORIZON);
    SetVec3(lit, "zenithColor", ZENITH);
    SetVec3(lit, "envGround", ENV_GROUND);
    SetVec3(lit, "sunDir", sunDir);
    SetVec3(lit, "sunColor", SUN_COLOR);
    SetVec3(lit, "skyAmbient", SKY_AMBIENT);
    SetVec3(lit, "groundAmbient", GROUND_AMBIENT);
    SetVec3(lit, "viewPos", viewPos);
    SetVec3(lit, "fogColor", HORIZON);
    SetFloat(lit, "fogDensity", fog);
    SetInt(lit, "useShadows", shadows);
    SetFloat(lit, "sunVisibility", sunVisibility);
}

void BeginWorldPass(const Camera3D& cam) {
    BeginMode3D(cam);
    if (!ready) return;
    SetCommonLighting(SUN_DIR, cam.position, FOG_DENSITY, 1, 1.0f, { 0, 1, 0 });
    SetShaderValueMatrix(lit, Loc(lit, "lightVP"), lightVP);
    int slot = 10;
    rlActiveTextureSlot(slot);
    rlEnableTexture(shadowMap.depth.id);
    rlActiveTextureSlot(0);
    SetShaderValue(lit, Loc(lit, "shadowMap"), &slot, SHADER_UNIFORM_INT);
    SurfaceObject(0.25f);
}

void EndWorldPass() {
    EndMode3D();
}

void SetViewModelLighting(const Camera3D& worldCam, float sunVisibility) {
    if (!ready) return;
    // The viewmodel is drawn with the camera at the origin looking down -Z, so
    // the sun has to be expressed in camera space
    // (rotate the direction: transform it and subtract where the origin goes)
    Matrix view = GetCameraMatrix(worldCam);
    Vector3 sunView = Vector3Normalize(Vector3Subtract(Vector3Transform(SUN_DIR, view), Vector3Transform({ 0, 0, 0 }, view)));
    // Sky above and ground below keep their world meaning in camera space
    Vector3 upView = Vector3Normalize(Vector3Subtract(Vector3Transform({ 0, 1, 0 }, view), Vector3Transform({ 0, 0, 0 }, view)));
    SetCommonLighting(sunView, { 0, 0, 0 }, 0.0f, 0, sunVisibility, upView);
    SurfaceObject(0.35f);
}

void SurfaceWorldMapped(float tileSize, float specular, bool faceMapped) {
    if (!ready) return;
    rlDrawRenderBatchActive();
    SetInt(lit, "worldMapped", 1);
    SetInt(lit, "faceMapped", faceMapped ? 1 : 0);
    SetInt(lit, "materialMode", 0);
    SetFloat(lit, "tileSize", tileSize);
    SetFloat(lit, "specular", specular);
}

void SurfaceObject(float specular) {
    if (!ready) return;
    rlDrawRenderBatchActive();
    SetInt(lit, "worldMapped", 0);
    SetInt(lit, "materialMode", 0);
    SetFloat(lit, "specular", specular);
}

void SurfacePbr(float metallic, float roughness, bool roughMap, bool normalMap, float occlusion) {
    if (!ready) return;
    rlDrawRenderBatchActive();
    SetInt(lit, "worldMapped", 0);
    SetInt(lit, "materialMode", 1);
    SetFloat(lit, "metallic", metallic);
    SetFloat(lit, "roughness", roughness);
    SetInt(lit, "useRoughMap", roughMap ? 1 : 0);
    SetInt(lit, "useNormalMap", normalMap ? 1 : 0);
    SetFloat(lit, "occlusion", occlusion);
}

void SetFlashLight(int slot, Vector3 position, Vector3 color, float radius) {
    if (!IsShaderValid(lit) || slot < 0 || slot > 1) return;
    SetVec3(lit, TextFormat("lightPos[%d]", slot), position);
    SetVec3(lit, TextFormat("lightColor[%d]", slot), color);
    SetFloat(lit, TextFormat("lightRadius[%d]", slot), radius);
}

bool SunVisible(Vector3 point, const World& world) {
    return HasLineOfSight(point, Vector3Add(point, Vector3Scale(SUN_DIR, -120.0f)), world);
}

} // namespace Render
