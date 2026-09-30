"""Client for NInfer's OpenAI-style /v1/chat/completions (Qwen3.8-27B, thinking model).

NInfer returns the private reasoning as `reasoning_content` and the answer as
`content`.  `reasoning_effort` can be set per request: none | low | medium | xhigh.
"""

from __future__ import annotations

import base64
import io
import json
from typing import Iterator

import httpx
from PIL import Image

from .config import Config


class QwenError(RuntimeError):
    pass


def lan_ip() -> str:
    """This PC's address on the LAN (what the Tandy uses), for messages the Tandy shows."""
    import socket
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("192.0.2.1", 9))          # no packet is sent; just picks the outgoing interface
        ip = s.getsockname()[0]
        s.close()
        return ip
    except OSError:
        return "this PC"


def not_running() -> QwenError:
    return QwenError(f"Qwen (NInfer) is not running on the MindServer PC ({lan_ip()}). "
                     "Start it there (START-MINDSERVER.bat), or wait until it has finished loading.")


def image_part(img: Image.Image, max_side: int = 1024, png: bool = False) -> dict:
    """A picture for the vision model.  png=True keeps dithered pixels exact (JPEG would smear them)."""
    img = img.convert("RGB")
    if max(img.size) > max_side:
        img.thumbnail((max_side, max_side), Image.Resampling.NEAREST if png else Image.Resampling.LANCZOS)
    buf = io.BytesIO()
    if png:
        img.save(buf, "PNG")
        mime = "image/png"
    else:
        img.save(buf, "JPEG", quality=90)
        mime = "image/jpeg"
    return {"type": "image_url",
            "image_url": {"url": f"data:{mime};base64," + base64.b64encode(buf.getvalue()).decode("ascii")}}


class QwenClient:
    def __init__(self, config: Config):
        self.config = config
        self._model = None

    @property
    def url(self) -> str:
        return self.config["qwen"]["url"].rstrip("/")

    def model(self) -> str:
        name = self.config["qwen"]["model"].strip()
        if name:
            return name
        if self._model is None:
            try:
                r = httpx.get(self.url + "/v1/models", timeout=5)
                self._model = r.json()["data"][0]["id"]
            except Exception as e:
                raise not_running() from e
        return self._model

    def _body(self, messages: list, effort: str, max_tokens: int | None, stream: bool) -> dict:
        q = self.config["qwen"]
        body = {"model": self.model(), "messages": messages, "stream": stream,
                "max_tokens": int(max_tokens or q["max_tokens"])}
        if effort:
            body["reasoning_effort"] = effort
        if q.get("temperature"):
            body["temperature"] = float(q["temperature"])
        return body

    def stream(self, messages: list, effort: str = "low", max_tokens: int | None = None) -> Iterator[tuple[str, str]]:
        """Yields ('reasoning', text), ('content', text) and finally ('done', finish_reason)."""
        body = self._body(messages, effort, max_tokens, True)
        timeout = httpx.Timeout(connect=5, read=300, write=30, pool=5)
        try:
            with httpx.stream("POST", self.url + "/v1/chat/completions", json=body, timeout=timeout) as r:
                if r.status_code != 200:
                    raise QwenError(f"Qwen error {r.status_code}: {r.read().decode(errors='replace')[:200]}")
                finish = "stop"
                for line in r.iter_lines():
                    if not line.startswith("data:"):
                        continue
                    data = line[5:].strip()
                    if data == "[DONE]":
                        break
                    chunk = json.loads(data)
                    for ch in chunk.get("choices", []):
                        d = ch.get("delta", {})
                        if d.get("reasoning_content"):
                            yield "reasoning", d["reasoning_content"]
                        if d.get("content"):
                            yield "content", d["content"]
                        if ch.get("finish_reason"):
                            finish = ch["finish_reason"]
                yield "done", finish
        except httpx.HTTPError as e:
            raise not_running() from e

    def complete(self, messages: list, effort: str = "low", max_tokens: int | None = None) -> str:
        return "".join(t for kind, t in self.stream(messages, effort, max_tokens) if kind == "content").strip()
