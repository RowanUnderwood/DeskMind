"""Chats with Qwen: storage, streaming replies, <draw> pictures, vision, prompt enhancement.

Reply stream (one line each, CP437):
    C <chat id>      first line: the chat this reply belongs to (new chats get an id here)
    S <status>       e.g. "S thinking", "S drawing 45"
    T <text>         append text        N   line break
    I <id> <title>   a picture is ready (download it with GET /img/<id>)
    E <message>      error              D   done

Transcript format (GET /chat/<id>, POST /chat/<id>/sync, and DeskMind's local .TCH files):
    >U               a user message starts; the following lines are its text
    >A               an assistant message starts
    !P <image id>    (inside a user message) the user attached this picture
    !I <id> <title>  (inside an assistant message) a picture was drawn
    !M <id> <title>  (inside an assistant message) a song was composed (music.py; GET /music/<id>)

Clients that can play songs ask with music=1: the system prompt then gets the music prompt and the
reply stream can carry
    M <id> <title>   a song is ready (download it with GET /music/<id>)

Qwen asks for pictures, songs and web searches with tool calls (tools.py; setting qwen.tools).  A web
search adds a status line while it runs:
    S searching <query>
With qwen.tools off, or if NInfer rejects tools, Qwen writes <draw>/<music> tags instead (text.DrawSplitter,
which also stays on in tool mode as a safety net).  System prompt = system_chat + (tools_chat [+ search_chat]
[+ music_tools]) or (draw_tags [+ music_chat]).
"""

from __future__ import annotations

import json
import logging
import os
import re
import secrets
import threading
import time
from datetime import datetime

from . import dither as D
from . import text as TX
from . import tools as TL
from .config import DATA_DIR, HELPER_DIR, Config
from .qwen import QwenClient, QwenError, image_part
from .search import SearchError, WebSearch
from .store import Store, make_title

log = logging.getLogger("mindserver.chat")

CHAT_DIR = os.path.join(DATA_DIR, "chats")
PROMPT_DIR = os.path.join(HELPER_DIR, "prompts")
PROMPTS = {"chat": "system_chat.txt", "enhance": "enhance.txt", "vision": "vision.txt",
           # A CGA PC (requests with mode=cga) gets its own version of each prompt
           "chat_cga": "system_chat_cga.txt", "enhance_cga": "enhance_cga.txt", "vision_cga": "vision_cga.txt",
           "music_chat": "music_chat.txt", "music_spec": "music_spec.txt",
           # How Qwen draws/composes/searches: tool calls, or the older <draw>/<music> tags
           "tools_chat": "tools_chat.txt", "tools_chat_cga": "tools_chat_cga.txt", "search_chat": "search_chat.txt",
           "music_tools": "music_tools.txt", "draw_tags": "draw_tags.txt", "draw_tags_cga": "draw_tags_cga.txt",
           "music_speaker": "music_speaker.txt"}
HISTORY_CHARS = 24000          # how much earlier conversation is sent along
OLD_DRAW_NOTE = re.compile(r"\[You drew picture [0-9A-Za-z]{1,12}:[^\]]*\]", re.IGNORECASE)


def load_prompt(name: str, mode: str | None = None) -> str:
    """Prompt `name`; with mode "cga" its CGA version if that file exists."""
    if mode == "cga" and os.path.exists(os.path.join(PROMPT_DIR, PROMPTS.get(name + "_cga", "-"))):
        name += "_cga"
    with open(os.path.join(PROMPT_DIR, PROMPTS[name]), encoding="utf-8") as f:
        return f.read().strip()


def save_prompt(name: str, text: str) -> None:
    path = os.path.join(PROMPT_DIR, PROMPTS[name])
    with open(path + ".tmp", "w", encoding="utf-8") as f:
        f.write(text.strip() + "\n")
    os.replace(path + ".tmp", path)


# ====================================================================== storage

class ChatStore:
    def __init__(self, folder: str = CHAT_DIR):
        self.folder = folder
        os.makedirs(folder, exist_ok=True)
        self.lock = threading.Lock()

    def path(self, id_: str) -> str:
        return os.path.join(self.folder, f"{id_.upper()}.json")

    def exists(self, id_: str) -> bool:
        return os.path.exists(self.path(id_))

    def new(self, first_text: str = "") -> dict:
        while True:
            id_ = secrets.token_hex(4).upper()
            if not self.exists(id_):
                break
        now = datetime.now().isoformat(timespec="seconds")
        return {"id": id_, "title": make_title(first_text) or "New chat", "created": now, "updated": now,
                "messages": []}

    def load(self, id_: str) -> dict:
        with open(self.path(id_), encoding="utf-8") as f:
            return json.load(f)

    def save(self, chat: dict) -> None:
        chat["updated"] = datetime.now().isoformat(timespec="seconds")
        with self.lock:
            tmp = self.path(chat["id"]) + ".tmp"
            with open(tmp, "w", encoding="utf-8") as f:
                json.dump(chat, f, indent=1)
            os.replace(tmp, self.path(chat["id"]))

    def delete(self, id_: str) -> None:
        with self.lock:
            try:
                os.remove(self.path(id_))
            except FileNotFoundError:
                pass

    def list(self) -> list[dict]:
        out = []
        for name in os.listdir(self.folder):
            if name.endswith(".json"):
                try:
                    with open(os.path.join(self.folder, name), encoding="utf-8") as f:
                        c = json.load(f)
                    out.append({k: c[k] for k in ("id", "title", "created", "updated")} | {"count": len(c["messages"])})
                except (OSError, ValueError, KeyError):
                    pass
        return sorted(out, key=lambda c: c["updated"], reverse=True)

    # ------------------------------------------------------------ transcript format

    @staticmethod
    def to_transcript(chat: dict) -> str:
        lines = []
        for m in chat["messages"]:
            lines.append(">U" if m["role"] == "user" else ">A")
            if m.get("image"):
                lines.append(f"!P {m['image']}")
            lines += TX.to_cp437(m.get("text", "")).split("\n")
            for d in m.get("drawn", []):
                lines.append(f"!I {d['id']} {d.get('title', '')}".rstrip())
            for s in m.get("music", []):
                lines.append(f"!M {s['id']} {s.get('title', '')}".rstrip())
        return "\n".join(lines) + "\n"

    @staticmethod
    def from_transcript(id_: str, text: str) -> dict:
        now = datetime.now().isoformat(timespec="seconds")
        chat = {"id": id_.upper(), "title": "", "created": now, "updated": now, "messages": []}
        cur = None
        for line in text.replace("\r\n", "\n").split("\n"):
            if line in (">U", ">A"):
                cur = {"role": "user" if line == ">U" else "assistant", "text": ""}
                chat["messages"].append(cur)
            elif cur is None:
                continue
            elif line.startswith("!P "):
                cur["image"] = line[3:].strip()[:8].upper()
            elif line.startswith("!I "):
                parts = line[3:].split(" ", 1)
                cur.setdefault("drawn", []).append({"id": parts[0].upper(), "title": parts[1] if len(parts) > 1 else ""})
            elif line.startswith("!M "):
                parts = line[3:].split(" ", 1)
                cur.setdefault("music", []).append({"id": parts[0].upper(), "title": parts[1] if len(parts) > 1 else ""})
            else:
                cur["text"] = (cur["text"] + "\n" + line) if cur["text"] else line
        for m in chat["messages"]:
            m["text"] = m["text"].rstrip("\n")
        first = next((m["text"] for m in chat["messages"] if m["role"] == "user"), "")
        chat["title"] = make_title(first) or "Chat"
        return chat


# ====================================================================== engine

class _ToolsRejected(Exception):
    """NInfer answered 400 to a request with tools: the reply is retried with <draw> tags."""


class ChatEngine:
    def __init__(self, config: Config, store: Store, jobs, chats: ChatStore | None = None, music=None):
        self.config = config
        self.store = store
        self.jobs = jobs
        self.music = music              # music.MusicService, or None
        self.chats = chats or ChatStore()
        self.qwen = QwenClient(config)
        self.search = WebSearch(config)
        jobs.prepare = self._prepare_job

    # ------------------------------------------------------------ helpers

    def _system(self, mode: str | None = None, tools: bool = False, music: bool = False,
                search: bool = False, speaker: bool = False) -> str:
        parts = [load_prompt("chat", mode)]
        if tools:
            parts.append(load_prompt("tools_chat", mode))
            if search:
                parts.append(load_prompt("search_chat"))
            if music:
                parts.append(load_prompt("music_tools"))
                if speaker:
                    parts.append(load_prompt("music_speaker"))
        else:
            parts.append(load_prompt("draw_tags", mode))
            if music:
                parts.append(load_prompt("music_chat"))
        return "\n\n".join(parts).replace("{date}", datetime.now().strftime("%A %d %B %Y"))

    def _drawn_prompt(self, d: dict) -> str:
        """What was drawn: the saved prompt, else the picture's own (chats synced from the Tandy keep only
        the title), else the title."""
        if d.get("prompt"):
            return d["prompt"]
        if self.store.exists(d["id"]):
            p = self.store.meta(d["id"]).get("prompt")
            if p:
                return p
        return d.get("title", "")

    def _history(self, chat: dict, tools: bool = False) -> list[dict]:
        """Earlier messages, newest kept first until the character budget runs out.

        A past drawing appears in the form Qwen must use again: with tools as a draw_picture call and its
        result (tools.replay; songs and searches too), else as the <draw> tag.  (A note like "[You drew
        picture ID: ...]" used to be added instead, and Qwen sometimes copied the note rather than drawing;
        such lines saved in old replies are left out.)"""
        out, used, n = [], 0, 1
        for m in reversed(chat["messages"]):
            t = OLD_DRAW_NOTE.sub("", m.get("text", "")).strip()
            if tools and m["role"] == "assistant":
                group = TL.replay(t, m.get("drawn", []), m.get("music", []), m.get("tools", []),
                                  self._drawn_prompt, n)
                n += len(m.get("drawn", [])) + len(m.get("music", [])) + len(m.get("tools", []))
                used += sum(len(g.get("content") or "") + len(json.dumps(g.get("tool_calls", ""))) for g in group)
                if used > HISTORY_CHARS and out:
                    break
                out += reversed(group)
                continue
            if m.get("drawn"):
                t += "\n" + "\n".join(f"<draw>{self._drawn_prompt(d)}</draw>" for d in m["drawn"])
            if m.get("music"):
                t += "\n" + "\n".join(f"<music>{s.get('request') or s.get('title', '')}</music>" for s in m["music"])
            if m.get("image"):
                t = f"[Attached picture {m['image']}]\n" + t
            used += len(t)
            if used > HISTORY_CHARS and out:
                break
            out.append({"role": m["role"], "content": t})
        return list(reversed(out))

    def _vision_text(self, image_id: str, earlier: bool, two: bool, mode: str | None = None) -> str:
        meta = self.store.meta(image_id)
        text = (load_prompt("vision", mode).replace("{image_id}", image_id)
                .replace("{title}", meta.get("title", "")).replace("{prompt}", meta.get("prompt", "")))
        if not two:
            text += ("\n\nOnly one version is attached this time: the picture exists only on the Tandy, so what "
                     "you see is the dithered version itself.")
        if earlier:
            text += ("\n\nThis is the picture from earlier in this conversation; it stays in view so you can "
                     "answer follow-up questions about it.")
        return text

    def _vision_parts(self, image_id: str, mode: str | None = None) -> tuple[list, bool]:
        """[original, what the Tandy shows] as image parts; one part if there is no separate original."""
        meta = self.store.meta(image_id)
        original = self.store.original(image_id)
        if meta.get("source") == "tandy-upload":
            return [image_part(original, png=True)], False
        s = self.config.dither_settings()
        s.mode = mode or meta.get("mode", s.mode)
        from . import tpi as TPI
        info = TPI.parse(self.store.tpi_bytes(image_id, s))
        tandy = D.preview_4x3(info.image_idx(), s, 1024, info.rgb(s))   # the real pixel shape, 4:3
        return [image_part(original), image_part(tandy, 1024, png=True)], True

    def _recent_picture(self, chat: dict) -> str | None:
        """The most recent picture in the chat: attached by the user or drawn by Qwen."""
        for m in reversed(chat["messages"]):
            if m.get("drawn"):
                return m["drawn"][-1]["id"]
            if m.get("image"):
                return m["image"]
        return None

    # ------------------------------------------------------------ enhancement

    def enhance(self, prompt: str, emit=None, mode: str | None = None) -> str:
        """Rewrite a picture idea into a generator prompt (streams 'S thinking' as keep-alive)."""
        msgs = [{"role": "system", "content": load_prompt("enhance", mode)}, {"role": "user", "content": prompt}]
        out, last = [], 0.0
        for kind, t in self.qwen.stream(msgs, self.config["qwen"]["effort_enhance"], 2500):
            if kind == "content":
                out.append(t)
            if emit and time.time() - last > 2:
                emit("S thinking")
                last = time.time()
        text = TX.clean(" ".join("".join(out).split())).strip().strip('"').strip()
        return text or prompt

    def _prepare_job(self, job) -> None:
        if getattr(job, "enhance", False):
            self.jobs._update(job, text="enhancing prompt")
            job.prompt = self.enhance(job.original_prompt or job.prompt, mode=job.mode)
            log.info("job %d enhanced: %s", job.id, job.prompt)

    # ------------------------------------------------------------ chat reply

    def reply(self, chat_id: str | None, user_text: str, image_id: str | None, emit,
              enhance_draw: bool | None = None, mode: str | None = None, music: bool = False,
              speaker: bool = False) -> dict:
        """`mode` is the asking machine's picture mode ("cga" for a CGA PC): it picks the prompts and
        the mode of pictures drawn in this reply.  None = the default (Tandy)."""
        user_text = user_text.strip()
        emit = TX.Coalescer(emit)
        if chat_id and self.chats.exists(chat_id):
            chat = self.chats.load(chat_id)
        else:
            chat = self.chats.new(user_text)
            if chat_id and len(chat_id) == 8:
                chat["id"] = chat_id.upper()
        emit(f"C {chat['id']}")

        if image_id and not self.store.exists(image_id):
            emit(f"E I don't have picture {image_id} on the server. Upload it first.")
            emit("D")
            return chat

        music = music and self.music is not None
        if self.config["qwen"].get("tools", True):
            try:
                return self._answer(chat, user_text, image_id, emit, enhance_draw, mode, music, True, speaker)
            except _ToolsRejected as e:
                log.warning("NInfer rejected tool calls (%s); this reply uses <draw> tags", e)
                chat["messages"].pop()             # the user message _answer added; it is added again
        return self._answer(chat, user_text, image_id, emit, enhance_draw, mode, music, False, speaker)

    def _answer(self, chat: dict, user_text: str, image_id: str | None, emit, enhance_draw: bool | None,
                mode: str | None, music: bool, tools: bool, speaker: bool = False) -> dict:
        """One reply, with tool calls (`tools`) or with tags."""
        search = tools and self.search.available()
        # One system message only: Qwen chat templates (ThinkingCap's at least) reject a later one
        system = self._system(mode, tools, music, search, speaker)
        styles = self.music.styles() if music else None
        msgs = [{"role": "system", "content": system}] + self._history(chat, tools)
        # The picture in view: the one attached now, or else the most recent one in this chat
        view_id, earlier = image_id, False
        if not view_id:
            recent = self._recent_picture(chat)
            if recent and self.store.exists(recent):
                view_id, earlier = recent, True
        if view_id:
            parts, two = self._vision_parts(view_id, mode)
            msgs[0]["content"] += "\n\n" + self._vision_text(view_id, earlier, two, mode)
            content = parts + [{"type": "text", "text": user_text}]
            effort = self.config["qwen"]["effort_vision"]
        else:
            content = user_text
            effort = self.config["qwen"]["effort_chat"]
        msgs.append({"role": "user", "content": content})

        umsg = {"role": "user", "text": user_text}
        if image_id:
            umsg["image"] = image_id.upper()
        chat["messages"].append(umsg)

        splitter, tt = TX.DrawSplitter(), TX.TandyText()
        visible_raw, finish, last_alive = [], "stop", 0.0
        draws, songs, searches = [], [], []
        max_rounds = int(self.config["search"].get("max_rounds", 3))
        rnd = 0
        try:
            while True:
                schema = TL.schemas(music, search and rnd < max_rounds, mode, styles) if tools else None
                calls, said = [], []
                try:
                    for kind, t in self.qwen.stream(msgs, effort, tools=schema):
                        if kind == "reasoning" and time.time() - last_alive > 5:
                            emit("S thinking")       # also a keep-alive for the Tandy's timeout
                            last_alive = time.time()
                        elif kind == "content":
                            said.append(t)
                            vis = splitter.feed(t)
                            visible_raw.append(vis)
                            for line in tt.feed(vis):
                                emit(line)
                        elif kind == "tool_call":
                            calls.append(t)
                        elif kind == "done":
                            finish = t
                except QwenError as e:
                    if tools and rnd == 0 and e.status == 400 and not said and not calls:
                        raise _ToolsRejected(str(e)) from e
                    raise
                for c in calls:
                    a = c["args"]
                    if a is None:
                        log.warning("tool call %s: the arguments aren't a JSON object", c["name"])
                    elif c["name"] == TL.DRAW and str(a.get("prompt", "")).strip():
                        draws.append(" ".join(str(a["prompt"]).split()))
                    elif c["name"] in TL.MUSIC_TOOLS and music:
                        songs.append((c["name"], a))
                if not search or rnd >= max_rounds or not any(
                        c["name"] in TL.SEARCH_TOOLS and c["args"] is not None for c in calls):
                    break
                # Search, hand Qwen the results, and let it go on with the same reply
                if "".join(said).strip():
                    visible_raw.append("\n\n")
                    for line in tt.feed("\n\n"):
                        emit(line)
                msgs.append(TL.call_msg(calls, "".join(said)))
                for c in calls:
                    msgs.append(TL.result_msg(c["id"], self._run_tool(c, emit, searches, music)))
                rnd += 1
            rest = splitter.finish()
            visible_raw.append(rest)
            for line in tt.feed(rest) + tt.flush():
                emit(line)
        except QwenError as e:
            emit(f"E {e}")
            emit("D")
            self.chats.save(chat)
            return chat

        draws += splitter.prompts                    # tags still count in tool mode
        songs += [(TL.MUSIC, {"description": m}) for m in splitter.music]
        answer = TX.clean("".join(visible_raw))
        if not answer and not draws and not songs:
            emit("E The answer was empty" + (" (cut off: raise max tokens)" if finish == "length" else ""))
        amsg = {"role": "assistant", "text": answer}
        if searches:
            amsg["tools"] = searches
        chat["messages"].append(amsg)
        self.chats.save(chat)

        if draws:
            self._draw(chat, amsg, draws[0], emit, enhance_draw, mode)
        if songs and music:
            for name, args in songs[:2]:                 # e.g. keep a sketch, then a new drum pattern
                self._music(chat, amsg, name, args, emit)
        emit("D")
        return chat

    def _run_tool(self, call: dict, emit, searches: list, music: bool) -> str:
        """The result for one tool call in a search round.  Searches are run and recorded in `searches`
        (with a shortened result, for later turns); a picture or song asked for in the same round is only
        acknowledged here and made after the reply, like any other."""
        name, a = call["name"], call["args"] or {}
        if name == TL.DRAW:
            return "The picture will be drawn when your reply is finished; it then appears in the chat."
        if name in TL.MUSIC_TOOLS:
            return ("This will be done when your reply is finished; the song then appears in the chat."
                    if music else "Songs can't be played on this computer.")
        if name not in TL.SEARCH_TOOLS:
            return f"There is no tool called {name}."
        what = " ".join(str(a.get("query") or a.get("id_or_url") or "").split())
        emit(f"S searching {TX.to_cp437(what)[:60]}".rstrip())
        t0 = time.time()
        try:
            text = self.search.search(what) if name == TL.SEARCH else self.search.read(what)
            log.info("%s %r: %d characters in %.1f s", name, what, len(text), time.time() - t0)
        except SearchError as e:
            log.warning("%s %r: %s", name, what, e)
            text = f"The search did not work ({e}). Tell the user you could not look it up just now."
        searches.append({"name": name, "args": a, "summary": text[:TL.SUMMARY_CHARS]})
        return text

    def _song_ref(self, chat: dict, which: str) -> dict | None:
        """A song of this chat: 'latest' (the newest one not replaced), or sketch 'A' / 'B'."""
        live = [e for m in chat["messages"] for e in m.get("music", [])
                if not e.get("replaced") and self.music.store.exists(e["id"])]
        if which in ("A", "B"):
            return next((e for e in reversed(live) if e.get("kind") == "sketch" and
                         e.get("title", "").startswith(f"Sketch {which}")), None)
        return live[-1] if live else None

    def _music(self, chat: dict, amsg: dict, name: str, args: dict, emit) -> None:
        """A song action after the reply: compose (or two sketches), revise, keep a sketch, or undo.
        New songs stream as 'M <id> <title>', the ones they replace as 'R <id>'."""
        request = ""
        if name == TL.MUSIC:
            request = TL.music_request(args)
            hints = {k: args[k] for k in ("style", "length") if args.get(k)}
            job = (self.music.submit_sketches if args.get("sketches") else self.music.submit)(request, hints=hints)
        else:
            ref = self._song_ref(chat, args.get("which") if name == TL.KEEP else (args.get("song") or "latest"))
            if not ref:
                emit("E There is no such song in this chat to change.")
                return
            request = ref.get("request", "")
            if name == TL.KEEP:
                job = self.music.submit_build(ref["id"], args.get("length", ""))
            elif name == TL.UNDO:
                job = self.music.submit_undo(ref["id"])
            else:
                job = self.music.submit_revise(ref["id"], self.music.edits_from_args(ref["id"], args), request)
        emit("S composing")
        line = job.line()
        deadline = time.time() + 10 * 60
        while job.status not in ("done", "error") and time.time() < deadline:
            new = self.music.wait_change(job, line, timeout=5.0)
            if new == line:
                emit("S composing")              # keep-alive for the Tandy's timeout
            line = new
        if job.status != "done":
            emit(f"E Composing failed: {job.error or 'timed out'}")
            return
        result = self._music_result(name, job)
        for k, (mid, title) in enumerate(job.results):
            emit(f"M {mid} {title}".rstrip())
            entry = {"id": mid, "title": title, "request": request,
                     "kind": self.music.store.meta(mid).get("kind", "song"), "tool": name, "args": args,
                     "result": result}
            if k:
                entry["same_call"] = True        # the second sketch of one compose_music call
            amsg.setdefault("music", []).append(entry)
        for r in job.replaced:
            emit(f"R {r}")
            for m in chat["messages"]:
                for e in m.get("music", []):
                    if e["id"] == r:
                        e["replaced"] = True
        self.chats.save(chat)

    @staticmethod
    def _music_result(name: str, job) -> str:
        titles = " and ".join(f'"{t}"' for _, t in job.results)
        if name == TL.MUSIC and len(job.results) > 1:
            return f"Two sketches were composed, {titles}; the user can play both and keep one (keep_sketch)."
        if name == TL.MUSIC:
            return TL.music_result(job.title)
        if name == TL.KEEP:
            return f"The sketch was kept and built into a full song, {titles} ({job.change}); the other sketch is gone."
        if name == TL.UNDO:
            return f"Undone: the song is back to its earlier version ({job.change})."
        return f"Revised: {job.change}. The new version {titles} replaced the old one in the chat."

    def _draw(self, chat: dict, amsg: dict, prompt: str, emit, enhance: bool | None,
              mode: str | None = None) -> None:
        if enhance is None:
            enhance = bool(self.config["qwen"].get("draw_enhance", False))
        job = self.jobs.submit(prompt, mode, title=make_title(prompt), enhance=enhance)
        emit("S drawing 0")
        line, shown = job.line(), 0
        deadline = time.time() + 15 * 60
        while job.status not in ("done", "error") and time.time() < deadline:
            new = self.jobs.wait_change(job, line, timeout=5.0)
            if new != line:
                line = new
                pct = int(job.progress * 100)
                if job.status == "running" and pct != shown:
                    emit(f"S drawing {pct}")
                    shown = pct
        if job.status == "done":
            emit(f"I {job.image_id} {job.title}".rstrip())
            amsg.setdefault("drawn", []).append({"id": job.image_id, "title": job.title, "prompt": job.prompt})
            self.chats.save(chat)
        else:
            emit(f"E Drawing failed: {job.error or 'timed out'}")
