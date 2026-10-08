"""Phase 0: generate one image with the trimmed Krea2 workflow and dither it.

    .venv\\Scripts\\python comfytest.py ["prompt"]

Needs ComfyUI running (run_comfy_image.bat, 4090, port 8188).  Saves the
original to samples\\ (so the server's /test/image uses it) and a dither
contact sheet to out\\.
"""

import os
import sys
import time

from mindserver.comfy import ComfyClient
from mindserver.config import Config

import dithertest

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    prompt = sys.argv[1] if len(sys.argv) > 1 else (
        "A friendly retro computer logo for '286 AI': a beige Tandy 1000 computer with a glowing "
        "brain on its screen, bold shapes, strong contrast, simple dark blue background, 1980s box art")
    cfg = Config()
    c = ComfyClient(cfg["comfy"]["url"], cfg["comfy"]["workflow"])
    if not c.alive():
        print("ComfyUI is not answering on", cfg["comfy"]["url"], "- start run_comfy_image.bat first")
        return 1
    p = cfg.comfy_profile()
    wf, seed = c.build(prompt, steps=p["steps"], shortside=p["shortside"],
                       lora_on=p["lora_on"], lora_strength=p["lora_strength"])
    print(f"seed {seed}: {prompt}")
    t0 = time.time()
    img = c.run(wf, progress=lambda f, t: print(f"  {t}" + (f" ({f:.0%})" if f is not None else "")))
    print(f"generated {img.size} in {time.time() - t0:.1f}s")
    os.makedirs(os.path.join(HERE, "samples"), exist_ok=True)
    path = os.path.join(HERE, "samples", f"comfy_{seed}.png")
    img.save(path)
    dithertest.sheet(path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
