"""MindServer settings (helper\\settings.json)."""

from __future__ import annotations

import json
import os
import threading

from .dither import DitherSettings

HELPER_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROJECT_DIR = os.path.dirname(HELPER_DIR)
SETTINGS_PATH = os.path.join(HELPER_DIR, "settings.json")
DATA_DIR = os.path.join(HELPER_DIR, "data")

DEFAULTS = {
    "server": {
        "host": "0.0.0.0",
        "port": 8286,
        "token": "",                      # optional shared secret (X-Token header / ?t=)
    },
    "qwen": {
        "url": "http://127.0.0.1:1234",
        "model": "",                      # blank = first model from /v1/models
        "effort_chat": "low",               # none | low | medium | xhigh (per request)
        "effort_enhance": "medium",
        "effort_vision": "low",
        "temperature": 0.0,                 # 0 = the model's own default
        "max_tokens": 4000,                 # thinking uses this budget too
        "draw_enhance": False,              # also run the enhancer on prompts from <draw> in chat
    },
    "comfy": {
        "url": "http://127.0.0.1:8188",
        "workflow": "workflows/krea2_tandy.json",
        "steps": 8,
        "shortside": 768,
        "lora_on": True,
        "lora_strength": 1.0,
    },
    "services": {
        "ninfer_bat": r"H:\Ninfer Qwen\launch-ninfer.bat",
        "ninfer_stop_bat": r"H:\Ninfer Qwen\stop-ninfer.bat",
        "comfy_bat": os.path.join(PROJECT_DIR, "run_comfy_image.bat"),
    },
    "dither": DitherSettings(engine="pillow", method="floyd-steinberg").to_dict(),
}


def _merge(base: dict, over: dict) -> dict:
    out = dict(base)
    for k, v in over.items():
        if isinstance(v, dict) and isinstance(out.get(k), dict):
            out[k] = _merge(out[k], v)
        else:
            out[k] = v
    return out


class Config:
    def __init__(self, path: str = SETTINGS_PATH):
        self.path = path
        self.lock = threading.Lock()
        self.data = _merge(DEFAULTS, {})
        self.load()

    def load(self) -> None:
        if os.path.exists(self.path):
            with open(self.path, encoding="utf-8") as f:
                self.data = _merge(DEFAULTS, json.load(f))

    def save(self) -> None:
        with self.lock:
            tmp = self.path + ".tmp"
            with open(tmp, "w", encoding="utf-8") as f:
                json.dump(self.data, f, indent=2)
            os.replace(tmp, self.path)

    def __getitem__(self, key):
        return self.data[key]

    def dither_settings(self) -> DitherSettings:
        return DitherSettings.from_dict(self.data["dither"])
