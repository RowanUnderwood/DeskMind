"""Turn a modern image into Tandy 16-colour pixels.

Pipeline: fit to 4:3 -> anamorphic resize to the Tandy mode (640x200 or
320x200) -> colour adjustments -> sharpen -> dither to the fixed RGBI
palette with one of three engines (hitherdither, didder, Pillow).

The result is an index array (0-15, one per Tandy pixel).  `pack()` turns it
into the bytes DeskMind copies to video memory: line order, two pixels per
byte, left pixel in the high nibble.
"""

from __future__ import annotations

import dataclasses
import os
import subprocess
import tempfile
from dataclasses import dataclass, field

import numpy as np
from PIL import Image, ImageEnhance, ImageFilter

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
}
THUMB = {
    "640": (160, 50),
    "320": (80, 50),
}

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


def dither(img: Image.Image, s: DitherSettings) -> np.ndarray:
    """RGB image at the target size -> index array."""
    pal = s.effective_palette()
    if s.engine == "hitherdither":
        idx = _dither_hither(img, s, pal)
    elif s.engine == "didder":
        idx = _dither_didder(img, s, pal)
    elif s.engine == "pillow":
        idx = _dither_pillow(img, s, pal)
    else:
        idx = _nearest_indices(np.asarray(img.convert("RGB")), pal)
    return np.clip(idx, 0, 15).astype(np.uint8)


def convert(img: Image.Image, s: DitherSettings, size: tuple[int, int] | None = None) -> np.ndarray:
    """Full pipeline: any image -> Tandy index array (h x w)."""
    return dither(prepare(img, s, size), s)


def thumbnail(img: Image.Image, s: DitherSettings) -> np.ndarray:
    """A separate small dither from the original (sharper than shrinking the big one)."""
    ts = dataclasses.replace(s, sharpen=max(s.sharpen, 0.8))
    return convert(img, ts, THUMB[s.mode])


# ---------------------------------------------------------------- output

def pack(idx: np.ndarray) -> bytes:
    """Index array -> Tandy bytes (line order, high nibble = left pixel)."""
    h, w = idx.shape
    if w % 2:
        raise ValueError("width must be even")
    pairs = idx.reshape(h, w // 2, 2)
    return ((pairs[:, :, 0] << 4) | pairs[:, :, 1]).astype(np.uint8).tobytes()


def unpack(data: bytes, w: int, h: int) -> np.ndarray:
    b = np.frombuffer(data, dtype=np.uint8)[: (w // 2) * h].reshape(h, w // 2)
    out = np.empty((h, w), dtype=np.uint8)
    out[:, 0::2] = b >> 4
    out[:, 1::2] = b & 15
    return out


def to_rgb(idx: np.ndarray, s: DitherSettings | None = None) -> Image.Image:
    pal = (s or DitherSettings()).effective_palette()
    lut = np.array([[(c >> 16) & 255, (c >> 8) & 255, c & 255] for c in pal], dtype=np.uint8)
    return Image.fromarray(lut[idx], "RGB")


def preview_4x3(idx: np.ndarray, s: DitherSettings | None = None, width: int = 640) -> Image.Image:
    """How it looks on the Tandy's 4:3 screen: pixels stretched to their real shape."""
    img = to_rgb(idx, s)
    return img.resize((width, width * 3 // 4), Image.Resampling.NEAREST)


def screen_shot(idx: np.ndarray, s: DitherSettings | None = None, width: int = 1280) -> Image.Image:
    """Full-size 4:3 picture of the Tandy screen, for docs and sharing.  Columns are
    repeated (nearest), rows go through a box filter: 200 lines don't divide 960 rows
    evenly, so a line that straddles two output rows is blended instead of jittering."""
    img = to_rgb(idx, s)
    return (img.resize((width, idx.shape[0] * 8), Image.Resampling.NEAREST)
               .resize((width, width * 3 // 4), Image.Resampling.BOX))
