"""Live check of MindServer's music endpoints (needs the music worker on port 8287; Qwen optional).

    .venv\\Scripts\\python -m tests.music_live

Starts a private MindServer on port 8290 with a temporary song store, then:
POST /music/gen (a JSON spec, and a description) -> GET /music/job/<n>?wait=1 -> M line,
GET /music/list, GET /music/<id> (a valid T3P1 stream), /title, /wav, /del.  An unknown style
falls back to a guessed one (a song is still made).
"""

import json
import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HERE = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(HERE, "Labtext2midi", "worker"))

import httpx  # noqa: E402
import t3  # noqa: E402

from mindserver.config import Config  # noqa: E402
from mindserver.music import MusicStore  # noqa: E402
from mindserver.server import MindServer  # noqa: E402

BASE = "http://127.0.0.1:8290"


def gen(body: str) -> list[str]:
    r = httpx.post(BASE + "/music/gen", content=body.encode(), timeout=10)
    assert r.status_code == 200 and r.text.startswith("J "), r.text
    job = r.text.split()[1]
    with httpx.stream("GET", f"{BASE}/music/job/{job}?wait=1", timeout=300) as s:
        return [line for line in s.iter_lines() if line]


def main():
    with tempfile.TemporaryDirectory() as d:
        cfg = Config(os.path.join(d, "settings.json"))
        cfg.data["server"]["port"] = 8290
        server = MindServer(cfg)
        server.music.store = MusicStore(os.path.join(d, "music"))
        server.start()
        time.sleep(0.5)
        try:
            ping = httpx.get(BASE + "/ping", timeout=5).text
            assert "music" in ping, ping
            t0 = time.time()
            lines = gen(json.dumps({"style": "adventure", "key": "G", "length": "short", "title": "Live Test Tune", "seed": 5}))
            print("spec job:", lines, f"{time.time() - t0:.1f} s")
            m = [x for x in lines if x.startswith("M ")]
            assert m and lines[-1] == "D", lines
            mid = m[0].split()[1]
            lines = gen("a slow spooky melody for the cave pictures")
            print("description job:", lines)
            assert any(x.startswith("M ") for x in lines), lines
            lst = httpx.get(BASE + "/music/list", timeout=5).text.strip().splitlines()
            print("list:", lst)
            assert len(lst) == 2 and all(len(x.split()[1]) == 12 for x in lst)
            data = httpx.get(f"{BASE}/music/{mid}", timeout=5).content
            rep = t3.validate(data)
            print("t3:", rep)
            assert httpx.get(f"{BASE}/music/{mid}/wav", timeout=5).content[:4] == b"RIFF"
            assert httpx.post(f"{BASE}/music/{mid}/title", content=b"Renamed", timeout=5).text.strip() == "OK"
            assert "Renamed" in httpx.get(BASE + "/music/list", timeout=5).text
            bad = gen(json.dumps({"style": "polka"}))
            print("bad style:", bad)
            assert httpx.post(f"{BASE}/music/{mid}/del", timeout=5).text.strip() == "OK"
            assert httpx.get(f"{BASE}/music/{mid}", timeout=5).status_code == 404
            print("MUSIC LIVE OK")
        finally:
            server.stop()


if __name__ == "__main__":
    main()
