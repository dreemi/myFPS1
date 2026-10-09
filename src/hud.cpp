#include "hud.hpp"
#include "raymath.h"
#include <cmath>

namespace Hud {

// --- Style -----------------------------------------------------------------------
static const Color TEXT = { 236, 236, 230, 255 };
static const Color DIM = { 170, 172, 168, 255 };
static const Color ACCENT = { 255, 186, 84, 255 };   // amber
static const Color DANGER = { 232, 72, 58, 255 };
static const Color GOLDEN = { 255, 214, 92, 255 };
static const Color PANEL = { 10, 12, 14, 150 };
static const Color BRASS = { 206, 168, 92, 255 };

static Font font{};
static bool ownFont = false;
static Texture2D vignette{};

Font UiFont() { return font; }

bool Init() {
    // Soft dark corners (drawn over the 3D view, under the HUD)
    Image img = GenImageColor(256, 144, BLANK);
    Color* px = (Color*)img.data;
    for (int y = 0; y < 144; y++)
        for (int x = 0; x < 256; x++) {
            float u = (x + 0.5f) / 256 * 2 - 1, v = (y + 0.5f) / 144 * 2 - 1;
            float d = sqrtf(u * u * 0.8f + v * v);
            px[y * 256 + x] = { 0, 0, 0, (unsigned char)(255 * Clamp((d - 0.55f) / 0.9f, 0.0f, 1.0f) * 0.55f) };
        }
    vignette = LoadTextureFromImage(img);
    UnloadImage(img);
    SetTextureFilter(vignette, TEXTURE_FILTER_BILINEAR);

    // Bahnschrift ships with Windows 10+; read it from the system at runtime
    const char* candidates[] = { "C:/Windows/Fonts/bahnschrift.ttf", "C:/Windows/Fonts/segoeui.ttf" };
    for (const char* path : candidates) {
        if (FileExists(path)) {
            font = LoadFontEx(path, 72, nullptr, 0);
            if (font.texture.id) {
                GenTextureMipmaps(&font.texture);
                SetTextureFilter(font.texture, TEXTURE_FILTER_TRILINEAR);
                ownFont = true;
                return true;
            }
        }
    }
    font = GetFontDefault();
    return false;
}

void Shutdown() {
    if (vignette.id) UnloadTexture(vignette);
    if (ownFont) UnloadFont(font);
    ownFont = false;
}

// --- Helpers -------------------------------------------------------------------------
enum Align { LEFT, CENTER, RIGHT };

static Color A(Color c, float a) { return { c.r, c.g, c.b, (unsigned char)(c.a * Clamp(a, 0.0f, 1.0f)) }; }

static float Spacing(float size) { return size * 0.04f; }

static Vector2 Measure(const char* text, float size) { return MeasureTextEx(font, text, size, Spacing(size)); }

// Text with a soft drop shadow so it reads over bright sky and dark walls alike
static void Text(const char* text, float x, float y, float size, Color color, Align align = LEFT, float alpha = 1.0f) {
    Vector2 m = Measure(text, size);
    if (align == CENTER) x -= m.x / 2;
    if (align == RIGHT) x -= m.x;
    DrawTextEx(font, text, { x + 1.5f, y + 2.0f }, size, Spacing(size), A({ 0, 0, 0, 170 }, alpha * color.a / 255.0f));
    DrawTextEx(font, text, { x, y }, size, Spacing(size), A(color, alpha));
}

static void Panel(Rectangle r, float alpha = 1.0f) {
    DrawRectangleRounded(r, 0.25f, 8, A(PANEL, alpha));
}

// A bar with a dark 1px outline, for crosshair pieces
static void OutlinedRect(float x, float y, float w, float h, Color c) {
    DrawRectangleRec({ x - 1, y - 1, w + 2, h + 2 }, A({ 0, 0, 0, 140 }, c.a / 255.0f));
    DrawRectangleRec({ x, y, w, h }, c);
}

static void OutlinedLine(Vector2 a, Vector2 b, float thick, Color c) {
    DrawLineEx(a, b, thick + 2.0f, A({ 0, 0, 0, 150 }, c.a / 255.0f));
    DrawLineEx(a, b, thick, c);
}

// --- Pieces ----------------------------------------------------------------------------

static void Crosshair(const State& s) {
    float alpha = Clamp(1.0f - s.aim * 2.0f, 0.0f, 1.0f);
    if (alpha <= 0.0f || !s.alive) return;
    float cx = s.screenW / 2.0f, cy = s.screenH / 2.0f;
    float gap = 4.0f + s.spreadPx + s.recoil * 6.0f;
    float len = 9.0f, t = 2.0f;
    Color c = A(TEXT, alpha);
    OutlinedRect(cx - gap - len, cy - t / 2, len, t, c);
    OutlinedRect(cx + gap, cy - t / 2, len, t, c);
    OutlinedRect(cx - t / 2, cy - gap - len, t, len, c);
    OutlinedRect(cx - t / 2, cy + gap, t, len, c);
    OutlinedRect(cx - 1, cy - 1, 2, 2, c);
}

static void HitMarker(const State& s) {
    if (s.hitMarker <= 0.0f) return;
    float cx = s.screenW / 2.0f, cy = s.screenH / 2.0f;
    float k = Clamp(s.hitMarker / 0.12f, 0.0f, 1.0f);
    float inner = 6.0f, outer = (s.hitKilled ? 17.0f : 12.0f) + 3.0f * (1.0f - k);
    Color c = s.hitKilled ? DANGER : (s.hitHeadshot ? GOLDEN : TEXT);
    float thick = s.hitKilled ? 3.0f : 2.0f;
    for (int i = 0; i < 4; i++) {
        float a = (45.0f + 90.0f * i) * DEG2RAD;
        Vector2 d = { cosf(a), sinf(a) };
        OutlinedLine({ cx + d.x * inner, cy + d.y * inner }, { cx + d.x * outer, cy + d.y * outer }, thick, A(c, fminf(1.0f, k * 1.5f)));
    }
}

static void KillBanner(const State& s) {
    if (s.killBanner <= 0.0f) return;
    float t = s.killBanner;                 // counts down from 1.6
    float alpha = fminf(1.0f, t / 0.4f) * fminf(1.0f, (1.6f - t) / 0.08f + 0.3f);
    float cx = s.screenW / 2.0f, y = s.screenH / 2.0f + 46.0f;
    float pop = 1.0f + 0.25f * fmaxf(0.0f, (t - 1.45f) / 0.15f);
    Text("ELIMINATED", cx, y, 26.0f * pop, DANGER, CENTER, alpha);
    if (s.killBannerHeadshot) Text("HEADSHOT", cx, y + 30.0f, 18.0f, GOLDEN, CENTER, alpha);
}

static void HealthPanel(const State& s) {
    float x = 24, y = s.screenH - 86.0f, w = 270, h = 62;
    Panel({ x, y, w, h });
    float frac = (float)s.health / s.maxHealth;
    Color c = frac > 0.5f ? TEXT : (frac > 0.25f ? ACCENT : DANGER);
    // Medical cross
    float ix = x + 26, iy = y + h / 2;
    DrawRectangleRec({ ix - 11, iy - 4, 22, 8 }, c);
    DrawRectangleRec({ ix - 4, iy - 11, 8, 22 }, c);
    Text(TextFormat("%d", s.health), x + 50, y + 8, 42, c);
    // Segmented bar
    float bx = x + 126, by = y + 26, bw = 128, bh = 10;
    int segs = 10;
    float sw = (bw - (segs - 1) * 3.0f) / segs;
    for (int i = 0; i < segs; i++) {
        float fill = Clamp(frac * segs - i, 0.0f, 1.0f);
        Rectangle r = { bx + i * (sw + 3.0f), by, sw, bh };
        DrawRectangleRec(r, { 255, 255, 255, 38 });
        if (fill > 0.0f) DrawRectangleRec({ r.x, r.y, r.width * fill, r.height }, c);
    }
    Text("HEALTH", bx, y + 40, 13, DIM);
}

static void AmmoPanel(const State& s) {
    float cx = s.screenW - 78.0f, cy = s.screenH - 76.0f, R = 42.0f;
    Panel({ s.screenW - 278.0f, cy - 52.0f, 254.0f, 104.0f });
    // The revolver's cylinder: six chambers, loaded ones are brass
    DrawCircleV({ cx, cy }, R, { 34, 34, 38, 230 });
    DrawRing({ cx, cy }, R - 3, R, 0, 360, 48, { 80, 80, 86, 255 });
    for (int i = 0; i < s.magazine; i++) {
        float a = (-90.0f + 360.0f / s.magazine * i) * DEG2RAD; // the next round to fire is at the top
        Vector2 p = { cx + cosf(a) * 24.0f, cy + sinf(a) * 24.0f };
        bool loaded = i < s.ammo && !s.reloading;
        DrawCircleV(p, 10.5f, { 18, 18, 20, 255 });
        if (loaded) {
            DrawCircleV(p, 8.5f, BRASS);
            DrawCircleV(p, 3.0f, { 150, 112, 56, 255 }); // primer
        } else {
            DrawRing(p, 7.5f, 9.0f, 0, 360, 24, { 70, 70, 74, 255 });
        }
    }
    DrawCircleV({ cx, cy }, 5.0f, { 70, 70, 74, 255 });
    if (s.reloading) {
        DrawRing({ cx, cy }, R + 3, R + 7, -90, -90 + 360 * s.reloadProgress, 64, ACCENT);
        Text("RELOADING", cx - R - 18, cy - 30, 18, ACCENT, RIGHT);
    }
    Color c = s.ammo == 0 && !s.reloading ? DANGER : TEXT;
    Text(TextFormat("%d", s.reloading ? 0 : s.ammo), cx - R - 18, cy - 8, 46, c, RIGHT);
    Text(TextFormat("/ %d", s.magazine), cx - R - 18, cy + 34, 16, DIM, RIGHT);
    if (s.ammo == 0 && !s.reloading && s.alive)
        Text("[R] RELOAD", s.screenW / 2.0f, s.screenH / 2.0f + 40.0f, 18, DANGER, CENTER, 0.6f + 0.4f * sinf((float)GetTime() * 8.0f));
}

static void Score(const State& s) {
    float right = s.screenW - 24.0f;
    const char* line = TextFormat("KILLS  %d     DEATHS  %d", s.kills, s.deaths);
    Vector2 m = Measure(line, 18);
    Panel({ right - m.x - 20, 14, m.x + 20, 32 });
    Text(line, right - 10, 20, 18, TEXT, RIGHT);
    if (!s.feed) return;
    float y = 54;
    for (const FeedEntry& e : *s.feed) {
        float alpha = fminf(1.0f, e.life / 0.5f);
        Vector2 em = Measure(e.text.c_str(), 16);
        Panel({ right - em.x - 20, y, em.x + 20, 26 }, alpha);
        Text(e.text.c_str(), right - 10, y + 5, 16, e.color, RIGHT, alpha);
        y += 30;
    }
}

static void DamageArcs(const State& s) {
    if (!s.damage) return;
    Vector2 c = { s.screenW / 2.0f, s.screenH / 2.0f };
    for (const DamageMarker& d : *s.damage) {
        float alpha = fminf(1.0f, d.life / 0.6f);
        float a = -90.0f + d.angle * RAD2DEG; // screen angle: up is ahead
        DrawRing(c, 128, 138, a - 22, a + 22, 32, A(DANGER, 0.85f * alpha));
        DrawRing(c, 124, 128, a - 14, a + 14, 24, A({ 255, 200, 190, 255 }, 0.5f * alpha));
    }
}

static void Vignette(const State& s) {
    float low = s.alive && s.health <= 30 ? (0.35f + 0.15f * sinf((float)GetTime() * 5.0f)) * (1.0f - s.health / 30.0f + 0.3f) : 0.0f;
    float a = Clamp(s.hurt * 0.6f + low, 0.0f, 0.75f);
    if (a <= 0.01f) return;
    int w = s.screenW, h = s.screenH;
    Color red = A({ 150, 0, 0, 255 }, a), clear = { 150, 0, 0, 0 };
    DrawRectangleGradientV(0, 0, w, h / 4, red, clear);
    DrawRectangleGradientV(0, h - h / 4, w, h / 4, clear, red);
    DrawRectangleGradientH(0, 0, w / 5, h, red, clear);
    DrawRectangleGradientH(w - w / 5, 0, w / 5, h, clear, red);
}

static const char* CONTROLS[][2] = {
    { "WASD", "Move" }, { "MOUSE", "Look" }, { "SPACE", "Jump" }, { "SHIFT", "Sprint" },
    { "CTRL / C", "Crouch" }, { "Q / E", "Lean" }, { "LMB", "Fire" }, { "RMB", "Aim" },
    { "R", "Reload" }, { "ENTER", "Respawn at other end" }, { "ESC", "Pause" }, { "F3", "Show FPS" },
};

static void Controls(float x, float y, float alpha) {
    for (int i = 0; i < 12; i++) {
        float cx = x + (i % 2) * 250.0f, cy = y + (i / 2) * 30.0f;
        Text(CONTROLS[i][0], cx + 90, cy, 18, ACCENT, RIGHT, alpha);
        Text(CONTROLS[i][1], cx + 104, cy, 18, TEXT, LEFT, alpha);
    }
}

static Rectangle ButtonRect(const State& s, int i) {
    return { s.screenW / 2.0f - 120.0f, s.screenH / 2.0f - 30.0f + i * 60.0f, 240.0f, 46.0f };
}

int PauseMenuClick(const State& s) {
    if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) return 0;
    Vector2 m = GetMousePosition();
    for (int i = 0; i < 2; i++) if (CheckCollisionPointRec(m, ButtonRect(s, i))) return i + 1;
    return 0;
}

static void PauseMenu(const State& s) {
    DrawRectangle(0, 0, s.screenW, s.screenH, { 6, 8, 10, 170 });
    Text("PAUSED", s.screenW / 2.0f, s.screenH / 2.0f - 130.0f, 56, TEXT, CENTER);
    const char* labels[2] = { "RESUME", "QUIT" };
    Vector2 m = GetMousePosition();
    for (int i = 0; i < 2; i++) {
        Rectangle r = ButtonRect(s, i);
        bool hover = CheckCollisionPointRec(m, r);
        DrawRectangleRounded(r, 0.2f, 8, hover ? Color{ 255, 186, 84, 230 } : Color{ 30, 32, 36, 220 });
        Text(labels[i], r.x + r.width / 2, r.y + 11, 24, hover ? Color{ 20, 18, 14, 255 } : TEXT, CENTER);
    }
    Controls(s.screenW / 2.0f - 250.0f, s.screenH / 2.0f + 110.0f, 0.9f);
}

static void TitleScreen(const State& s) {
    DrawRectangleGradientV(0, 0, s.screenW, s.screenH, { 6, 8, 12, 150 }, { 6, 8, 12, 235 });
    float cx = s.screenW / 2.0f;
    Text("DUSTYARD", cx, 150, 96, TEXT, CENTER);
    DrawRectangle((int)cx - 150, 252, 300, 3, ACCENT);
    Text("One revolver. One rival. Three lanes.", cx, 268, 22, DIM, CENTER);
    Controls(cx - 250.0f, 340.0f, 1.0f);
    float blink = 0.55f + 0.45f * sinf((float)GetTime() * 3.0f);
    Text("CLICK TO PLAY", cx, s.screenH - 110.0f, 30, ACCENT, CENTER, blink);
}

static void DeathScreen(const State& s) {
    DrawRectangle(0, 0, s.screenW, s.screenH, { 20, 0, 0, 120 });
    float cx = s.screenW / 2.0f, cy = s.screenH / 2.0f;
    Text("YOU DIED", cx, cy - 70, 76, DANGER, CENTER);
    Text("Respawning", cx, cy + 20, 20, TEXT, CENTER);
    float frac = Clamp(1.0f - s.deathTimer / 2.5f, 0.0f, 1.0f);
    DrawRectangleRec({ cx - 120, cy + 52, 240, 6 }, { 255, 255, 255, 40 });
    DrawRectangleRec({ cx - 120, cy + 52, 240 * frac, 6 }, TEXT);
}

void Draw(const State& s) {
    if (vignette.id)
        DrawTexturePro(vignette, { 0, 0, (float)vignette.width, (float)vignette.height }, { 0, 0, (float)s.screenW, (float)s.screenH },
                       { 0, 0 }, 0, WHITE);
    if (!s.started) { TitleScreen(s); return; }

    Vignette(s);
    if (s.alive) {
        Crosshair(s);
        HitMarker(s);
        KillBanner(s);
        DamageArcs(s);
        HealthPanel(s);
        AmmoPanel(s);
    } else {
        DeathScreen(s);
    }
    Score(s);
    if (s.showFps) Text(TextFormat("%d FPS", GetFPS()), 24, 18, 18, { 120, 230, 120, 255 });
    if (s.paused) PauseMenu(s);
}

} // namespace Hud
