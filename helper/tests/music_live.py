"""Live check of MindServer's music endpoints (needs the music worker on port 8287; Qwen optional).

    .venv\\Scripts\\python -m tests.music_live [--worker http://127.0.0.1:8288] [--quick]

Starts a private MindServer on port 8290 with a temporary song store, then:
POST /music/gen (a JSON spec, and a description) -> GET /music/job/<n>?wait=1 -> M line,
GET /music/list, GET /music/<id> (a valid T3P1 stream), /title, /wav, /del.  An unknown style
falls back to a guessed one (a song is still made).
Revisions (unless --quick): two sketches -> keep A (B goes: M + R lines) -> revise (new lead, next
drums, key up) -> info -> undo -> two sketches like a song; the PC-speaker preview; an 'extended'
song (over 32,000 bytes: DeskMind streams it); a song from before revisions (drums yes, new lead no).
"""

import argparse
import glob
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


def gen(body: str, path: str = "/music/gen") -> list[str]:
    r = httpx.post(BASE + path, content=body.encode(), timeout=10)
    assert r.status_code == 200 and r.text.startswith("J "), r.text
    job = r.text.split()[1]
    with httpx.stream("GET", f"{BASE}/music/job/{job}?wait=1", timeout=600) as s:
        return [line for line in s.iter_lines() if line]


def ids(lines: list[str], code: str) -> list[str]:
    return [x.split()[1] for x in lines if x.startswith(code + " ")]


def listed() -> list[str]:
    return [x.split()[0] for x in httpx.get(BASE + "/music/list", timeout=5).text.strip().splitlines()]


def revisions(server):
    t0 = time.time()
    lines = gen(json.dumps({"style": "blues", "seed": 11}), "/music/sketch")
    print("sketches:", lines, f"{time.time() - t0:.1f} s")
    a, b = ids(lines, "M")
    ms = [x for x in lines if x.startswith("M ")]
    assert "Sketch A" in ms[0] and "Sketch B" in ms[1], ms
    ka = server.music.store.meta(a)["report"]["key"]
    assert ka == server.music.store.meta(b)["report"]["key"], "the two sketches are in different keys"
    assert set(listed()) >= {a, b}

    lines = gen("", f"/music/{a}/build")
    print("keep A:", lines)
    (full,) = ids(lines, "M")
    assert ids(lines, "R") == [a, b] and full in listed() and a not in listed() and b not in listed()
    assert not httpx.get(f"{BASE}/music/{b}", timeout=5).status_code == 200      # the other sketch is gone
    assert not any("Sketch" in x for x in lines if x.startswith("M "))

    lines = gen(json.dumps({"regenerate": ["lead"], "drum_pattern": "next", "key": "up"}), f"/music/{full}/revise")
    print("revise:", lines)
    (rev,) = ids(lines, "M")
    assert ids(lines, "R") == [full] and rev in listed() and full not in listed()
    info = dict(x.split(" ", 1) for x in httpx.get(f"{BASE}/music/{rev}/info", timeout=5).text.strip().splitlines())
    print("info:", info)
    assert info["rev"] == "3" and info["parent"] == full and info["project"] == a and "lead" in info["change"]
    assert info["revisable"] == "full"

    lines = gen("", f"/music/{rev}/undo")
    print("undo:", lines)
    assert ids(lines, "M") == [full] and ids(lines, "R") == [rev] and full in listed() and rev not in listed()
    assert httpx.post(f"{BASE}/music/{full}/revise", content=b"{}", timeout=5).status_code == 400   # nothing to change

    lines = gen(json.dumps({"like": full}), "/music/sketch")
    print("try two like it:", lines)
    assert len(ids(lines, "M")) == 2

    spk = httpx.get(f"{BASE}/music/{full}/wav?speaker=1", timeout=60).content
    assert spk[:4] == b"RIFF" and len(spk) > 100000, len(spk)
    print("speaker preview:", len(spk), "bytes")

    lines = gen(json.dumps({"style": "cracktro", "length": "extended", "seed": 3}))
    (ext,) = ids(lines, "M")
    data = httpx.get(f"{BASE}/music/{ext}", timeout=30).content
    rep = t3.validate(data, None)
    print("extended:", rep["bytes"], "bytes", rep["seconds"], "s")
    assert rep["bytes"] > 32000 and rep["seconds"] > 150

    # A song from before revisions (copied from the real store): no arrangement kept
    src = sorted(glob.glob(os.path.join(HERE, "helper", "data", "music", "*.json")))
    if src:
        sid = os.path.basename(src[0])[:8]
        m = json.load(open(src[0], encoding="utf-8"))
        with open(os.path.join(HERE, "helper", "data", "music", sid + ".mid"), "rb") as f:
            midi = f.read()
        with open(os.path.join(HERE, "helper", "data", "music", sid + ".t3"), "rb") as f:
            t3b = f.read()
        old = server.music.store.add(t3b, midi, b"RIFF", {k: m[k] for k in ("title", "request", "spec", "report") if k in m})
        lines = gen(json.dumps({"drums": "none", "key": "down"}), f"/music/{old}/revise")
        print("v1 song, drums + key:", lines)
        assert ids(lines, "M") and ids(lines, "R") == [old]
        new_old = ids(lines, "M")[0]
        lines = gen(json.dumps({"regenerate": ["lead"]}), f"/music/{new_old}/revise")
        print("v1 song, new lead:", lines)
        assert any(x.startswith("E ") for x in lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--worker", default="http://127.0.0.1:8287")
    ap.add_argument("--quick", action="store_true", help="only the original checks")
    args = ap.parse_args()
    with tempfile.TemporaryDirectory() as d:
        cfg = Config(os.path.join(d, "settings.json"))
        cfg.data["server"]["port"] = 8290
        cfg.data["music"]["url"] = args.worker
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
            if not args.quick:
                revisions(server)
            print("MUSIC LIVE OK")
        finally:
            server.stop()


if __name__ == "__main__":
    main()
