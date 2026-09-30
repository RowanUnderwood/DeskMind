# Plan: Tandy 286 generative-AI client + PC helper

## Context

We want a native DOS program for the Tandy 1000 TL/3 (10 MHz 286, 640K, Tandy video, no XMS) that uses AI on this PC over the LAN,
through PicoMEM WiFi (NE2000 packet driver at INT 60h, mTCP already installed). The AI services are **NInfer / Qwen3.8-27B** (RTX 5090,
`http://192.168.2.192:1234`, OpenAI-compatible, vision on) and **ComfyUI Krea2** (RTX 4090, port 8188). A **helper app on this PC** does
everything too heavy for the 286: JSON, base64, vision uploads, prompt enhancement, resizing and dithering to Tandy format, and
thumbnails. The DOS side only moves text lines and ready-to-display image bytes.

**Verdict: feasible.** Every hard part (TCP/IP, 16-colour graphics, mouse, sound) is proven or already installed on the machine. Most of
the risk is in one place: 640x200x16 mode, which has no BIOS support. Phase 0 settles it before anything else is built on it.

## Findings and corrections (read first)

1. **640x200x16 ("ETGA", Tandy Video II) is real on the TL/3, but the BIOS doesn't support it.** You have to program the video registers directly
   (as DeskMate, Sargon V, ST:25th and the TANDY11 TSR do). It needs **64K** of video RAM at the top of conventional memory. DOS only
   reserves the top 16K today (624K visible), so the app must claim the other 48K itself (as ADJMEM does). That leaves about **525K** for the
   app. That's enough, but it must be designed for. The fallback is 320x200x16 (BIOS mode 9, 32K, proven with Prince of Persia).
   The image format carries a mode flag, so either mode works.
2. **Tall pixels:** 640x200 on a 4:3 screen gives pixels about 2.4x taller than they are wide. The helper must resize anamorphically (4:3 image
   to 640x200) *before* dithering. Otherwise every image comes out stretched. Thumbnails are the same (160x50 shows as 4:3).
3. **Fixed palette:** the 16-colour modes show the fixed RGBI 16 colours. There is no palette to choose, so image quality comes from
   pre-processing and dithering alone. Colour 6 shows as brown or dark yellow depending on the monitor (CM-5 vs CM-11), so the palette
   RGB values must be a helper setting.
4. **Mouse:** CuteMouse can't draw a cursor in Tandy 16-colour modes (and 640x200x16 is non-BIOS). The app reads INT 33h for
   position and buttons only, and draws its own cursor.
5. **Sound:** WiFi (W) boot mode turns off the Sound Blaster, so the **Tandy SN76496 (port C0h)** is the only practical sound source.
   That matches feature #1.
6. **ComfyUI workflow:** the SeedVR2 stage upscales to 4000px, which is wasted for a 640x200 target and costs minutes. The helper
   uses a trimmed API workflow: Krea2 Turbo, 8 steps, 4:3 at a short side of about 768, random seed, KNPV3_1 LoRA optional, SeedVR2 nodes removed,
   output taken from `/history` + `/view`.
7. **Helper GUI must not be a browser page.** A browser tab uses VRAM on GPU 0, which NInfer needs (see `H:\Ninfer Qwen\launch-ninfer.bat`
   notes). So the GUI is native Qt (PySide6).
8. **Qwen output** is Unicode + Markdown from a thinking model. The helper strips the reasoning, strips or maps Markdown, and transliterates to
   CP437. Chat requests use a per-request low reasoning effort so replies stay quick.
9. **Stable image identity:** a chat about an image refers to it by an 8-hex **ID** in the file header, not by its filename. That way, renaming a
   file on the Tandy never breaks the helper's link to the original PNG. If the helper has lost the original, the DOS app uploads its
   dithered copy (64K) as a fallback.
10. **8KB clusters:** each image is **one file** (header + prompt + thumbnail + full image, about 69K), not three small files.
11. **W mode leaves only 2.7K of upper memory:** the app is a normal EXE, not a TSR. The **stand-alone slideshow** needs no network, so it runs in the
    normal boot too.

## Name ideas

| DOS program | PC helper | Theme |
|---|---|---|
| **DeskMind** (`DESKMIND.EXE`) | **MindServer** | DeskMate homage. It feels native to the Tandy |
| **DreamMate** | **DreamForge** | DeskMate homage plus image making |
| **Muse/286** (`MUSE.EXE`) | **Mainframe** | The small machine asks the big iron |
| **Oracle 1000** | **Temple** | Tandy 1000 naming |
| **ChatMate** | **Big Iron** | Friendly and retro |
| **Tandy Dreams** | **Dither Forge** | The helper's main job |

Slideshow: `SLIDES.EXE` / "Gallery Show". Until you pick, this plan uses **`<APP>`** / **`<HELPER>`** and `C:\<APP>\` on the Tandy.

## Architecture

```
Tandy (W mode) --HTTP/1.0 over mTCP--> Helper :8286 (this PC, Python)
                                         |-> NInfer 127.0.0.1:1234 (5090)  chat / enhance / vision
                                         |-> ComfyUI 127.0.0.1:8188 (4090) Krea2 generation (+ /ws progress)
                                         '-> data\  originals PNG, TPI files, chats JSON, settings
```

**Protocol** (plain HTTP/1.0, `Connection: close`, so it can be tested with curl and mTCP `HTGET`). Bodies are simple CP437 text lines,
and binary only for images. Streamed replies are coded lines: `T <text>` (append), `P` (paragraph), `I <id> <title>` (image ready),
`S <status/progress %>`, `E <error>`, `D` (done).

| Endpoint | Purpose |
|---|---|
| `GET /ping` | Version and status of the helper, Qwen and ComfyUI |
| `POST /chat` (chat id, text, optional image id) | Streamed reply. Can trigger drawing (#5) |
| `POST /enhance` | Enhanced prompt, which the user can edit before generating |
| `POST /gen` → `GET /job/<n>` | Start generation, poll progress (from ComfyUI's websocket) |
| `GET /img/<id>` | TPI file (thumbnail + full image) |
| `GET /chats`, `GET /chat/<id>`, `POST /chat/<id>/del`, `POST /chat/<id>/sync` | Resume, delete, or re-upload a local transcript |
| `POST /img/<id>/upload` | Fallback when the helper lacks the original |
| `GET /list` | Images the helper holds, to sync images made in the helper GUI |
| `GET /files/<name>` | Serves the latest DOS build. `UPDATE.BAT` on the Tandy fetches it with HTGET |

**Natural-language drawing (#5):** the system prompt tells Qwen to put `<draw>prompt</draw>` in its reply when the user wants a picture.
The helper catches the tag, optionally enhances the prompt, runs ComfyUI, streams `S` progress and then `I <id>`. The DOS app downloads the TPI and puts the
thumbnail in the chat; clicking it shows the image full screen. A tag works whether or not NInfer supports OpenAI `tools`. Native
tool calls can be added later.

**TPI file (`<ID>.TPI`, 8.3):** 128-byte header (magic `TPI1`, mode, ID, date, 40-char title, offsets), then the prompt text, then the
160x50 thumbnail, then the full image **already in the video-memory bank order**, so display is one `REP MOVSW` per bank. Rename in
the gallery changes the title (and optionally the 8.3 name). Delete removes the file (and, optionally, the helper's copy).

## Helper (`H:\Dos Projects\DOS 286 generative AI applications\helper\`)

Python 3.12 venv (PySide6, numpy, Pillow, httpx, websocket-client, hitherdither from git). `didder.exe` in `tools\`.

- `server.py`: stdlib `ThreadingHTTPServer` on 0.0.0.0:8286 (optional shared token), runs in a thread under the GUI.
- `qwen.py`: OpenAI chat-completions client. Streaming, reasoning stripped, per-task effort (chat low, enhance medium), vision
  (original PNG downscaled to about 1024px, as a base64 `image_url`), CP437 transliteration, word-safe output.
- `comfy.py`: loads a trimmed copy of `krea2_native ... API (1).json` (`workflows\krea2_tandy.json`), patches the prompt text (node 6), the seed
  (node 2), the latent (node 15), and the LoRA on/off/strength (node 18). Posts to `/prompt`, follows `/ws`, fetches the result.
- `dither.py`: pre-process (crop or letterbox to 4:3, brightness, contrast, saturation, gamma, sharpen), anamorphic resize to 640x200 (or 320x200), then
  a dither engine: **hitherdither** (Yliluoma ordered, Bayer, cluster-dot, Floyd-Steinberg/Atkinson/Sierra/Stucki…) or **didder**
  (linear-light, strength, serpentine). The fixed 16-colour palette is editable. Thumbnails are dithered separately from the original (not scaled down from
  the dithered image). `tpi.py` packs the indices into the video-memory bank order.
  *Research result:* no existing tool targets Tandy 640x200x16 (Dithertron does CGA/EGA only), but hitherdither and didder solve fixed-palette
  dithering well. We only add the Tandy-specific resize and packing, so there's no need to write our own ditherer.
- `store.py`: `data\images\<ID>.png|.tpi|.json`, `data\chats\<id>.json`.
- `prompts\`: `system_chat.txt` (the "you are on a Tandy 1000 TL/3, 286, 16 colours, 80 columns; answer briefly in plain ASCII; use `<draw>`"
  persona, #4), `enhance.txt` (tuned for images that dither well: bold shapes, strong contrast, simple backgrounds), `vision.txt`.
- **GUI (`gui.py`)** with tabs:
  - **Services**: status lights, start/stop for NInfer and ComfyUI, a log, and connected clients.
  - **AI**: URLs, effort per task, temperature, max tokens, and the system/enhance/vision prompts.
  - **Generation**: steps, resolution, LoRA, seed mode, and workflow file.
  - **Dither Lab**: load any image or the last one generated. Shows the original and the Tandy preview side by side at *true 4:3 aspect* plus a 1:1 pixel
    zoom, with live sliders for every dither and pre-process setting, A/B of two presets, saving presets, and "send test image to Tandy" (queued in `/list`).
  - **Tandy**: port, token, palette RGB, and default video mode.
- Settings in `helper\settings.json`.

**Launchers (project root):**
- `START-<HELPER>.bat`: `start "NInfer" cmd /c call "H:\Ninfer Qwen\launch-ninfer.bat"` (first, because it's the pickiest about VRAM), then
  `start "ComfyUI" cmd /c call run_comfy_image.bat`, then `pythonw helper\main.py`. The helper polls `/health` and `/system_stats` and shows when each is ready.
- `STOP-<HELPER>.bat`: runs `stop-ninfer.bat` and stops the ComfyUI process listening on 8188.
- One-time firewall rule for TCP 8286 (admin command, documented in the README).

## DOS program (`dos\`, Open Watcom v2 C, large model, `-0` so it also runs on 86Box's 8086 SL/2, asm for blits)

Toolchain to install: **Open Watcom v2** (not present yet) and the **mTCP source** (GPLv3, TCP/DNS/HTTP code as in `HTGET`), so the program
reads the existing `C:\MTCP\MTCP.CFG` and packet driver at INT 60h. The program is GPLv3 as a result.

Modules: `video.c` (mode set and restore, 64K memory claim, bank-aware blits, 8x8 font with 80x25 cells, clipping), `gui.c` (DeskMate-style
menu bar, windows, buttons, text fields, list boxes, scroll bars, dialogs, drawn cursor), `mouse.c` (INT 33h with range 0-639/0-199), `kbd.c`,
`sound.c` (SN76496: startup chime, reply blip, "image ready" arpeggio; setting on/off), `net.c` (mTCP HTTP client, streamed line
parser, timeouts, "no packet driver, boot with W" message and an offline mode), `tpi.c`, `ems.c` (image cache and chat scrollback in the 4MB EMS),
`cfg.c` (`<APP>.CFG`, CRLF).

Screens:
1. **Chat**: scrollback with word wrap, inline 160x50 thumbnails (click to view full screen), "thinking…" animation, attach image
   (pick from the gallery, then ask about it via vision), chat list with resume and delete (#8). The local transcript is kept in `CHATS\<id>.TCH`.
2. **Create** (#6): prompt box, an "Enhance" toggle with an editable result box (or auto-send), progress bar, then save and view. The enhancer instruction
   is editable here and in the helper.
3. **Gallery**: thumbnail grid (4x3 per page) and list view (title, date, size), view full screen, rename, delete (with confirm), "ask Qwen about this",
   "sync from helper", and slideshow of the current selection.
4. **Settings**: helper IP and port, token, sound on/off, auto-enhance, review the enhanced prompt, slideshow delay and transition.

**Slideshow (`SLIDES.EXE`, no network code, runs in either boot mode):** `SLIDES [dir] [/D secs] [/T effect] [/R]`, loads the next image
into EMS or the second buffer while one shows. Transitions that are cheap on a 286: horizontal/vertical wipe, venetian blinds, bank-interleave reveal (free with the 4-bank
layout), LFSR block dissolve (8x4 blocks), box in/out, and a push. Any key or click exits. The same code is used by the Gallery.

**On the Tandy:** `C:\<APP>\` (EXEs, CFG, `PICS\`, `CHATS\`), launchers `C:\PLAY\<APP>.BAT` and `C:\PLAY\SLIDES.BAT` (already on PATH),
added to `MENU.TXT`. Installed with the parent project's image method (fresh dated backup from the card, mtools on a work copy,
`7z t`, `fsck.fat -n`, copy back, `cmp`, new backup). During development, it's faster to pull builds over the network with `UPDATE.BAT` (HTGET).

## Phases

0. **Feasibility spikes (gates everything):**
   a. `V640.EXE`: set 640x200x16 by programming the registers (reference: 86Box `vid_tandy` source, TANDY11/Grafix docs, TL technical reference),
      claim the top 64K, draw colour bars and a test TPI, restore text mode. Run in 86Box SL/2, then on the **real TL/3**. If it fails, use 320x200x16 (mode 9).
   b. `NETTEST.EXE`: mTCP HTTP GET of a 64K file from a stub helper over PicoMEM WiFi. Measure throughput. Also run in 86Box with an NE2000
      and SLiRP (helper at 10.0.2.2).
   c. Mouse position and buttons in the custom mode, and SN76496 beeps.
   d. Helper: trimmed ComfyUI workflow on the 4090, plus a first Dither Lab script comparing engines on sample images.
1. **Helper core**: store, comfy, dither, tpi, GUI (Services, Generation, Dither Lab, Tandy tabs), `/ping` `/gen` `/job` `/img` `/list` `/files`. Can be tested now without NInfer.
2. **Helper Qwen**: chat streaming, CP437, system prompts, enhance, vision, `<draw>` handling, chat store and sync. **Needs NInfer, so test after a clean reboot.**
3. **Launchers and firewall.**
4. **DOS base libraries**: video, font, GUI widgets, mouse, keyboard, sound, net, TPI, EMS, config.
5. **DOS screens**: Create, then Gallery, then Chat (with drawing and vision), then Settings.
6. **SLIDES.EXE** and the Gallery slideshow.
7. **Install on the card, README and CLAUDE.md**, and a line in the parent PDF/menu if you want one.

## Answer to #9 (sub-project CLAUDE.md)

It's safe. Claude Code loads every `CLAUDE.md` from the working folder up to the drive root. A session started in this subfolder therefore sees
**both** the parent's (games project) and this one's. When you work in the parent folder, this one only loads when files here are touched. Nothing
conflicts. The new `CLAUDE.md` will say it's a separate project and which parent rules still apply (image writing method, backups, 8.3 names, CRLF,
`TANDY-SYSTEM.md`) and which don't (games.csv pipeline, tiers). I'll also add a one-line pointer to the parent's "Files in this folder" table.

## Suggested additions (included unless you say no)

- Regenerate and "variation" (same prompt, new seed) buttons. Show the seed in the metadata.
- Offline mode: Gallery and slideshow work without the network, and chat shows an offline notice.
- Enhancer prompt tuned for dithering ("bold, high-contrast, limited palette, simple background").
- `UPDATE.BAT` build pull, and a helper "send to Tandy" queue.
- Optional shared token, because NInfer on the LAN has no key.

## Verification

- Helper: unit checks that `tpi.py` packing round-trips (unpacking a TPI gives back the preview PNG exactly). Dither Lab visual check. `curl` against every
  endpoint. A ComfyUI generation end to end on the 4090. Qwen endpoints after a clean boot, with `launch-ninfer.bat`.
- DOS: builds with wmake. 86Box SL/2 + XT-IDE + **NE2000/SLiRP** VM (extends the existing `86Box\vm_sl2_*` setup and `vmtest.py`/`snap.ps1`
  screenshots). Covers mode set, gallery, slideshow transitions, and a chat against the helper. Real-TL/3 checks for the 640x200 mode, WiFi throughput,
  mouse, Tandy sound and free memory (`MEM` before and after).
- Card install checked with the parent project's `7z t` / extract-diff / `fsck.fat -n` / `cmp` routine, with dated backups.
