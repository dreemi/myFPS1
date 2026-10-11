"""
The textures every gun shares (assets/models/gun_textures/): blued steel and oiled walnut,
each an albedo (neutral for the steel: the game tints it per material), a roughness map
and a normal map, all tiling. Plus the revolver's checkered grip, cut from the same walnut
at its real size (its grip is mapped as one panel, not tiled).

Run with Blender 4.x (for its bundled numpy):
  blender --background --factory-startup --python tools/make_gun_textures.py -- assets/models/gun_textures

Scale: the rifles map one repeat to 25 cm of gun (box_uvs(ob, 0.25) in make_fal.py and
make_l42.py), so a texel is about a quarter of a millimetre; the revolver's small parts
map one repeat to 8 cm. Wood grain runs along u (along the gun).
"""
import bpy, sys, os, math
import numpy as np

args = sys.argv[sys.argv.index("--") + 1:]
OUT_DIR = os.path.abspath(args[0])
os.makedirs(OUT_DIR, exist_ok=True)
S = 1024
TILE = 0.25            # meters of gun per repeat (rifles)
rng = np.random.default_rng(42)


def log(*a):
    print("[gun_textures]", *a)


def save(name, rgb):
    """rgb: (h, w, 3) floats 0..1, row 0 = bottom (Blender's convention)."""
    h, w = rgb.shape[:2]
    img = bpy.data.images.new(name, w, h, alpha=False)
    px = np.ones((h, w, 4), dtype=np.float32)
    px[..., :3] = np.clip(rgb, 0, 1)
    img.pixels.foreach_set(px.ravel())
    img.filepath_raw = os.path.join(OUT_DIR, name + ".png")
    img.file_format = "PNG"
    img.save()
    log("wrote", name)


def gray(a):
    return np.repeat(np.clip(a, 0, 1)[..., None], 3, axis=2)


# --- Noise: value noise at any (U, V), tiling every whole unit, cells stretched as asked ----
class Noise:
    def __init__(self, cu, cv):
        self.cu, self.cv = cu, cv
        self.g = rng.random((cv, cu))

    def __call__(self, U, V):
        x, y = U * self.cu, V * self.cv
        x0, y0 = np.floor(x).astype(np.int64), np.floor(y).astype(np.int64)
        fx, fy = x - x0, y - y0
        fx, fy = fx * fx * (3 - 2 * fx), fy * fy * (3 - 2 * fy)
        x0, y0 = x0 % self.cu, y0 % self.cv
        x1, y1 = (x0 + 1) % self.cu, (y0 + 1) % self.cv
        g = self.g
        a = g[y0, x0] * (1 - fx) + g[y0, x1] * fx
        b = g[y1, x0] * (1 - fx) + g[y1, x1] * fx
        return a * (1 - fy) + b * fy


class Fbm:
    def __init__(self, cu, cv, octaves):
        self.layers = [Noise(cu * 2 ** o, cv * 2 ** o) for o in range(octaves)]

    def __call__(self, U, V):
        out, amp, total = 0.0, 1.0, 0.0
        for n in self.layers:
            out = out + amp * n(U, V)
            total += amp
            amp *= 0.5
        return out / total


def smoothstep(a, b, x):
    t = np.clip((x - a) / (b - a), 0, 1)
    return t * t * (3 - 2 * t)


def normal_map(height, strength):
    """OpenGL-style tangent normals from a height field (the game flips green for its rows)."""
    dhx = np.roll(height, -1, 1) - np.roll(height, 1, 1)
    dhy = np.roll(height, -1, 0) - np.roll(height, 1, 0)
    n = np.stack([-dhx * strength, -dhy * strength, np.ones_like(height)], axis=2)
    n /= np.linalg.norm(n, axis=2, keepdims=True)
    return n * 0.5 + 0.5


v, u = (np.mgrid[0:S, 0:S] + 0.5) / S


# --- Walnut ---------------------------------------------------------------------------------
# Flat-sawn: growth rings run as long streaks along the gun, swept into arches ("cathedrals")
# where the ring surface dips through the board; semi-ring-porous, so the open pores crowd
# the start of each ring and show as fine dark dashes along the grain. Oiled: a soft sheen,
# duller in the pores. A few handling dings.
RINGS = 46                      # across one repeat (25 cm): ~5 mm apart
ring_sweep = Fbm(2, 2, 3)
ring_wobble = Fbm(6, 10, 3)
ring_spacing = Fbm(1, 5, 3)     # rings crowd together and spread apart across the board
streaks = Fbm(1, 18, 3)         # long lighter and darker streaks along the grain
fibre = Fbm(4, 300, 2)
pore = Noise(70, 900)
figure = Fbm(3, 3, 4)
hue = Fbm(2, 4, 3)


def walnut(U, V):
    """Albedo, roughness, height of walnut at (U, V) in repeats (U along the grain)."""
    r = (V * RINGS + (ring_sweep(U, V) - 0.5) * 6.0 + (ring_wobble(U, V) - 0.5) * 0.7
         + (ring_spacing(U, V) - 0.5) * 10.0)
    ph = r - np.floor(r)                                     # 0 = start of a ring (earlywood)
    late = smoothstep(0.42, 0.85, ph) * (1 - smoothstep(0.93, 1.0, ph))
    fib = fibre(U, V)
    early = 1 - smoothstep(0.0, 0.32, ph)
    pores = smoothstep(0.74, 0.88, pore(U, V)) * (0.30 + 0.70 * early)
    fig = figure(U, V)
    tone = (0.62 + 0.40 * (fig - 0.5) - 0.28 * late + 0.14 * (fib - 0.5)
            + 0.40 * (streaks(U, V) - 0.5))
    light = np.array([0.40, 0.25, 0.15])
    dark = np.array([0.105, 0.062, 0.038])
    col = dark + (light - dark) * np.clip(tone, 0, 1)[..., None]
    warm = (hue(U, V) - 0.5)[..., None]
    col = col * (1 + warm * np.array([0.22, 0.04, -0.12]))  # some boards redder, some greyer
    col = col * (1 - 0.50 * pores)[..., None]
    rough = 0.42 + 0.10 * late + 0.06 * (fib - 0.5) + 0.30 * pores
    height = -0.55 * pores - 0.10 * late + 0.06 * fib
    return col, rough, height


def dings(shape, count, scale_px):
    """Small dents: (darkening, depth) fields, tiling."""
    h, w = shape
    dark, depth = np.zeros(shape), np.zeros(shape)
    yy, xx = np.mgrid[0:h, 0:w]
    for _ in range(count):
        cx, cy = rng.random() * w, rng.random() * h
        rx, ry = scale_px * (0.5 + rng.random()), scale_px * (0.3 + rng.random() * 0.5)
        dx = (xx - cx + w / 2) % w - w / 2
        dy = (yy - cy + h / 2) % h - h / 2
        d = (dx / rx) ** 2 + (dy / ry) ** 2
        bump = np.exp(-d * 2.0)
        depth -= bump * (0.4 + 0.6 * rng.random())
        dark += bump * 0.25
    return np.clip(dark, 0, 0.5), depth


col, rough, height = walnut(u, v)
dk, dent = dings((S, S), 14, 6.0)
col = col * (1 - dk)[..., None]
height = height + 0.5 * dent
save("wood_albedo", col)
save("wood_rough", gray(rough + 0.15 * dk))
save("wood_normal", normal_map(height, 3.0))
log("wood: albedo mean %.3f, roughness mean %.3f" % (col.mean(), rough.mean()))


# --- Blued steel ------------------------------------------------------------------------------
# Neutral (the game's tint makes it blued, parkerized, bright or brass). Uneven bluing, fine
# machining marks along the part, handling scratches (brighter and smoother: the finish worn
# through to polished steel) and a little pitting.
mottle = Fbm(3, 3, 4)
machining = Fbm(6, 700, 2)
grain = Noise(400, 400)
U, V = u, v
mt = mottle(U, V)
mach = machining(U, V)
albedo = 0.60 + 0.10 * (mt - 0.5) + 0.05 * (mach - 0.5) + 0.03 * (grain(U, V) - 0.5)
rough = 0.31 + 0.10 * (mt - 0.5) + 0.07 * (mach - 0.5)
height = 0.25 * mach + 0.04 * grain(U, V)

# Scratches: thin lines, mostly along the part, anti-aliased by splatting samples
scr = np.zeros((S, S))
for _ in range(110):
    x0, y0 = rng.random() * S, rng.random() * S
    ang = rng.normal(0.0, 0.35) + (math.pi / 2 if rng.random() < 0.15 else 0.0)
    length = 10 + rng.random() ** 2 * 140
    depth = 0.3 + 0.7 * rng.random()
    n = int(length * 3)
    t = np.linspace(0, length, n)
    curve = rng.normal(0, 0.0006)
    xs = x0 + t * np.cos(ang + curve * t)
    ys = y0 + t * np.sin(ang + curve * t)
    fade = np.sqrt(np.clip(np.sin(np.pi * t / length), 0, 1)) * depth
    for ox, oy, w in ((0, 0, 1.0), (1, 0, 0.35), (-1, 0, 0.35), (0, 1, 0.35), (0, -1, 0.35)):
        np.maximum.at(scr, ((ys.astype(int) + oy) % S, (xs.astype(int) + ox) % S), fade * w)
albedo = albedo + 0.09 * scr
rough = rough - 0.10 * scr
height = height - 0.6 * scr

# Pitting: tiny dark, rough specks
pits = smoothstep(0.80, 0.92, Noise(260, 260)(U, V)) * smoothstep(0.55, 0.8, Fbm(4, 4, 2)(U, V))
albedo = albedo - 0.18 * pits
rough = rough + 0.20 * pits
height = height - 0.8 * pits
save("metal_albedo", gray(albedo))
save("metal_rough", gray(rough))
save("metal_normal", normal_map(height, 1.6))
log("metal: albedo mean %.3f, roughness mean %.3f" % (albedo.mean(), rough.mean()))


# --- The revolver's grip: the same walnut, as one panel, checkered ----------------------------
# Its UVs cover this (y, z) rectangle of the side of the grip (make_revolver.py: GRIP_UV_Y/Z)
GRIP_UV_Y = (-0.060, 0.016)
GRIP_UV_Z = (-0.116, 0.022)
Y = GRIP_UV_Y[0] + u * (GRIP_UV_Y[1] - GRIP_UV_Y[0])
Z = GRIP_UV_Z[0] + v * (GRIP_UV_Z[1] - GRIP_UV_Z[0])
# Grain runs down the grip's rake (as the blank was cut)
rake = math.atan2(1.0, 0.30)
along = (Y * math.sin(rake) - Z * math.cos(rake)) / TILE
across = (Y * math.cos(rake) + Z * math.sin(rake)) / TILE
col, rough, height = walnut(along + 0.37, across + 0.61)
# Checkered panel: a raked rounded rectangle on each side, pyramids cut at 0.9 rad
centre = -0.0205 + (Z + 0.050) * 0.30
d_side = np.abs(Y - centre) - 0.0105
d_end = np.maximum(-0.096 - Z, Z + 0.026)
dist = np.maximum(d_side, d_end)                     # < 0 inside
inside = dist < 0
a = (Y * math.cos(0.9) + Z * math.sin(0.9)) * 1100
b = (Y * math.cos(-0.9 + math.pi) + Z * math.sin(-0.9 + math.pi)) * 1100
tri = lambda t: 1 - 2 * np.abs(t - np.floor(t) - 0.5)
pyr = np.minimum(tri(a), tri(b))                     # 1 at the points, 0 in the grooves
border = np.exp(-(dist / 0.0007) ** 2)               # the cut line round the panel
cavity = np.where(inside, 1 - pyr, 0.0)
col = col * (1 - 0.45 * cavity[..., None] - 0.5 * border[..., None])
rough = rough + 0.25 * cavity + 0.2 * border
gh = np.where(inside, 0.4 + 0.6 * pyr, 1.0) - 0.8 * border
save("grip_albedo", col)
save("grip_rough", gray(rough))
save("grip_normal", normal_map(gh * 0.6 + height * 0.05, 0.9))
