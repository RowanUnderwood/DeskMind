# DeskMind + MindServer: generative AI on the Tandy 1000 TL/3

## What this is

A **separate project** inside `H:\Dos Projects\`. It is not part of the games install in the parent folder.

- **DeskMind** (`DESKMIND.EXE`): a native DOS program for the Tandy 1000 TL/3 (10 MHz 286, Tandy Video II). It does AI chat,
  image generation, a picture gallery and slideshows, talking over PicoMEM WiFi (mTCP) to MindServer.
- **MindServer** (`helper\`): a Python/Qt app on this PC. It talks to NInfer/Qwen (5090, port 1234) and ComfyUI Krea2 (4090, port 8188),
  and does everything too heavy for the 286: JSON, vision uploads, prompt enhancement, dithering to Tandy format and thumbnails.
- `SLIDES.EXE`: a stand-alone slideshow with no network code.

The full plan and design decisions are in **`PLAN.md`**. Read it before changing the architecture.

## Which parent rules apply

The parent `H:\Dos Projects\CLAUDE.md` also loads here. From it, these **still apply**:
- `TANDY-SYSTEM.md` is the hardware reference (memory, video, sound, network, W boot mode).
- Card-image writes: check that `D:\HDD\D62_500M.img` exists, take a fresh dated backup in `..\backup\`, write a work copy with mtools,
  check it (`7z t`, extract-diff, `fsck.fat -n`), copy it back once, `cmp`, then take a new backup.
- DOS files use 8.3 names and CRLF line endings. Nothing new in `C:\` except the one folder `C:\DESKMIND\`.

These **don't apply**: the tiers, the 86Box game tests, and the collection rules. Exception: DeskMind and SLIDES are
rows in the parent's `games.csv` (source `OWN`, recipe field `home="DESKMIND"`). So the games pipeline makes
`C:\PLAY\DESKMIND.BAT`/`SLIDES.BAT`, the MENU.TXT lines and the PDF entries, and never copies or overwrites `C:\DESKMIND`.

## Target facts that drive the design

- **Video:** 640x200x16 (Tandy Video II "mode E") isn't in the BIOS; the program sets the registers directly (`dos\src\video.cpp`, from TANDOTS.ASM).
  The video RAM is **linear at A000:0000, 320 bytes per line, left pixel in the high nibble**, and lives in the top 64K of conventional memory
  (576K-640K). DOS only reserves the top 16K, so `vid_reserve()` claims 576K-624K with a last-fit DOS allocation. The fallback is 320x200x16
  (BIOS mode 9, 4 interleaved 8K banks at B800).
- Pixels are about 2.4x taller than they are wide in 640x200, so images are resized anamorphically **on the helper**.
- **Mouse:** INT 33h is used for position and buttons only (CuteMouse can't draw in this mode). DeskMind draws its own cursor and never
  shows the driver's cursor.
- **Sound:** SN76496 at port C0h. W (WiFi) boot turns off the Sound Blaster.
- **Network:** mTCP (packet driver at INT 60h, `MTCPCFG` env var). Plain HTTP/1.0 to MindServer on port 8286.

## Layout

| Path | What |
|---|---|
| `PLAN.md` | Approved plan |
| `REDDIT.md` | Reddit launch reactions (2026-10-01) and lessons for the next post: title, voice, ideas. Read it before writing any public post |
| `dos\` | DeskMind sources (C-style C++, Open Watcom v2). `build.bat` builds everything into `dos\out\` |
| `dos\spike\` | Phase 0 test programs (V640, NETTEST) |
| `helper\` | MindServer (Python) |
| `tools\ow2\` | Open Watcom v2 snapshot 2026-09-01 (extracted tarball, not installed) |
| `tools\mtcp\` | mTCP source 2025-01-10 (GPLv3). **Read-only**: the DOS build compiles it in place, objects go to `dos\obj\` |
| `tools\dl\` | Downloaded archives |

## Building

- DOS: run `dos\build.bat` (sets `WATCOM`, `PATH`, `INCLUDE` itself). Model `-ml`, code `-0` (8086), so the same EXE runs on 86Box's
  SL/2 (an 8086) and the real 286.
- DeskMind links mTCP, so **DeskMind is GPLv3**.

## Status (2026-09-30)

**Phase 0 (feasibility) is DONE and passed on the real TL/3** (round 3, from `C:\DMTEST`, W boot): V640 in 640x200x16 with
mouse, Tandy sound, image and transitions, 2 sessions and about 50 sound clicks, no garbage, clean exit. NETTEST all 5 tests passed twice
(56-79 KB/s over PicoMEM WiFi; about 1 s per 64,000-byte image) and exited cleanly. ComfyUI: the trimmed Krea2 workflow made 1024x768
in 10.1 s on the 4090. NInfer answers as model `qwen3.8-27b`.

**Phase 1 (MindServer core + GUI) is DONE** (tested on this PC; the Tandy side of it comes with DeskMind in Phase 4-5):
- `mindserver/tpi.py` (TPI format, documented in the file), `store.py` (`data\images\<ID>.png/.json/.640.tpi/.320.tpi`,
  one cached TPI per mode, dropped when the dither defaults change), `jobs.py` (one ComfyUI job at a time, progress),
  `services.py` (health polling, start/stop), `server.py` (endpoints below), `gui.py` (6 tabs).
- Tests: `python -m tests.test_core` (TPI and store, no services needed), `tests.gui_shots` / `tests.gui_gen` (off-screen,
  set `QT_QPA_PLATFORM=offscreen` and `QT_QPA_FONTDIR=C:/Windows/Fonts`; they write to `out\settings_test.json`, never the real settings).
- Endpoints: `GET /ping`, `POST /gen[?mode=&seed=&title=]` (body = prompt) -> `J <n>`, `GET /job/<n>[?wait=1]` (status lines
  `S running <pct> <text>`, then `I <id> <title>` or `E <msg>`, then `D` when streaming), `GET /img/<id>[?mode=320]` (TPI),
  `GET /img/<id>/thumb`, `POST /img/<id>/title`, `POST /img/<id>/del`, `GET /list` (`<id> <yyyymmddhhmm> <mode> <title>`),
  `GET /files/<NAME.EXT>` (from `dos\out`), plus the Phase 0 `/test/*`.

**Phase 2 (Qwen) is DONE** (tested live against NInfer `qwen3.8-27b` and ComfyUI):
- `qwen.py` (streaming, per-request `reasoning_effort` none|low|medium|xhigh; reasoning arrives as `reasoning_content`),
  `text.py` (CP437 + Markdown cleanup that works on streamed pieces, `<draw>` splitter, `Coalescer` that merges T lines to
  about 48 characters or 0.25 s), `chat.py` (ChatStore in `data\chats\<ID>.json`, ChatEngine: reply, vision, draw, enhance).
- Prompts are plain files in `helper\prompts\` (`system_chat.txt`, `enhance.txt`, `vision.txt`), read on every request, and
  editable in the GUI AI tab and via `GET/POST /prompt/<chat|enhance|vision>`.
- Endpoints: `POST /chat[?id=&img=&draw_enhance=]` (body = message; streams `C <chat>`, `S thinking`, `T`, `N`, `S drawing <pct>`,
  `I <id> <title>`, `E`, `D`), `GET /chats` (`<id> <stamp> <count> <title>`), `GET /chat/<id>` (transcript: `>U`/`>A` blocks,
  `!P <img>` attached, `!I <id> <title>` drawn), `POST /chat/<id>/sync`, `POST /chat/<id>/del`, `POST /enhance` (streams
  `S thinking`... `T <prompt>`, `D`), `POST /gen?enhance=1`, `POST /img/<id>/upload` (TPI from the Tandy).
- Measured: chat reply about 2 s (effort low), vision about 2 s, "draw me a 286 AI logo" in chat 9 s (reply, Krea2, dither),
  enhance 3 s, generate with enhancement 11 s.
- Bullet points become CP437 FEh (■). Careful: CP437 byte F9 is "∙", but Unicode "ù" (U+00F9) encodes to 97h.
- Phase 3 (launchers + firewall) is done too. The launcher skips services that already answer.

**Phase 4 (DOS base libraries) is built and tested in 86Box; waiting for a real-TL/3 run of GUITEST:**
- `video.cpp`: line-offset table, table-driven opaque 8x8 text (a 256-entry table per colour pair), `vid_scroll_up`, `vid_text_n`.
- `sys.cpp` (keys K_*, BIOS ticks, EMS through INT 67h), `cfg.cpp` (DESKMIND.CFG), `tpi.cpp` (header, thumbnail,
  `tpi_show` reads straight from the file into video memory with no 64K buffer, rename in place), `net.cpp` + `http_get_file`
  (streams any size to disk) + `net_token`.
- `gui.cpp`: DeskMate-style menu bar with drop-downs (F10 / Alt+letter / mouse), windows with shadows, buttons,
  check box, single-line edit, word-wrapping memo, list with scroll bar, msg_box / input_box / memo_box / busy box,
  status bar, and save-under for dialogs.
- `dos\spike\guitest.cpp` = GUITEST.EXE (pictures list + live thumbnail, View / Rename / Prompt / Delete / Sync, test menu).
- **86Box: the network plus the switch into or out of 640x200x16 crashes 86Box.exe** (2 of 2 runs with the network, 0 of 2 without; same fault
  offset). The real TL/3 ran the network and the mode together fine. For the VM, run `GUITEST /NOMOUSE /NONET` and put pictures into
  `vm\files\DMTEST\PICS\` (build_vm.sh copies them).
- Python on Windows writes CRLF in text mode. Keep `tools\build_vm.sh` LF-only (`sed -i 's/\r$//'`).
- **Real TL/3 round 4: GUITEST all working** (sync, list, thumbnails, view, rename, dialogs, mouse). 8x8 text is now
  164 us per character (was 560). The user saw flicker while typing: the widgets cleared and redrew everything. Fixed:
  widgets now draw in place (`s_full` is only set by `form_draw`), and the memo keeps a cell cache (`s_mcache`) and redraws only changed
  cells. The list and edit redraw padded opaque rows, and scroll bars redraw only when they change (`Widget.sbn/sbtop`).
- `dos\makefile` lists header dependencies explicitly (`HDRS`): `.AUTODEPEND` once missed a gui.h struct change and
  left guitest.obj stale (a big white box on screen). After changing a struct, `build.bat clean` is the safe choice.

**Phase 5 (DeskMind screens) is built; tested offline in 86Box; the real-TL/3 online test is next (TESTING.md round 5):**
- `deskmind.cpp` (main loop, menus, F2-F5), `app.cpp` (paths, picture download/upload/view, status and spinner, ping,
  Settings dialog, About, picture picker), `scr_chat.cpp`, `scr_create.cpp`, `scr_gallery.cpp`. DESKMIND.EXE is about 138K.
- Installed on the card in **C:\DESKMIND** (DESKMIND.CFG, PICS\ and CHATS\ are created on first run).
- Chat: far text buffer, display lines, 5 thumbnail slices of 10 px per picture, scroll by memory move, local `.TCH` files
  (same format as the server transcript), synced to MindServer when opened.
- Build gotchas: `app.h` defines `SCR_H` (a size), so `scr.h` uses the guard `DM_SCR_H`; `cfg.h` uses `DM_CFG_H`
  because the build defines `CFG_H` for mTCP. The makefile's `hdrs.chk` stamp deletes all objects when any header
  changes. Don't list headers as object dependencies: then wmake stops noticing `.cpp` changes.
- VM screenshots: `tools\snap.ps1` is DPI-aware (with display scaling above 100% the parent's snap.ps1 captured only the
  top-left part). 86Box still crashes now and then around this video mode even without the network; it's harmless, just retry.
- **Real TL/3 round 5 (DeskMind 0.5): everything worked**, with 9 fix requests. Fixed in 0.5.1:
  1. the Qwen-down error now names the MindServer PC's LAN IP (`qwen.not_running()`, `lan_ip()`), not 127.0.0.1;
  2. the Dither Lab raw-pixel view explains why it looks squashed;
  3. vision gets the original **and** the dithered Tandy version (`_vision_parts`, PNG for the dithered one), and the
     most recent picture in a chat (attached or drawn) **stays in view** for follow-ups (`_recent_picture`);
  4. the Gallery list view had no titles (`list_set` was passed 0 as the item function);
  5. Enter in the Gallery now views (focus returns to View after every action; switching to the list focuses the list);
  6. pointer freezes: the idle `/ping` blocked for up to 10 s. It now runs every 2 min with 3 s timeouts, and
     `net_wait_hook = gui_pump` keeps the pointer moving in every network wait;
  7. `msg_box` buttons size to their label;
  8. "No answer from ..." after a chat drawing: mTCP has **one socket**, and downloading the picture mid-reply killed the
     reply's connection. Pictures are now downloaded after the reply ends. **Never open a second HTTP connection
     while one is streaming**;
  9. the pointer moved in 8-pixel steps: CuteMouse rounds to text cells in mode 3. `mouse_init` shows it BIOS mode 6
     during the reset (BDA 0040:0049) so it reports every pixel.
  Also: the pointer is hidden while a chat reply streams (a stale-pointer glitch showed "jus}" once), and Create offers
  "Draw it as typed" when Qwen is down.
- User confirmed 0.5.1 on the TL/3 (smooth pointer, all fixes working).

**Phase 6 (slideshows) is built; next is the real-TL/3 test (TESTING.md round 6):**
- `slide.cpp`: 9 transitions (cut, wipe right, wipe down, blinds, interlace, dissolve, box out, box in, slide in) made of
  plain memory copies from a 64000-byte buffer to A000. One picture buffer: the next picture preloads while one shows,
  and the 3-second title strip is repaired by re-reading its 11 lines from the file (`tpi_lines_to_screen`).
- `slides.cpp` = SLIDES.EXE (36K, no network): reads DESKMIND.CFG next to it for PICS, delay and effect; `/D /E /R /NOTITLE
  /ONCE /BENCH /NOMOUSE`. In DeskMind: Gallery Slideshow button, Pictures menu, F6; Settings has an effect cycle button.
- 86Box crashes on `SLIDES /BENCH` (2 of 2) around the switch back to text mode. Get the timings from the real TL/3.
- Real TL/3 `SLIDES /BENCH`: cut 110, wipe right 275, wipe down 110, blinds 550, interlace 275, dissolve 330,
  box out 605, box in 220, slide in 935 ms.
- 0.6.1 fixes: the Settings dialog had 15 controls in a 14-slot array (the overflow corrupted port/seconds/effect: smileys,
  dead Cancel), so now `MAXW 20` plus a guard. New `slide_shuffle` setting ("Slideshow: random order") used by the Gallery
  (the selected picture first, then random) and SLIDES (`/R` forces it). `slide_make_order()` seeds from ticks^PIT.
  `SLIDES /LIST` prints the play order (tested in DOSBox: /R differs every run). The user's CFG had `slide_effect=1`
  (Cut), probably saved by the bug; it was reset to 0 (Random) on the card.

**Phase 7 (0.7: README, launchers, limits) is DONE and installed on the card** (`backup\..._after-deskmind-07.img`):
- `README.md` (full user guide) and `dos\README.TXT` (80-column CRLF version in `C:\DESKMIND`).
- Launchers go through the parent pipeline (see "Which parent rules apply"). `tools\card_install.ps1 -Also PLAY` lets an
  install change other root folders; a file spec `src=PLAY/NAME` goes to `C:\PLAY\NAME`.
- Slideshow order (0.8.4): without random, pictures play **oldest first, newest last**, then loop (`slide_make_order` reverses
  the newest-first lists; the Gallery starts at the selected picture and goes on to newer ones). Counter "n/total" = the
  n-th oldest (`count - order[pos]`), also in random order. `SLIDES /LIST` says "oldest first" (checked in DOSBox).
  On the card 2026-10-09 (backups `..._before/after-deskmind-084.img`); user confirmed the new order on the real TL/3 (2026-10-09).
- **Measured memory (real TL/3, 0.8.4, Normal boot = offline, 2026-10-09):** Help > About "Free memory" (largest DOS block,
  AH=48h) = **160K** at startup (Chat screen open), **61K** after Gallery thumbs + a 2-picture slideshow. The 99K drop is
  mostly the 64K slideshow buffer, which Watcom's far heap keeps for reuse instead of returning to DOS, so About under-reports
  the real headroom after a slideshow. Not yet measured in W (online) mode, where mTCP's buffers (~54K: 20 packet buffers,
  10 send buffers, 8K receive) should come off these figures. EMS: 4080K free, unused. Put big new data in EMS.
  W boot (online): **83K** at startup, **60K** after a chat with a drawing: below the 64K that a heap slideshow buffer needed.
- **Slideshow buffer in EMS (0.8.4, 2026-10-09).** `slide.cpp` `buf_alloc()` maps 4 EMS pages over the whole page frame
  (`ems_frame_map`, D000 on the Tandy) and reads pictures straight into it; no EMS -> an exact `_dos_allocmem` block, freed
  afterwards. Measured in DOSBox: a 64000-byte `_fmalloc` took **101,552** bytes from DOS (Watcom grows its last heap segment
  to 64K, then adds a new 64K one) and `_fheapshrink` didn't give it back. `_heapwalk`/`_fheapwalk` return `_HEAPBADBEGIN`
  in this OW snapshot (same bug as `_heapchk`), so heap-internal free space can't be measured. About now adds "slideshow
  in EMS / fits / needs 63K" (`slide_mem_note`). `/NOEMS` on DESKMIND and SLIDES keeps the buffer out of EMS.
  Same release fixes a start bug from the order change: SLIDES passed start 0 (= newest), so it began on the newest
  and `/ONCE` stopped after one picture. SLIDES now passes -1; the Gallery passes `s_sel` only if it is > 0 or random.
  86Box `vm\dm_ems` (SL/2 + Lo-tech EMS, `[Other peripherals]` frame C0000 because XT-IDE's ROM is at D000; LTEMM.EXE
  /p:C000 /i:260 in CONFIG.SYS, image `vm\dm_ems.img` made from dm_test.img): SLIDES from EMS and with /NOEMS both
  play 6/6 oldest first; MEM conventional and EMS free identical before and after. 86Box crashed (0x53dee3) on some runs.
  On the card (backups `..._before/after-deskmind-084-ems.img`); Real TL/3 W boot (2026-10-09, photos 20261009_145823/150243/150349): About says "slideshow in EMS"; free
  **160K** at startup, **123K** after a chat with a drawing, **89K** after a Gallery slideshow (5 shown, worked). Each later
  drop is not a leak we can see: Watcom never returns freed memory to DOS, and dialogs' save-unders (About itself is about
  34K) leave reusable holes, so About's DOS figure is a lower bound. The earlier 83K W-startup reading (0.8.4 before EMS)
  is unexplained; probably not a fresh start. Check later whether the figure levels off with use.
- Limits never fail silently now:
  - Gallery `MAX_PICS` 500, picker 200, chat list 100: all keep the **newest** entries when full (DOS returns files in
    folder order) and say "newest N of M".
  - Sync reads `/list` line by line (no 8000-byte buffer) and fetches after the list closes; it stops when the gallery is full.
- Chat limits (40,000 text bytes, 300 messages, 1400 display lines): `chat_room()` returns 0 = ok, 1 = nearly full (a notice
  once per chat), or 2 = full (Send offers Continue, which saves and reloads and so trims, or New). Text that didn't fit
  sets `s_cut`, and a notice follows the reply.
- Long chats open at their **newest part** (about half the buffers). Only the end of the file is read (`tch_fit` keeps a ring of
  the last message starts), the title comes from `tch_title`, and a "(Earlier messages ... not shown)" notice comes first.
- `save_chat` **only appends** the messages after `s_saved`, at byte `s_end` (then `chsize`), so the hidden older part is never
  rewritten. Two bugs from before 0.7 are fixed: a reopened long chat would have lost its newest part on the next save,
  and paragraphs over 199 characters got hard line breaks on every save.
- **Speed lesson (8 MHz 8086 VM): Watcom `fgets` costs about 1000 cycles per character** (20 KB: 2.4 s). An `ftell` per line
  and the 512-byte stdio buffers made it worse: the first 0.7 draft took 34 s to open and 25 s to save a 120 KB chat.
  `rd_line()` (fread in 2K chunks plus memchr) brought that to 1.3 s to open and 55 ms to save. Avoid fgets on anything big.
- `dos\spike\chattest.cpp` + `chattest.bat` build CHATTEST.EXE, a text-mode test of load/save (it includes scr_chat.cpp).
  Checked in DOSBox and the VM: every transcript stays byte-identical, with only the new messages appended.
- VM key injection (`keys.ps1`) goes to the foreground window, so the user working on this PC swallows keys. Prefer test
  programs that run by themselves from TEST.BAT and print their results.
- Real TL/3 round 7: launchers, MENU, README.TXT, Sync and the 120 KB test chat (opens fast, no pause on save) all good.
  0.7.1 fixes the one bug found: the scroll thumb was drawn above the chat panel, up into the menu bar. `(h - th) * top`
  overflowed a 16-bit int (124 * 550), in `scr_chat.cpp` and in the gui.cpp list (Gallery list past about 250
  pictures). Both now use long math. **Watch for int overflow in any product with a line or picture count.**
  About now lists the limits (newest 500 pictures, newest 100 chats, about 40,000 characters per chat).
- MindServer GUI (2026-10-01): Gallery "Export as PNG…" / "Export all as PNGs…" (default folder `docs\`) save the TPI as
  the Tandy shows it, 1280x960 4:3 (`dither.screen_shot`, `Store.export_png`, names from `store.file_slug(title)`).
  Services "Shut down all" stops NInfer (stop bat) and ComfyUI (port kill) unless they're down, then closes MindServer.
- **Qwen model (2026-10-04): thinkingcap by default.** NInfer now runs its native Windows backend (`launch-ninfer.bat`
  `BACKEND=windows`, v3 models). Setting `services.ninfer_model` (`thinkingcap` | `full` | blank = the bat's own `WIN_MODEL`,
  which is `full`), chosen in the Services tab. `Services.start()` and `START-MINDSERVER.bat` (via `python -m mindserver.services
  model`) preset `WIN_MODEL` in NInfer's environment. Both models answer as `qwen3.8-27b`; the only way to tell them apart is
  `H:\Ninfer Qwen\logs\current-model.txt` (plus `current-context.txt`), written by the bat on every start. `services.ninfer_running()`
  reads them; a running NInfer with the other model is reused **with a warning only** (log, Services label, `services check`
  in the bat), never restarted, because another app (Video Narrator) may be using it. thinkingcap = 65,536 context with 1 GiB
  spare (needs ~28.5 GiB free on GPU 0); full gets 131,072. DeskMind's biggest request is ~15k tokens.
  **Only one `system` message, first.** The thinkingcap chat template raises (HTTP 400 `invalid_prompt`, chat_template.jinja
  line 106) on a later system message; `full` tolerated it. Vision used to add a second one; `_vision_text` is now appended to
  the leading system message. `tests\qwen_smoke.py` (live, needs all services) runs chat, memory, Markdown cleanup, `<draw>`,
  vision follow-up, attached vision and enhance through MindServer's HTTP API and writes `out\qwen_smoke_<model>.json`;
  it deletes its chats and pictures. 2026-10-04: 8/8 on thinkingcap (twice) and on full; replies 0.5-1.5 s, draw 8 s.
- **Tandy facts in `system_chat.txt` (2026-10-04).** Both models called the TL/3 a portable and thinkingcap said "1 MB of RAM",
  so the prompt has an "About the Tandy" block (use when relevant, don't recite, say when unsure), sourced from TANDY-SYSTEM.md.
  The TL/3 is from **February 1991** (Tech Monitor, 19 Feb 1991), not 1989 as the prompts said; `enhance.txt` fixed too.
  Old prompts: `helper\out\*.txt.bak-20261004`. Smoke test gained "tandy facts" (640 / 1991 / 720, no laptop) and
  "no spec recital" (Paris answer without specs): 10/10 on thinkingcap.
- **Chat pictures in the Gallery (2026-10-04).** Chat downloaded drawn pictures into PICS after the reply but never called
  `gallery_rescan()` (Create does), so they appeared only after a Sync or restart. `scr_chat.cpp` now rescans when it
  downloaded any. Installed on the card (only `DESKMIND\DESKMIND.EXE` changed; backups `..._before/after-deskmind-chatgallery.img`);
  user confirmed on the real TL/3 (2026-10-04).
- **Missing chat pictures (2026-10-04, chat DF21D117 / picture C7B1069D).** The server had the picture; the Tandy's download after the
  reply failed and was ignored, leaving a grey thumbnail and "Could not show this picture". Cause of that one failure unknown (no log, possibly WiFi),
  but `net.cpp` chose local ports with an **unseeded `rand()`**, so every run reused the same port sequence; now `next_port()` starts at
  BIOS ticks ^ PIT and counts up. Failed downloads now add a chat notice; a missing picture's caption says "click to download" and
  `app_view_pic()` downloads any missing picture before showing it (then `gallery_rescan()`). Qwen had also said "There you go" without a
  `<draw>` (it could see the picture through `_recent_picture`); `system_chat.txt` now forbids claiming a picture without a draw and
  says to click to re-download (replayed on test copies 3/3 honest). Old prompt: `helper\out\system_chat.txt.bak-20261004b`.
  Installed on the card (only `DESKMIND\DESKMIND.EXE` changed; backups `..._before/after-deskmind-picretry.img`); user confirmed on the real TL/3 (2026-10-04).
  `card_install.ps1 -Files` needs **absolute** paths (a relative one fails in WSL as `/mnt/./...`).
- **Scroll bars (2026-10-04).** Shared code in `gui.cpp`: `sb_thumb` (box geometry, long math), `ui_scrollbar`, and
  `sb_track` (press until release: drag the box, or page toward the click and repeat after 6 ticks until the box reaches
  the pointer; callback `apply(ctx, top, dragging)`, with a final `dragging = 0` call after a pause or release).
  Used by Chat, the list widget (Gallery list, chat picker, Chats dialog) and a new per-page bar on the Gallery grid
  (x 622; it redraws its 6 disk thumbnails only when the pointer pauses or on release). Lists set `WF_FREEVIEW` when the
  bar moves the view so `draw_list` stops pulling it back to the selection; keys/clicks/`list_set` clear it. The memo
  only got the click-above/below fix (its view follows the cursor). New `vid_scroll_down` makes upward chat scrolls
  of less than a page cheap. 86Box (keys only) checked grid bar + chat line scroll; **dragging confirmed on the
  real TL/3 (2026-10-04)** (86Box mouse can't be scripted). CuteMouse 1.9.1 has no wheel; 2.x would be needed for wheel scrolling.
- **Fake "[You drew picture ...]" replies (2026-10-04, chat F483488D).** `_history` used to append
  `[You drew picture ID: prompt]` to Qwen's own past messages; Qwen sometimes copied that line (with an invented ID)
  instead of writing `<draw>`, so nothing was drawn. History now shows past drawings as `<draw>PROMPT</draw>` (prompt
  from the chat, else the picture's metadata, since Tandy-synced transcripts keep only titles) and strips old bracket
  lines from saved text. `DrawSplitter` also accepts the bracket form as a draw request (safety net, ID dropped).
  Prompt: "click to download" only if the earlier reply really drew it, otherwise draw it now (old prompt
  `helper\out\system_chat.txt.bak-20261004c`). Replay of the failing turn: 3/3 real drawings; smoke test 10/10.
  Tests in `tests/test_core.py` (`test_old_draw_note`, `test_history`). Gallery grid bar got a white sunken frame
  (on the grey window its track was invisible); card backups `..._before/after-deskmind-gallerybar.img`; user confirmed both on the real TL/3 (2026-10-04).
- **Workflow dropdown (2026-10-08).** The Generation tab picks any API-format workflow in `helper\workflows\` (`comfy.list_workflows()`);
  it applies to the GUI test, Create and chat draws. `comfy.find_roles()` finds the patched nodes by type, not ID: the single KSampler,
  the CLIPTextEncode upstream of its positive input, the size node (WLSH ratio latent, `ResolutionSelector` or `EmptyLatentImage`,
  always 4:3 landscape from "short side"), SaveImage (prefix `DeskMind/dm`) and an optional rgthree LoRA loader. Steps/size/LoRA live in
  `comfy.profiles[<workflow>]` (`Config.comfy_profile()`, falls back to the old flat values). `krea2_fine_v5.json` = the user's
  "krea2SimpleFine (b)(API)" export, unchanged (UI version in `workflows\ui\`): 12 steps, no LoRA, 1048x784 at short side 768,
  about 25 s vs 18 s for turbo, photographic rather than graphic. The UI file's subgraph has stale inner model names
  (`Krea2_FineV4_INT8`, `qwen3vl_4b_int8_convrot`); harmless while the promoted outer values are used.
- `tools\card_install.ps1`: mtools calls get `</dev/null` (a name-clash question once hung it for 40 minutes), steps are
  timestamped, and it logs to `tools\card_install.log` when run as `... *> card_install.log`.
- **Delete Everywhere / rename (2026-10-04, on the card: backups `..._before/after-deskmind-deletefix.img`).** Delete Everywhere used to delete the
  local file first, then wait out the 10 s connect timeout and ignore the result (picture came back on the next Sync). Now
  `app_post_short()` (3 s connect) asks MindServer **first**; 200/404 = ok, otherwise "Delete here|Cancel" with the error.
  Rename warns when MindServer kept the old title. Also fixed `q[100]` overflow in the delete question (40-char titles).
- **CGA mode (plan: `~\.claude\plans\pasted-content-id-028a-delete-everywher-parallel-quasar.md`).** For a plain 286 with
  CGA (card/monitor not known yet; RGB assumed). GUI will be CGA mode 6 (640x200x2, same layout, mono theme); pictures
  320x200x4 or 640x200x2, chosen per picture. **Server side done (2026-10-04):** mode `cga` (`?mode=cga` on `/gen`, `/img`,
  `/img/<id>/thumb`, `/chat`, `/enhance`); `dither.render()` returns `Dithered` (idx, layout, rgb); `cga_choose()` scores
  6 palettes x 16 backgrounds + mono with Lab dither-mix error (lightness weighted 2x: otherwise dark grey beat black
  backgrounds) and a grain penalty, about 0.3 s. TPI modes 3 = cga4, 4 = cga2 (byte 90 palette, 91 colour); CGA thumbs are
  1-bit 160x50 (1000 bytes); image data is 16000 bytes either way. `TpiInfo.image_idx()/thumb_idx()/rgb()` replace raw
  `D.unpack`. Prompts `*_cga.txt` used when mode=cga (generic 286 facts: fill in the real machine later). Dither Lab has
  CGA palette/background/mode-5 controls; "Save as default" never stores mode cga (DeskMind on the Tandy asks without
  `?mode=`). Live check on a second instance (port 8290): CGA facts, CGA draw (cga2 lighthouse), visio  **DOS side done (DeskMind 0.8, on the card 2026-10-04, backups `..._before/after-deskmind-cga.img`):**
  - `video.cpp`: VM_CGA2 (BIOS mode 6, GUI) and VM_CGA4 (mode 4/5, pictures), B800 with 2 banks of 80-byte lines. All
    primitives are generic over 4/2/1 bpp (`vid_bpp`, `vid_shift`, `Span` masks; scrolls copy exact pixels). Logical
    colours map per mode (`s_map`): mono = LGRAY(7) and 9-15 white, rest black; 4-colour = nearest palette entry.
    `vid_switch()` changes graphics mode keeping the saved text mode; `vid_cga_palette(pal, bg)` writes 3D9 and uses BIOS
    mode 5 for the cyan/red sets. **A Tandy works the same way**: in its CGA modes the colour select logic runs first and
    its result then goes through the palette registers (BIOS identity). Writing palette regs 10h-13h did nothing (86Box
    `vid_tandy.c` confirms). No `vid_reserve()` and no tail protection in CGA.
  - **Characters 128-255:** a plain AT leaves INT 1Fh empty, so check marks, accents and Qwen's bullets were '?'. Built-in
    `fonthi.cpp` (from public-domain font8x8, `tools\make_fonthi.py`, a few glyphs hand-drawn) is used when INT 1Fh is
    0 or points at uniform bytes. The Tandy keeps its BIOS font.
  - `gui.cpp`: mono bevels are outlines, the desktop is a 0xAA/0x55 checkerboard, focused checkboxes get an underline.
  - `tpi.cpp`: `tpi_cga` filter (a CGA run ignores Tandy files and vice versa), `tpi_set_mode()`, `tpi_thumb_pitch()`.
    The chat thumbnail slices used `tw / 2`.
  - `cfg_cga` (= `/CGA` or `vid_detect() != VT_SLTL`), `cfg_pics()` (PICSCGA via new `pics_cga` setting, so saving
    settings never overwrites `pics`), `mode=cga` on `/gen`, `/chat`, `/enhance`, `/img`; `app_pc()` says "PC" in
    messages; `app_gui_mode()` returns to mode 6 after a picture or slideshow.
  - `sound.cpp`: Tandy chip only when `vid_detect() != VT_OTHER`, else PC speaker (PIT ch 2, voice 1, cut at decay 9).
  - SLIDES: `/CGA` is read before the folder default; transitions scale with `U = vid_pitch / 80`; mode/palette switch
    per picture (blank first when only the palette changes); 16000-byte buffer in CGA.
  - VM: `vm\dm_at` = AMI 286 10 MHz + real CGA, disk `vm\dm_at.img` (made by `build_vm.sh`). Its BIOS stops at an
    "EXIT FOR BOOT" menu: send Enter at about 30 s. 86Box ran CGA mode with network and mouse without crashing (Sync of 65
    CGA pictures, live chat). Checked: AT/CGA (palettes 0, 1, mode 5, mono, slideshow, SLIDES, chat thumbs, dialogs),
    SL/2 `/CGA` (palettes), SL/2 normal 16-colour regression. Real TL/3: TESTING.md round 8.
  - **Real TL/3 (2026-10-04): two colourful photos came out black and white** (they read as grey on the CM-5). Not a
    palette bug: mono bias 0.85 won near-ties (cost 29.1 vs 28.2). Now black and white needs chroma < 3 (greyscale
    picture, `GREY_MONO_BIAS` 0.85) or must beat colour by `cga_mono_bias` = 1.1; only the noir silhouette of 74 stays
    mono. Server `*.cga.tpi` caches were deleted; the Tandy keeps old files until they are deleted there and re-synced.then SLIDES `/CGA`.
- **0.8.1 (2026-10-04): Sync chime, both picture folders.** Real TL/3: a Sync that fetched other-mode pictures left the
  chime's last note ringing until a button click. The cause isn't proven (the INT 1Ch handler must have missed ticks; suspect the
  folder scan right after `snd_play`). The fix: Gallery Sync plays the chime **after** `gallery_rescan`/`gallery_draw`, and
  `snd_poll()` (called from `gui_poll`) stops any sequence still running 3 ticks past its length, by the BDA tick counter.
  Delete Everywhere also removes the copy in the other mode's folder (`app_twin_path`, `cfg_pics_other`: PICS vs PICSCGA),
  and Rename retitles it. MindServer's delete/rename already covered every cached mode. Plain Delete stays this-mode only.
  Gallery Rename/Delete results go through `note()`, because `gallery_draw()` used to overwrite their status lines at once.
  86Box: rename (SL/2) and Everywhere (AT/CGA with network) checked both folders and the server. Chime: TESTING.md round 8.1.
- **0.8.2 (2026-10-04): stuck sounds, take two.** With 0.8.1 other sounds still stuck at **full volume** now and then (Chat
  open, reply beep, Delete click), each until the next sound. The watchdog never fired, so those sequences had ended and the
  silence had been sent: the chip lost writes, so missed ticks were not the cause. (The 0.8.1 "folder scan" theory was wrong
  for these.) The likely cause is back-to-back port C0h writes: the SN76496 needs about 9 us per byte, and a lost latch byte
  sends the next data byte to the wrong register. A PicoMEM snooping C0h is the alternative. Fix in `sound.cpp`: `sn_write`
  waits about 12 us (12 reads of port 61h), and the tick handler rewrites all four volumes on every tick (`refresh()`; on
  the PC speaker it forces the gate off when idle). `snd_mode` (0 old, 1 paced, 2 paced + refresh) exists only for
  `SNDTEST.EXE` (`dos\spike\sndtest.cpp`), which stress-plays the effects with LISTEN pauses and an optional disk load.
  86Box can't reproduce the stick (smoke test only). Real TL/3 results: TESTING.md round 8.2.
- **0.8.3 (2026-10-04): stuck sounds, the real cause.** 0.8.2 didn't help, and SNDTEST (no network, buffered reads)
  never stuck. A new SNDTEST with real disk writes made the 86Box Tandy crash ("Divide overflow") on the first sound. The
  disassembly (capstone; `wdis` crashes) showed it: Watcom's large model assumes **SS = DGROUP**, so the static helpers called
  from the INT 1Ch handler (`start_note`, `refresh`, `v_silence`/`v_vol` via `s_spk`, `sn_write` via `snd_mode`, `ring_put`)
  reached our variables through **SS**. A tick that lands in DOS, the disk BIOS or the packet driver (another SS) read and
  wrote foreign memory: a garbage `s_spk` sent the end-of-sound silence to the PC speaker instead of the chip, so the note
  rang on until the next sound. The speaker path arrived with 0.8 (CGA), which is why the user only noticed then; network
  and disk work make foreign ticks common. **Fix: `sound.obj` is compiled with `-zu`** (explicit makefile rule). It now has 0
  `ss:` references, and SNDTEST with disk and network load runs clean in 86Box (miss 0). The 0.8.1/0.8.2 changes (chime after
  scan, watchdog, paced writes, per-tick refresh) stay as harmless safety nets.
  Diagnostics kept: `snd_isr_count`, a 64-event ring (PLAY/END/STOP/WATCHDOG/NET/STALL/F9/DISK+/DISK-/SETTLE) with a STALL
  detector in `snd_poll`; **F9** anywhere (`gui_f9` hook) appends `snd_diag()` to `SOUND.LOG` in steps 1 (snapshot only),
  2 (chip silence), 3 (speaker gate off), 4 (PIT 2 + multiplexer reset). `net_event_hook` logs connections, disk markers
  wrap the rescan, delete, chat load/save/list and downloads, and `snd_settle()` waits for a playing sound before them.
  `SNDTEST [/D] [/N] [/Q] [/Rn] [ip]`: D = temp-file writes + PICS header reads, N = 1 MB `/test/bytes` loops, Q = no
  sounds, Rn = quit after n rounds (unattended VM runs). 86Box still crashes now and then at 0x53dee3 (known); it happened more
  often right after `build_vm.sh`, so wait a moment or retry.
- `dos\out\V640.EXE`: 640x200x16 mode set, memory claim (segment 9000h), colours, image, speed, mouse + Tandy sound, transitions.
- `dos\out\NETTEST.EXE <ip>`: ping, 3x64K download, streamed lines, upload/echo, downloads `TEST640.RAW`/`TEST320.RAW`.
- `helper\`: MindServer skeleton (`/ping`, `/test/*`), dither pipeline, ComfyUI client, `dithertest.py`, `comfytest.py`.
- 86Box results (SL/2, 8 MHz 8086): full-screen copy 64 ms, clear 65 ms, 8x8 char 1.7 ms, wipe 770 ms, blinds 550 ms,
  dissolve 935 ms. Download through the emulated NE1000 about 97 KB/s (64,000 bytes in 0.65 s).

## Known quirks

- **Interrupt handlers: compile with `-zu`.** Watcom's large model assumes SS = DGROUP and reaches static data through
  SS. Any function an interrupt handler calls runs on the interrupted program's stack, so it must be in a module compiled
  with `-zu` (see the `sound.obj` rule in `dos\makefile`). Check the EXE with capstone for `ss:[` in that code segment.

- **Open Watcom v2 snapshot `_heapchk()` reports BADNODE from program start**, even in an empty program. So mTCP's
  "End: heap is corrupted!" at exit is a false alarm. Don't chase it in our code.
- **86Box 6.0 crashes intermittently** (access violation in 86Box.exe, always offset 0x53dee3) around V640 when CuteMouse was
  reset by `mouse_init()`. The crash comes when V640 leaves graphics mode. `V640 /GO /NOMOUSE` ran 2 of 2 clean; with the mouse,
  3 of 4 crashed; sound made no difference. The original TANDOTS.COM never crashed. Treated as an emulator quirk: use
  `/NOMOUSE` for automated VM runs (`tools\vm_matrix.ps1`). (Writing CRTC reg 9 first in `set_640()` also removed one crash path.)
- **Real TL/3, round 1:** with DOS=UMB, MS-DOS keeps its UMB link block (owner 8, "SC") at the very top of conventional memory,
  inside the video RAM. So the last-fit claim lands just below 9000h. `vid_reserve()` now accepts that when the gap holds only DOS
  system blocks, and `vid_open()`/`vid_close()` save/restore that "tail" and unlink UMBs while in graphics mode.
- **Real TL/3, round 2 (from E:):** V640 worked in both boot modes (claim 8FFF-9BFF plus the 1-paragraph SC block). NETTEST passed
  all 5 tests at 79 KB/s but froze while exiting. Real-machine speed: full-screen copy 77 ms, clear 88 ms, 8x8 char 0.56 ms, wipe 330 ms,
  dissolve 330 ms. Colour 6 shows brown-orange on the CM-5.
- **Never print (printf/stdout) while a graphics mode is on.** The BIOS still thinks it's in text mode and writes the characters into the
  video RAM (round 2 "garbage rows", one per log line). DeskMind must route all messages through its own GUI.
- In the 86Box VM the network card is an **NE1000 at 320h, IRQ 3** (Crynwr `NE1000.COM`), because XT-IDE uses 300h. The Crynwr
  NE2000 driver got no packets on the `ne2k8` card. The PicoMEM's own `NE2000.COM` only works with a PicoMEM.
- Key injection into 86Box sometimes drops a key; V640 also quits on Q.

## Testing

- The 86Box SL/2 (`..\86Box\`) emulates the 640x200x16 mode (Video Array reg 5 bit 0) and the Tandy sound chip. It's the first stop for video and sound.
- DeskMind VM: `vm\dm_sl2\` (SL/2 + XT-IDE + NE1000 on SLiRP; MindServer is `10.0.2.2` from inside). `wsl sh tools/build_vm.sh`
  rebuilds `vm\dm_test.img` from `..\86Box\test\base.img` + `vm\files\` + `dos\out\*.EXE`. `tools\vmrun.ps1 -Tag X -Shots "60,80"
  -Keys "56={ENTER};62=3"` boots it off-screen and saves screenshots in `vm\shots\`. Read logs back with
  `mtype -i vm/dm_test.img@@32256 ::/DMTEST/V640.LOG` (WSL). MindServer must be running on this PC.
- Real TL/3: test builds go into **`C:\DMTEST`** on the hard-disk image, not the SD card via E: (the user's choice after round 2:
  PMDFS behaves differently from a mounted disk image). Use `tools\card_install.ps1 -Tag <what>`. It takes a dated backup,
  mtools-writes a work copy, checks it (fsck.fat -n, 7z t, 8.3-only via `tools\check_83.py`, full extract `diff -rq`), copies it back once,
  compares, and saves an "after" backup in `..\backup\`. mtools is used instead of pyfatfs (pyfatfs has the multi-file FAT bug;
  see `H:\Tandy\CLAUDE.md`). The old `D:\DMTEST` folder on the card holds round 1-2 logs only.
- NInfer can only start from a clean boot (thinkingcap needs about 28.5 GB free on GPU 0, full about 24.5). Don't try to start it during a session without asking.
