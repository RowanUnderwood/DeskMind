"""ComfyUI client: run a Krea2 API workflow from workflows/ and fetch the image.

Any ComfyUI *API-format* workflow in workflows/*.json can be used (the
Generation tab picks one).  Node IDs differ between workflows, so the nodes
patched per request are found by role:

    sampler  the KSampler                      seed, steps
    prompt   first CLIPTextEncode upstream of  text
             the sampler's positive input
    size     Empty Latent by Ratio (WLSH)      4:3 landscape, shortside
             or ResolutionSelector            4:3, megapixels from shortside
             or EmptyLatentImage              4:3 width/height from shortside
    lora     Power Lora Loader (rgthree)       lora_1 on/strength (optional)
    save     SaveImage                         filename_prefix DeskMind/dm

workflows/krea2_tandy.json is the user's Krea2 Turbo workflow with the
SeedVR2 upscaler, VRAM-cleanup and comparer nodes removed;
workflows/krea2_fine_v5.json is their Krea 2 fine V5 workflow (UI version in
workflows/ui/ for editing in ComfyUI).
"""

from __future__ import annotations

import copy
import glob
import io
import json
import os
import random
import time
import uuid

import httpx
from PIL import Image

from .config import HELPER_DIR


class ComfyError(RuntimeError):
    pass


SAVE_PREFIX = "DeskMind/dm"
SIZE_NODES = ("Empty Latent by Ratio (WLSH)", "ResolutionSelector", "EmptyLatentImage")


def find_roles(wf: dict) -> dict:
    """Map role -> node id for an API-format workflow; raises ComfyError if a needed node is missing."""
    if not isinstance(wf, dict) or not wf or not all(isinstance(n, dict) and "class_type" in n for n in wf.values()):
        raise ComfyError("not an API-format workflow (export it with 'Export (API)')")

    def of(cls):
        return [k for k, n in wf.items() if n["class_type"] == cls]

    roles = {}
    samplers = of("KSampler")
    if len(samplers) != 1:
        raise ComfyError(f"needs exactly one KSampler (found {len(samplers)})")
    roles["sampler"] = samplers[0]
    # Walk back from the positive input (through e.g. the Krea2 Rebalance node) to the text encoder.
    link, seen = wf[samplers[0]]["inputs"].get("positive"), set()
    while isinstance(link, list) and link and link[0] in wf and link[0] not in seen:
        nid = link[0]
        seen.add(nid)
        node = wf[nid]
        if node["class_type"] == "CLIPTextEncode":
            roles["prompt"] = nid
            break
        link = node["inputs"].get("conditioning")
    if "prompt" not in roles:
        raise ComfyError("no CLIPTextEncode found upstream of the KSampler's positive input")
    for cls in SIZE_NODES:
        if of(cls):
            roles["size"] = of(cls)[0]
            break
    else:
        raise ComfyError("no size node (" + ", ".join(SIZE_NODES) + ")")
    saves = of("SaveImage")
    if not saves:
        raise ComfyError("no SaveImage node")
    roles["save"] = saves[0]
    loras = of("Power Lora Loader (rgthree)")
    if loras and isinstance(wf[loras[0]]["inputs"].get("lora_1"), dict):
        roles["lora"] = loras[0]
    return roles


def list_workflows() -> tuple[list[str], dict[str, str]]:
    """Usable workflows (paths relative to the helper dir, e.g. workflows/x.json) and {path: reason} for the rest."""
    ok, bad = [], {}
    for path in sorted(glob.glob(os.path.join(HELPER_DIR, "workflows", "*.json"))):
        rel = "workflows/" + os.path.basename(path)
        try:
            with open(path, encoding="utf-8") as f:
                find_roles(json.load(f))
            ok.append(rel)
        except (OSError, ValueError, ComfyError) as e:
            bad[rel] = str(e)
    return ok, bad


class ComfyClient:
    def __init__(self, url: str = "http://127.0.0.1:8188", workflow: str = "workflows/krea2_tandy.json"):
        self.url = url.rstrip("/")
        self.workflow = workflow
        path = workflow if os.path.isabs(workflow) else os.path.join(HELPER_DIR, workflow)
        with open(path, encoding="utf-8") as f:
            self.template = json.load(f)
        try:
            self.roles = find_roles(self.template)
        except ComfyError as e:
            raise ComfyError(f"workflow {workflow}: {e}") from None
        self.client_id = uuid.uuid4().hex

    def has_lora(self) -> bool:
        return "lora" in self.roles

    def alive(self) -> bool:
        try:
            return httpx.get(f"{self.url}/system_stats", timeout=2).status_code == 200
        except httpx.HTTPError:
            return False

    def build(self, prompt: str, seed: int | None = None, steps: int = 8, shortside: int = 768,
              lora_on: bool = True, lora_strength: float = 1.0) -> tuple[dict, int]:
        wf = copy.deepcopy(self.template)
        r = self.roles
        if seed is None or seed < 0:
            seed = random.randrange(1, 2**48)
        wf[r["prompt"]]["inputs"]["text"] = prompt
        wf[r["sampler"]]["inputs"]["seed"] = seed
        wf[r["sampler"]]["inputs"]["steps"] = steps
        size = wf[r["size"]]
        if size["class_type"] == "Empty Latent by Ratio (WLSH)":
            size["inputs"].update(shortside=shortside, aspect="4:3", direction="landscape")
        elif size["class_type"] == "ResolutionSelector":
            size["inputs"].update(aspect_ratio="4:3 (Standard)",
                                  megapixels=round(shortside * shortside * 4 / 3 / 1e6, 3))
        else:
            size["inputs"].update(width=shortside * 4 // 3 // 8 * 8, height=shortside // 8 * 8)
        wf[r["save"]]["inputs"]["filename_prefix"] = SAVE_PREFIX
        if "lora" in r:
            lora = wf[r["lora"]]["inputs"]["lora_1"]
            lora["on"] = bool(lora_on)
            lora["strength"] = float(lora_strength)
        return wf, seed

    def run(self, workflow: dict, progress=None, timeout: float = 600) -> Image.Image:
        """Queue the workflow, report progress(fraction, text), return the image."""
        r = httpx.post(f"{self.url}/prompt", json={"prompt": workflow, "client_id": self.client_id}, timeout=30)
        if r.status_code != 200:
            raise ComfyError(f"ComfyUI rejected the workflow: {r.text[:300]}")
        prompt_id = r.json()["prompt_id"]

        ws = None
        try:
            import websocket
            ws = websocket.create_connection(
                f"{self.url.replace('http', 'ws', 1)}/ws?clientId={self.client_id}", timeout=2)
        except Exception:
            ws = None      # fall back to polling /history

        start = time.time()
        while time.time() - start < timeout:
            if ws is not None:
                try:
                    msg = ws.recv()
                except Exception:
                    msg = None
                if isinstance(msg, str):
                    m = json.loads(msg)
                    d = m.get("data", {})
                    if m.get("type") == "progress" and progress and d.get("prompt_id", prompt_id) == prompt_id:
                        progress(d["value"] / max(1, d["max"]), f"step {d['value']}/{d['max']}")
                    elif m.get("type") == "executing" and progress and d.get("node"):
                        progress(None, f"node {d['node']}")
            else:
                time.sleep(0.5)
            h = httpx.get(f"{self.url}/history/{prompt_id}", timeout=10).json()
            if prompt_id in h:
                entry = h[prompt_id]
                status = entry.get("status", {})
                if status.get("status_str") == "error":
                    raise ComfyError(f"ComfyUI error: {json.dumps(status.get('messages', ''))[:400]}")
                for node_out in entry.get("outputs", {}).values():
                    for img in node_out.get("images", []):
                        if img.get("type") == "output":
                            if ws is not None:
                                ws.close()
                            v = httpx.get(f"{self.url}/view", params={
                                "filename": img["filename"], "subfolder": img.get("subfolder", ""),
                                "type": "output"}, timeout=60)
                            return Image.open(io.BytesIO(v.content)).convert("RGB")
        if ws is not None:
            ws.close()
        raise ComfyError("ComfyUI timed out")
