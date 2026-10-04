"""Health checks and start/stop for NInfer (Qwen) and ComfyUI.

    python -m mindserver.services model   prints the configured NInfer model (for START-MINDSERVER.bat)
    python -m mindserver.services check   prints a warning if the running NInfer holds the other model
"""

from __future__ import annotations

import logging
import os
import subprocess
import sys
import threading
import time

import httpx

from .config import Config

log = logging.getLogger("mindserver.services")

NEW_CONSOLE = getattr(subprocess, "CREATE_NEW_CONSOLE", 0)
NINFER_MODELS = ("thinkingcap", "full")     # launch-ninfer.bat's WIN_MODEL values; "" = its own default


def check_qwen(url: str) -> str:
    try:
        r = httpx.get(url.rstrip("/") + "/health", timeout=2)
        return "up" if r.status_code == 200 else f"http {r.status_code}"
    except httpx.HTTPError:
        return "down"


def check_comfy(url: str) -> str:
    try:
        r = httpx.get(url.rstrip("/") + "/system_stats", timeout=2)
        return "up" if r.status_code == 200 else f"http {r.status_code}"
    except httpx.HTTPError:
        return "down"


def ninfer_running(config: Config) -> tuple[str, str]:
    """(model, context) from the stamps launch-ninfer.bat writes on every start; "" if unknown.
    Both models answer as qwen3.8-27b, so nothing over HTTP tells them apart."""
    logs = os.path.join(os.path.dirname(config["services"]["ninfer_bat"]), "logs")
    out = []
    for name in ("current-model.txt", "current-context.txt"):
        try:
            with open(os.path.join(logs, name), encoding="ascii", errors="replace") as f:
                out.append(f.read().strip())
        except OSError:
            out.append("")
    return out[0], out[1]


def ninfer_mismatch(config: Config) -> str:
    """A warning when the running NInfer holds another model than the setting asks for, else ""."""
    want = config["services"].get("ninfer_model", "").strip()
    have, _ = ninfer_running(config)
    if not want or not have or have == want:
        return ""
    return (f"NInfer is running {have}, but MindServer is set to {want}. It keeps using {have}; "
            "stop NInfer and start it again to switch (another app may be using it).")


def _port(url: str) -> int:
    return int(url.rstrip("/").rsplit(":", 1)[1])


def kill_port(port: int) -> None:
    ps = (f"Get-NetTCPConnection -LocalPort {port} -State Listen -ErrorAction SilentlyContinue | "
          "ForEach-Object { Stop-Process -Id $_.OwningProcess -Force -ErrorAction SilentlyContinue }")
    subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True,
                   creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))


class Services:
    """Polls both services every few seconds; the server's /ping and the GUI read `status`."""

    def __init__(self, config: Config, on_change=None, interval: float = 3.0):
        self.config = config
        self.status = {"qwen": "unknown", "comfy": "unknown"}
        self.on_change = on_change
        self.interval = interval
        threading.Thread(target=self._loop, name="services", daemon=True).start()

    def _loop(self) -> None:
        while True:
            new = {"qwen": check_qwen(self.config["qwen"]["url"]),
                   "comfy": check_comfy(self.config["comfy"]["url"])}
            if new != self.status:
                if new["qwen"] == "up" and self.status["qwen"] != "up":
                    model, ctx = ninfer_running(self.config)
                    log.info("NInfer is up: %s, context %s", model or "model unknown", ctx or "unknown")
                    warn = ninfer_mismatch(self.config)
                    if warn:
                        log.warning(warn)
                self.status = new
                if self.on_change:
                    self.on_change(dict(new))
            time.sleep(self.interval)

    def start(self, name: str) -> None:
        s = self.config["services"]
        bat = s["ninfer_bat"] if name == "qwen" else s["comfy_bat"]
        title = "NInfer (5090)" if name == "qwen" else "ComfyUI (4090)"
        env = os.environ.copy()
        model = s.get("ninfer_model", "").strip()
        if name == "qwen" and model:
            env["WIN_MODEL"] = model            # launch-ninfer.bat lets a caller preset it
        subprocess.Popen(["cmd", "/c", "start", title, "cmd", "/c", "call", bat], creationflags=NEW_CONSOLE, env=env)

    def stop(self, name: str) -> None:
        if name == "qwen":
            subprocess.Popen(["cmd", "/c", "call", self.config["services"]["ninfer_stop_bat"]],
                             creationflags=NEW_CONSOLE)
        else:
            kill_port(_port(self.config["comfy"]["url"]))


if __name__ == "__main__":
    cfg = Config()
    if sys.argv[1:] == ["model"]:
        print(cfg["services"].get("ninfer_model", "").strip())
    elif sys.argv[1:] == ["check"]:
        msg = ninfer_mismatch(cfg)
        if msg:
            print("WARNING: " + msg)
    else:
        print(__doc__)
