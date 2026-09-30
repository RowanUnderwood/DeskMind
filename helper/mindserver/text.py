"""Make model output safe for DeskMind: CP437 characters, no Markdown.

`TandyText` works on a stream: feed() pieces as they arrive and it returns
cleaned text in word-sized chunks, holding back anything that might still
be the start of Markdown or a <draw> tag.
"""

from __future__ import annotations

import re
import unicodedata

# Characters that CP437 lacks, mapped to close equivalents
_MAP = {
    "‘": "'", "’": "'", "‚": ",", "‛": "'", "′": "'",
    "“": '"', "”": '"', "„": '"', "″": '"',
    "–": "-", "—": "-", "―": "-", "−": "-", "‐": "-", "‑": "-",
    "…": "...", "•": "■", "●": "■", "‣": ">", "⁃": "-",
    " ": " ", " ": " ", "​": "", "‍": "", "️": "",
    "™": "(TM)", "©": "(C)", "®": "(R)", "€": "EUR",
    "←": "<-", "→": "->", "↔": "<->", "⇒": "=>", "≤": "<=", "≥": ">=",
    "×": "x", "≈": "~", "≠": "!=", "✓": "v", "✔": "v", "✗": "x",
    "\U0001F642": ":)", "\U0001F600": ":D", "\U0001F603": ":D", "\U0001F604": ":D", "\U0001F609": ";)",
    "\U0001F60A": ":)", "\U0001F601": ":D", "\U0001F622": ":(", "\U0001F61E": ":(", "\U0001F44D": "(thumbs up)",
}


def to_cp437(text: str) -> str:
    """Replace what CP437 can't show; drop the rest (emoji etc.)."""
    out = []
    for ch in text:
        if ch in _MAP:
            out.append(_MAP[ch])
            continue
        try:
            ch.encode("cp437")
            out.append(ch)
            continue
        except UnicodeEncodeError:
            pass
        base = unicodedata.normalize("NFKD", ch).encode("cp437", "ignore").decode("cp437")
        out.append(base)
    return "".join(out)


_INLINE = [
    (re.compile(r"\*\*(.+?)\*\*"), r"\1"),
    (re.compile(r"__(.+?)__"), r"\1"),
    (re.compile(r"(?<![\w*])\*(?!\s)(.+?)(?<!\s)\*(?![\w*])"), r"\1"),
    (re.compile(r"`([^`]*)`"), r"\1"),
    (re.compile(r"\[([^\]]+)\]\((?:[^)]+)\)"), r"\1"),
]
_LINE_START = [
    (re.compile(r"^\s{0,3}#{1,6}\s+"), ""),          # headings
    (re.compile(r"^\s{0,3}[-*+]\s+"), "■ "),    # bullets -> CP437 small square (FEh)
    (re.compile(r"^\s{0,3}>\s?"), ""),               # quotes
    (re.compile(r"^\s{0,3}```.*$"), ""),             # code fences
    (re.compile(r"^\s{0,3}(-{3,}|\*{3,}|_{3,})\s*$"), ""),   # rules
]


def clean_line(line: str) -> str:
    for rx, rep in _LINE_START:
        line = rx.sub(rep, line)
    for rx, rep in _INLINE:
        line = rx.sub(rep, line)
    return to_cp437(line.replace("**", "").replace("`", ""))


def clean(text: str) -> str:
    """Whole-text version (history, enhanced prompts)."""
    return "\n".join(clean_line(l) for l in text.replace("\r\n", "\n").split("\n")).strip()


class TandyText:
    """Streaming cleaner.  feed() returns a list of protocol lines: 'T <text>' and 'N' (newline)."""

    def __init__(self):
        self.line = ""          # text of the current line not yet sent
        self.sent = 0           # how much of self.line was already sent (cleaned)
        self.at_line_start = True
        self.blank_run = 0

    def _emit_text(self, raw: str, final: bool) -> list[str]:
        if not raw:
            return []
        txt = clean_line(raw) if self.at_line_start else to_cp437(
            re.sub(r"\*\*|__|`", "", raw))
        self.at_line_start = False
        return [f"T {txt}"] if txt else []

    def feed(self, piece: str) -> list[str]:
        out: list[str] = []
        piece = piece.replace("\r", "")
        for ch in piece:
            if ch == "\n":
                out += self._emit_text(self.line[self.sent:], True)
                if self.line.strip() or not self.at_line_start:
                    self.blank_run = 0
                    out.append("N")
                else:
                    self.blank_run += 1
                    if self.blank_run == 1:
                        out.append("N")          # keep one blank line between paragraphs
                self.line, self.sent, self.at_line_start = "", 0, True
                continue
            self.line += ch
        # Send whole words that are safe: up to the last space, unless Markdown may be open
        pending = self.line[self.sent:]
        cut = pending.rfind(" ")
        if cut > 0:
            chunk = pending[:cut + 1]
            if chunk.count("**") % 2 == 0 and chunk.count("`") % 2 == 0 and "[" not in chunk:
                if not (self.at_line_start and len(chunk.strip()) <= 3 and chunk.strip() in ("#", "##", "###", "-", "*", "+", ">")):
                    out += self._emit_text(chunk, False)
                    self.sent += len(chunk)
        return out

    def flush(self) -> list[str]:
        out = self._emit_text(self.line[self.sent:], True)
        self.line, self.sent = "", 0
        return out


class Coalescer:
    """Merges consecutive 'T' lines so the Tandy gets fewer, larger network packets.
    Text is held until it reaches `size` characters or `delay` seconds; any other line flushes it."""

    def __init__(self, emit, size: int = 48, delay: float = 0.25):
        self.emit, self.size, self.delay = emit, size, delay
        self.buf, self.since = "", 0.0

    def __call__(self, line: str) -> None:
        import time
        if line.startswith("T "):
            if not self.buf:
                self.since = time.time()
            self.buf += line[2:]
            if len(self.buf) >= self.size or time.time() - self.since >= self.delay:
                self.flush()
            return
        self.flush()
        self.emit(line)

    def flush(self) -> None:
        if self.buf:
            self.emit("T " + self.buf)
            self.buf = ""


class DrawSplitter:
    """Separates '<draw>...</draw>' from visible text in a stream."""

    OPEN, CLOSE = "<draw>", "</draw>"

    def __init__(self):
        self.buf = ""
        self.inside = False
        self.prompts: list[str] = []
        self.current = ""

    def feed(self, piece: str) -> str:
        """Returns the visible text from this piece (tag contents are collected)."""
        self.buf += piece
        visible = ""
        while True:
            if not self.inside:
                i = self.buf.lower().find(self.OPEN)
                if i >= 0:
                    visible += self.buf[:i]
                    self.buf = self.buf[i + len(self.OPEN):]
                    self.inside = True
                    continue
                # keep a possible partial "<draw" at the end
                keep = 0
                for n in range(1, len(self.OPEN)):
                    if self.buf.lower().endswith(self.OPEN[:n]):
                        keep = n
                visible += self.buf[:len(self.buf) - keep]
                self.buf = self.buf[len(self.buf) - keep:]
                return visible
            i = self.buf.lower().find(self.CLOSE)
            if i >= 0:
                self.current += self.buf[:i]
                self.prompts.append(" ".join(self.current.split()))
                self.current = ""
                self.buf = self.buf[i + len(self.CLOSE):]
                self.inside = False
                continue
            keep = 0
            for n in range(1, len(self.CLOSE)):
                if self.buf.lower().endswith(self.CLOSE[:n]):
                    keep = n
            self.current += self.buf[:len(self.buf) - keep]
            self.buf = self.buf[len(self.buf) - keep:]
            return visible

    def finish(self) -> str:
        """End of stream: an unclosed tag still counts as a prompt."""
        rest = ""
        if self.inside and (self.current + self.buf).strip():
            self.prompts.append(" ".join((self.current + self.buf).split()))
        elif not self.inside:
            rest = self.buf
        self.buf, self.current, self.inside = "", "", False
        return rest
