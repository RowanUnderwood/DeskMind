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


def test_pack_bits():
    import numpy as np
    rng = np.random.default_rng(1)
    for bits, w in ((4, 640), (2, 320), (1, 640)):
        idx = rng.integers(0, 1 << bits, (200, w), dtype=np.uint8)
        data = D.pack(idx, bits)
        assert len(data) == w * 200 * bits // 8
        assert (D.unpack(data, w, 200, bits) == idx).all()
    # Leftmost pixel in the high bits, as the CGA expects
    assert D.pack(np.array([[1, 2, 3, 0]], dtype=np.uint8), 2) == bytes([0b01101100])
    assert D.pack(np.array([[1, 0, 0, 0, 0, 0, 0, 1]], dtype=np.uint8), 1) == bytes([0x81])
    print("pack bits ok")


def test_cga_choose():
    s = D.DitherSettings(engine="pillow", mode="cga")
    # Flat cyan/magenta/white bands on black: palette 1 bright (11/13/15), background black
    bands = Image.new("RGB", (400, 300))
    for i, c in enumerate([(0, 0, 0), (85, 255, 255), (255, 85, 255), (255, 255, 255)]):
        bands.paste(c, (i * 100, 0, i * 100 + 100, 300))
    layout, pal, bg, _ = D.cga_choose(bands, s)
    assert (layout, pal, bg) == ("cga4", 3, 0), (layout, pal, bg)
    # Green/red/yellow on blue: palette 0 bright with a blue background
    bands2 = Image.new("RGB", (400, 300))
    for i, c in enumerate([(0, 0, 170), (85, 255, 85), (255, 85, 85), (255, 255, 85)]):
        bands2.paste(c, (i * 100, 0, i * 100 + 100, 300))
    assert D.cga_choose(bands2, s)[:3] == ("cga4", 1, 1)
    # A grey gradient: black and white wins
    grad = Image.linear_gradient("L").resize((400, 300)).convert("RGB")
    assert D.cga_choose(grad, s)[0] == "cga2"
    # Overrides
    assert D.cga_choose(bands, D.DitherSettings(mode="cga", cga_choice="mono"))[0] == "cga2"
    assert D.cga_choose(grad, D.DitherSettings(mode="cga", cga_choice="2: x", cga_bg=1))[:3] == ("cga4", 2, 1)
    assert D.cga_choose(bands, D.DitherSettings(mode="cga", cga_mode5=False))[1] < 4
    print("cga choose ok")


def test_tpi_cga():
    img = sample()
    for choice, layout in (("auto", None), ("1: x", "cga4"), ("mono", "cga2")):
        s = D.DitherSettings(engine="pillow", mode="cga", cga_choice=choice)
        d, th = D.render(img, s), D.thumbnail(img, s)
        assert th.shape == (50, 160) and th.max() <= 1
        assert d.layout == (layout or d.layout)
        assert d.idx.shape == ((200, 320) if d.layout == "cga4" else (200, 640))
        assert d.idx.max() < len(d.rgb)
        data = tpi.build("CAFE0001", d.layout, d.idx, th, "CGA", "a prompt", 7, None, d.cga_pal, d.cga_color)
        info = tpi.parse(data)
        assert info.mode == "cga" and info.layout == d.layout
        assert (info.cga_pal, info.cga_color) == (d.cga_pal, d.cga_color)
        assert (info.image_idx() == d.idx).all() and (info.thumb_idx() == th).all()
        assert info.rgb(s) == d.rgb
        assert len(info.thumb) == 1000 and len(info.image) == 16000          # both CGA layouts
        print(f"tpi cga {choice}: {d.describe()}")


def test_prompts_cga():
    from mindserver.chat import PROMPTS, load_prompt
    for name in ("chat", "enhance", "vision"):
        tandy, cga = load_prompt(name), load_prompt(name, "cga")
        assert tandy != cga and "CGA" in cga and "CGA" not in tandy.split("Tandy Video II")[0]
        assert load_prompt(name, "640") == tandy
    assert "{date}" in load_prompt("chat", "cga") and "{image_id}" in load_prompt("vision", "cga")
    assert all(k in PROMPTS for k in ("chat_cga", "enhance_cga", "vision_cga"))
    print("cga prompts ok")


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
        scga = D.DitherSettings(engine="pillow", mode="cga")
        assert tpi.parse(st.tpi_bytes(id_, scga)).mode == "cga"
        assert os.path.exists(st.tpi_path(id_, "cga"))
        st.rename(id_, "CGA logo")
        assert tpi.parse(st.tpi_bytes(id_, scga)).title == "CGA logo"
        assert tpi.parse(st.tpi_bytes(id_)).mode == "640"          # its own mode is untouched
        assert [m["id"] for m in st.list()] == [id_]
        st.delete(id_)
        assert not st.exists(id_) and st.list() == []
        assert not os.listdir(tmp), os.listdir(tmp)
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


def test_music_splitter():
    from mindserver.text import DrawSplitter
    sp = DrawSplitter()
    vis = "".join(sp.feed(x) for x in ["Here is a tune! <mu", "sic>a fast cracktro", " tune</mus", "ic> and <draw>a cat</draw>."])
    vis += sp.finish()
    assert sp.music == ["a fast cracktro tune"] and sp.prompts == ["a cat"], (sp.music, sp.prompts)
    assert vis == "Here is a tune!  and .", repr(vis)
    print("music splitter ok")


def test_music_transcript():
    from mindserver.chat import ChatStore
    chat = {"id": "0A0B0C0D", "title": "t", "created": "2026-10-09T10:00:00", "updated": "2026-10-09T10:00:00",
            "messages": [{"role": "user", "text": "Make a song"},
                         {"role": "assistant", "text": "Coming up.", "music": [{"id": "12345678", "title": "Raster Rush"}],
                          "drawn": [{"id": "AABBCCDD", "title": "A cat"}]}]}
    back = ChatStore.from_transcript("0A0B0C0D", ChatStore.to_transcript(chat))
    assert back["messages"][1]["music"] == [{"id": "12345678", "title": "Raster Rush"}]
    assert back["messages"][1]["drawn"] == [{"id": "AABBCCDD", "title": "A cat"}]
    print("music transcript ok")


def test_music_spec_and_store():
    from mindserver.music import MusicStore, clean_spec, guess_style
    s = clean_spec({"style": "Dungeon", "key": "d minor", "tempo": "500", "energy": "LOW", "drums": "lots",
                    "length": "long", "title": "The \"Crypt\"", "extra": "dropped", "seed": "7"})
    assert s["style"] == "dungeon" and s["key"] == "D" and s["tempo"] == 180 and s["energy"] == "low"
    assert "drums" not in s and s["length"] == "long" and "extra" not in s and s["seed"] == 7, s
    assert clean_spec({"key": "Bb"})["key"] == "Bb" and clean_spec({"key": "f#"})["key"] == "F#"
    assert clean_spec({}, "a spooky cave theme")["style"] == "dungeon"
    assert guess_style("a happy village song") == "adventure" and guess_style("make it fast") == "cracktro"
    with tempfile.TemporaryDirectory() as d:
        st = MusicStore(d)
        a = st.add(b"T3P1aaaa", b"MThd", b"RIFF", {"title": "One", "report": {"seconds": 61.4}})
        b = st.add(b"T3P1bbbb", b"MThd", b"RIFF", {"title": "Two"})
        assert st.exists(a) and st.t3(a) == b"T3P1aaaa" and len(st.list()) == 2
        st.set_title(a, "Renamed song")
        assert st.meta(a)["title"] == "Renamed song"
        st.delete(b)
        assert not st.exists(b) and [m["id"] for m in st.list()] == [a]
    print("music spec and store ok")


def test_workflows():
    from mindserver.comfy import ComfyClient, list_workflows
    ok, bad = list_workflows()
    assert "workflows/krea2_tandy.json" in ok and "workflows/krea2_fine_v5.json" in ok, (ok, bad)
    for name, lora in (("workflows/krea2_tandy.json", True), ("workflows/krea2_fine_v5.json", False)):
        c = ComfyClient(workflow=name)
        assert c.has_lora() == lora
        wf, seed = c.build("a red cube", seed=1234, steps=11, shortside=768, lora_on=False, lora_strength=0.5)
        r = c.roles
        assert seed == 1234
        assert wf[r["prompt"]]["inputs"]["text"] == "a red cube"
        assert wf[r["sampler"]]["inputs"]["seed"] == 1234 and wf[r["sampler"]]["inputs"]["steps"] == 11
        assert wf[r["save"]]["inputs"]["filename_prefix"] == "DeskMind/dm"
        size = wf[r["size"]]["inputs"]
        if "megapixels" in size:
            assert size["aspect_ratio"] == "4:3 (Standard)" and abs(size["megapixels"] - 0.786) < 0.001
        else:
            assert size["aspect"] == "4:3" and size["direction"] == "landscape" and size["shortside"] == 768
        if lora:
            assert wf[r["lora"]]["inputs"]["lora_1"] == {"on": False, "lora": "KNPV3_1.safetensors", "strength": 0.5}
        assert c.template[r["prompt"]]["inputs"]["text"] != "a red cube"     # template untouched
    assert ComfyClient(workflow="workflows/krea2_tandy.json").roles["prompt"] == "6"   # through the Rebalance node
    assert ComfyClient(workflow="workflows/krea2_fine_v5.json").roles["prompt"] == "104"
    print("workflows ok")


def test_profiles():
    import json
    from mindserver.config import Config
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "settings.json")
        with open(path, "w", encoding="utf-8") as f:     # settings from before profiles
            json.dump({"comfy": {"workflow": "workflows/krea2_tandy.json", "steps": 9, "shortside": 640,
                                 "lora_on": False, "lora_strength": 0.7}}, f)
        cfg = Config(path)
        assert cfg.comfy_profile() == {"steps": 9, "shortside": 640, "lora_on": False, "lora_strength": 0.7}
        fine = cfg.comfy_profile("workflows/krea2_fine_v5.json")
        assert fine["steps"] == 12 and fine["shortside"] == 640
        cfg.set_comfy_profile("workflows/krea2_fine_v5.json", steps=10, bogus=1)
        cfg.data["comfy"]["workflow"] = "workflows/krea2_fine_v5.json"
        cfg.save()
        cfg2 = Config(path)
        assert cfg2.comfy_profile()["steps"] == 10 and "bogus" not in cfg2["comfy"]["profiles"]["workflows/krea2_fine_v5.json"]
        assert cfg2.comfy_profile("workflows/krea2_tandy.json")["steps"] == 9
    print("profiles ok")


if __name__ == "__main__":
    test_tpi_roundtrip()
    test_pack_bits()
    test_cga_choose()
    test_tpi_cga()
    test_prompts_cga()
    test_store()
    test_title()
    test_screen_shot()
    test_text_stream()
    test_old_draw_note()
    test_history()
    test_transcript()
    test_music_splitter()
    test_music_transcript()
    test_music_spec_and_store()
    test_workflows()
    test_profiles()
    print("ALL OK")
