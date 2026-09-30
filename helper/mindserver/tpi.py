"""TPI: the DeskMind picture file (one file per image, 8.3 name <ID>.TPI).

Layout (little endian), 128-byte header then the data blocks:

    0   char[4]  magic "TPI1"
    4   u8       mode: 1 = 640x200x16, 2 = 320x200x16
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
    90..127      reserved (0)

Pixel data (thumb and image) is 2 pixels per byte, left pixel in the high
nibble, lines in order top to bottom.  For 640x200 that is exactly the
video memory layout (one copy); for 320x200 DeskMind copies line by line
into the four interleaved banks.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from datetime import datetime

import numpy as np

from . import dither as D

MAGIC = b"TPI1"
HEADER = 128
MODE_CODE = {"640": 1, "320": 2}
CODE_MODE = {v: k for k, v in MODE_CODE.items()}
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


def build(id_: str, mode: str, image_idx: np.ndarray, thumb_idx: np.ndarray,
          title: str, prompt: str, seed: int = 0, created: datetime | None = None) -> bytes:
    created = created or datetime.now()
    img = D.pack(image_idx)
    th = D.pack(thumb_idx)
    pr = cp437(" ".join(prompt.split()), PROMPT_MAX)
    h, w = image_idx.shape
    th_h, th_w = thumb_idx.shape

    prompt_off = HEADER
    thumb_off = prompt_off + len(pr)
    image_off = thumb_off + len(th)

    hdr = bytearray(HEADER)
    struct.pack_into("<4sBBHHHH8sHBBBBI", hdr, 0, MAGIC, MODE_CODE[mode], 0, w, h, th_w, th_h,
                     id_.upper().encode("ascii")[:8].ljust(8, b"0"),
                     created.year, created.month, created.day, created.hour, created.minute,
                     seed & 0xFFFFFFFF)
    hdr[32:32 + TITLE_LEN] = cp437(title, TITLE_LEN - 1).ljust(TITLE_LEN, b"\0")
    struct.pack_into("<IHIHIH", hdr, 72, prompt_off, len(pr), thumb_off, len(th), image_off, len(img))
    return bytes(hdr) + pr + th + img


def parse(data: bytes) -> TpiInfo:
    if data[:4] != MAGIC:
        raise ValueError("not a TPI file")
    (_, mode, _, w, h, tw, th, id_, year, month, day, hour, minute, seed) = \
        struct.unpack_from("<4sBBHHHH8sHBBBBI", data, 0)
    title = data[32:32 + TITLE_LEN].split(b"\0")[0].decode("cp437")
    po, pl, to, tl, io, il = struct.unpack_from("<IHIHIH", data, 72)
    return TpiInfo(id_.decode("ascii"), CODE_MODE[mode], w, h, tw, th,
                   datetime(year, month, day, hour, minute), seed, title,
                   data[po:po + pl].decode("cp437"), data[to:to + tl], data[io:io + il])


def set_title(data: bytes, title: str) -> bytes:
    b = bytearray(data)
    b[32:32 + TITLE_LEN] = cp437(title, TITLE_LEN - 1).ljust(TITLE_LEN, b"\0")
    return bytes(b)
