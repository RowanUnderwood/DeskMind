"""Short live check of DeskMind's Qwen features through MindServer's HTTP API (what the Tandy calls).

    .venv\\Scripts\\python -m tests.qwen_smoke [--url http://127.0.0.1:8286] [--keep]

Needs MindServer, NInfer and ComfyUI running.  Runs chat, chat memory, Markdown cleanup, <draw> in chat,
a vision follow-up on the drawn picture, vision on an attached gallery picture and the prompt enhancer.
Prints a summary and writes out\\qwen_smoke_<model>.json (model from NInfer's current-model.txt), so runs
against thinkingcap and full can be compared.  Test chats and drawn pictures are deleted unless --keep.
"""

import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import httpx  # noqa: E402

from mindserver.config import HELPER_DIR, Config  # noqa: E402
from mindserver.services import ninfer_running  # noqa: E402

TIMEOUT = httpx.Timeout(connect=5, read=300, write=30, pool=5)


def post_stream(url: str, path: str, body: str, params: dict | None = None) -> tuple[list[str], float]:
    t0 = time.time()
    lines = []
    with httpx.stream("POST", url + path, params=params, content=body.encode("cp437"), timeout=TIMEOUT) as r:
        for line in r.iter_lines():
            lines.append(line)
    return lines, time.time() - t0


def reply_text(lines: list[str]) -> str:
    out = []
    for ln in lines:
        if ln.startswith("T "):
            out.append(ln[2:])
        elif ln == "N":
            out.append("\n")
    return "".join(out)


def first(lines: list[str], tag: str) -> str | None:
    return next((ln[len(tag) + 1:] for ln in lines if ln.startswith(tag + " ")), None)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default="http://127.0.0.1:8286")
    ap.add_argument("--keep", action="store_true", help="keep the test chats and drawn pictures")
    args = ap.parse_args()
    url = args.url.rstrip("/")
    model, ctx = ninfer_running(Config())
    results, chats, images = [], [], []

    def case(name: str, lines: list[str], secs: float, ok: bool, why: str = ""):
        err = first(lines, "E")
        if err:
            ok, why = False, f"E {err}"
        bad = [ln for ln in lines if "<draw" in ln or "</draw" in ln or "<think" in ln]
        if bad:
            ok, why = False, f"tag leaked: {bad[0][:60]}"
        text = reply_text(lines)
        results.append({"case": name, "ok": ok, "why": why, "secs": round(secs, 1), "reply": text, "lines": lines})
        print(f"{'PASS' if ok else 'FAIL'}  {name:<16} {secs:5.1f} s  {why}")
        print("      " + text.replace("\n", " / ")[:300])
        c = first(lines, "C")
        if c and c not in chats:
            chats.append(c)
        i = first(lines, "I")
        if i:
            images.append(i.split()[0])

    print(f"NInfer model: {model or '?'}, context {ctx or '?'}")
    print(httpx.get(url + "/ping", timeout=5).text.strip().replace("\n", " | "))

    lines, s = post_stream(url, "/chat", "In two sentences: what was the Tandy 1000 TL/3?")
    t = reply_text(lines)
    case("chat", lines, s, bool(t.strip()) and "Tandy" in t, "" if "Tandy" in t else "no 'Tandy' in reply")

    lines, s = post_stream(url, "/chat", "About this computer, one short line each: how much memory does it have, "
                           "what year is it from, is it a laptop, and what is its floppy drive?")
    t = reply_text(lines).lower()
    miss = [w for w in ("640", "1991", "720") if w not in t]
    wrong = [w for w in ("1 mb", "1mb", "1989", "1.44", "5.25") if w in t]
    lap = "laptop" in t and not any(n in t for n in ("not a laptop", "no,", "desktop"))
    case("tandy facts", lines, s, not miss and not wrong and not lap,
         ", ".join(filter(None, [f"missing {miss}" if miss else "", f"wrong {wrong}" if wrong else "",
                                 "says laptop" if lap else ""])))

    lines, s = post_stream(url, "/chat", "What is the capital of France? One sentence.")
    t = reply_text(lines)
    spec = [w for w in ("80286", "640 KB", "MHz", "CM-5", "PicoMEM") if w in t]
    case("no spec recital", lines, s, "Paris" in t and not spec, f"mentions {spec}" if spec else "")

    lines, s = post_stream(url, "/chat", "My cat is called Pixel. Reply with just OK.")
    cid = first(lines, "C")
    case("memory: tell", lines, s, bool(cid))
    lines, s = post_stream(url, "/chat", "What is my cat called? One word.", {"id": cid})
    case("memory: recall", lines, s, "pixel" in reply_text(lines).lower(), "")

    lines, s = post_stream(url, "/chat", "List three 1980s home computers as a bulleted list, with one bold word.")
    t = reply_text(lines)
    md = [m for m in ("**", "#", "* ", "- ") if any(ln.lstrip().startswith(m) for ln in t.split("\n"))]
    md += ["**"] if "**" in t and "**" not in md else []
    case("markdown", lines, s, "■" in t and not md, f"markdown left: {md}" if md else
         ("" if "■" in t else "no CP437 bullets"))

    lines, s = post_stream(url, "/chat", "Draw me a lighthouse on a cliff in a storm.")
    did = first(lines, "C")
    img = first(lines, "I")
    case("draw in chat", lines, s, bool(img), "" if img else "no picture (I line)")
    if img:
        lines, s = post_stream(url, "/chat", "What colours are strongest in that picture? One sentence.", {"id": did})
        case("vision follow-up", lines, s, bool(reply_text(lines).strip()))

    pics = httpx.get(url + "/list", timeout=10).text.splitlines()
    pic = next((p for p in pics if p.split()[0] not in images), None)
    if pic:
        pid, title = pic.split()[0], " ".join(pic.split()[3:])
        lines, s = post_stream(url, "/chat", "Describe this picture in one sentence.", {"img": pid})
        case("vision attached", lines, s, bool(reply_text(lines).strip()), f"picture {pid} '{title}'")

    lines, s = post_stream(url, "/enhance", "a cat using an old computer")
    t = first(lines, "T") or ""
    case("enhance", lines, s, len(t) > 40, "" if len(t) > 40 else "prompt too short")

    if not args.keep:
        for c in chats:
            httpx.post(f"{url}/chat/{c}/del", timeout=10)
        for i in images:
            httpx.post(f"{url}/img/{i}/del", timeout=10)

    out = os.path.join(HELPER_DIR, "out", f"qwen_smoke_{model or 'unknown'}.json")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", encoding="utf-8") as f:
        json.dump({"model": model, "context": ctx, "when": time.strftime("%Y-%m-%d %H:%M"),
                   "results": results}, f, indent=2, ensure_ascii=False)
    failed = [r["case"] for r in results if not r["ok"]]
    print(f"{len(results) - len(failed)}/{len(results)} passed" + (f"; failed: {', '.join(failed)}" if failed else ""))
    print("details:", out)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
