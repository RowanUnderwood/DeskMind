"""Music for DeskMind: songs for the Tandy's 3-voice chip, composed by the MIDI-GPT worker.

The worker (Labtext2midi\\worker\\music_worker.py, the MIDI-GPT environment on the RTX 3090,
http://127.0.0.1:8287) composes, revises and compiles; MindServer keeps the songs and their jobs:

    data\\music\\<ID>.t3       the register stream the Tandy plays (T3P1; up to 30,000 bytes it plays from
                              memory, an 'extended' or 'epic' song streams from disk)
    data\\music\\<ID>.mid      the same song as MIDI          <ID>.wav   a preview from the T3 stream
    data\\music\\<ID>.arr.json the worker's arrangement (every section's parts): what revisions start from
    data\\music\\<ID>.json     title, created, request, spec, the worker's report, and the revision fields:
        project  the first song's ID (all revisions of one song share it)
        rev      1, 2, ...          parent  the revision it was made from
        kind     'song' or 'sketch' (8 bars; 'pair' = the other sketch of the two)
        change   what this revision changed (one line)
        head     false for a revision that was replaced or undone (the list shows heads only)

Each revision is an immutable song with its own ID, so the Tandy downloads it like any other.  A job's
result lines say which songs are new and which they replace:
    M <id> <title>     a new song (or a revision, or the parent after an undo)
    R <id>             replaced: the Tandy deletes its copy

A request is either a description ("a spooky tune for the dungeon pictures"), which Qwen turns into
a spec with prompts\\music_spec.txt, or a spec as JSON.  Specs are checked here and again by the
worker; Qwen never writes notes or register data.  The styles are the worker's (GET /health): new
genres ship in the worker's recipes.py, after Labtext2midi\\worker\\genre_lab.py and a listening test.
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
STYLES = ("cracktro", "adventure", "dungeon")          # until the worker says otherwise
DESCRIPTIONS = {"cracktro": "fast demo-scene music: minor key, quick arpeggios, driving beat",
                "adventure": "a bright, memorable game melody at a moderate pace, major key",
                "dungeon": "slow, dark and mysterious, minor key, sparse"}
CHOICES = {"energy": ("low", "medium", "high"), "drums": ("none", "light", "full"),
           "length": ("short", "medium", "long", "extended", "epic")}
KEYS = ("C", "C#", "Db", "D", "Eb", "E", "F", "F#", "Gb", "G", "Ab", "A", "Bb", "B")
PARTS = ("lead", "accompaniment", "bass")


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
        self.lock = threading.RLock()

    def path(self, id_: str, ext: str) -> str:
        return os.path.join(self.folder, f"{id_.upper()}.{ext}")

    def exists(self, id_: str) -> bool:
        return os.path.exists(self.path(id_, "t3"))

    def new_id(self) -> str:
        while True:
            id_ = secrets.token_hex(4).upper()
            if not self.exists(id_):
                return id_

    def add(self, t3: bytes, midi: bytes, wav: bytes, meta: dict, arrangement: dict | None = None) -> str:
        id_ = self.new_id()
        meta = dict(meta, id=id_, created=datetime.now().isoformat(timespec="seconds"))
        meta.setdefault("project", id_)
        meta.setdefault("rev", 1)
        meta.setdefault("kind", "song")
        meta.setdefault("head", True)
        with self.lock:
            if arrangement is not None:
                with open(self.path(id_, "arr.json"), "w", encoding="utf-8") as f:
                    json.dump(arrangement, f)
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

    def update(self, id_: str, **fields) -> None:
        with self.lock:
            m = self.meta(id_)
            m.update(fields)
            self._write_meta(id_, m)

    def arrangement(self, id_: str) -> dict | None:
        try:
            with open(self.path(id_, "arr.json"), encoding="utf-8") as f:
                return json.load(f)
        except (OSError, ValueError):
            return None

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
            for ext in ("t3", "mid", "wav", "json", "arr.json", "spk.wav"):
                try:
                    os.remove(self.path(id_, ext))
                except FileNotFoundError:
                    pass

    def list(self, heads_only: bool = True) -> list[dict]:
        out = []
        for name in os.listdir(self.folder):
            if name.lower().endswith(".t3"):
                m = self.meta(name[:-3])
                if m.get("head", True) or not heads_only:
                    out.append(m)
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


def clean_spec(raw: dict, description: str = "", styles=STYLES) -> dict:
    """Only known fields with allowed values; anything else is dropped (the worker clamps again)."""
    spec = {}
    style = str(raw.get("style") or "").lower()
    spec["style"] = style if style in styles else guess_style(description, styles)
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
        spec["tempo"] = max(50, min(200, int(raw["tempo"])))
    except (KeyError, TypeError, ValueError):
        pass
    title = make_title(str(raw.get("title") or "")) or make_title(description)
    if title:
        spec["title"] = title[:39]
    spec["seed"] = int(raw["seed"]) & 0x7FFFFFFF if str(raw.get("seed", "")).isdigit() else secrets.randbelow(1 << 30)
    if raw.get("sketch"):
        spec["sketch"] = True
    return spec


def guess_style(text: str, styles=STYLES) -> str:
    t = text.lower()
    for s in styles:                                  # a genre named outright
        if re.search(rf"\b{re.escape(s)}\b", t):
            return s
    if re.search(r"dungeon|dark|spooky|creepy|scary|haunt|cave|crypt|night|sad|slow|myster", t):
        return "dungeon"
    if re.search(r"adventure|quest|happy|cheer|bright|village|journey|hero|march|sunny|travel", t):
        return "adventure"
    return "cracktro"


# ====================================================================== jobs

class MusicJob:
    def __init__(self, id_: int, request: str, spec: dict | None, kind: str = "compose", base: str = "",
                 edits: dict | None = None, hints: dict | None = None):
        self.id = id_
        self.request = request
        self.spec = spec
        self.kind = kind                  # compose, sketch, revise, build, undo
        self.base = base                  # the song a revise/build/undo starts from
        self.edits = edits or {}
        self.hints = hints or {}          # style / length chosen by the chat tool call, over the spec's
        self.status = "queued"            # queued, running, done, error
        self.stage = "waiting"
        self.music_id = ""                # the first new song (old callers)
        self.title = ""
        self.results: list[tuple[str, str]] = []      # new songs: (id, title)
        self.replaced: list[str] = []                 # songs they replace
        self.change = ""
        self.error = ""
        self.created = time.time()

    def line(self) -> str:
        if self.status == "done":
            return f"M {self.music_id} {self.title}".rstrip()
        if self.status == "error":
            return f"E {self.error}"
        return f"S {self.status} {self.stage}"

    def done_lines(self) -> list[str]:
        """Every result: 'M <id> <title>' per new song, then 'R <id>' per replaced one."""
        return [f"M {i} {t}".rstrip() for i, t in self.results] + [f"R {r}" for r in self.replaced]


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
        self._styles = (STYLES, dict(DESCRIPTIONS), 0.0)
        threading.Thread(target=self._loop, name="music", daemon=True).start()

    # ------------------------------------------------------------ public

    def _queue(self, job: MusicJob) -> MusicJob:
        with self.cond:
            job.id = self.next_id
            self.next_id += 1
            self.jobs[job.id] = job
        self.q.put(job)
        return job

    def submit(self, request: str, spec: dict | None = None, hints: dict | None = None) -> MusicJob:
        """A new song from a description or a spec (hints: style / length that win over Qwen's choice)."""
        return self._queue(MusicJob(0, request, spec, hints=hints))

    def submit_sketches(self, request: str, spec: dict | None = None, hints: dict | None = None) -> MusicJob:
        """Two 8-bar sketches of the same request (different tunes) to choose from."""
        return self._queue(MusicJob(0, request, spec, "sketch", hints=hints))

    def submit_revise(self, song: str, edits: dict, request: str = "") -> MusicJob:
        """A new revision of a song: edits as compose.revise takes them (regenerate, drums, drum_pattern,
        tempo, transpose, length)."""
        return self._queue(MusicJob(0, request, None, "revise", song.upper(), edits))

    def submit_build(self, sketch: str, length: str = "") -> MusicJob:
        """Keep this sketch: it becomes a full song (theme kept note for note), the other sketch goes."""
        return self._queue(MusicJob(0, "", None, "build", sketch.upper(), {"length": length or "medium"}))

    def submit_undo(self, song: str) -> MusicJob:
        """Back to the revision this one was made from."""
        return self._queue(MusicJob(0, "", None, "undo", song.upper()))

    def edits_from_args(self, song: str, args: dict) -> dict:
        """Simple choices (what Qwen's revise_music and DeskMind's Revise dialog send) as compose.revise's
        edits: regenerate [lead, accompaniment, bass], drums none/light/full, drum_pattern next,
        tempo faster/slower (or a number), key up/down (a whole step), length."""
        edits = {}
        regen = [p for p in args.get("regenerate") or [] if p in PARTS]
        if regen:
            edits["regenerate"] = regen
        if args.get("drums") in CHOICES["drums"]:
            edits["drums"] = args["drums"]
        if args.get("drum_pattern"):
            edits["drum_pattern"] = "next"
        tempo = args.get("tempo")
        if tempo in ("faster", "slower"):
            m = self.store.meta(song)
            t = (m.get("report") or {}).get("tempo") or (m.get("spec") or {}).get("tempo") or 120
            edits["tempo"] = round(t * 1.12) if tempo == "faster" else round(t / 1.12)
        elif str(tempo or "").isdigit():
            edits["tempo"] = int(tempo)
        if args.get("key") in ("up", "down"):
            edits["transpose"] = 2 if args["key"] == "up" else -2
        elif str(args.get("transpose", "")).lstrip("-").isdigit():
            edits["transpose"] = int(args["transpose"])
        if args.get("length") in CHOICES["length"]:
            edits["length"] = args["length"]
        return edits

    def like(self, song: str) -> tuple[str, dict]:
        """(request, spec) for new sketches like this song: its style, key, energy, drums and tempo."""
        m = self.store.meta(song)
        spec = {k: v for k, v in (m.get("spec") or {}).items() if k in ("style", "key", "energy", "drums", "tempo")}
        title = re.sub(r"^Sketch [AB]: ", "", m.get("title", ""))
        if title:
            spec["title"] = title
        return m.get("request", "") or title, spec

    def get(self, id_: int) -> MusicJob | None:
        return self.jobs.get(id_)

    def wait_change(self, job: MusicJob, line: str, timeout: float = 5.0) -> str:
        with self.cond:
            self.cond.wait_for(lambda: job.line() != line, timeout)
            return job.line()

    def styles(self) -> tuple[tuple, dict]:
        """(style names, descriptions) the worker offers, asked again after a minute."""
        names, desc, when = self._styles
        if time.time() - when > 60:
            try:
                h = httpx.get(self.config["music"]["url"].rstrip("/") + "/health", timeout=2).json()
                if h.get("styles"):
                    names = tuple(h["styles"])
                    desc = dict(DESCRIPTIONS, **(h.get("descriptions") or {}))
            except (httpx.HTTPError, ValueError):
                pass
            self._styles = (names, desc, time.time())
        return names, desc

    def info(self, id_: str) -> dict:
        m = self.store.meta(id_)
        return {"project": m.get("project", id_), "rev": m.get("rev", 1), "parent": m.get("parent", ""),
                "kind": m.get("kind", "song"), "pair": m.get("pair", ""), "change": m.get("change", ""),
                "style": (m.get("spec") or {}).get("style", ""), "head": m.get("head", True),
                "revisable": "full" if self.store.arrangement(id_) else "cheap"}

    def spec_from_text(self, description: str) -> dict:
        """Qwen picks style, key, tempo, energy, drums, length and a title; without Qwen a keyword guess."""
        names, desc = self.styles()
        raw = None
        if self.qwen is not None:
            try:
                with open(os.path.join(PROMPT_DIR, "music_spec.txt"), encoding="utf-8") as f:
                    system = f.read().strip()
                system = system.replace("{styles}", "\n".join(f'  "{n}": {desc.get(n, "")}' for n in names))
                msgs = [{"role": "system", "content": system}, {"role": "user", "content": description}]
                out = [t for kind, t in self.qwen.stream(msgs, self.config["music"]["effort_spec"], 1500)
                       if kind == "content"]
                raw = _first_json("".join(out))
            except Exception as e:                   # Qwen down or odd output: the guess below
                log.warning("music spec from Qwen failed: %s", e)
        return clean_spec(raw or {}, description, names)

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

    def _url(self) -> str | None:
        url = self.config["music"]["url"].rstrip("/")
        return url if check_worker(url) == "up" else None

    def _down(self, job: MusicJob) -> None:
        state = check_worker(self.config["music"]["url"])
        self._set(job, status="error",
                  error="The music composer is " + ("still loading" if state == "loading" else "not running") +
                        " on the PC. Start it in MindServer's Services tab.")

    def _done(self, job: MusicJob, results, replaced=(), change="") -> None:
        self._set(job, status="done", music_id=results[0][0] if results else "", title=results[0][1] if results else "",
                  results=list(results), replaced=list(replaced), change=change)

    def _store(self, out: dict, meta: dict) -> str:
        t3, midi, wav = (base64.b64decode(out[k]) for k in ("t3", "midi", "wav"))
        return self.store.add(t3, midi, wav, dict(meta, report=out.get("report", {})), out.get("arrangement"))

    def _run(self, job: MusicJob) -> None:
        if job.kind == "undo":                        # no model needed
            return self._undo(job)
        url = self._url()
        if not url:
            return self._down(job)
        if job.kind in ("revise", "build"):
            return self._revise(job, url)
        spec = job.spec
        if spec is None:
            self._set(job, status="running", stage="planning")
            spec = self.spec_from_text(job.request)
        else:
            spec = clean_spec(spec, job.request, self.styles()[0])
        if job.hints.get("style") in self.styles()[0]:
            spec["style"] = job.hints["style"]
        if job.hints.get("length") in CHOICES["length"]:
            spec["length"] = job.hints["length"]
        self._set(job, status="running", stage="composing")
        if job.kind == "sketch":
            ids = []
            for k, letter in enumerate("AB"):
                s = dict(spec, sketch=True, seed=(spec["seed"] + k * 104729) & 0x7FFFFFFF)
                if k and not s.get("key"):           # the same key and tempo as A: only the tune differs
                    s["key"] = first.get("key")
                    s.setdefault("tempo", first.get("tempo"))
                log.info("music job %d: sketch %s %s", job.id, letter, s)
                out = httpx.post(url + "/compose", json=s, timeout=600).json()
                if not out.get("ok"):
                    return self._set(job, status="error", error="Composing failed: " + str(out.get("error")))
                if not k:
                    first = out.get("report") or {}
                title = f"Sketch {letter}: " + (make_title(out.get("title") or spec.get("title") or job.request) or "Song")
                ids.append((self._store(out, {"title": title[:39], "request": job.request, "spec": s, "kind": "sketch"}),
                            title[:39]))
            self.store.update(ids[0][0], pair=ids[1][0])
            self.store.update(ids[1][0], pair=ids[0][0])
            log.info("music job %d: sketches %s", job.id, ids)
            return self._done(job, ids)
        log.info("music job %d: %s", job.id, spec)
        r = httpx.post(url + "/compose", json=spec, timeout=600)
        out = r.json()
        if not out.get("ok"):
            return self._set(job, status="error", error="Composing failed: " + str(out.get("error", r.status_code)))
        title = make_title(out.get("title") or spec.get("title") or job.request) or "Song"
        mid = self._store(out, {"title": title, "request": job.request, "spec": spec})
        log.info("music job %d: %s %s (%d bytes)", job.id, mid, title, len(self.store.t3(mid)))
        self._done(job, [(mid, title)])

    def _revise(self, job: MusicJob, url: str) -> None:
        base = job.base
        if not self.store.exists(base):
            return self._set(job, status="error", error=f"No song {base}")
        m = self.store.meta(base)
        req = {"edits": dict(job.edits), "seed": secrets.randbelow(1 << 30)}
        arr = self.store.arrangement(base)
        if arr:
            req["arrangement"] = arr
        else:                                         # a song from before revisions: drums, tempo and key only
            with open(self.store.path(base, "mid"), "rb") as f:
                req["midi"] = base64.b64encode(f.read()).decode()
            req["report"] = dict(m.get("report") or {}, **{k: v for k, v in (m.get("spec") or {}).items()
                                                            if k not in (m.get("report") or {})})
        replaced = [base]
        if job.kind == "build":
            if m.get("kind") != "sketch":
                return self._set(job, status="error", error="Only a sketch can be built into a full song")
            req["edits"]["build"] = True
            if m.get("pair") and self.store.exists(m["pair"]):
                replaced.append(m["pair"])
        self._set(job, status="running", stage="composing")
        log.info("music job %d: %s %s %s", job.id, job.kind, base, req["edits"])
        out = httpx.post(url + "/revise", json=req, timeout=600).json()
        if not out.get("ok"):
            return self._set(job, status="error", error="Revising failed: " + str(out.get("error")))
        change = out.get("report", {}).get("change", "")
        title = re.sub(r"^Sketch [AB]: ", "", m.get("title", "Song")) if job.kind == "build" else m.get("title", "Song")
        made = (m.get("report") or {}).get("title")                 # the title the composer made up ...
        if made and title == made and out.get("report", {}).get("title"):
            title = make_title(out["report"]["title"]) or title       # ... follows the song (a key change)
        new = self._store(out, {"title": title, "request": m.get("request", ""), "spec": m.get("spec", {}),
                                "project": m.get("project", base), "rev": int(m.get("rev", 1)) + 1, "parent": base,
                                "change": change, "kind": "song"})
        self.store.update(base, head=False)
        if job.kind == "build":
            for other in replaced[1:]:
                self.store.delete(other)
        log.info("music job %d: %s -> %s (%s)", job.id, base, new, change)
        self._done(job, [(new, title)], replaced, change)

    def _undo(self, job: MusicJob) -> None:
        m = self.store.meta(job.base)
        parent = m.get("parent", "")
        if not parent or not self.store.exists(parent):
            return self._set(job, status="error", error="This is the first version: there is nothing to undo")
        self.store.update(job.base, head=False, undone=True)
        self.store.update(parent, head=True)
        pm = self.store.meta(parent)
        log.info("music job %d: undo %s -> %s", job.id, job.base, parent)
        self._done(job, [(parent, pm.get("title", "Song"))], [job.base], f"back to version {pm.get('rev', 1)}")
