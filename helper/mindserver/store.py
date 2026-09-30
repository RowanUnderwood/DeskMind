"""Image library: data\\images\\<ID>.png (original), <ID>.json (metadata), <ID>.tpi.

The 8-hex-digit ID is the image's identity everywhere (DeskMind file name,
chat references, vision requests).  Renaming on the Tandy only changes the
title, never the ID.
"""

from __future__ import annotations

import json
import os
import secrets
import threading
from datetime import datetime

from PIL import Image

from . import dither as D
from . import tpi
from .config import DATA_DIR

IMG_DIR = os.path.join(DATA_DIR, "images")


def make_title(prompt: str, limit: int = tpi.TITLE_LEN - 1) -> str:
    """A short title from the prompt: first words, cut at a word boundary."""
    words = " ".join(prompt.replace("\n", " ").split())
    if len(words) <= limit:
        return words
    cut = words[:limit + 1].rsplit(" ", 1)[0]
    return (cut if len(cut) > limit // 2 else words[:limit]).rstrip(",.;:")


class Store:
    def __init__(self, folder: str = IMG_DIR):
        self.folder = folder
        os.makedirs(folder, exist_ok=True)
        self.lock = threading.Lock()

    # ------------------------------------------------------------ paths

    def path(self, id_: str, ext: str) -> str:
        return os.path.join(self.folder, f"{id_.upper()}.{ext}")

    def exists(self, id_: str) -> bool:
        return os.path.exists(self.path(id_, "json"))

    def new_id(self) -> str:
        while True:
            id_ = secrets.token_hex(4).upper()
            if not self.exists(id_):
                return id_

    # ------------------------------------------------------------ create / read

    def add(self, image: Image.Image, prompt: str, settings: D.DitherSettings, *,
            title: str | None = None, seed: int = 0, original_prompt: str = "",
            source: str = "comfy") -> dict:
        """Save an original image and build its TPI.  Returns the metadata."""
        with self.lock:
            id_ = self.new_id()
            image.convert("RGB").save(self.path(id_, "png"))
            meta = {
                "id": id_,
                "title": title or make_title(prompt),
                "prompt": prompt,
                "original_prompt": original_prompt or prompt,
                "seed": int(seed),
                "created": datetime.now().isoformat(timespec="seconds"),
                "mode": settings.mode,
                "source": source,
            }
            self._write_meta(meta)
        self.build_tpi(id_, settings)
        return meta

    def import_tpi(self, data: bytes) -> dict:
        """Recreate an entry from a TPI uploaded by the Tandy (when the original PNG is gone).
        The 'original' is then the dithered picture itself, stretched back to 4:3."""
        info = tpi.parse(data)
        idx = D.unpack(info.image, info.width, info.height)
        with self.lock:
            D.preview_4x3(idx, width=1024).save(self.path(info.id, "png"))
            meta = {"id": info.id, "title": info.title, "prompt": info.prompt, "original_prompt": info.prompt,
                    "seed": info.seed, "created": info.created.isoformat(timespec="seconds"),
                    "mode": info.mode, "source": "tandy-upload"}
            self._write_meta(meta)
            with open(self.tpi_path(info.id, info.mode), "wb") as f:
                f.write(data)
        return meta

    def meta(self, id_: str) -> dict:
        with open(self.path(id_, "json"), encoding="utf-8") as f:
            return json.load(f)

    def _write_meta(self, meta: dict) -> None:
        tmp = self.path(meta["id"], "json.tmp")
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(meta, f, indent=2)
        os.replace(tmp, self.path(meta["id"], "json"))

    def original(self, id_: str) -> Image.Image:
        return Image.open(self.path(id_, "png")).convert("RGB")

    def list(self) -> list[dict]:
        out = []
        for name in os.listdir(self.folder):
            if name.endswith(".json"):
                try:
                    with open(os.path.join(self.folder, name), encoding="utf-8") as f:
                        out.append(json.load(f))
                except (OSError, ValueError):
                    pass
        return sorted(out, key=lambda m: m.get("created", ""), reverse=True)

    # ------------------------------------------------------------ Tandy file

    def tpi_path(self, id_: str, mode: str) -> str:
        return self.path(id_, f"{mode}.tpi")

    def build_tpi(self, id_: str, settings: D.DitherSettings) -> bytes:
        """(Re)dither the original with `settings` and write <ID>.<mode>.tpi (one cache per mode)."""
        meta = self.meta(id_)
        src = self.original(id_)
        idx = D.convert(src, settings)
        th = D.thumbnail(src, settings)
        created = datetime.fromisoformat(meta["created"])
        data = tpi.build(id_, settings.mode, idx, th, meta["title"], meta["prompt"],
                         meta.get("seed", 0), created)
        with self.lock:
            with open(self.tpi_path(id_, settings.mode), "wb") as f:
                f.write(data)
        return data

    def tpi_bytes(self, id_: str, settings: D.DitherSettings | None = None) -> bytes:
        """The TPI for `id_` in settings.mode (default: the image's own mode), built if missing."""
        mode = settings.mode if settings else self.meta(id_).get("mode", "640")
        p = self.tpi_path(id_, mode)
        if os.path.exists(p):
            with open(p, "rb") as f:
                return f.read()
        if settings is None:
            settings = D.DitherSettings(mode=mode)
        return self.build_tpi(id_, settings)

    def drop_tpi_cache(self, id_: str) -> None:
        """Forget the Tandy files (after the dither settings changed); rebuilt on next request."""
        for mode in D.MODES:
            try:
                os.remove(self.tpi_path(id_, mode))
            except FileNotFoundError:
                pass

    # ------------------------------------------------------------ edit

    def rename(self, id_: str, title: str) -> dict:
        title = " ".join(title.split())[: tpi.TITLE_LEN - 1]
        with self.lock:
            meta = self.meta(id_)
            meta["title"] = title
            self._write_meta(meta)
            for mode in D.MODES:
                p = self.tpi_path(id_, mode)
                if os.path.exists(p):
                    with open(p, "rb") as f:
                        data = tpi.set_title(f.read(), title)
                    with open(p, "wb") as f:
                        f.write(data)
        return meta

    def delete(self, id_: str) -> None:
        with self.lock:
            for ext in ("png", "json", "640.tpi", "320.tpi"):
                try:
                    os.remove(self.path(id_, ext))
                except FileNotFoundError:
                    pass
