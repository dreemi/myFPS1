#include "render.hpp"
#include "scenery_mesh.hpp"
#include "raymath.h"
#include "rlgl.h"
#include <cstring>
#include <algorithm>
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
// The level: one merged model per surface kind (boxes), the scenery's models, the ground
struct LevelPart {
    Model model;
    SurfaceLook look;
    enum Kind { Boxes, Scenery, Leaves, Terrain } kind;
    BoundingBox bounds = { { -1e6f, -1e6f, -1e6f }, { 1e6f, 1e6f, 1e6f } };
    bool background = false;   // left out of the shadow pass
};
static Matrix cullVP = MatrixIdentity();   // the current pass's view-projection, for skipping what's off screen
static Vector3 viewPos{};
static std::vector<int> drawOrder;
static std::vector<LevelPart> level;
static Texture2D groundMask{};
static Vector2 maskOrigin{}, maskSize{};
static TerrainLook terrainLook;
static float fogDensity = FOG_DENSITY;
static bool inShadowPass = false;
static bool shadowFollows = false;   // big maps: the shadow map covers the area round the camera
static float shadowExtent = 96.0f;
static std::string assets;                 // where models are loaded from
// Scanned models placed in the level: each file loaded once, drawn wherever it stands
struct PropModel { std::string name; Model model; };
struct PropDraw { int model; Matrix transform; BoundingBox bounds; };
static std::vector<PropModel> propModels;
static std::vector<PropDraw> props;
// GPU skinning: the bone attributes are only switched on while a character is drawn
// (raylib would otherwise bind bone buffers for every mesh, and most have none)
static int boneIdsLoc = -1, boneWeightsLoc = -1, boneMatricesLoc = -1;

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
    assets = assetDir;
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
    lit.locs[SHADER_LOC_MAP_ROUGHNESS] = Loc(lit, "texture3");  // the terrain's extra textures
    lit.locs[SHADER_LOC_MAP_OCCLUSION] = Loc(lit, "texture4");
    SetFloat(lit, "alphaCut", 0.0f);
    boneIdsLoc = lit.locs[SHADER_LOC_VERTEX_BONEIDS];
    boneWeightsLoc = lit.locs[SHADER_LOC_VERTEX_BONEWEIGHTS];
    boneMatricesLoc = lit.locs[SHADER_LOC_BONE_MATRICES];
    lit.locs[SHADER_LOC_VERTEX_BONEIDS] = lit.locs[SHADER_LOC_VERTEX_BONEWEIGHTS] = lit.locs[SHADER_LOC_BONE_MATRICES] = -1;
    SetInt(lit, "skinned", 0);
    SetInt(lit, "twoSided", 0);
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
        for (int k = 0; k < 12; k++) m.materials[i].maps[k].texture = Texture2D{}; // raylib's MAX_MATERIAL_MAPS
    for (int i = 0; i < m.materialCount; i++) m.materials[i].shader = Shader{ rlGetShaderIdDefault(), rlGetShaderLocsDefault() };
    UnloadModel(m);
    m = Model{};
}

static void ClearLevel() {
    for (LevelPart& p : level) UnloadKeepTextures(p.model);
    level.clear();
    for (PropModel& m : propModels) {
        DetachLitShader(m.model);
        UnloadModel(m.model);
    }
    propModels.clear();
    props.clear();
    if (groundMask.id) UnloadTexture(groundMask);
    groundMask = Texture2D{};
}

void Shutdown() {
    ClearLevel();
    if (shadowMap.id) rlUnloadFramebuffer(shadowMap.id);
    if (shadowMap.depth.id) rlUnloadTexture(shadowMap.depth.id);
    if (IsShaderValid(lit)) UnloadShader(lit);
    if (IsShaderValid(sky)) UnloadShader(sky);
    ready = false;
}

bool Ready() { return ready; }

bool BeginSkinned() {
    if (!ready || boneIdsLoc < 0 || boneWeightsLoc < 0 || boneMatricesLoc < 0) return false;
    rlDrawRenderBatchActive();
    lit.locs[SHADER_LOC_VERTEX_BONEIDS] = boneIdsLoc;
    lit.locs[SHADER_LOC_VERTEX_BONEWEIGHTS] = boneWeightsLoc;
    lit.locs[SHADER_LOC_BONE_MATRICES] = boneMatricesLoc;
    SetInt(lit, "skinned", 1);
    return true;
}

void EndSkinned() {
    if (!ready) return;
    rlDrawRenderBatchActive();
    lit.locs[SHADER_LOC_VERTEX_BONEIDS] = lit.locs[SHADER_LOC_VERTEX_BONEWEIGHTS] = lit.locs[SHADER_LOC_BONE_MATRICES] = -1;
    SetInt(lit, "skinned", 0);
}
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
                        Texture2D normal, float groundHalfW = ARENA_WIDTH / 2, float groundHalfL = ARENA_LENGTH / 2) {
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
        float hw = groundHalfW, hl = groundHalfL;
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

static Model WrapMesh(Mesh mesh, Texture2D albedo, Texture2D rough, Texture2D normal) {
    Model m = LoadModelFromMesh(mesh);
    m.materials[0].shader = lit;
    m.materials[0].maps[MATERIAL_MAP_ALBEDO].texture = albedo;
    m.materials[0].maps[MATERIAL_MAP_METALNESS].texture = rough;
    m.materials[0].maps[MATERIAL_MAP_NORMAL].texture = normal;
    return m;
}

// The painted ground: one big quad (to the horizon), textured by the mask in the shader
static void BuildTerrain(const Scenery::GroundMask& g, const WorldMaterials& mat) {
    Image img = { (void*)g.rgba.data(), g.width, g.height, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
    groundMask = LoadTextureFromImage(img);
    SetTextureFilter(groundMask, TEXTURE_FILTER_BILINEAR);
    SetTextureWrap(groundMask, TEXTURE_WRAP_CLAMP);
    maskOrigin = g.origin;
    maskSize = { g.width * g.cell, g.height * g.cell };
    terrainLook = mat.terrain;
    const float e = 1500.0f;
    Mesh mesh{};
    mesh.vertexCount = 4;
    mesh.triangleCount = 2;
    mesh.vertices = (float*)MemAlloc(12 * sizeof(float));
    mesh.normals = (float*)MemAlloc(12 * sizeof(float));
    mesh.texcoords = (float*)MemAlloc(8 * sizeof(float));
    mesh.indices = (unsigned short*)MemAlloc(6 * sizeof(unsigned short));
    const float v[] = { -e, 0, -e, -e, 0, e, e, 0, e, e, 0, -e };
    const unsigned short idx[] = { 0, 1, 2, 0, 2, 3 };
    for (int i = 0; i < 12; i++) mesh.vertices[i] = v[i];
    for (int i = 0; i < 4; i++) {
        mesh.normals[i * 3] = 0; mesh.normals[i * 3 + 1] = 1; mesh.normals[i * 3 + 2] = 0;
        mesh.texcoords[i * 2] = 0; mesh.texcoords[i * 2 + 1] = 0;
    }
    for (int i = 0; i < 6; i++) mesh.indices[i] = idx[i];
    UploadMesh(&mesh, false);
    Model m = WrapMesh(mesh, mat.terrain.grass, mat.terrain.dirt, mat.terrain.field);
    m.materials[0].maps[MATERIAL_MAP_ROUGHNESS].texture = mat.terrain.forest;
    m.materials[0].maps[MATERIAL_MAP_OCCLUSION].texture = groundMask;
    level.push_back({ m, SurfaceLook{}, LevelPart::Terrain });
}

void BuildLevel(const World& world, const WorldMaterials& mat) {
    if (!ready || !mat.loaded) return;
    ClearLevel();
    fogDensity = world.fogDensity;
    shadowFollows = world.length > 120.0f;
    shadowExtent = shadowFollows ? 150.0f : 96.0f;
    const Scenery* sc = world.scenery.get();
    bool paintedGround = sc && sc->hasGround;
    for (int s = 1; s < (int)Surface::Count; s++) {
        std::vector<const WorldBox*> boxes;
        for (const WorldBox& b : world)
            if ((int)b.surface == s && !b.isTarget && !b.Hidden()) boxes.push_back(&b);
        bool ground = (Surface)s == Surface::Concrete && !paintedGround; // a paved ground (Dustyard)
        if (boxes.empty() && !ground) continue;
        const SurfaceLook& look = mat.Of((Surface)s);
        // The ground reaches well past the edges of maps whose edges are open to view
        float gw = world.map == MapId::Dustyard ? world.width / 2 : world.width / 2 + 150.0f;
        float gl = world.map == MapId::Dustyard ? world.length / 2 : world.length / 2 + 150.0f;
        level.push_back({ BuildModel(boxes, ground, look.albedo, look.rough, look.normal, gw, gl), look, LevelPart::Boxes });
    }
    if (sc) {
        SceneryMeshes meshes = BuildSceneryMeshes(*sc);
        for (SceneryMeshes::Group& g : meshes.solids) {
            const SurfaceLook& look = mat.Of(g.surface);
            for (SceneryMeshes::Piece& p : g.pieces)
                level.push_back({ WrapMesh(p.mesh, look.albedo, look.rough, look.normal), look, LevelPart::Scenery, p.bounds, p.background });
        }
        for (SceneryMeshes::Piece& p : meshes.leaves)
            level.push_back({ WrapMesh(p.mesh, mat.leaves, Texture2D{}, Texture2D{}), SurfaceLook{}, LevelPart::Leaves, p.bounds, p.background });
        if (paintedGround) BuildTerrain(sc->ground, mat);
        // Scanned models
        for (const Scenery::Prop& p : sc->props) {
            int index = -1;
            for (int i = 0; i < (int)propModels.size(); i++)
                if (propModels[i].name == p.model) index = i;
            if (index < 0) {
                Model m = LoadModel((assets + "models/scans/" + p.model + ".glb").c_str());
                if (m.meshCount == 0) {
                    TraceLog(LOG_WARNING, "RENDER: scan model %s missing", p.model.c_str());
                    continue;
                }
                for (int k = 0; k < m.materialCount; k++)
                    for (int map : { MATERIAL_MAP_ALBEDO, MATERIAL_MAP_NORMAL }) {
                        Texture2D& t = m.materials[k].maps[map].texture;
                        if (t.id == 0 || t.id == rlGetTextureIdDefault()) continue;
                        GenTextureMipmaps(&t);
                        SetTextureFilter(t, TEXTURE_FILTER_TRILINEAR);
                        SetTextureFilter(t, TEXTURE_FILTER_ANISOTROPIC_8X);
                    }
                ApplyLitShader(m);
                propModels.push_back({ p.model, m });
                index = (int)propModels.size() - 1;
            }
            Matrix xf = MatrixMultiply(MatrixMultiply(MatrixScale(p.scale, p.scale, p.scale), MatrixRotateY(p.yaw)),
                                       MatrixTranslate(p.position.x, p.position.y, p.position.z));
            xf = MatrixMultiply(propModels[index].model.transform, xf);
            // World bounds: the model's box, turned and moved
            BoundingBox mb = GetModelBoundingBox(propModels[index].model);
            BoundingBox wb = { { 1e9f, 1e9f, 1e9f }, { -1e9f, -1e9f, -1e9f } };
            for (int c = 0; c < 8; c++) {
                Vector3 corner = { c & 1 ? mb.max.x : mb.min.x, c & 2 ? mb.max.y : mb.min.y, c & 4 ? mb.max.z : mb.min.z };
                Vector3 w = Vector3Transform(corner, xf);
                wb.min = Vector3Min(wb.min, w);
                wb.max = Vector3Max(wb.max, w);
            }
            props.push_back({ index, xf, wb });
        }
    }
}

// Is any of the box inside the view volume of `vp`? (all corners outside one clip plane = no)
static bool InView(const BoundingBox& b, const Matrix& vp) {
    if (b.min.x < -1e5f) return true;
    int outside[6] = { 0, 0, 0, 0, 0, 0 };
    for (int i = 0; i < 8; i++) {
        Vector3 p = { i & 1 ? b.max.x : b.min.x, i & 2 ? b.max.y : b.min.y, i & 4 ? b.max.z : b.min.z };
        float x = vp.m0 * p.x + vp.m4 * p.y + vp.m8 * p.z + vp.m12;
        float y = vp.m1 * p.x + vp.m5 * p.y + vp.m9 * p.z + vp.m13;
        float z = vp.m2 * p.x + vp.m6 * p.y + vp.m10 * p.z + vp.m14;
        float w = vp.m3 * p.x + vp.m7 * p.y + vp.m11 * p.z + vp.m15;
        outside[0] += x < -w; outside[1] += x > w;
        outside[2] += y < -w; outside[3] += y > w;
        outside[4] += z < -w; outside[5] += z > w;
    }
    for (int k = 0; k < 6; k++)
        if (outside[k] == 8) return false;
    return true;
}

void DrawLevel() {
    // Nearest first, so the depth test throws away what's hidden behind (leaves especially)
    drawOrder.clear();
    for (int i = 0; i < (int)level.size(); i++) {
        const LevelPart& p = level[i];
        if (inShadowPass && p.background) continue;
        if (!InView(p.bounds, cullVP)) continue;
        drawOrder.push_back(i);
    }
    if (!inShadowPass) {
        auto dist = [&](int i) {
            const BoundingBox& b = level[i].bounds;
            if (b.min.x < -1e5f) return 0.0f;
            Vector3 c = Vector3Scale(Vector3Add(b.min, b.max), 0.5f);
            return Vector3DistanceSqr(c, viewPos);
        };
        std::stable_sort(drawOrder.begin(), drawOrder.end(), [&](int a, int b) { return dist(a) < dist(b); });
    }
    for (int i : drawOrder) {
        const LevelPart& p = level[i];
        switch (p.kind) {
            case LevelPart::Boxes:
                SurfaceWorldMapped(p.look.tile, p.look.specular, p.look.faceMapped);
                DrawModel(p.model, { 0, 0, 0 }, 1.0f, p.look.tint);
                break;
            case LevelPart::Scenery:
                SurfacePbr(0.0f, 1.0f, true, true, 1.0f);
                DrawModel(p.model, { 0, 0, 0 }, 1.0f, p.look.tint);
                break;
            case LevelPart::Leaves:
                SurfaceObject(0.05f);
                SetFloat(lit, "alphaCut", 0.5f);
                rlDisableBackfaceCulling();
                DrawModel(p.model, { 0, 0, 0 }, 1.0f, WHITE);
                rlDrawRenderBatchActive();
                rlEnableBackfaceCulling();
                SetFloat(lit, "alphaCut", 0.0f);
                break;
            case LevelPart::Terrain: {
                if (inShadowPass) break; // the ground only receives shadows
                rlDrawRenderBatchActive();
                SetInt(lit, "worldMapped", 0);
                SetInt(lit, "materialMode", 2);
                SetFloat(lit, "specular", 0.0f);
                SetShaderValue(lit, Loc(lit, "maskOrigin"), &maskOrigin, SHADER_UNIFORM_VEC2);
                SetShaderValue(lit, Loc(lit, "maskSize"), &maskSize, SHADER_UNIFORM_VEC2);
                Vector4 tiles = { terrainLook.grassTile, terrainLook.dirtTile, terrainLook.fieldTile, terrainLook.forestTile };
                SetShaderValue(lit, Loc(lit, "terrainTiles"), &tiles, SHADER_UNIFORM_VEC4);
                DrawModel(p.model, { 0, 0, 0 }, 1.0f, WHITE);
                break;
            }
        }
    }
    // Scanned models: one-sided scans (a facade has no back), so drawn from both sides
    if (!props.empty()) {
        SurfacePbr(0.0f, 0.9f, false, true, 1.0f);
        SetInt(lit, "twoSided", 1);
        rlDisableBackfaceCulling();
        for (const PropDraw& p : props) {
            if (!InView(p.bounds, cullVP)) continue;
            const Model& m = propModels[p.model].model;
            for (int i = 0; i < m.meshCount; i++) DrawMesh(m.meshes[i], m.materials[m.meshMaterial[i]], p.transform);
        }
        rlDrawRenderBatchActive();
        rlEnableBackfaceCulling();
        SetInt(lit, "twoSided", 0);
    }
    SurfaceObject(0.25f);
}

// --- Passes --------------------------------------------------------------------

void BeginShadowPass(Vector3 focus) {
    if (!ready) return;
    // An orthographic "camera" at the sun: framing the whole arena on a small map, or the
    // ground round the player on a big one (moved in whole shadow-map texels, so the
    // shadows' edges don't crawl as the player moves)
    Vector3 target = { 0, 0, 0 };
    if (shadowFollows) {
        Vector3 f = SUN_DIR, r = Vector3Normalize(Vector3CrossProduct(f, { 0, 1, 0 })), u = Vector3CrossProduct(r, f);
        float texel = shadowExtent / SHADOW_SIZE;
        float a = roundf(Vector3DotProduct(focus, r) / texel) * texel, b = roundf(Vector3DotProduct(focus, u) / texel) * texel;
        target = Vector3Add(Vector3Add(Vector3Scale(r, a), Vector3Scale(u, b)), Vector3Scale(f, Vector3DotProduct(focus, f)));
    }
    inShadowPass = true;
    Camera3D sun{};
    sun.target = target;
    sun.position = Vector3Add(target, Vector3Scale(SUN_DIR, -80.0f));
    sun.up = { 0, 1, 0 };
    sun.fovy = shadowExtent; // ortho height in meters
    sun.projection = CAMERA_ORTHOGRAPHIC;
    BeginTextureMode(shadowMap);
    ClearBackground(WHITE);
    BeginMode3D(sun);
    lightVP = MatrixMultiply(rlGetMatrixModelview(), rlGetMatrixProjection());
    cullVP = lightVP;
}

void EndShadowPass() {
    inShadowPass = false;
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
    cullVP = MatrixMultiply(rlGetMatrixModelview(), rlGetMatrixProjection());
    viewPos = cam.position;
    if (!ready) return;
    SetCommonLighting(SUN_DIR, cam.position, fogDensity, 1, 1.0f, { 0, 1, 0 });
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
