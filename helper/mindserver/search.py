"""Web search for the chat, through the MCP server mcp-web-search-tool (DuckDuckGo without a key).

The server is a Node program installed once, pinned, under helper\\mcp:
    npm install --prefix helper\\mcp mcp-web-search-tool@2.1.0
MCP over stdio is newline-delimited JSON-RPC 2.0, so this file talks to it directly (initialize, then
tools/call) instead of pulling in the async MCP SDK.  One server process is shared by all chats; it starts
on the first search and is restarted once if it died.

Qwen gets two tools of our own (tools.py): web_search(query) -> the server's web_search (Markdown only,
the duplicate JSON block is cut) and read_page(id_or_url) -> its fetch_url.  Results keep the server's
"untrusted content" banner.  fetch_url refuses private, loopback and link-local hosts by itself.
"""

from __future__ import annotations

import html
import json
import logging
import os
import re
import shutil
import subprocess
import threading

from .config import HELPER_DIR, Config

log = logging.getLogger("mindserver.search")

SERVER_JS = os.path.join(HELPER_DIR, "mcp", "node_modules", "mcp-web-search-tool", "build", "index.js")
PROTOCOL = "2025-06-18"
JSON_BLOCK = re.compile(r"```json.*?(```|$)", re.DOTALL)


class SearchError(RuntimeError):
    pass


class McpStdio:
    """One MCP server process; call() is safe from several threads."""

    def __init__(self, argv: list[str]):
        self.argv = argv
        self.proc: subprocess.Popen | None = None
        self.lock = threading.Lock()                 # start/stop and writes
        self.cond = threading.Condition()
        self.replies: dict[int, dict] = {}
        self.next_id = 1

    def alive(self) -> bool:
        return self.proc is not None and self.proc.poll() is None

    def start(self, timeout: float = 15) -> None:
        with self.lock:
            if self.alive():
                return
            self.proc = subprocess.Popen(
                self.argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                cwd=os.path.dirname(self.argv[-1]), creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            threading.Thread(target=self._read, args=(self.proc,), name="mcp-out", daemon=True).start()
            threading.Thread(target=self._log, args=(self.proc,), name="mcp-err", daemon=True).start()
        self.request("initialize", {"protocolVersion": PROTOCOL, "capabilities": {},
                                    "clientInfo": {"name": "MindServer", "version": "1"}}, timeout)
        self._send({"jsonrpc": "2.0", "method": "notifications/initialized"})

    def stop(self) -> None:
        with self.lock:
            if self.proc:
                try:
                    self.proc.kill()
                except OSError:
                    pass
                self.proc = None

    def _send(self, msg: dict) -> None:
        with self.lock:
            if not self.alive():
                raise SearchError("the search server is not running")
            try:
                self.proc.stdin.write((json.dumps(msg) + "\n").encode("utf-8"))
                self.proc.stdin.flush()
            except OSError as e:
                raise SearchError(f"the search server closed ({e})") from e

    def _read(self, proc: subprocess.Popen) -> None:
        for raw in proc.stdout:
            try:
                msg = json.loads(raw)
            except ValueError:
                continue
            if isinstance(msg, dict) and "id" in msg and ("result" in msg or "error" in msg):
                with self.cond:
                    self.replies[msg["id"]] = msg
                    self.cond.notify_all()
        with self.cond:
            self.cond.notify_all()                   # wake waiters: the process ended

    @staticmethod
    def _log(proc: subprocess.Popen) -> None:
        for raw in proc.stderr:
            line = raw.decode("utf-8", errors="replace").strip()
            if line:
                log.info("%s", line)

    def request(self, method: str, params: dict, timeout: float) -> dict:
        with self.cond:
            id_ = self.next_id
            self.next_id += 1
        self._send({"jsonrpc": "2.0", "id": id_, "method": method, "params": params})
        with self.cond:
            if not self.cond.wait_for(lambda: id_ in self.replies or not self.alive(), timeout):
                raise SearchError(f"no answer from the search server in {timeout:.0f} s")
            msg = self.replies.pop(id_, None)
        if msg is None:
            raise SearchError("the search server stopped")
        if "error" in msg:
            raise SearchError(str(msg["error"].get("message", msg["error"])))
        return msg["result"]


class WebSearch:
    def __init__(self, config: Config):
        self.config = config
        self.mcp: McpStdio | None = None

    @property
    def settings(self) -> dict:
        return self.config["search"]

    def problem(self) -> str:
        """Why search can't be used, or "" when it can."""
        if not self.settings.get("enabled"):
            return "switched off"
        if not os.path.exists(SERVER_JS):
            return "mcp-web-search-tool is not installed in helper\\mcp"
        if not shutil.which(self.settings.get("node") or "node"):
            return "Node.js was not found"
        return ""

    def available(self) -> bool:
        return not self.problem()

    def _call(self, name: str, args: dict) -> str:
        timeout = float(self.settings.get("timeout", 20))
        for attempt in (1, 2):
            try:
                if self.mcp is None or not self.mcp.alive():
                    self.mcp = McpStdio([shutil.which(self.settings.get("node") or "node"), SERVER_JS])
                    self.mcp.start()
                res = self.mcp.request("tools/call", {"name": name, "arguments": args}, timeout)
                break
            except SearchError as e:
                if attempt == 2 or (self.mcp and self.mcp.alive()):
                    raise                            # a live server's error: no point restarting
                log.warning("search server: %s; starting it again", e)
                self.mcp = None
        text = "\n".join(c.get("text", "") for c in res.get("content", []) if c.get("type") == "text").strip()
        if res.get("isError"):
            raise SearchError(text[:300] or "the search failed")
        return text

    def search(self, query: str) -> str:
        text = self._call("web_search", {"search_term": query, "count": int(self.settings.get("count", 5))})
        text = JSON_BLOCK.sub("", text).strip()       # the same results again as JSON: not needed
        text = html.unescape(text).replace("call `fetch_url`", "call read_page")   # Qwen's name for it
        return text[: int(self.settings.get("max_chars", 6000))]

    def read(self, id_or_url: str) -> str:
        n = int(self.settings.get("max_chars", 6000))
        return self._call("fetch_url", {"id_or_url": id_or_url, "max_chars": max(200, n)})[:n + 1000]

    def close(self) -> None:
        if self.mcp:
            self.mcp.stop()
            self.mcp = None
