"""ComfyUI client: run the trimmed Krea2 workflow and fetch the image.

The workflow (workflows/krea2_tandy.json) is the user's Krea2 API workflow
with the SeedVR2 upscaler, VRAM-cleanup and comparer nodes removed and a
SaveImage node on the base image.  Nodes patched per request:

    6  CLIPTextEncode        prompt text
    2  KSampler              seed, steps
    15 Empty Latent by Ratio shortside (4:3 landscape)
    18 Power Lora Loader     LoRA on/off and strength
"""

from __future__ import annotations

import copy
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


class ComfyClient:
    def __init__(self, url: str = "http://127.0.0.1:8188", workflow: str = "workflows/krea2_tandy.json"):
        self.url = url.rstrip("/")
        path = workflow if os.path.isabs(workflow) else os.path.join(HELPER_DIR, workflow)
        with open(path, encoding="utf-8") as f:
            self.template = json.load(f)
        self.client_id = uuid.uuid4().hex

    def alive(self) -> bool:
        try:
            return httpx.get(f"{self.url}/system_stats", timeout=2).status_code == 200
        except httpx.HTTPError:
            return False

    def build(self, prompt: str, seed: int | None = None, steps: int = 8, shortside: int = 768,
              lora_on: bool = True, lora_strength: float = 1.0) -> tuple[dict, int]:
        wf = copy.deepcopy(self.template)
        if seed is None or seed < 0:
            seed = random.randrange(1, 2**48)
        wf["6"]["inputs"]["text"] = prompt
        wf["2"]["inputs"]["seed"] = seed
        wf["2"]["inputs"]["steps"] = steps
        wf["15"]["inputs"]["shortside"] = shortside
        wf["15"]["inputs"]["aspect"] = "4:3"
        wf["15"]["inputs"]["direction"] = "landscape"
        lora = wf["18"]["inputs"].get("lora_1")
        if isinstance(lora, dict):
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
