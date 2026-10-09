"""MindServer HTTP/1.0 server for DeskMind.

Plain HTTP/1.0 with `Connection: close`, so the Tandy side stays simple and
everything can be tried with curl or mTCP's HTGET.  Text bodies are CP437.
Streamed replies are coded lines:

    T <text>       append text          P            paragraph break
    I <id> <title> image ready          S <status>   status / progress
    M <id> <title> song ready
    E <message>    error                D            done
"""

from __future__ import annotations

import logging
import os
import re
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

from PIL import Image

from . import dither as D
from . import text as TX
from . import tpi
from .chat import PROMPTS, ChatEngine, load_prompt, save_prompt
from .config import HELPER_DIR, PROJECT_DIR, Config
from .qwen import QwenError
from .store import make_title
from .jobs import JobQueue
from .music import MusicService
from .services import Services
from .store import Store

VERSION = "0.1"
log = logging.getLogger("mindserver")


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"
    server_version = f"MindServer/{VERSION}"
    app: "MindServer" = None   # set by MindServer

    # ------------------------------------------------------------ helpers

    def log_message(self, fmt, *args):
        log.info("%s %s", self.client_address[0], fmt % args)

    def _query(self) -> dict:
        q = parse_qs(urlparse(self.path).query)
        return {k: v[0] for k, v in q.items()}

    def _body(self) -> bytes:
        n = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(n) if n > 0 else b""

    def _authorised(self) -> bool:
        token = self.app.config["server"]["token"]
        if not token:
            return True
        return self.headers.get("X-Token") == token or self._query().get("t") == token

    def send_bytes(self, data: bytes, ctype: str = "application/octet-stream", status: int = 200):
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def send_text(self, text: str, status: int = 200):
        self.send_bytes(text.replace("\r\n", "\n").replace("\n", "\r\n").encode("cp437", "replace"),
                        "text/plain; charset=cp437", status)

    def start_stream(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/plain; charset=cp437")
        self.end_headers()

    def stream_line(self, line: str):
        self.wfile.write((line + "\r\n").encode("cp437", "replace"))
        self.wfile.flush()

    # ------------------------------------------------------------ routing

    def do_GET(self):
        self._route("GET")

    def do_POST(self):
        self._route("POST")

    def _route(self, method: str):
        path = urlparse(self.path).path.rstrip("/") or "/"
        self.app.note_client(self.client_address[0])
        if not self._authorised():
            self.send_text("E Wrong token\n", 403)
            return
        fn, args = ROUTES.get((method, path)), ()
        if fn is None:
            for m, rx, f in PATTERN_ROUTES:
                hit = rx.fullmatch(path) if m == method else None
                if hit:
                    fn, args = f, hit.groups()
                    break
        if fn is None:
            self.send_text(f"E Unknown request {method} {path}\n", 404)
            return
        try:
            fn(self, *args)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            log.info("client went away")
        except Exception as e:          # report instead of dropping the Tandy's connection
            log.exception("error in %s %s", method, path)
            try:
                self.send_text(f"E {type(e).__name__}: {e}\n", 500)
            except Exception:
                pass


# ---------------------------------------------------------------- handlers

def h_ping(h: Handler):
    st = h.app.status()
    h.send_text(
        f"OK MindServer {VERSION}\n"
        f"qwen {st['qwen']}\n"
        f"comfy {st['comfy']}\n"
        f"music {st.get('music', 'unknown')}\n"
        f"you {h.client_address[0]}\n"
    )


def h_test_bytes(h: Handler):
    n = max(0, min(int(h._query().get("n", 65536)), 1 << 20))
    pattern = bytes(range(256)) * (n // 256 + 1)
    h.send_bytes(pattern[:n])


def h_test_stream(h: Handler):
    h.start_stream()
    h.stream_line("S thinking")
    time.sleep(0.8)
    words = ("Hello from MindServer! This line arrives a few words at a time, "
             "the way a chat reply from Qwen will.").split()
    for i in range(0, len(words), 3):
        h.stream_line("T " + " ".join(words[i:i + 3]) + " ")
        time.sleep(0.25)
    h.stream_line("P")
    h.stream_line("T Box drawing test: ╔═╗ éè ½ ░▒▓")
    h.stream_line("D")


def h_test_echo(h: Handler):
    h.send_bytes(h._body(), "text/plain")


def h_test_image(h: Handler):
    """The first sample image through the current dither settings."""
    mode = h._query().get("mode", "640")
    if mode not in ("640", "320"):                  # Phase 0 test: Tandy layouts only
        h.send_text("E mode must be 640 or 320\n", 400)
        return
    sample = h.app.sample_image()
    s = h.app.config.dither_settings()
    s.mode = mode
    idx = D.convert(sample, s)
    h.send_bytes(D.pack(idx))


def _mode(h: Handler) -> str:
    mode = h._query().get("mode") or h.app.config["dither"]["mode"]
    if mode not in D.MODES:
        raise ValueError("mode must be 640, 320 or cga")
    return mode


def _text_body(h: Handler) -> str:
    return h._body().decode("cp437", "replace").strip()


def h_gen(h: Handler):
    """POST /gen[?mode=640&seed=N&title=..]  body = prompt.  Answers 'J <job>'."""
    prompt = _text_body(h)
    if not prompt:
        h.send_text("E Empty prompt\n", 400)
        return
    q = h._query()
    enhance = q.get("enhance") == "1"
    title = q.get("title") or (make_title(prompt) if enhance else None)
    job = h.app.jobs.submit(prompt, _mode(h), int(q.get("seed", -1)), title, enhance=enhance)
    h.send_text(f"J {job.id}\n")


# ---------------------------------------------------------------- chat (Phase 2)

def h_chat(h: Handler):
    """POST /chat[?id=<chat>&img=<image>&draw_enhance=0|1&mode=cga]  body = the user's message.  Streams the reply.
    mode=cga: a CGA PC is asking (its own prompts, pictures drawn in CGA)."""
    q = h._query()
    text = _text_body(h)
    if not text:
        h.send_text("E Empty message\n", 400)
        return
    de = q.get("draw_enhance")
    mode = _mode(h) if q.get("mode") else None
    h.start_stream()
    h.app.chat.reply(q.get("id"), text, (q.get("img") or "").upper() or None, h.stream_line,
                     None if de is None else de == "1", mode, q.get("music") == "1")


def h_chats(h: Handler):
    """GET /chats: '<id> <yyyymmddhhmm> <messages> <title>' per chat, most recent first."""
    lines = []
    for c in h.app.chat.chats.list():
        stamp = c["updated"][:16].replace("-", "").replace("T", "").replace(":", "")
        lines.append(f"{c['id']} {stamp} {c['count']} {TX.to_cp437(c['title'])}")
    h.send_text("\n".join(lines) + ("\n" if lines else ""))


def h_chat_get(h: Handler, id_: str):
    chats = h.app.chat.chats
    if not chats.exists(id_):
        h.send_text(f"E No chat {id_.upper()}\n", 404)
        return
    h.send_text(chats.to_transcript(chats.load(id_)))


def h_chat_del(h: Handler, id_: str):
    h.app.chat.chats.delete(id_)
    h.send_text("OK\n")


def h_chat_sync(h: Handler, id_: str):
    """POST /chat/<id>/sync  body = transcript (the Tandy's copy wins)."""
    chats = h.app.chat.chats
    chat = chats.from_transcript(id_, _text_body(h))
    chats.save(chat)
    h.send_text(f"OK {len(chat['messages'])}\n")


def h_enhance(h: Handler):
    """POST /enhance[?mode=cga]  body = picture idea.  Streams 'S thinking' keep-alives, then 'T <prompt>' and 'D'."""
    idea = _text_body(h)
    if not idea:
        h.send_text("E Empty prompt\n", 400)
        return
    mode = _mode(h) if h._query().get("mode") else None
    h.start_stream()
    try:
        h.stream_line("T " + TX.to_cp437(h.app.chat.enhance(idea, h.stream_line, mode)))
    except QwenError as e:
        h.stream_line(f"E {e}")
    h.stream_line("D")


def h_prompt_get(h: Handler, name: str):
    if name not in PROMPTS:
        h.send_text("E Unknown prompt\n", 404)
        return
    h.send_text(TX.to_cp437(load_prompt(name)) + "\n")


def h_prompt_set(h: Handler, name: str):
    if name not in PROMPTS:
        h.send_text("E Unknown prompt\n", 404)
        return
    text = _text_body(h)
    if len(text) < 10:
        h.send_text("E Prompt too short\n", 400)
        return
    save_prompt(name, text)
    h.send_text("OK\n")


def h_upload(h: Handler, id_: str):
    """POST /img/<id>/upload  body = TPI file from the Tandy (when MindServer lost the original)."""
    data = h._body()
    info = tpi.parse(data)
    if info.id.upper() != id_.upper():
        h.send_text("E The file's id does not match\n", 400)
        return
    if not h.app.store.exists(id_):
        h.app.store.import_tpi(data)
    h.send_text("OK\n")


def h_job(h: Handler, job_id: str):
    """GET /job/<n>: one status line.  ?wait=1 streams lines until the job ends."""
    job = h.app.jobs.get(int(job_id))
    if job is None:
        h.send_text("E No such job\n", 404)
        return
    if h._query().get("wait") != "1":
        h.send_text(job.line() + "\n")
        return
    h.start_stream()
    line = job.line()
    h.stream_line(line)
    deadline = time.time() + 15 * 60
    while job.status not in ("done", "error") and time.time() < deadline:
        new = h.app.jobs.wait_change(job, line, timeout=5.0)
        if new != line:
            line = new
            h.stream_line(line)
    if job.status not in ("done", "error"):
        h.stream_line("E Timed out")
    h.stream_line("D")


def _get_id(h: Handler, id_: str) -> str | None:
    id_ = id_.upper()
    if not h.app.store.exists(id_):
        h.send_text(f"E No image {id_}\n", 404)
        return None
    return id_


def h_img(h: Handler, id_: str):
    """GET /img/<id>[?mode=320]: the TPI file."""
    id_ = _get_id(h, id_)
    if id_:
        s = h.app.config.dither_settings()
        s.mode = _mode(h)
        h.send_bytes(h.app.store.tpi_bytes(id_, s))


def h_thumb(h: Handler, id_: str):
    """GET /img/<id>/thumb[?mode=cga]: just the thumbnail pixels (for gallery sync)."""
    id_ = _get_id(h, id_)
    if id_:
        s = None
        if h._query().get("mode"):
            s = h.app.config.dither_settings()
            s.mode = _mode(h)
        h.send_bytes(tpi.parse(h.app.store.tpi_bytes(id_, s)).thumb)


def h_title(h: Handler, id_: str):
    id_ = _get_id(h, id_)
    if id_:
        meta = h.app.store.rename(id_, _text_body(h))
        h.send_text(f"OK {meta['title']}\n")


def h_delete(h: Handler, id_: str):
    id_ = _get_id(h, id_)
    if id_:
        h.app.store.delete(id_)
        h.send_text("OK\n")


def h_list(h: Handler):
    """GET /list: '<id> <yyyymmddhhmm> <mode> <title>' per image, newest first."""
    lines = []
    for m in h.app.store.list():
        stamp = m.get("created", "")[:16].replace("-", "").replace("T", "").replace(":", "")
        lines.append(f"{m['id']} {stamp} {m.get('mode', '640')} {m.get('title', '')}")
    h.send_text("\n".join(lines) + ("\n" if lines else ""))


# ---------------------------------------------------------------- music

def h_music_gen(h: Handler):
    """POST /music/gen  body = a description, or a JSON spec ({"style": ...}).  Answers 'J <job>'."""
    text = _text_body(h)
    if not text:
        h.send_text("E Empty request\n", 400)
        return
    spec = None
    if text.lstrip().startswith("{"):
        import json
        try:
            spec = json.loads(text)
        except ValueError:
            h.send_text("E Bad JSON\n", 400)
            return
    job = h.app.music.submit(text if spec is None else "", spec)
    h.send_text(f"J {job.id}\n")


def h_music_job(h: Handler, job_id: str):
    """GET /music/job/<n>[?wait=1]: 'S running composing' ... then 'M <id> <title>' or 'E <msg>' (and 'D' when waiting)."""
    job = h.app.music.get(int(job_id))
    if job is None:
        h.send_text("E No such job\n", 404)
        return
    if h._query().get("wait") != "1":
        h.send_text(job.line() + "\n")
        return
    h.start_stream()
    line = job.line()
    h.stream_line(line)
    deadline = time.time() + 10 * 60
    while job.status not in ("done", "error") and time.time() < deadline:
        new = h.app.music.wait_change(job, line, timeout=5.0)
        if new != line:
            line = new
            h.stream_line(line)
    if job.status not in ("done", "error"):
        h.stream_line("E Timed out")
    h.stream_line("D")


def h_music_list(h: Handler):
    """GET /music/list: '<id> <yyyymmddhhmm> <seconds> <title>' per song, newest first."""
    lines = []
    for m in h.app.music.store.list():
        stamp = m.get("created", "")[:16].replace("-", "").replace("T", "").replace(":", "") or "000000000000"
        secs = round(m.get("report", {}).get("seconds", 0))
        lines.append(f"{m['id']} {stamp} {secs} {TX.to_cp437(m.get('title', ''))}")
    h.send_text("\n".join(lines) + ("\n" if lines else ""))


def _music_id(h: Handler, id_: str) -> str | None:
    id_ = id_.upper()
    if not h.app.music.store.exists(id_):
        h.send_text(f"E No song {id_}\n", 404)
        return None
    return id_


def h_music_get(h: Handler, id_: str):
    """GET /music/<id>: the T3P1 stream the Tandy plays."""
    id_ = _music_id(h, id_)
    if id_:
        h.send_bytes(h.app.music.store.t3(id_))


def h_music_wav(h: Handler, id_: str):
    """GET /music/<id>/wav: the preview, for listening on a PC."""
    id_ = _music_id(h, id_)
    if id_:
        with open(h.app.music.store.path(id_, "wav"), "rb") as f:
            h.send_bytes(f.read(), "audio/wav")


def h_music_title(h: Handler, id_: str):
    id_ = _music_id(h, id_)
    if id_:
        h.app.music.store.set_title(id_, _text_body(h))
        h.send_text("OK\n")


def h_music_del(h: Handler, id_: str):
    h.app.music.store.delete(id_)
    h.send_text("OK\n")


def h_files(h: Handler, name: str):
    """GET /files/<NAME.EXT>: the latest DOS build from dos\\out (for UPDATE.BAT)."""
    folder = os.path.join(PROJECT_DIR, "dos", "out")
    path = os.path.join(folder, name.upper())
    if not os.path.isfile(path):
        h.send_text(f"E No file {name}\n", 404)
        return
    with open(path, "rb") as f:
        h.send_bytes(f.read())


ROUTES = {
    ("GET", "/ping"): h_ping,
    ("GET", "/test/bytes"): h_test_bytes,
    ("GET", "/test/stream"): h_test_stream,
    ("POST", "/test/echo"): h_test_echo,
    ("GET", "/test/image"): h_test_image,
    ("POST", "/gen"): h_gen,
    ("GET", "/list"): h_list,
    ("POST", "/chat"): h_chat,
    ("GET", "/chats"): h_chats,
    ("POST", "/enhance"): h_enhance,
    ("POST", "/music/gen"): h_music_gen,
    ("GET", "/music/list"): h_music_list,
}

_ID = r"([0-9A-Fa-f]{8})"
PATTERN_ROUTES = [
    ("GET", re.compile(r"/job/(\d+)"), h_job),
    ("GET", re.compile(rf"/img/{_ID}"), h_img),
    ("GET", re.compile(rf"/img/{_ID}/thumb"), h_thumb),
    ("POST", re.compile(rf"/img/{_ID}/title"), h_title),
    ("POST", re.compile(rf"/img/{_ID}/del"), h_delete),
    ("GET", re.compile(r"/files/([A-Za-z0-9_\-]{1,8}\.[A-Za-z0-9]{1,3})"), h_files),
    ("POST", re.compile(rf"/img/{_ID}/upload"), h_upload),
    ("GET", re.compile(rf"/chat/{_ID}"), h_chat_get),
    ("POST", re.compile(rf"/chat/{_ID}/del"), h_chat_del),
    ("POST", re.compile(rf"/chat/{_ID}/sync"), h_chat_sync),
    ("GET", re.compile(r"/music/job/(\d+)"), h_music_job),
    ("GET", re.compile(rf"/music/{_ID}"), h_music_get),
    ("GET", re.compile(rf"/music/{_ID}/wav"), h_music_wav),
    ("POST", re.compile(rf"/music/{_ID}/title"), h_music_title),
    ("POST", re.compile(rf"/music/{_ID}/del"), h_music_del),
    ("GET", re.compile(r"/prompt/(\w+)"), h_prompt_get),
    ("POST", re.compile(r"/prompt/(\w+)"), h_prompt_set),
]


# ---------------------------------------------------------------- server

class MindServer:
    def __init__(self, config: Config):
        self.config = config
        self.httpd: ThreadingHTTPServer | None = None
        self.thread: threading.Thread | None = None
        self.clients: dict[str, float] = {}
        self.store = Store()
        self.jobs = JobQueue(config, self.store)
        self.services = Services(config)
        self.chat = ChatEngine(config, self.store, self.jobs)
        self.music = MusicService(config, self.chat.qwen)
        self.chat.music = self.music

    def status(self) -> dict:
        return dict(self.services.status)

    def note_client(self, ip: str):
        self.clients[ip] = time.time()

    def sample_image(self) -> Image.Image:
        folder = os.path.join(HELPER_DIR, "samples")
        files = sorted(f for f in os.listdir(folder) if f.lower().endswith((".png", ".jpg", ".jpeg")))
        return Image.open(os.path.join(folder, files[0]))

    def start(self):
        Handler.app = self
        cfg = self.config["server"]
        self.httpd = ThreadingHTTPServer((cfg["host"], int(cfg["port"])), Handler)
        self.httpd.daemon_threads = True
        self.thread = threading.Thread(target=self.httpd.serve_forever, name="http", daemon=True)
        self.thread.start()
        log.info("MindServer %s listening on %s:%s", VERSION, cfg["host"], cfg["port"])

    def stop(self):
        if self.httpd:
            self.httpd.shutdown()
            self.httpd.server_close()
            self.httpd = None
