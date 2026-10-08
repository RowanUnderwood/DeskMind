"""Turn a modern image into Tandy 16-colour pixels.

Pipeline: fit to 4:3 -> anamorphic resize to the Tandy mode (640x200 or
320x200) -> colour adjustments -> sharpen -> dither to the fixed RGBI
palette with one of three engines (hitherdither, didder, Pillow).

The result is an index array (0-15, one per Tandy pixel).  `pack()` turns it
into the bytes DeskMind copies to video memory: line order, two pixels per
byte, left pixel in the high nibble.

Mode "cga" is for a plain CGA PC.  `render()` picks per picture either
320x200 in 4 colours (one of the six CGA palettes plus any background colour,
indices 0-3) or 640x200 black and white (indices 0-1), whichever reproduces
the picture best (`cga_choose`).  CGA thumbnails are always black and white,
because DeskMind's CGA screen is 640x200 in 2 colours.
"""

from __future__ import annotations

import dataclasses
import os
import subprocess
import tempfile
from dataclasses import dataclass, field

import numpy as np
from PIL import Image, ImageEnhance, ImageFilter, ImageOps

# CGA/Tandy RGBI colours.  Colour 6 is brown on most monitors; some Tandy
# monitors show dark yellow instead (see the `brown` setting).
TANDY_RGB = [
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
]
DARK_YELLOW = 0xAAAA00

MODES = {
    "640": (640, 200),
    "320": (320, 200),
    "cga": (320, 200),                 # or 640x200 black and white, chosen per picture
}
THUMB = {
    "640": (160, 50),
    "320": (80, 50),
    "cga": (160, 50),                  # always black and white
}

# CGA mode 4/5 palettes: background (any of the 16 colours, chosen per picture) + three
# fixed colours.  Index = the TPI's palette byte: bit 0 = high intensity.
CGA_SETS = [
    ("green/red/brown", (2, 4, 6)), ("green/red/yellow (bright)", (10, 12, 14)),
    ("cyan/magenta/grey", (3, 5, 7)), ("cyan/magenta/white (bright)", (11, 13, 15)),
    ("cyan/red/grey (mode 5)", (3, 4, 7)), ("cyan/red/white (mode 5, bright)", (11, 12, 15)),
]
CGA_CHOICES = ["auto", "mono"] + [f"{i}: {n}" for i, (n, _) in enumerate(CGA_SETS)]
COLOUR_NAMES = ["black", "blue", "green", "cyan", "red", "magenta", "brown", "light grey",
                "dark grey", "light blue", "light green", "light cyan", "light red",
                "light magenta", "yellow", "white"]

ENGINES = ["hitherdither", "didder", "pillow", "none"]
HITHER_METHODS = ["floyd-steinberg", "atkinson", "jarvis-judice-ninke", "stucki", "burkes",
                  "sierra3", "sierra2", "sierra-2-4a", "bayer", "yliluoma", "cluster-dot"]
DIDDER_METHODS = ["FloydSteinberg", "Atkinson", "JarvisJudiceNinke", "Stucki", "Burkes",
                  "Sierra", "TwoRowSierra", "SierraLite", "StevenPigeon",
                  "bayer 2x2", "bayer 4x4", "bayer 8x8", "bayer 16x16",
                  "odm ClusteredDot4x4", "odm ClusteredDotDiagonal8x8", "odm Vertical5x3",
                  "odm Horizontal3x5"]

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DIDDER_EXE = os.path.join(ROOT, "tools", "didder", "didder.exe")


@dataclass
class DitherSettings:
    mode: str = "640"                  # "640" or "320"
    fit: str = "crop"                  # crop | letterbox | stretch (to 4:3)
    brightness: float = 1.0            # 1.0 = unchanged
    contrast: float = 1.15
    saturation: float = 1.3
    gamma: float = 1.0                 # <1 brighter mid-tones, >1 darker
    sharpen: float = 0.6               # unsharp-mask amount at target size, 0 = off
    engine: str = "hitherdither"
    method: str = "floyd-steinberg"    # engine-specific, see *_METHODS
    order: int = 4                     # matrix size for bayer/yliluoma/cluster-dot
    threshold: int = 64                # hitherdither ordered threshold (0-255 range)
    strength: float = 0.8              # didder --strength (error-diffusion amount)
    serpentine: bool = True            # didder edm --serpentine
    brown: bool = True                 # colour 6 = brown (False = dark yellow)
    palette: list = field(default_factory=lambda: list(TANDY_RGB))
    # CGA ("cga" mode): "auto", "mono" or "<n>: <palette name>" from CGA_CHOICES
    cga_choice: str = "auto"
    cga_bg: int = -1                   # background colour 0-15, -1 = auto
    cga_mode5: bool = True             # allow the mode-5 cyan/red palettes (some clones lack them)
    cga_mono_bias: float = 1.1         # colour pictures: black and white must score this much better
                                       # (0.85 chose it for two colourful photos that looked grey on the Tandy)

    def to_dict(self) -> dict:
        return dataclasses.asdict(self)

    @classmethod
    def from_dict(cls, d: dict) -> "DitherSettings":
        known = {f.name for f in dataclasses.fields(cls)}
        return cls(**{k: v for k, v in d.items() if k in known})

    def effective_palette(self) -> list[int]:
        pal = list(self.palette[:16])
        if not self.brown:
            pal[6] = DARK_YELLOW
        return pal


# ---------------------------------------------------------------- stages

def fit_4x3(img: Image.Image, fit: str) -> Image.Image:
    """Crop, pad or leave the image so it has a 4:3 shape."""
    img = img.convert("RGB")
    w, h = img.size
    target = 4 / 3
    if fit == "stretch" or abs(w / h - target) < 0.005:
        return img
    if fit == "letterbox":
        if w / h > target:
            nh = round(w / target)
            canvas = Image.new("RGB", (w, nh), (0, 0, 0))
            canvas.paste(img, (0, (nh - h) // 2))
        else:
            nw = round(h * target)
            canvas = Image.new("RGB", (nw, h), (0, 0, 0))
            canvas.paste(img, ((nw - w) // 2, 0))
        return canvas
    # crop (centre)
    if w / h > target:
        nw = round(h * target)
        x = (w - nw) // 2
        return img.crop((x, 0, x + nw, h))
    nh = round(w / target)
    y = (h - nh) // 2
    return img.crop((0, y, w, y + nh))


def prepare(img: Image.Image, s: DitherSettings, size: tuple[int, int] | None = None) -> Image.Image:
    """Everything before dithering.  Returns an RGB image at the Tandy size."""
    size = size or MODES[s.mode]
    img = fit_4x3(img, s.fit)
    # Anamorphic: a 4:3 picture squashed into 640x200 (or 320x200)
    img = img.resize(size, Image.Resampling.LANCZOS, reducing_gap=3.0)
    if s.brightness != 1.0:
        img = ImageEnhance.Brightness(img).enhance(s.brightness)
    if s.contrast != 1.0:
        img = ImageEnhance.Contrast(img).enhance(s.contrast)
    if s.saturation != 1.0:
        img = ImageEnhance.Color(img).enhance(s.saturation)
    if s.gamma != 1.0:
        inv = 1.0 / s.gamma
        lut = [min(255, round(255 * ((i / 255) ** inv))) for i in range(256)]
        img = img.point(lut * 3)
    if s.sharpen > 0:
        img = img.filter(ImageFilter.UnsharpMask(radius=1.0, percent=int(s.sharpen * 150), threshold=2))
    return img


def _nearest_indices(rgb: np.ndarray, pal: list[int]) -> np.ndarray:
    """Map every pixel to the closest palette colour (plain RGB distance)."""
    p = np.array([[(c >> 16) & 255, (c >> 8) & 255, c & 255] for c in pal], dtype=np.int32)
    flat = rgb.reshape(-1, 3).astype(np.int32)
    d = ((flat[:, None, :] - p[None, :, :]) ** 2).sum(axis=2)
    return d.argmin(axis=1).astype(np.uint8).reshape(rgb.shape[:2])


def _dither_hither(img: Image.Image, s: DitherSettings, pal: list[int]) -> np.ndarray:
    import hitherdither
    palette = hitherdither.palette.Palette(pal)
    m = s.method
    if m == "bayer":
        out = hitherdither.ordered.bayer.bayer_dithering(img, palette, [s.threshold] * 3, order=s.order)
    elif m == "yliluoma":
        out = hitherdither.ordered.yliluoma.yliluomas_1_ordered_dithering(img, palette, order=s.order)
    elif m == "cluster-dot":
        out = hitherdither.ordered.cluster.cluster_dot_dithering(img, palette, [s.threshold] * 3, order=s.order)
    else:
        out = hitherdither.diffusion.error_diffusion_dithering(img, palette, method=m, order=2)
    # hitherdither returns a "P" image whose indices follow our palette order
    return np.asarray(out, dtype=np.uint8).copy()


def _dither_didder(img: Image.Image, s: DitherSettings, pal: list[int]) -> np.ndarray:
    if not os.path.exists(DIDDER_EXE):
        raise RuntimeError(f"didder not found at {DIDDER_EXE}")
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, "in.png")
        dst = os.path.join(tmp, "out.png")
        img.save(src)
        cmd = [DIDDER_EXE, "-i", src, "-o", dst,
               "--palette", " ".join(f"{c:06X}" for c in pal),
               "--strength", f"{round(s.strength * 100)}%"]
        parts = s.method.split()
        if parts[0] in ("bayer", "odm"):
            cmd += parts
        else:
            cmd += ["edm"] + (["--serpentine"] if s.serpentine else []) + parts
        r = subprocess.run(cmd, capture_output=True, text=True,
                           creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        if r.returncode != 0:
            raise RuntimeError(f"didder failed: {r.stderr.strip() or r.stdout.strip()}")
        out = np.asarray(Image.open(dst).convert("RGB"))
    return _nearest_indices(out, pal)


def _dither_pillow(img: Image.Image, s: DitherSettings, pal: list[int]) -> np.ndarray:
    pimg = Image.new("P", (1, 1))
    flat = []
    for c in pal:
        flat += [(c >> 16) & 255, (c >> 8) & 255, c & 255]
    pimg.putpalette(flat + [0] * (768 - len(flat)))
    out = img.quantize(palette=pimg, dither=Image.Dither.FLOYDSTEINBERG)
    return np.asarray(out, dtype=np.uint8).copy()


def dither(img: Image.Image, s: DitherSettings, pal: list[int] | None = None) -> np.ndarray:
    """RGB image at the target size -> index array (into `pal`, default the 16 colours)."""
    pal = pal or s.effective_palette()
    if s.engine == "hitherdither":
        idx = _dither_hither(img, s, pal)
    elif s.engine == "didder":
        idx = _dither_didder(img, s, pal)
    elif s.engine == "pillow":
        idx = _dither_pillow(img, s, pal)
    else:
        idx = _nearest_indices(np.asarray(img.convert("RGB")), pal)
    return np.clip(idx, 0, len(pal) - 1).astype(np.uint8)


def convert(img: Image.Image, s: DitherSettings, size: tuple[int, int] | None = None) -> np.ndarray:
    """Full pipeline: any image -> Tandy index array (h x w).  Tandy modes only; see render()."""
    return dither(prepare(img, s, size), s)


def _mono(img: Image.Image, s: DitherSettings) -> np.ndarray:
    """Black and white dither of an RGB image (by brightness, so hue doesn't matter)."""
    grey = ImageOps.autocontrast(img.convert("L"), cutoff=1)
    return dither(grey.convert("RGB"), s, [0x000000, 0xFFFFFF])


def thumbnail(img: Image.Image, s: DitherSettings) -> np.ndarray:
    """A separate small dither from the original (sharper than shrinking the big one)."""
    ts = dataclasses.replace(s, sharpen=max(s.sharpen, 0.8))
    if s.mode == "cga":
        return _mono(prepare(img, ts, THUMB["cga"]), ts)
    return convert(img, ts, THUMB[s.mode])


# ---------------------------------------------------------------- CGA

@dataclass
class Dithered:
    """A dithered picture: indices into `rgb`, and how DeskMind shows it."""
    idx: np.ndarray
    layout: str                        # "640" | "320" | "cga4" | "cga2"
    rgb: list                          # palette (RGB ints) the indices refer to
    cga_pal: int = 0                   # cga4: CGA_SETS index
    cga_color: int = 0                 # cga4: background colour; cga2: foreground colour

    def describe(self) -> str:
        if self.layout == "cga4":
            return f"CGA 320x200 {CGA_SETS[self.cga_pal][0]}, background {COLOUR_NAMES[self.cga_color]}"
        if self.layout == "cga2":
            return "CGA 640x200 black and white"
        return f"Tandy {self.layout}x200, 16 colours"


def cga4_rgb(s: DitherSettings, pal: int, bg: int) -> list[int]:
    full = s.effective_palette()
    return [full[bg]] + [full[c] for c in CGA_SETS[pal][1]]


def cga_rgb(s: DitherSettings | None, layout: str, pal: int, colour: int) -> list[int]:
    """The RGB palette of a CGA picture, from its TPI palette/colour bytes."""
    s = s or DitherSettings()
    if layout == "cga2":
        return [0x000000, s.effective_palette()[colour]]
    return cga4_rgb(s, pal, colour)


def _linear(rgb: np.ndarray) -> np.ndarray:
    c = rgb.astype(np.float64) / 255
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def _lab(lin: np.ndarray) -> np.ndarray:
    """Linear sRGB (..., 3) -> CIE Lab (D65)."""
    m = np.array([[0.4124, 0.3576, 0.1805], [0.2126, 0.7152, 0.0722], [0.0193, 0.1192, 0.9505]])
    xyz = lin @ m.T / np.array([0.9505, 1.0, 1.089])
    f = np.where(xyz > 0.008856, np.cbrt(xyz), 7.787 * xyz + 16 / 116)
    return np.stack([116 * f[..., 1] - 16, 500 * (f[..., 0] - f[..., 1]), 200 * (f[..., 1] - f[..., 2])], -1)


def _mix_weights(n: int, steps: int) -> np.ndarray:
    """Every way to mix n colours in 1/steps parts (the colours a dither can fake)."""
    out = []

    def rec(prefix, left, k):
        if k == 1:
            out.append(prefix + [left])
            return
        for i in range(left + 1):
            rec(prefix + [i], left - i, k - 1)
    rec([], steps, n)
    return np.array(out, dtype=np.float64) / steps


_W4 = _mix_weights(4, 4)               # 35 mixes for 320x200 in 4 colours
_W2 = _mix_weights(2, 8)               # 9 mixes: 640x200 has twice the pixels per area
NOISE = 0.35                           # penalty for mixing far-apart colours (visible grain)
# Lightness counts double: without it a dark grey background often won over black (flat, no contrast)
LAB_WEIGHT = np.array([2.0, 1.0, 1.0])
GREY_CHROMA = 3.0                      # mean Lab chroma below this = a greyscale picture
GREY_MONO_BIAS = 0.85                  # ...which gets black and white's double detail by preference


def _palette_cost(sample_lab: np.ndarray, pal: list[int], w: np.ndarray) -> float:
    """Mean Lab error of the best dither mix per sample pixel, plus a grain penalty."""
    p = np.array([[(c >> 16) & 255, (c >> 8) & 255, c & 255] for c in pal])
    plin = _linear(p)
    mix_lab = _lab(w @ plin) * LAB_WEIGHT                      # (mixes, 3); light mixes linearly
    p_lab = _lab(plin) * LAB_WEIGHT                            # (colours, 3)
    spread = (w * ((p_lab[None, :, :] - mix_lab[:, None, :]) ** 2).sum(2)).sum(1)
    d = np.sqrt(((sample_lab[:, None, :] - mix_lab[None, :, :]) ** 2).sum(2))
    return float((d + NOISE * np.sqrt(spread)[None, :]).min(1).mean())


def cga_choose(img: Image.Image, s: DitherSettings) -> tuple[str, int, int, float]:
    """Pick ("cga4", palette, background) or ("cga2", 0, 15) for an image, honouring
    s.cga_choice and s.cga_bg.  Returns (layout, palette, colour, cost)."""
    if s.cga_choice == "mono":
        return "cga2", 0, 15, 0.0
    small = prepare(img, dataclasses.replace(s, sharpen=0), (80, 50))
    plain = _lab(_linear(np.asarray(small).reshape(-1, 3)))
    grey = float(np.hypot(plain[:, 1], plain[:, 2]).mean()) < GREY_CHROMA
    lab = plain * LAB_WEIGHT
    if s.cga_choice[:1].isdigit():
        sets = [int(s.cga_choice.split(":")[0])]
    else:
        sets = [i for i in range(len(CGA_SETS)) if s.cga_mode5 or i < 4]
    bgs = [s.cga_bg] if 0 <= s.cga_bg <= 15 else range(16)
    best = ("cga4", sets[0], 0, float("inf"))
    for i in sets:
        for bg in bgs:
            cost = _palette_cost(lab, cga4_rgb(s, i, bg), _W4)
            if cost < best[3]:
                best = ("cga4", i, bg, cost)
    if not s.cga_choice[:1].isdigit():
        # Black and white is scored against the colour image (it loses all colour), then biased
        mono = _palette_cost(lab, [0x000000, 0xFFFFFF], _W2) * (GREY_MONO_BIAS if grey else s.cga_mono_bias)
        if mono < best[3]:
            best = ("cga2", 0, 15, mono)
    return best


def render(img: Image.Image, s: DitherSettings) -> Dithered:
    """Full pipeline for any mode: any image -> Dithered."""
    if s.mode != "cga":
        return Dithered(convert(img, s), s.mode, s.effective_palette())
    layout, pal, colour, _ = cga_choose(img, s)
    rgb = cga_rgb(s, layout, pal, colour)
    if layout == "cga2":
        return Dithered(_mono(prepare(img, s, (640, 200)), s), layout, rgb, 0, colour)
    return Dithered(dither(prepare(img, s, (320, 200)), s, rgb), layout, rgb, pal, colour)


# ---------------------------------------------------------------- output

def pack(idx: np.ndarray, bits: int = 4) -> bytes:
    """Index array -> video bytes, line order, leftmost pixel in the high bits.
    bits = 4 (Tandy), 2 (CGA 4 colours) or 1 (CGA black and white)."""
    h, w = idx.shape
    per = 8 // bits
    if w % per:
        raise ValueError(f"width must be a multiple of {per}")
    groups = idx.reshape(h, w // per, per).astype(np.uint8)
    out = np.zeros((h, w // per), dtype=np.uint8)
    for k in range(per):
        out |= (groups[:, :, k] & ((1 << bits) - 1)) << (8 - bits * (k + 1))
    return out.tobytes()


def unpack(data: bytes, w: int, h: int, bits: int = 4) -> np.ndarray:
    per = 8 // bits
    b = np.frombuffer(data, dtype=np.uint8)[: (w // per) * h].reshape(h, w // per)
    out = np.empty((h, w), dtype=np.uint8)
    for k in range(per):
        out[:, k::per] = (b >> (8 - bits * (k + 1))) & ((1 << bits) - 1)
    return out


def to_rgb(idx: np.ndarray, s: DitherSettings | None = None, pal: list[int] | None = None) -> Image.Image:
    pal = pal or (s or DitherSettings()).effective_palette()
    lut = np.array([[(c >> 16) & 255, (c >> 8) & 255, c & 255] for c in pal], dtype=np.uint8)
    return Image.fromarray(lut[idx], "RGB")


def preview_4x3(idx: np.ndarray, s: DitherSettings | None = None, width: int = 640,
                pal: list[int] | None = None) -> Image.Image:
    """How it looks on the Tandy's 4:3 screen: pixels stretched to their real shape."""
    img = to_rgb(idx, s, pal)
    return img.resize((width, width * 3 // 4), Image.Resampling.NEAREST)


def screen_shot(idx: np.ndarray, s: DitherSettings | None = None, width: int = 1280,
                pal: list[int] | None = None) -> Image.Image:
    """Full-size 4:3 picture of the Tandy screen, for docs and sharing.  Columns are
    repeated (nearest), rows go through a box filter: 200 lines don't divide 960 rows
    evenly, so a line that straddles two output rows is blended instead of jittering."""
    img = to_rgb(idx, s, pal)
    return (img.resize((width, idx.shape[0] * 8), Image.Resampling.NEAREST)
               .resize((width, width * 3 // 4), Image.Resampling.BOX))
