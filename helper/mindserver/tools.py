"""The chat's tools for Qwen (OpenAI function calling) and how past replies are replayed to it.

    draw_picture(prompt)                  a picture, drawn after the reply (chat._draw)
    compose_music(description, style, length, sketches)   a song (or two 8-bar sketches to choose from),
                                          composed after the reply (chat._music); music=1 clients only
    revise_music(song, regenerate, drums, drum_pattern, tempo, key, length)   a new revision of a song
    keep_sketch(which, length)            keep sketch A or B: it becomes a full song, the other goes
    undo_music(song)                      back to the version a song was revised from
    web_search(query), read_page(id_or_url)   search.py; Qwen gets the results and answers in the same reply

Drawing and composing don't need another Qwen round: Qwen writes its sentence and the call together,
and the reply ends.  A search does: the results go back as a `tool` message and Qwen continues.

History: a stored assistant message (chat.py) keeps `drawn`, `music` and `tools` (searches, with a
shortened result).  It is replayed as real tool calls with their results, because a plain-text note
("[drew picture ...]") gets copied by Qwen instead of calling the tool.  Picture IDs are left out of the
results: Qwen only quotes them back.
"""

from __future__ import annotations

import json

DRAW, MUSIC, SEARCH, READ = "draw_picture", "compose_music", "web_search", "read_page"
REVISE, KEEP, UNDO = "revise_music", "keep_sketch", "undo_music"
MUSIC_TOOLS = (MUSIC, REVISE, KEEP, UNDO)
SEARCH_TOOLS = (SEARCH, READ)
STYLES = ("cracktro", "adventure", "dungeon")
LENGTHS = ("short", "medium", "long", "extended", "epic")
SUMMARY_CHARS = 1500           # how much of a search result is kept in the chat for later turns


def _fn(name: str, description: str, props: dict, required: list[str]) -> dict:
    return {"type": "function", "function": {"name": name, "description": description, "parameters": {
        "type": "object", "properties": props, "required": required}}}


def schemas(music: bool = False, search: bool = False, mode: str | None = None, styles=None) -> list[dict]:
    """styles: (names, descriptions) of the music styles the composer offers (MusicService.styles)."""
    screen = ("on CGA in 4 colours or black and white" if mode == "cga" else "in 16 colours at 640x200")
    out = [_fn(DRAW, f"Draw a picture with the image generator. It appears in the chat a few seconds after "
                     f"your reply, shown {screen} with dithering.",
               {"prompt": {"type": "string", "description": "English, 1 to 3 concrete sentences: subject, "
                           "setting, style and colours. Words to appear in the picture go in quotes."}},
               ["prompt"])]
    if music:
        names, desc = styles or (STYLES, {})
        out.append(_fn(MUSIC, "Compose a new song for the Tandy's 3-voice sound chip. It appears in the chat as a "
                              "music card a few seconds after your reply. With sketches=true the composer writes "
                              "two short 8-bar sketches (A and B) for the user to choose from instead.",
                       {"description": {"type": "string", "description": "Mood, speed and style, and what the "
                                        "song is for if the user said."},
                        "style": {"type": "string", "enum": list(names),
                                  "description": "; ".join(f"{n} = {desc.get(n, n)}" for n in names)},
                        "length": {"type": "string", "enum": list(LENGTHS), "description":
                                   "short about 1 minute, medium 1-2, long 2-3, extended about 4, epic 5-8 minutes. "
                                   "Default medium."},
                        "sketches": {"type": "boolean", "description": "Two short sketches to choose from (when "
                                     "the user wants to pick a direction, or asks for options)."}},
                       ["description"]))
        song = {"type": "string", "enum": ["latest", "A", "B"],
                "description": "latest = the newest song in this chat; A or B = one of the two sketches."}
        out.append(_fn(REVISE, "Make a new version of a song in this chat. Parts not named in regenerate stay "
                               "exactly as they are (locked). The old version is replaced; undo_music brings it back.",
                       {"song": song,
                        "regenerate": {"type": "array", "items": {"type": "string", "enum": ["lead", "accompaniment", "bass"]},
                                       "description": "Parts to write anew (the lead is the tune). Empty = keep all notes."},
                        "drums": {"type": "string", "enum": ["none", "light", "full"]},
                        "drum_pattern": {"type": "string", "enum": ["next"], "description": "A different drum pattern."},
                        "tempo": {"type": "string", "enum": ["faster", "slower"]},
                        "key": {"type": "string", "enum": ["up", "down"], "description": "Up or down a whole step."},
                        "length": {"type": "string", "enum": list(LENGTHS)}},
                       ["song"]))
        out.append(_fn(KEEP, "Keep one of the two sketches: it becomes a full song with its tune unchanged; the "
                             "other sketch is deleted.",
                       {"which": {"type": "string", "enum": ["A", "B"]},
                        "length": {"type": "string", "enum": list(LENGTHS)}}, ["which"]))
        out.append(_fn(UNDO, "Go back to the version of a song before its last revision.",
                       {"song": {"type": "string", "enum": ["latest"]}}, []))
    if search:
        out.append(_fn(SEARCH, "Search the web. Returns the top results: title, link, snippet and a result id.",
                       {"query": {"type": "string", "description": "What to search for."}}, ["query"]))
        out.append(_fn(READ, "Read the text of a web page: a result id from web_search (like r_1a2b3c4d5e6f) "
                             "or a full http(s) address.",
                       {"id_or_url": {"type": "string"}}, ["id_or_url"]))
    return out


def music_request(args: dict) -> str:
    """compose_music's arguments as the free-text request the music service takes."""
    desc = " ".join(str(args.get("description", "")).split())
    style = str(args.get("style", "")).lower()
    if style in STYLES and style not in desc.lower():
        desc = f"{desc} ({style} style)" if desc else f"a {style} song"
    return desc


def call_msg(calls: list[dict], content: str | None = None) -> dict:
    """An assistant message carrying tool calls ({"id", "name", "args"})."""
    return {"role": "assistant", "content": content or None,
            "tool_calls": [{"id": c["id"], "type": "function",
                            "function": {"name": c["name"], "arguments": json.dumps(c["args"] or {})}}
                           for c in calls]}


def result_msg(call_id: str, text: str) -> dict:
    return {"role": "tool", "tool_call_id": call_id, "content": text}


def drawn_result(title: str) -> str:
    return f'The picture "{title}" was drawn and is shown in the chat.' if title else \
        "The picture was drawn and is shown in the chat."


def music_result(title: str) -> str:
    return f'The song "{title}" was composed and is in the chat as a music card.' if title else \
        "The song was composed and is in the chat as a music card."


def replay(text: str, drawn: list[dict], music: list[dict], searches: list[dict], drawn_prompt,
           start: int = 1) -> list[dict]:
    """One stored assistant message as OpenAI messages: [searches + results], then the text with its
    drawing/song calls and their results.  `drawn_prompt(d)` gives a picture's prompt.  Call ids are
    h<start>, h<start+1>, ... (unique within one request)."""
    out, n = [], start
    if searches:
        calls = []
        for s in searches:
            calls.append({"id": f"h{n}", "name": s.get("name", SEARCH), "args": s.get("args", {})})
            n += 1
        out.append(call_msg(calls))
        out += [result_msg(c["id"], s.get("summary") or "(result not kept)") for c, s in zip(calls, searches)]
    calls, results = [], []
    for d in drawn:
        calls.append({"id": f"h{n}", "name": DRAW, "args": {"prompt": drawn_prompt(d)}})
        results.append(drawn_result(d.get("title", "")))
        n += 1
    for s in music:
        if s.get("same_call"):
            continue
        if s.get("tool") and s["tool"] != MUSIC:          # a revision, keep or undo: replayed as itself
            calls.append({"id": f"h{n}", "name": s["tool"], "args": s.get("args") or {}})
            results.append(s.get("result") or music_result(s.get("title", "")))
        else:
            calls.append({"id": f"h{n}", "name": MUSIC, "args": s.get("args") or
                          {"description": s.get("request") or s.get("title", "")}})
            results.append(s.get("result") or music_result(s.get("title", "")))
        n += 1
    if calls:
        out.append(call_msg(calls, text))
        out += [result_msg(c["id"], r) for c, r in zip(calls, results)]
    else:
        out.append({"role": "assistant", "content": text})
    return out
