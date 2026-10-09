"""
Generates the map's building textures (assets/textures/): plaster, sandstone
blocks, weathered wood planks and wooden crates. Each gets a colour map, a
normal map (OpenGL convention: green points up the image) and a roughness map.
All tile seamlessly except the crate, which covers one face of a crate.

Run with Blender 4.x (for its bundled numpy):
  blender --background --factory-startup --python tools/make_textures.py -- assets/textures
"""
import bpy, sys, os, math
import numpy as np

OUT = os.path.abspath(sys.argv[sys.argv.index("--") + 1])
os.makedirs(OUT, exist_ok=True)
N = 1024
rng = np.random.default_rng(11)


def log(*a):
    print("[textures]", *a)


# --- Helpers (arrays are top-down: row 0 is the top of the image) ------------------

def value_noise(size, cells, seed=None):
    """Tileable smooth value noise in 0..1 (cells x cells lattice)."""
    g = (np.random.default_rng(seed) if seed is not None else rng).random((cells, cells))
    idx = np.arange(size) * cells / size
    i0 = np.floor(idx).astype(int)
    f = idx - i0
    f = f * f * (3 - 2 * f)
    i1 = (i0 + 1) % cells
    a = g[i0][:, i0] * (1 - f)[None, :] + g[i0][:, i1] * f[None, :]
    b = g[i1][:, i0] * (1 - f)[None, :] + g[i1][:, i1] * f[None, :]
    return a * (1 - f)[:, None] + b * f[:, None]


def fbm(size, base, octaves, gain=0.5):
    out, amp, total = np.zeros((size, size)), 1.0, 0.0
    for o in range(octaves):
        out += amp * value_noise(size, base * 2 ** o)
        total += amp
        amp *= gain
    return out / total


def stretched(size, cx, cy):
    """Tileable noise with different cell counts across (cx) and down (cy): streaks."""
    g = rng.random((cy, cx))
    xs = np.arange(size) * cx / size
    ys = np.arange(size) * cy / size
    x0, y0 = np.floor(xs).astype(int), np.floor(ys).astype(int)
    fx, fy = xs - x0, ys - y0
    fx, fy = fx * fx * (3 - 2 * fx), fy * fy * (3 - 2 * fy)
    x1, y1 = (x0 + 1) % cx, (y0 + 1) % cy
    a = g[y0][:, x0] * (1 - fx)[None, :] + g[y0][:, x1] * fx[None, :]
    b = g[y1][:, x0] * (1 - fx)[None, :] + g[y1][:, x1] * fx[None, :]
    return a * (1 - fy)[:, None] + b * fy[:, None]


def normals_from_height(h, strength):
    dx = (np.roll(h, -1, 1) - np.roll(h, 1, 1)) * strength
    dy = (np.roll(h, 1, 0) - np.roll(h, -1, 0)) * strength  # up the image is +y
    n = np.stack([-dx, -dy, np.ones_like(h)], axis=2)
    n /= np.linalg.norm(n, axis=2, keepdims=True)
    return n * 0.5 + 0.5


def save(name, rgb):
    """rgb: (h, w, 3) or (h, w) floats 0..1, top-down. Saved as PNG."""
    if rgb.ndim == 2:
        rgb = np.repeat(rgb[..., None], 3, axis=2)
    h, w = rgb.shape[:2]
    img = bpy.data.images.new(name, w, h, alpha=False)
    px = np.ones((h, w, 4), dtype=np.float32)
    px[..., :3] = np.clip(rgb[::-1], 0, 1)  # Blender stores the bottom row first
    img.pixels.foreach_set(px.ravel())
    img.filepath_raw = os.path.join(OUT, name + ".png")
    img.file_format = "PNG"
    img.save()
    log("wrote", name)


def srgb(*c):
    return np.array(c) / 255.0


def lerp_color(a, b, t):
    return a + (b - a) * t[..., None]


Y, X = np.mgrid[0:N, 0:N] / N  # 0..1 down and across


# --- Plaster: limewashed render over brick, stained and chipped (tile 3 m) ----------
def plaster():
    base = srgb(214, 196, 162)
    warm = srgb(196, 168, 124)
    stains = fbm(N, 3, 4)
    mottle = fbm(N, 16, 3)
    grain = fbm(N, 128, 2)
    color = lerp_color(base, warm, np.clip((stains - 0.35) * 1.6, 0, 1))
    color *= (0.94 + 0.10 * (mottle - 0.5) + 0.06 * (grain - 0.5))[..., None]
    # Rain streaks running down
    streak = stretched(N, 48, 3)
    color *= (1.0 - 0.05 * np.clip((streak - 0.6) * 3, 0, 1))[..., None]
    # Chipped patches showing the brick underneath
    chip_field = fbm(N, 6, 4)
    chipped = chip_field > 0.76
    edge = np.clip((chip_field - 0.72) / 0.04, 0, 1)
    # Brick under the render: 24 x 7 cm bricks, running bond
    rows = 3.0 / 0.075
    by = Y * rows
    row = np.floor(by)
    bx = X * (3.0 / 0.25) + 0.5 * (row % 2)
    mortar = (np.minimum(by - row, 1 - (by - row)) < 0.12) | (np.minimum(bx % 1, 1 - bx % 1) < 0.04)
    brick = lerp_color(srgb(168, 96, 66), srgb(140, 78, 54), value_noise(N, 64))
    brick = np.where(mortar[..., None], srgb(170, 158, 140), brick)
    color = np.where(chipped[..., None], brick, color)
    # A soft lip of broken render around each chip
    color *= (1.0 - 0.18 * (edge * (1 - edge) * 4))[..., None]

    height = 0.5 + 0.25 * (fbm(N, 64, 3) - 0.5) + 0.15 * (grain - 0.5)
    height = np.where(chipped, 0.1 - 0.15 * mortar, height)
    save("plaster_color", color)
    save("plaster_normal", normals_from_height(height, 6.0))
    rough = 0.88 + 0.08 * (mottle - 0.5)
    save("plaster_rough", np.where(chipped, 0.92, rough))


# --- Sandstone ashlar blocks, 60 x 30 cm, running bond (tile 2.4 m) ------------------
def sandstone():
    cols, rows = 4, 8
    by = Y * rows
    row = np.floor(by)
    bx = X * cols + 0.5 * (row % 2)
    col = np.floor(bx)
    fy, fx = by - row, bx - col
    # Distance to the block's edge, in metres
    d = np.minimum(np.minimum(fx, 1 - fx) * 0.6, np.minimum(fy, 1 - fy) * 0.3)
    wear = fbm(N, 24, 3)
    mortar_w = 0.008 + 0.006 * wear
    in_block = d > mortar_w
    # Each block its own shade of tan
    block_id = ((row * 7 + col % cols) % 32).astype(int)
    shade = np.random.default_rng(3).random(64)[block_id]
    light, dark = srgb(210, 178, 130), srgb(176, 140, 94)
    color = lerp_color(dark, light, 0.35 + 0.5 * shade)
    color *= (0.92 + 0.14 * (fbm(N, 32, 3) - 0.5))[..., None]
    pits = value_noise(N, 256) > 0.82
    color *= np.where(pits, 0.85, 1.0)[..., None]
    color = np.where(in_block[..., None], color, srgb(176, 160, 132))
    bevel = np.clip((d - mortar_w) / 0.02, 0, 1)
    height = np.where(in_block, 0.4 + 0.6 * np.sqrt(bevel) + 0.08 * (wear - 0.5) - 0.1 * pits, 0.0)
    save("sandstone_color", color)
    save("sandstone_normal", normals_from_height(height, 5.0))
    save("sandstone_rough", np.where(in_block, 0.80 + 0.1 * (wear - 0.5), 0.95))


# --- Weathered wood planks, 20 cm wide, running across (tile 2 m) --------------------
def planks():
    count = 10
    py = Y * count
    plank = np.floor(py)
    fy = py - plank
    # Each plank is cut somewhere along its length
    cut = np.random.default_rng(5).random(count)[plank.astype(int)]
    fx = (X + cut) % 1.0
    seam = (np.minimum(fy, 1 - fy) < 0.035) | (np.minimum(fx, 1 - fx) < 0.004)
    tone = np.random.default_rng(6).random(count)[plank.astype(int)]
    warp = fbm(N, 8, 3)
    fibres = stretched(N, 5, 260)
    grain = 0.65 * fibres + 0.35 * (np.sin((fy * 7 + warp * 3 + tone * 10) * math.pi) * 0.5 + 0.5)
    rings = stretched(N, 3, 90)
    old, fresh = srgb(118, 98, 78), srgb(160, 128, 92)
    color = lerp_color(old, fresh, 0.3 + 0.4 * tone)
    color *= (0.85 + 0.15 * grain + 0.08 * (rings - 0.5))[..., None]
    color *= (0.9 + 0.2 * (fbm(N, 4, 3) - 0.5))[..., None]  # sun bleaching
    color = np.where(seam[..., None], color * 0.35, color)
    nails = np.zeros((N, N), bool)
    for x in (0.03, 0.97):  # a pair of nails near each plank end
        nx = (x - cut[plank.astype(int)]) % 1.0
        nails |= ((fx - x) ** 2 * 40 + (fy - 0.5) ** 2) < 0.004
    color = np.where(nails[..., None], srgb(60, 50, 45), color)
    height = np.where(seam, 0.0, 0.6 + 0.1 * grain + 0.05 * rings)
    save("planks_color", color)
    save("planks_normal", normals_from_height(height, 5.0))
    save("planks_rough", np.where(seam, 0.95, 0.75 + 0.1 * (1 - grain)))


# --- Crate face: a frame of boards with a diagonal brace over vertical planks ---------
def crate():
    frame = 0.11
    in_frame = (X < frame) | (X > 1 - frame) | (Y < frame) | (Y > 1 - frame)
    # Diagonal brace from bottom-left to top-right
    diag = np.abs((X - (1 - Y))) / math.sqrt(2)
    in_brace = (diag < 0.055) & ~in_frame
    # Vertical planks behind
    pcount = 5
    px = X * pcount
    pf = px - np.floor(px)
    seam = np.minimum(pf, 1 - pf) < 0.02
    ptone = np.random.default_rng(8).random(pcount)[np.minimum(np.floor(px), pcount - 1).astype(int)]
    warp = fbm(N, 6, 3)
    grain_v = 0.65 * stretched(N, 260, 5) + 0.35 * (np.sin((pf * 6 + warp * 3 + ptone * 9) * math.pi) * 0.5 + 0.5)
    pine, pine2 = srgb(196, 160, 106), srgb(170, 130, 82)
    color = lerp_color(pine2, pine, 0.3 + 0.5 * ptone) * (0.86 + 0.14 * grain_v)[..., None]
    color = np.where(seam[..., None], color * 0.4, color)
    # Frame and brace boards: a touch darker, grain along their length
    grain_h = 0.7 * stretched(N, 4, 300) + 0.3 * (np.sin((Y * 30 + warp * 3) * math.pi) * 0.5 + 0.5)
    board = lerp_color(srgb(160, 124, 80), srgb(184, 146, 96), fbm(N, 12, 2)) * (0.88 + 0.12 * grain_h)[..., None]
    color = np.where((in_frame | in_brace)[..., None], board, color)
    # Board edges darken; a stencil band; dirt toward the bottom
    edge_dist = np.minimum.reduce([np.abs(X - frame), np.abs(X - (1 - frame)), np.abs(Y - frame), np.abs(Y - (1 - frame))])
    color *= np.where(edge_dist < 0.006, 0.45, 1.0)[..., None]
    color *= np.where((np.abs(diag - 0.055) < 0.005) & ~in_frame, 0.5, 1.0)[..., None]
    color *= (1.0 - 0.25 * np.clip(Y - 0.6, 0, 1) * fbm(N, 8, 3))[..., None]
    nails = np.zeros((N, N), bool)
    for cx in (frame / 2, 1 - frame / 2):
        for cy in (frame / 2, 1 - frame / 2, 0.5):
            nails |= ((X - cx) ** 2 + (Y - cy) ** 2) < 0.00012
    color = np.where(nails[..., None], srgb(55, 50, 48), color)
    height = np.where(in_frame | in_brace, 0.9, np.where(seam, 0.0, 0.5)) + 0.05 * grain_v
    save("crate_color", color)
    save("crate_normal", normals_from_height(height, 4.0))
    save("crate_rough", np.where(seam, 0.95, 0.78))


# --- Sand: packed, wind-rippled, with pebbles (tile 4 m) ------------------------------
def sand():
    warp = fbm(N, 4, 3)
    ripples = np.sin((Y * 38 + X * 6 + warp * 5) * math.pi * 2) * 0.5 + 0.5
    ripples *= fbm(N, 3, 2)  # ripples come and go
    patches = fbm(N, 5, 4)
    grain = fbm(N, 256, 1)
    color = lerp_color(srgb(168, 140, 100), srgb(188, 160, 116), patches)
    color *= (0.93 + 0.08 * ripples + 0.08 * (grain - 0.5))[..., None]
    # Pebbles: small darker stones scattered about
    pv = value_noise(N, 180)
    pebbles = pv > 0.86
    stone = lerp_color(srgb(150, 130, 105), srgb(175, 160, 140), value_noise(N, 90))
    color = np.where(pebbles[..., None], stone, color)
    # Darker where feet have scuffed it
    color *= (1.0 - 0.10 * np.clip((fbm(N, 6, 3) - 0.55) * 4, 0, 1))[..., None]
    height = 0.3 * ripples + 0.1 * grain + np.where(pebbles, 0.5 * (pv - 0.86) / 0.14, 0.0)
    save("sand_color", color)
    save("sand_normal", normals_from_height(height, 4.0))
    save("sand_rough", np.where(pebbles, 0.75, 0.95))


sand()
plaster()
sandstone()
planks()
crate()
