"""A MindServer for testing DeskMind's chat songs without NInfer: Qwen's answer is canned.

    .venv\\Scripts\\python -m tests.music_fake_qwen [seconds] [port]

Every chat reply is "Here is a fast cracktro tune for you! <music>a fast cracktro tune</music>",
so the reply stream carries a real song from the music worker (it must run on port 8287):
S composing ... M <id> <title> ... D.  Chats and songs go to a temporary folder; ComfyUI and
the real data are not touched.  Default 600 s on port 8286 (what the 86Box VM uses as 10.0.2.2).
"""

import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from mindserver.chat import ChatStore  # noqa: E402
from mindserver.config import Config  # noqa: E402
from mindserver.music import MusicStore  # noqa: E402
from mindserver.server import MindServer  # noqa: E402

REPLY = "Here is a fast cracktro tune for you! <music>a fast cracktro tune with full drums</music>"


def fake_stream(msgs, effort=None, max_tokens=None):
    if any("ONE JSON object" in str(m.get("content", "")) for m in msgs):     # the spec request
        yield "content", '{"style": "cracktro", "energy": "high", "drums": "full", "length": "short", "title": "Fake Qwen Rush"}'
    else:
        for k in range(0, len(REPLY), 7):
            yield "content", REPLY[k:k + 7]
    yield "done", "stop"


def main():
    secs = int(sys.argv[1]) if len(sys.argv) > 1 else 600
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 8286
    with tempfile.TemporaryDirectory() as d:
        cfg = Config(os.path.join(d, "settings.json"))
        cfg.data["server"]["port"] = port
        server = MindServer(cfg)
        server.chat.chats = ChatStore(os.path.join(d, "chats"))
        server.chat.qwen.stream = fake_stream
        server.music.store = MusicStore(os.path.join(d, "music"))
        server.start()
        print(f"fake-Qwen MindServer on port {port} for {secs} s", flush=True)
        time.sleep(secs)
        for m in server.music.store.list():
            print("song made:", m["id"], m.get("title"), flush=True)
        server.stop()


if __name__ == "__main__":
    main()
