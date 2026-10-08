DESKMIND 0.8.3 - AI chat and pictures for the Tandy 1000 TL/3 (and CGA)
===========================================================================

DeskMind talks over the PicoMEM WiFi to MindServer on the PC, which runs
the Qwen AI (chat) and Krea 2 (drawing). Pictures come back ready to show
in 640x200 with 16 colours.

STARTING
  1. On the PC: START-MINDSERVER.bat (NInfer wants a freshly booted PC).
  2. Boot the Tandy with W (WiFi).
  3. Type DESKMIND at any prompt.

  Without WiFi DeskMind starts offline: Gallery and slideshow still work.
  First time: press F5 (Settings) and check the MindServer address/port.
  DESKMIND /NONET = start offline     DESKMIND /NOMOUSE = keyboard only
  DESKMIND /CGA   = CGA mode (below)

CGA MODE (DESKMIND /CGA, automatic on a PC without Tandy 1000 SL/TL video)
  For a plain 286 with a CGA card; on the Tandy it is a way to try it out.
  The screen is 640x200 in black and white, laid out as usual. Pictures
  are made for CGA by MindServer: each one in 320x200 with the best of the
  CGA palettes and background colours, or in 640x200 black and white.
  Thumbnails are black and white. CGA pictures live in PICSCGA\ (the
  Tandy pictures in PICS\ are left alone); Sync fetches CGA versions.
  Qwen knows it is talking to a 286 with CGA, and draws bolder pictures.
  Sound comes from the PC speaker on a non-Tandy PC.

KEYS
  F2 Chat   F3 Create   F4 Gallery   F5 Settings   F6 Slideshow
  F10 or Alt+letter = menus    Tab = next control    Enter = default button
  Esc = cancel a dialog, or stop a reply or drawing

CHAT (F2)
  Type, press Enter. PgUp/PgDn or the scroll bar scroll (click above or
  below its box, or drag the box). Ask for a picture in plain words
  ("draw a castle at sunset") and Qwen draws it. Click a thumbnail to see
  it full screen. Picture = attach a gallery picture and ask about it.
  Chats = reopen or delete saved chats.   New = fresh chat.
  A chat holds about 40,000 characters. When it is full, Continue hides
  the older messages (they stay saved) and goes on.

CREATE (F3)
  Describe a picture. Qwen can improve the prompt first (and let you edit
  it). Krea 2 draws it in about 10 seconds. Again = same prompt, new seed.
  Pictures menu > Enhance rules = how Qwen improves prompts.

GALLERY (F4)
  Grid or List; arrows, PgUp/PgDn or the scroll bar move. Enter views.
  Rename, Delete (this Tandy only) or Everywhere (MindServer too),
  Everywhere and Rename also change the copy in the other mode's folder
  (PICS or PICSCGA). Other PCs keep their own copies.
  Ask Qwen, Sync (fetch pictures made on the PC), Slideshow.
  Shows the newest 500 pictures.

SLIDESHOW (F6, or SLIDES from DOS - no network needed)
  SLIDES [folder] [/D seconds] [/E effect] [/R] [/NOTITLE] [/ONCE] [/LIST]
    /E  0 random, 1 cut, 2 wipe right, 3 wipe down, 4 blinds, 5 interlace,
        6 dissolve, 7 box out, 8 box in, 9 slide in
    /R  random order       /CGA  CGA pictures (automatic on a non-Tandy PC)
  Space/Right next, Left back, P pause, T titles, E try effects, Esc quit.
  "7/16" in the title strip = the 7th-newest picture, as in the Gallery.

FILES
  DESKMIND.CFG   settings           PICS\*.TPI   pictures (about 68K each)
  CHATS\*.TCH    chat transcripts   SLIDES.EXE   stand-alone slideshow
  PICSCGA\*.TPI  CGA pictures (about 17K each)

TROUBLE?
  "No network"                  boot with W
  "MindServer is not answering" start it on the PC; check F5 Settings
  "Qwen ... is not running"     start NInfer on the PC (after a clean boot)
  "Not enough memory" (slides)  start a new chat or restart DeskMind

The full guide is README.md in the "DOS 286 generative AI applications"
folder on the PC. DeskMind uses mTCP and is GPLv3.
