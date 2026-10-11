#include "materials.hpp"
#include "raymath.h"
#include "rlgl.h"

// Color map darkened by its ambient-occlusion map (the scene has no lighting,
// so AO is what gives the paving gaps and the steel corrugations their depth),
// with mipmaps and anisotropic filtering so it doesn't shimmer at a distance.
static Texture2D LoadSurfaceTexture(const std::string& colorPath, const std::string& aoPath) {
    Image color = LoadImage(colorPath.c_str());
    if (color.data == nullptr) return Texture2D{};
    ImageFormat(&color, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    Image ao = aoPath.empty() ? Image{} : LoadImage(aoPath.c_str());
    if (ao.data != nullptr && ao.width == color.width && ao.height == color.height) {
        ImageFormat(&ao, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
        unsigned char* c = (unsigned char*)color.data;
        const unsigned char* o = (const unsigned char*)ao.data;
        for (int i = 0; i < color.width * color.height; i++) {
            for (int ch = 0; ch < 3; ch++) c[i * 4 + ch] = (unsigned char)(c[i * 4 + ch] * o[i * 4] / 255);
        }
    }
    if (ao.data != nullptr) UnloadImage(ao);

    Texture2D tex = LoadTextureFromImage(color);
    UnloadImage(color);
    GenTextureMipmaps(&tex);
    SetTextureFilter(tex, TEXTURE_FILTER_TRILINEAR);       // blend between mip levels
    SetTextureFilter(tex, TEXTURE_FILTER_ANISOTROPIC_16X); // stay sharp at shallow angles
    SetTextureWrap(tex, TEXTURE_WRAP_REPEAT);
    return tex;
}

// Normal / roughness maps: used as data by the lit shader, same filtering
static Texture2D LoadDataTexture(const std::string& path) {
    Texture2D tex = LoadTexture(path.c_str());
    if (tex.id == 0) return tex;
    GenTextureMipmaps(&tex);
    SetTextureFilter(tex, TEXTURE_FILTER_TRILINEAR);
    SetTextureFilter(tex, TEXTURE_FILTER_ANISOTROPIC_16X);
    SetTextureWrap(tex, TEXTURE_WRAP_REPEAT);
    return tex;
}

WorldMaterials LoadWorldMaterials(const std::string& assetDir) {
    WorldMaterials m;
    std::string dir = assetDir + "textures/";
    // Photo-scanned sets (ambientCG): colour + AO, normal, roughness
    auto scanned = [&](Surface s, const char* name, float tile, float specular) {
        SurfaceLook& l = m.surfaces[(int)s];
        l.albedo = LoadSurfaceTexture(dir + name + "_color.jpg", dir + name + "_ao.jpg");
        l.normal = LoadDataTexture(dir + name + "_normal.jpg");
        l.rough = LoadDataTexture(dir + name + "_roughness.jpg");
        l.tile = tile;
        l.specular = specular;
    };
    // Generated sets (tools/make_textures.py)
    auto generated = [&](Surface s, const char* name, float tile, float specular, bool faceMapped, Color tint = WHITE) {
        SurfaceLook& l = m.surfaces[(int)s];
        l.albedo = LoadSurfaceTexture(dir + name + "_color.png", "");
        l.normal = LoadDataTexture(dir + name + "_normal.png");
        l.rough = LoadDataTexture(dir + name + "_rough.png");
        l.tile = tile;
        l.specular = specular;
        l.faceMapped = faceMapped;
        l.tint = tint;
    };
    scanned(Surface::Concrete, "paving", 3.0f, 0.25f);
    scanned(Surface::Steel, "steel", 2.0f, 0.9f);
    generated(Surface::Plaster, "plaster", 3.0f, 0.15f, false);
    generated(Surface::Sandstone, "sandstone", 2.4f, 0.2f, false);
    generated(Surface::Wood, "planks", 2.0f, 0.2f, false);
    generated(Surface::Crate, "crate", 1.0f, 0.2f, true);
    generated(Surface::Sand, "sand", 4.0f, 0.1f, false);
    generated(Surface::Painted, "planks", 1.2f, 0.3f, false, { 150, 205, 215, 255 }); // faded teal shutters and doors
    generated(Surface::Stone, "stone", 2.5f, 0.15f, false);
    generated(Surface::Logs, "logs", 2.0f, 0.15f, false);
    generated(Surface::Shingles, "shingles", 2.0f, 0.15f, false);
    generated(Surface::Bark, "bark", 2.0f, 0.1f, false);
    generated(Surface::Dirt, "dirt", 3.0f, 0.05f, false);
    m.terrain.grass = LoadSurfaceTexture(dir + "grass_color.png", "");
    m.terrain.dirt = m.surfaces[(int)Surface::Dirt].albedo;   // shared with the Dirt surface
    m.terrain.field = LoadSurfaceTexture(dir + "field_color.png", "");
    m.terrain.forest = LoadSurfaceTexture(dir + "forestfloor_color.png", "");
    m.leaves = LoadSurfaceTexture(dir + "leaves_color.png", "");
    m.loaded = m.terrain.grass.id && m.terrain.field.id && m.terrain.forest.id && m.leaves.id;
    for (int s = 1; s < (int)Surface::Count; s++) {
        const SurfaceLook& l = m.surfaces[s];
        if (!l.albedo.id || !l.normal.id || !l.rough.id) m.loaded = false;
    }
    if (!m.loaded) TraceLog(LOG_WARNING, "MATERIALS: textures missing from %s, using plain colors", dir.c_str());
    return m;
}

void UnloadWorldMaterials(WorldMaterials& m) {
    for (SurfaceLook& l : m.surfaces)
        for (Texture2D t : { l.albedo, l.normal, l.rough })
            if (t.id) UnloadTexture(t);
    for (Texture2D t : { m.terrain.grass, m.terrain.field, m.terrain.forest, m.leaves }) // dirt is the Dirt surface's
        if (t.id) UnloadTexture(t);
    m = WorldMaterials{};
}

static Color Shade(Color c, float k) {
    return { (unsigned char)(c.r * k), (unsigned char)(c.g * k), (unsigned char)(c.b * k), c.a };
}

// One textured box. Each face gets a fixed brightness (top brightest) as a
// cheap stand-in for lighting, so edges and corners stay readable.
static void DrawTexturedBox(Vector3 center, Vector3 size, Texture2D tex, float tile, Color tint) {
    struct Face { Vector3 n, r, u; float shade; }; // normal, texture right, texture up (r x u = n)
    static const Face faces[6] = {
        { { 1, 0, 0 }, { 0, 0, -1 }, { 0, 1, 0 }, 0.82f },
        { { -1, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 }, 0.82f },
        { { 0, 0, 1 }, { 1, 0, 0 }, { 0, 1, 0 }, 0.68f },
        { { 0, 0, -1 }, { -1, 0, 0 }, { 0, 1, 0 }, 0.68f },
        { { 0, 1, 0 }, { 1, 0, 0 }, { 0, 0, -1 }, 1.00f },
        { { 0, -1, 0 }, { 1, 0, 0 }, { 0, 0, 1 }, 0.50f },
    };
    Vector3 half = Vector3Scale(size, 0.5f);
    auto extent = [&](Vector3 axis) { return fabsf(axis.x) * half.x + fabsf(axis.y) * half.y + fabsf(axis.z) * half.z; };

    rlSetTexture(tex.id);
    rlBegin(RL_QUADS);
    for (const Face& f : faces) {
        Color c = Shade(tint, f.shade);
        rlColor4ub(c.r, c.g, c.b, c.a);
        rlNormal3f(f.n.x, f.n.y, f.n.z);
        Vector3 base = Vector3Add(center, Vector3Scale(f.n, extent(f.n)));
        Vector3 r = Vector3Scale(f.r, extent(f.r)), u = Vector3Scale(f.u, extent(f.u));
        // Counter-clockwise seen from outside: bottom-left, bottom-right, top-right, top-left
        Vector3 corners[4] = { Vector3Subtract(Vector3Subtract(base, r), u), Vector3Subtract(Vector3Add(base, r), u),
                               Vector3Add(Vector3Add(base, r), u), Vector3Add(Vector3Subtract(base, r), u) };
        for (Vector3 p : corners) {
            // World-space mapping: texture coordinates come from the position itself
            rlTexCoord2f(Vector3DotProduct(p, f.r) / tile, -Vector3DotProduct(p, f.u) / tile);
            rlVertex3f(p.x, p.y, p.z);
        }
    }
    rlEnd();
    rlSetTexture(0);
}

void DrawWorldBox(const WorldBox& box, const WorldMaterials& m) {
    Color tint = box.hitFlash > 0.0f ? RED : WHITE;
    if (m.loaded && box.surface != Surface::Plain) {
        const SurfaceLook& l = m.Of(box.surface);
        Color c = box.hitFlash > 0.0f ? RED : l.tint;
        DrawTexturedBox(box.position, box.size, l.albedo, l.tile, c);
    } else {
        DrawCubeV(box.position, box.size, box.hitFlash > 0.0f ? RED : box.color);
        DrawCubeWiresV(box.position, box.size, Fade(BLACK, 0.4f));
    }
}

Model LoadGroundModel(float width, float length, const WorldMaterials& m) {
    const float tile = m.Of(Surface::Concrete).tile;
    const float hw = width / 2, hl = length / 2;
    Mesh mesh = { 0 };
    mesh.vertexCount = 4;
    mesh.triangleCount = 2;
    mesh.vertices = (float*)MemAlloc(4 * 3 * sizeof(float));
    mesh.texcoords = (float*)MemAlloc(4 * 2 * sizeof(float));
    mesh.normals = (float*)MemAlloc(4 * 3 * sizeof(float));
    mesh.indices = (unsigned short*)MemAlloc(6 * sizeof(unsigned short));

    const float verts[] = { -hw, 0, -hl,   -hw, 0, hl,   hw, 0, hl,   hw, 0, -hl };
    const unsigned short idx[] = { 0, 1, 2,   0, 2, 3 };  // counter-clockwise seen from above
    for (int i = 0; i < 12; i++) mesh.vertices[i] = verts[i];
    for (int i = 0; i < 4; i++) {
        // Same world-space mapping as the boxes' top faces (u = x, v = z)
        mesh.texcoords[i * 2] = verts[i * 3] / tile;
        mesh.texcoords[i * 2 + 1] = verts[i * 3 + 2] / tile;
        mesh.normals[i * 3] = 0; mesh.normals[i * 3 + 1] = 1; mesh.normals[i * 3 + 2] = 0;
    }
    for (int i = 0; i < 6; i++) mesh.indices[i] = idx[i];
    UploadMesh(&mesh, false);

    Model model = LoadModelFromMesh(mesh);
    if (m.loaded) {
        model.materials[0].maps[MATERIAL_MAP_DIFFUSE].texture = m.Of(Surface::Concrete).albedo; // owned by WorldMaterials
    } else {
        model.materials[0].maps[MATERIAL_MAP_DIFFUSE].color = { 80, 95, 80, 255 };
    }
    return model;
}

void UnloadGroundModel(Model& ground) {
    // Hand the borrowed texture back so UnloadModel doesn't free it too
    ground.materials[0].maps[MATERIAL_MAP_DIFFUSE].texture = { rlGetTextureIdDefault(), 1, 1, 1,
                                                               PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
    UnloadModel(ground);
}
