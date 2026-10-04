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
"""

from __future__ import annotations

import json
import logging
import os
import secrets
import threading
import time
from datetime import datetime

from . import dither as D
from . import text as TX
from .config import DATA_DIR, HELPER_DIR, Config
from .qwen import QwenClient, QwenError, image_part
from .store import Store, make_title

log = logging.getLogger("mindserver.chat")

CHAT_DIR = os.path.join(DATA_DIR, "chats")
PROMPT_DIR = os.path.join(HELPER_DIR, "prompts")
PROMPTS = {"chat": "system_chat.txt", "enhance": "enhance.txt", "vision": "vision.txt"}
HISTORY_CHARS = 24000          # how much earlier conversation is sent along


def load_prompt(name: str) -> str:
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
            else:
                cur["text"] = (cur["text"] + "\n" + line) if cur["text"] else line
        for m in chat["messages"]:
            m["text"] = m["text"].rstrip("\n")
        first = next((m["text"] for m in chat["messages"] if m["role"] == "user"), "")
        chat["title"] = make_title(first) or "Chat"
        return chat


# ====================================================================== engine

class ChatEngine:
    def __init__(self, config: Config, store: Store, jobs, chats: ChatStore | None = None):
        self.config = config
        self.store = store
        self.jobs = jobs
        self.chats = chats or ChatStore()
        self.qwen = QwenClient(config)
        jobs.prepare = self._prepare_job

    # ------------------------------------------------------------ helpers

    def _system(self) -> str:
        return load_prompt("chat").replace("{date}", datetime.now().strftime("%A %d %B %Y"))

    def _history(self, chat: dict) -> list[dict]:
        """Earlier messages, newest kept first until the character budget runs out."""
        out, used = [], 0
        for m in reversed(chat["messages"]):
            t = m.get("text", "")
            if m.get("drawn"):
                t += "\n" + "\n".join(f"[You drew picture {d['id']}: {d.get('prompt', d.get('title', ''))}]"
                                      for d in m["drawn"])
            if m.get("image"):
                t = f"[Attached picture {m['image']}]\n" + t
            used += len(t)
            if used > HISTORY_CHARS and out:
                break
            out.append({"role": m["role"], "content": t})
        return list(reversed(out))

    def _vision_text(self, image_id: str, earlier: bool, two: bool) -> str:
        meta = self.store.meta(image_id)
        text = (load_prompt("vision").replace("{image_id}", image_id)
                .replace("{title}", meta.get("title", "")).replace("{prompt}", meta.get("prompt", "")))
        if not two:
            text += ("\n\nOnly one version is attached this time: the picture exists only on the Tandy, so what "
                     "you see is the dithered 16-colour version itself.")
        if earlier:
            text += ("\n\nThis is the picture from earlier in this conversation; it stays in view so you can "
                     "answer follow-up questions about it.")
        return text

    def _vision_parts(self, image_id: str) -> tuple[list, bool]:
        """[original, what the Tandy shows] as image parts; one part if there is no separate original."""
        meta = self.store.meta(image_id)
        original = self.store.original(image_id)
        if meta.get("source") == "tandy-upload":
            return [image_part(original, png=True)], False
        s = self.config.dither_settings()
        s.mode = meta.get("mode", s.mode)
        from . import tpi as TPI
        info = TPI.parse(self.store.tpi_bytes(image_id, s))
        idx = D.unpack(info.image, info.width, info.height)
        tandy = D.preview_4x3(idx, s, 1024)          # the Tandy's real pixel shape, 4:3
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

    def enhance(self, prompt: str, emit=None) -> str:
        """Rewrite a picture idea into a generator prompt (streams 'S thinking' as keep-alive)."""
        msgs = [{"role": "system", "content": load_prompt("enhance")}, {"role": "user", "content": prompt}]
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
            job.prompt = self.enhance(job.original_prompt or job.prompt)
            log.info("job %d enhanced: %s", job.id, job.prompt)

    # ------------------------------------------------------------ chat reply

    def reply(self, chat_id: str | None, user_text: str, image_id: str | None, emit,
              enhance_draw: bool | None = None) -> dict:
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

        msgs = [{"role": "system", "content": self._system()}] + self._history(chat)
        # The picture in view: the one attached now, or else the most recent one in this chat
        view_id, earlier = image_id, False
        if not view_id:
            recent = self._recent_picture(chat)
            if recent and self.store.exists(recent):
                view_id, earlier = recent, True
        if view_id:
            parts, two = self._vision_parts(view_id)
            # One system message only: Qwen chat templates (ThinkingCap's at least) reject a later one
            msgs[0]["content"] += "\n\n" + self._vision_text(view_id, earlier, two)
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
        try:
            for kind, t in self.qwen.stream(msgs, effort):
                if kind == "reasoning" and time.time() - last_alive > 5:
                    emit("S thinking")               # also a keep-alive for the Tandy's timeout
                    last_alive = time.time()
                elif kind == "content":
                    vis = splitter.feed(t)
                    visible_raw.append(vis)
                    for line in tt.feed(vis):
                        emit(line)
                elif kind == "done":
                    finish = t
            rest = splitter.finish()
            visible_raw.append(rest)
            for line in tt.feed(rest) + tt.flush():
                emit(line)
        except QwenError as e:
            emit(f"E {e}")
            emit("D")
            self.chats.save(chat)
            return chat

        answer = TX.clean("".join(visible_raw))
        if not answer and not splitter.prompts:
            emit("E The answer was empty" + (" (cut off: raise max tokens)" if finish == "length" else ""))
        amsg = {"role": "assistant", "text": answer}
        chat["messages"].append(amsg)
        self.chats.save(chat)

        if splitter.prompts:
            self._draw(chat, amsg, splitter.prompts[0], emit, enhance_draw)
        emit("D")
        return chat

    def _draw(self, chat: dict, amsg: dict, prompt: str, emit, enhance: bool | None) -> None:
        if enhance is None:
            enhance = bool(self.config["qwen"].get("draw_enhance", False))
        job = self.jobs.submit(prompt, title=make_title(prompt), enhance=enhance)
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
