"""Health checks and start/stop for NInfer (Qwen) and ComfyUI."""

from __future__ import annotations

import subprocess
import threading
import time

import httpx

from .config import Config

NEW_CONSOLE = getattr(subprocess, "CREATE_NEW_CONSOLE", 0)


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
                self.status = new
                if self.on_change:
                    self.on_change(dict(new))
            time.sleep(self.interval)

    def start(self, name: str) -> None:
        s = self.config["services"]
        bat = s["ninfer_bat"] if name == "qwen" else s["comfy_bat"]
        title = "NInfer (5090)" if name == "qwen" else "ComfyUI (4090)"
        subprocess.Popen(["cmd", "/c", "start", title, "cmd", "/c", "call", bat], creationflags=NEW_CONSOLE)

    def stop(self, name: str) -> None:
        if name == "qwen":
            subprocess.Popen(["cmd", "/c", "call", self.config["services"]["ninfer_stop_bat"]],
                             creationflags=NEW_CONSOLE)
        else:
            kill_port(_port(self.config["comfy"]["url"]))
