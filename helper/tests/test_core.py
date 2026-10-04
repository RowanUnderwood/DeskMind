"""Core checks without ComfyUI or NInfer:  .venv\\Scripts\\python -m tests.test_core"""

import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from PIL import Image  # noqa: E402

from mindserver import dither as D  # noqa: E402
from mindserver import tpi  # noqa: E402
from mindserver.store import Store, file_slug, make_title  # noqa: E402


def sample() -> Image.Image:
    folder = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "samples")
    return Image.open(os.path.join(folder, sorted(os.listdir(folder))[0]))


def test_tpi_roundtrip():
    img = sample()
    for mode in ("640", "320"):
        s = D.DitherSettings(engine="pillow", mode=mode)
        idx, th = D.convert(img, s), D.thumbnail(img, s)
        data = tpi.build("1A2B3C4D", mode, idx, th, "A title é", "a prompt ░ with  spaces", 123456789)
        info = tpi.parse(data)
        w, h = D.MODES[mode]
        tw, tht = D.THUMB[mode]
        assert (info.width, info.height, info.thumb_w, info.thumb_h) == (w, h, tw, tht)
        assert info.id == "1A2B3C4D" and info.mode == mode and info.seed == 123456789
        assert info.title == "A title é" and info.prompt == "a prompt ░ with spaces"
        assert (D.unpack(info.image, w, h) == idx).all()
        assert (D.unpack(info.thumb, tw, tht) == th).all()
        assert len(data) == 128 + len(info.prompt) + w * h // 2 + tw * tht // 2
        print(f"tpi {mode}: {len(data)} bytes ok")


def test_store():
    with tempfile.TemporaryDirectory() as tmp:
        st = Store(tmp)
        s = D.DitherSettings(engine="pillow")
        meta = st.add(sample(), "A friendly retro computer logo for a 286 AI with a glowing brain", s, seed=42)
        id_ = meta["id"]
        assert len(id_) == 8 and st.exists(id_)
        assert tpi.parse(st.tpi_bytes(id_)).title == meta["title"]
        st.rename(id_, "My logo")
        assert tpi.parse(st.tpi_bytes(id_)).title == "My logo"
        s320 = D.DitherSettings(engine="pillow", mode="320")
        assert tpi.parse(st.tpi_bytes(id_, s320)).mode == "320"
        assert [m["id"] for m in st.list()] == [id_]
        st.delete(id_)
        assert not st.exists(id_) and st.list() == []
        print("store ok")


def test_title():
    assert make_title("short") == "short"
    t = make_title("A very long prompt about a beige Tandy 1000 computer with a glowing brain on the screen")
    assert len(t) <= 39 and not t.endswith(" ")
    print("title ok:", t)
    assert file_slug('Dungeon Wraiths: "3D" é!') == "dungeon-wraiths-3d"
    assert file_slug("***") == "picture"


def test_screen_shot():
    for mode in ("640", "320"):
        img = D.screen_shot(D.convert(sample(), D.DitherSettings(engine="pillow", mode=mode)))
        assert img.size == (1280, 960) and img.mode == "RGB"
    print("screen shot ok")


def _stream(pieces):
    from mindserver.text import DrawSplitter, TandyText
    ds, tt, lines = DrawSplitter(), TandyText(), []
    for p in pieces:
        lines += tt.feed(ds.feed(p))
    lines += tt.feed(ds.finish()) + tt.flush()
    text = "".join(l[2:] if l.startswith("T ") else "\n" for l in lines)
    return text, ds.prompts, lines


def test_text_stream():
    reply = ("Sure! **Here** is a `logo` — it’s great \U0001F600.\n\n## Notes\n- one\n- two\n"
             "I'll draw it now. <draw>A beige Tandy 1000 with a glowing brain, bold shapes</draw> Enjoy!")
    for size in (1, 2, 3, 5, 7, 64):                   # every chunking must give the same result
        pieces = [reply[i:i + size] for i in range(0, len(reply), size)]
        text, prompts, lines = _stream(pieces)
        assert prompts == ["A beige Tandy 1000 with a glowing brain, bold shapes"], (size, prompts)
        assert "**" not in text and "`" not in text and "<draw>" not in text and "##" not in text, (size, text)
        assert "Here is a logo - it's great :D." in text, (size, text)
        assert "■ one" in text and "Enjoy!" in text, (size, text)
        text.encode("cp437")
    print("text stream ok:", repr(text))


def test_old_draw_note():
    # Qwen once copied the old history note instead of writing <draw>: it must still draw, and not show
    reply = ("Let's go back to painterly:\n[You drew picture A4B2C3D1: A plump old witch, warm candle light]"
             " Enjoy! [not a picture] <DRAW>second</draw>")
    for size in (1, 2, 3, 5, 7, 64):
        pieces = [reply[i:i + size] for i in range(0, len(reply), size)]
        text, prompts, _ = _stream(pieces)
        assert prompts == ["A plump old witch, warm candle light", "second"], (size, prompts)
        assert "You drew" not in text and "A4B2" not in text, (size, text)
        assert "Let's go back to painterly:" in text and "Enjoy! [not a picture]" in text, (size, text)
    text, prompts, _ = _stream(["Here: [You drew picture 1234ABCD: unclosed at the end"])
    assert prompts == ["unclosed at the end"] and "You drew" not in text, (prompts, text)
    print("old draw note ok")


def test_history():
    from mindserver.chat import ChatEngine

    class FakeStore:
        def exists(self, i):
            return i == "AABBCCDD"

        def meta(self, i):
            return {"prompt": "the stored prompt"}

    eng = ChatEngine.__new__(ChatEngine)              # only _history and its helpers are used
    eng.store = FakeStore()
    chat = {"messages": [
        {"role": "user", "text": "draw a cat"},
        {"role": "assistant", "text": "Here!", "drawn": [{"id": "AABBCCDD", "title": "A cat"}]},
        {"role": "user", "text": "again"},
        {"role": "assistant", "text": "Sure:\n[You drew picture A4B2C3D1: a fake one]", "drawn": []},
        {"role": "assistant", "text": "Done", "drawn": [{"id": "11111111", "title": "T", "prompt": "saved"}]}]}
    h = eng._history(chat)
    assert h[1]["content"] == "Here!\n<draw>the stored prompt</draw>", h[1]
    assert h[3]["content"] == "Sure:", h[3]
    assert h[4]["content"] == "Done\n<draw>saved</draw>", h[4]
    assert not any("You drew" in m["content"] for m in h)
    print("history ok")


def test_transcript():
    from mindserver.chat import ChatStore
    chat = {"id": "0A0B0C0D", "title": "t", "created": "2026-09-29T10:00:00", "updated": "2026-09-29T10:00:00",
            "messages": [{"role": "user", "text": "Draw me a cat", "image": "11223344"},
                         {"role": "assistant", "text": "Here you go!\nA second line.",
                          "drawn": [{"id": "AABBCCDD", "title": "A cat"}]}]}
    t = ChatStore.to_transcript(chat)
    back = ChatStore.from_transcript("0a0b0c0d", t)
    assert back["id"] == "0A0B0C0D"
    assert [m["role"] for m in back["messages"]] == ["user", "assistant"]
    assert back["messages"][0]["image"] == "11223344" and back["messages"][0]["text"] == "Draw me a cat"
    assert back["messages"][1]["text"] == "Here you go!\nA second line."
    assert back["messages"][1]["drawn"] == [{"id": "AABBCCDD", "title": "A cat"}]
    print("transcript ok")


if __name__ == "__main__":
    test_tpi_roundtrip()
    test_store()
    test_title()
    test_screen_shot()
    test_text_stream()
    test_old_draw_note()
    test_history()
    test_transcript()
    print("ALL OK")
