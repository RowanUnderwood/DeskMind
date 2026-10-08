"""TPI: the DeskMind picture file (one file per image, 8.3 name <ID>.TPI).

Layout (little endian), 128-byte header then the data blocks:

    0   char[4]  magic "TPI1"
    4   u8       mode: 1 = 640x200x16, 2 = 320x200x16 (Tandy)
                       3 = CGA 320x200x4, 4 = CGA 640x200x2
    5   u8       flags (0)
    6   u16      width            8   u16  height
    10  u16      thumb width      12  u16  thumb height
    14  char[8]  id (8 hex digits, upper case)
    22  u16      year   24 u8 month  25 u8 day  26 u8 hour  27 u8 minute
    28  u32      seed (low 32 bits)
    32  char[40] title, CP437, NUL padded
    72  u32 prompt offset   76 u16 prompt length   (CP437 text, no NUL)
    78  u32 thumb offset    82 u16 thumb length
    84  u32 image offset    88 u16 image length
    90  u8       CGA 320x200x4: palette (dither.CGA_SETS index; bit 0 = intensity,
                 4-5 = mode 5).  Otherwise 0.
    91  u8       CGA 320x200x4: background colour 0-15; CGA 640x200x2: foreground
                 colour (15).  Otherwise 0.
    92..127      reserved (0)

Pixel data (thumb and image) is 2 pixels per byte, left pixel in the high
nibble, lines in order top to bottom.  For 640x200 that is exactly the
video memory layout (one copy); for 320x200 DeskMind copies line by line
into the four interleaved banks.

CGA pictures pack 4 pixels per byte (mode 3) or 8 (mode 4), leftmost pixel in
the high bits, lines in order; DeskMind copies them into the two CGA banks.
Their thumbnails are always 1 bit per pixel (black and white), because
DeskMind's CGA screen is 640x200x2.  A CGA picture's mode string is "cga";
`layout` tells the two kinds apart.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from datetime import datetime

import numpy as np

from . import dither as D

MAGIC = b"TPI1"
HEADER = 128
LAYOUT_CODE = {"640": 1, "320": 2, "cga4": 3, "cga2": 4}
CODE_LAYOUT = {v: k for k, v in LAYOUT_CODE.items()}
BITS = {"640": 4, "320": 4, "cga4": 2, "cga2": 1}


def layout_mode(layout: str) -> str:
    """The delivery mode (store cache, ?mode=) of a layout."""
    return "cga" if layout.startswith("cga") else layout


def thumb_bits(layout: str) -> int:
    return 1 if layout.startswith("cga") else 4
TITLE_LEN = 40
PROMPT_MAX = 2000


def cp437(text: str, limit: int | None = None) -> bytes:
    b = text.encode("cp437", "replace")
    return b[:limit] if limit else b


@dataclass
class TpiInfo:
    id: str
    mode: str
    width: int
    height: int
    thumb_w: int
    thumb_h: int
    created: datetime
    seed: int
    title: str
    prompt: str
    thumb: bytes
    image: bytes
    layout: str = ""
    cga_pal: int = 0
    cga_color: int = 0

    def image_idx(self) -> np.ndarray:
        return D.unpack(self.image, self.width, self.height, BITS[self.layout])

    def thumb_idx(self) -> np.ndarray:
        return D.unpack(self.thumb, self.thumb_w, self.thumb_h, thumb_bits(self.layout))

    def rgb(self, s: "D.DitherSettings | None" = None) -> list[int]:
        """The palette the image indices refer to."""
        if self.layout.startswith("cga"):
            return D.cga_rgb(s, self.layout, self.cga_pal, self.cga_color)
        return (s or D.DitherSettings()).effective_palette()

    def thumb_rgb(self, s: "D.DitherSettings | None" = None) -> list[int]:
        return [0x000000, 0xFFFFFF] if self.layout.startswith("cga") else self.rgb(s)


def build(id_: str, layout: str, image_idx: np.ndarray, thumb_idx: np.ndarray,
          title: str, prompt: str, seed: int = 0, created: datetime | None = None,
          cga_pal: int = 0, cga_color: int = 0) -> bytes:
    """`layout` is "640", "320", "cga4" or "cga2" (see the module notes)."""
    created = created or datetime.now()
    img = D.pack(image_idx, BITS[layout])
    th = D.pack(thumb_idx, thumb_bits(layout))
    pr = cp437(" ".join(prompt.split()), PROMPT_MAX)
    h, w = image_idx.shape
    th_h, th_w = thumb_idx.shape

    prompt_off = HEADER
    thumb_off = prompt_off + len(pr)
    image_off = thumb_off + len(th)

    hdr = bytearray(HEADER)
    struct.pack_into("<4sBBHHHH8sHBBBBI", hdr, 0, MAGIC, LAYOUT_CODE[layout], 0, w, h, th_w, th_h,
                     id_.upper().encode("ascii")[:8].ljust(8, b"0"),
                     created.year, created.month, created.day, created.hour, created.minute,
                     seed & 0xFFFFFFFF)
    hdr[32:32 + TITLE_LEN] = cp437(title, TITLE_LEN - 1).ljust(TITLE_LEN, b"\0")
    struct.pack_into("<IHIHIH", hdr, 72, prompt_off, len(pr), thumb_off, len(th), image_off, len(img))
    if layout.startswith("cga"):
        hdr[90], hdr[91] = cga_pal, cga_color
    return bytes(hdr) + pr + th + img


def parse(data: bytes) -> TpiInfo:
    if data[:4] != MAGIC:
        raise ValueError("not a TPI file")
    (_, mode, _, w, h, tw, th, id_, year, month, day, hour, minute, seed) = \
        struct.unpack_from("<4sBBHHHH8sHBBBBI", data, 0)
    title = data[32:32 + TITLE_LEN].split(b"\0")[0].decode("cp437")
    po, pl, to, tl, io, il = struct.unpack_from("<IHIHIH", data, 72)
    layout = CODE_LAYOUT[mode]
    return TpiInfo(id_.decode("ascii"), layout_mode(layout), w, h, tw, th,
                   datetime(year, month, day, hour, minute), seed, title,
                   data[po:po + pl].decode("cp437"), data[to:to + tl], data[io:io + il],
                   layout, data[90], data[91])


def set_title(data: bytes, title: str) -> bytes:
    b = bytearray(data)
    b[32:32 + TITLE_LEN] = cp437(title, TITLE_LEN - 1).ljust(TITLE_LEN, b"\0")
    return bytes(b)
