"""Phase 0: compare dither engines on sample images.

    .venv\\Scripts\\python dithertest.py [image ...]

Writes helper\\out\\dithertest_<name>.png: a contact sheet of 4:3 previews
(how each result looks on the Tandy), with the time each engine took.
"""

import os
import sys
import time

from PIL import Image, ImageDraw

from mindserver import dither as D

HERE = os.path.dirname(os.path.abspath(__file__))

CANDIDATES = [
    ("pillow", "floyd-steinberg", {}),
    ("hitherdither", "floyd-steinberg", {}),
    ("hitherdither", "atkinson", {}),
    ("hitherdither", "sierra-2-4a", {}),
    ("hitherdither", "bayer", {"order": 4, "threshold": 64}),
    ("hitherdither", "yliluoma", {"order": 4}),
    ("didder", "FloydSteinberg", {"strength": 0.8}),
    ("didder", "Atkinson", {"strength": 1.0}),
    ("didder", "bayer 4x4", {"strength": 0.64}),
]


def check_roundtrip(idx):
    data = D.pack(idx)
    back = D.unpack(data, idx.shape[1], idx.shape[0])
    assert (back == idx).all(), "pack/unpack mismatch"
    return len(data)


def sheet(path: str) -> str:
    src = Image.open(path)
    name = os.path.splitext(os.path.basename(path))[0]
    tiles = []
    for engine, method, extra in CANDIDATES:
        s = D.DitherSettings(engine=engine, method=method, **extra)
        t0 = time.perf_counter()
        idx = D.convert(src, s)
        dt = time.perf_counter() - t0
        n = check_roundtrip(idx)
        used = len(set(idx.flatten().tolist()))
        label = f"{engine} / {method}  {dt:.2f}s  {used} colours  {n} bytes"
        print(label)
        tiles.append((D.preview_4x3(idx, s, 640), label))

    # Reference: the original fitted to 4:3
    ref = D.fit_4x3(src, "crop").resize((640, 480), Image.Resampling.LANCZOS)
    tiles.insert(0, (ref, "original (4:3)"))

    cols = 3
    rows = (len(tiles) + cols - 1) // cols
    W, H, LBL = 640, 480, 18
    out = Image.new("RGB", (cols * W, rows * (H + LBL)), (40, 40, 40))
    d = ImageDraw.Draw(out)
    for i, (img, label) in enumerate(tiles):
        x, y = (i % cols) * W, (i // cols) * (H + LBL)
        out.paste(img, (x, y + LBL))
        d.text((x + 4, y + 3), label, fill=(255, 255, 255))
    os.makedirs(os.path.join(HERE, "out"), exist_ok=True)
    dst = os.path.join(HERE, "out", f"dithertest_{name}.png")
    out.save(dst)
    print("->", dst)
    return dst


if __name__ == "__main__":
    paths = sys.argv[1:] or [os.path.join(HERE, "samples", f) for f in sorted(os.listdir(os.path.join(HERE, "samples")))]
    for p in paths:
        sheet(p)
