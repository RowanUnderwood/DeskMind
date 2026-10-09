"""Music for DeskMind: songs for the Tandy's 3-voice chip, composed by the MIDI-GPT worker.

The worker (Labtext2midi\\worker\\music_worker.py, the MIDI-GPT environment on the RTX 3090,
http://127.0.0.1:8287) composes and compiles; MindServer keeps the songs and their jobs:

    data\\music\\<ID>.t3     the register stream the Tandy plays (T3P1, at most 30,000 bytes)
    data\\music\\<ID>.mid    the same song as MIDI          <ID>.wav   a preview from the T3 stream
    data\\music\\<ID>.json   title, created, request, spec, the worker's report

A request is either a description ("a spooky tune for the dungeon pictures"), which Qwen turns into
a spec with prompts\\music_spec.txt, or a spec as JSON.  Specs are checked here and again by the
worker; Qwen never writes notes or register data.
"""

from __future__ import annotations

import base64
import json
import logging
import os
import queue
import re
import secrets
import threading
import time
from datetime import datetime

import httpx

from .config import DATA_DIR, HELPER_DIR
from .store import make_title

log = logging.getLogger("mindserver.music")

MUSIC_DIR = os.path.join(DATA_DIR, "music")
PROMPT_DIR = os.path.join(HELPER_DIR, "prompts")
STYLES = ("cracktro", "adventure", "dungeon")
CHOICES = {"energy": ("low", "medium", "high"), "drums": ("none", "light", "full"),
           "length": ("short", "medium", "long")}
KEYS = ("C", "C#", "Db", "D", "Eb", "E", "F", "F#", "Gb", "G", "Ab", "A", "Bb", "B")


def check_worker(url: str) -> str:
    try:
        r = httpx.get(url.rstrip("/") + "/health", timeout=2)
        if r.status_code != 200:
            return f"http {r.status_code}"
        j = r.json()
        return "up" if j.get("loaded") else ("error" if j.get("error") else "loading")
    except (httpx.HTTPError, ValueError):
        return "down"


# ====================================================================== store

class MusicStore:
    def __init__(self, folder: str = MUSIC_DIR):
        self.folder = folder
        os.makedirs(folder, exist_ok=True)
        self.lock = threading.Lock()

    def path(self, id_: str, ext: str) -> str:
        return os.path.join(self.folder, f"{id_.upper()}.{ext}")

    def exists(self, id_: str) -> bool:
        return os.path.exists(self.path(id_, "t3"))

    def new_id(self) -> str:
        while True:
            id_ = secrets.token_hex(4).upper()
            if not self.exists(id_):
                return id_

    def add(self, t3: bytes, midi: bytes, wav: bytes, meta: dict) -> str:
        id_ = self.new_id()
        meta = dict(meta, id=id_, created=datetime.now().isoformat(timespec="seconds"))
        with self.lock:
            for ext, data in (("mid", midi), ("wav", wav), ("t3", t3)):      # .t3 last: it marks the song as there
                with open(self.path(id_, ext), "wb") as f:
                    f.write(data)
            self._write_meta(id_, meta)
        return id_

    def _write_meta(self, id_: str, meta: dict) -> None:
        tmp = self.path(id_, "json.tmp")
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(meta, f, indent=1)
        os.replace(tmp, self.path(id_, "json"))

    def meta(self, id_: str) -> dict:
        try:
            with open(self.path(id_, "json"), encoding="utf-8") as f:
                return json.load(f)
        except (OSError, ValueError):
            return {"id": id_.upper(), "title": id_.upper()}

    def t3(self, id_: str) -> bytes:
        with open(self.path(id_, "t3"), "rb") as f:
            return f.read()

    def set_title(self, id_: str, title: str) -> None:
        with self.lock:
            m = self.meta(id_)
            m["title"] = make_title(title) or m.get("title", "")
            self._write_meta(id_, m)

    def delete(self, id_: str) -> None:
        with self.lock:
            for ext in ("t3", "mid", "wav", "json"):
                try:
                    os.remove(self.path(id_, ext))
                except FileNotFoundError:
                    pass

    def list(self) -> list[dict]:
        out = []
        for name in os.listdir(self.folder):
            if name.lower().endswith(".t3"):
                out.append(self.meta(name[:-3]))
        return sorted(out, key=lambda m: m.get("created", ""), reverse=True)


# ====================================================================== specs

def _first_json(text: str) -> dict | None:
    m = re.search(r"\{.*\}", text, re.S)
    if not m:
        return None
    try:
        v = json.loads(m.group(0))
        return v if isinstance(v, dict) else None
    except ValueError:
        return None


def clean_spec(raw: dict, description: str = "") -> dict:
    """Only known fields with allowed values; anything else is dropped (the worker clamps again)."""
    spec = {}
    style = str(raw.get("style") or "").lower()
    spec["style"] = style if style in STYLES else guess_style(description)
    for k, allowed in CHOICES.items():
        v = str(raw.get(k) or "").lower()
        if v in allowed:
            spec[k] = v
    key = str(raw.get("key") or "").strip()
    key = key.split()[0] if key else ""
    if key[:2].capitalize() in KEYS:
        spec["key"] = key[:2].capitalize()
    elif key[:1].upper() in KEYS:
        spec["key"] = key[:1].upper()
    try:
        spec["tempo"] = max(60, min(180, int(raw["tempo"])))
    except (KeyError, TypeError, ValueError):
        pass
    title = make_title(str(raw.get("title") or "")) or make_title(description)
    if title:
        spec["title"] = title[:39]
    spec["seed"] = int(raw["seed"]) & 0x7FFFFFFF if str(raw.get("seed", "")).isdigit() else secrets.randbelow(1 << 30)
    return spec


def guess_style(text: str) -> str:
    t = text.lower()
    if re.search(r"dungeon|dark|spooky|creepy|scary|haunt|cave|crypt|night|sad|slow|myster", t):
        return "dungeon"
    if re.search(r"adventure|quest|happy|cheer|bright|village|journey|hero|march|sunny|travel", t):
        return "adventure"
    return "cracktro"


# ====================================================================== jobs

class MusicJob:
    def __init__(self, id_: int, request: str, spec: dict | None):
        self.id = id_
        self.request = request
        self.spec = spec
        self.status = "queued"            # queued, running, done, error
        self.stage = "waiting"
        self.music_id = ""
        self.title = ""
        self.error = ""
        self.created = time.time()

    def line(self) -> str:
        if self.status == "done":
            return f"M {self.music_id} {self.title}".rstrip()
        if self.status == "error":
            return f"E {self.error}"
        return f"S {self.status} {self.stage}"


class MusicService:
    """One composition at a time on its own thread (the worker has the 3090; pictures have the 4090)."""

    def __init__(self, config, qwen=None, store: MusicStore | None = None):
        self.config = config
        self.qwen = qwen                  # a QwenClient, for descriptions -> specs
        self.store = store or MusicStore()
        self.jobs: dict[int, MusicJob] = {}
        self.next_id = 1
        self.q: queue.Queue[MusicJob] = queue.Queue()
        self.cond = threading.Condition()
        threading.Thread(target=self._loop, name="music", daemon=True).start()

    # ------------------------------------------------------------ public

    def submit(self, request: str, spec: dict | None = None) -> MusicJob:
        with self.cond:
            job = MusicJob(self.next_id, request, spec)
            self.next_id += 1
            self.jobs[job.id] = job
        self.q.put(job)
        return job

    def get(self, id_: int) -> MusicJob | None:
        return self.jobs.get(id_)

    def wait_change(self, job: MusicJob, line: str, timeout: float = 5.0) -> str:
        with self.cond:
            self.cond.wait_for(lambda: job.line() != line, timeout)
            return job.line()

    def spec_from_text(self, description: str) -> dict:
        """Qwen picks style, key, tempo, energy, drums, length and a title; without Qwen a keyword guess."""
        raw = None
        if self.qwen is not None:
            try:
                with open(os.path.join(PROMPT_DIR, "music_spec.txt"), encoding="utf-8") as f:
                    system = f.read().strip()
                msgs = [{"role": "system", "content": system}, {"role": "user", "content": description}]
                out = [t for kind, t in self.qwen.stream(msgs, self.config["music"]["effort_spec"], 1500)
                       if kind == "content"]
                raw = _first_json("".join(out))
            except Exception as e:                   # Qwen down or odd output: the guess below
                log.warning("music spec from Qwen failed: %s", e)
        return clean_spec(raw or {}, description)

    # ------------------------------------------------------------ worker

    def _set(self, job: MusicJob, **kw) -> None:
        with self.cond:
            for k, v in kw.items():
                setattr(job, k, v)
            self.cond.notify_all()

    def _loop(self) -> None:
        while True:
            job = self.q.get()
            try:
                self._run(job)
            except Exception as e:
                log.exception("music job %d", job.id)
                self._set(job, status="error", error=f"{type(e).__name__}: {e}")

    def _run(self, job: MusicJob) -> None:
        url = self.config["music"]["url"].rstrip("/")
        state = check_worker(url)
        if state != "up":
            self._set(job, status="error",
                      error="The music composer is " + ("still loading" if state == "loading" else "not running") +
                            " on the PC. Start it in MindServer's Services tab.")
            return
        spec = job.spec
        if spec is None:
            self._set(job, status="running", stage="planning")
            spec = self.spec_from_text(job.request)
        else:
            spec = clean_spec(spec, job.request)
        self._set(job, status="running", stage="composing")
        log.info("music job %d: %s", job.id, spec)
        r = httpx.post(url + "/compose", json=spec, timeout=600)
        out = r.json()
        if not out.get("ok"):
            self._set(job, status="error", error="Composing failed: " + str(out.get("error", r.status_code)))
            return
        t3, midi, wav = (base64.b64decode(out[k]) for k in ("t3", "midi", "wav"))
        title = make_title(out.get("title") or spec.get("title") or job.request) or "Song"
        mid = self.store.add(t3, midi, wav, {"title": title, "request": job.request, "spec": spec,
                                             "report": out.get("report", {})})
        log.info("music job %d: %s %s (%d bytes)", job.id, mid, title, len(t3))
        self._set(job, status="done", music_id=mid, title=title)
