# Tests on the real Tandy 1000 TL/3

## Round 7: DeskMind 0.7 (Phase 7), from any prompt

1. Type `MENU`: the APPLICATIONS section lists DESKMIND and SLIDES. Type `DESKMIND` from `C:\` (W boot): it starts,
   and after Exit you are back in `C:\`. `SLIDES` works the same way, in either boot mode.
2. Slideshow in random order: the "n/16" number belongs to the picture (the newest picture is always 1/16).
3. Chat: send a few messages in an old chat, Exit, start again and reopen it. Everything is there, and long answers keep
   their paragraphs.
4. Long-chat test: a generated 120 KB chat `LONG0001.TCH` is in `C:\DESKMIND\CHATS` (installed 2026-09-30; its
   first question starts "Q0000 a draw pixel 286..."). Open it: it should
   show the newest part within about 1-2 s with "(Earlier messages are saved, but not shown ...)" at the top. Send one
   message: no pause when it saves. Delete it afterwards with the Chats dialog.
5. Gallery Sync still fetches new pictures from MindServer (the list is now read line by line).
6. `TYPE C:\DESKMIND\README.TXT | MORE`: the short guide.

## Round 6: slideshows (Phase 6), from C:\DESKMIND

No network or MindServer needed: slideshows use the pictures already on the Tandy.

1. **Timing of every transition** (normal or W boot):
   ```
   C:
   CD \DESKMIND
   SLIDES /BENCH
   ```
   It plays each transition once, then prints how long each took (also in `SLIDES.LOG`).
2. **Stand-alone slideshow**: `SLIDES` (all pictures in DeskMind's PICS folder, newest first, 8 s each, random
   transitions, looping). Keys: Space/Enter/Right = next, Left = back, P = pause, T = title strip on/off,
   E = step through the effects (the name shows in the strip), Esc or a mouse click = quit.
   Options: `SLIDES /D 5` (seconds), `SLIDES /E 6` (always dissolve), `SLIDES /R` (random order),
   `SLIDES /NOTITLE`, `SLIDES /ONCE`, `SLIDES C:\SOMEWHERE` (another folder), `SLIDES /?` (help).
3. **In DeskMind**: Gallery > **Slideshow** (starts at the selected picture), Pictures menu > Slideshow, or **F6**.
   Settings (F5) has **Effect: ...** (click to cycle; Random = a different one each time) and the seconds per picture.

What to report: the /BENCH numbers, which transitions you like or dislike, and anything that looks wrong at the end
of a transition (every one should end with exactly the picture, no leftovers).

---

## Round 5: DeskMind 0.5 (Phase 5), from C:\DESKMIND

On the PC: **restart MindServer** (close its window, `START-MINDSERVER.bat`). This version sends keep-alives while
Qwen thinks. NInfer and ComfyUI must be up (the menu bar on the Tandy shows "Qwen ok  Draw ok").
Boot the Tandy with **W**, then:

```
C:
CD \DESKMIND
DESKMIND
```

The first start creates `DESKMIND.CFG`, `PICS\` and `CHATS\` there. F2 Chat, F3 Create, F4 Gallery, F5 Settings,
F10 or Alt+letter for the menus, Esc to leave.

1. **Gallery (F4) > Sync**: pulls all pictures from MindServer. Arrows move the selection, Enter or a double-click views
   a picture, and **List** switches to the list view. Try Rename and Delete.
2. **Chat (F2)**: type "Hi! What computer am I on?" and press Enter. You should see a thinking spinner in the status bar,
   then the reply appearing a few words at a time, with a blip.
   Then "Can you draw me a castle on a hill?": after a short reply, the status bar shows "Drawing... %", then the picture
   appears in the chat as a thumbnail. Click it (or scroll to it) to view it full screen.
3. **Ask about a picture**: Gallery > pick one > **Ask Qwen**. Chat opens with the picture attached; ask "What is in
   this picture?".
4. **Chats** button: the saved chats (`CHATS\*.TCH`). Open an older one, delete one, start a New one.
5. **Create (F3)**: type an idea, then Create. Qwen improves it and shows it for editing (OK), then a progress bar
   counts the drawing steps, and the picture shows full screen. **Again** makes another with the same prompt.
   **Enhance rules** lets you edit how Qwen rewrites prompts (it is saved on MindServer).
6. **Settings (F5)**: sound on/off and the other options. Settings are kept in DESKMIND.CFG.

What to report: anything slow, wrong or ugly, and how long a chat reply and a picture take on the Tandy.

---

## Round 4: GUITEST (Phase 4 base libraries), from C:\DMTEST

MindServer must be running on the PC (`START-MINDSERVER.bat`). Boot the Tandy with **W**, then:

```
C:
CD \DMTEST
GUITEST
```

1. It says "Starting the network...", then shows the DeskMate-style screen: a menu bar (File, Test, Help), a
   "Pictures" window (empty the first time), and a status bar.
2. **Sync** (button, or File > Sync from server): a "please wait" box counts the downloads. Then 6 pictures appear in
   the list. The first run creates `C:\DMTEST\PICS` and `C:\DMTEST\DESKMIND.CFG`.
3. With the mouse: click list entries (the thumbnail on the right follows), double-click one to view it full
   screen, and click or press a key to come back. The status bar shows how long loading took.
4. Try **Rename** (type a new title, OK), **Prompt** (shows the prompt the picture was made from), the **Test** menu
   (memo editor, input box, message box), and **Help > About** (shows free memory and EMS).
5. Keyboard: F10 or Alt+F / Alt+T / Alt+H open menus; arrows move; Tab moves between controls; Esc closes.
6. **File > Exit** (or Esc, Yes) takes you back to DOS.

Also run `V640` once more: screen 3's "8x8 character" time should be much lower than the 560 us from round 3
(new table-driven text).

What to report: anything that looks wrong or slow, whether the mouse cursor ever leaves marks behind, and whether
Sync worked. `DESKMIND.CFG` holds the server address if you ever need to change it.

---

# Phase 0: feasibility tests

## Round 3: run from the hard disk (C:\DMTEST)

Round 2 results: V640 and all NETTEST checks work on the real TL/3. Two bugs were left, both fixed in this build:
- The garbage rows at the top of the screen came from V640's log lines being echoed to the console while in graphics mode.
  Log lines now go to the file only while graphics are on.
- NETTEST froze on exit. It now prints `Stopping mTCP...`, `mTCP stopped. Closing the log...` and `Done.`, so the last line shows
  where it stops. Running from C: tells us whether E: (PMDFS) was involved.

Boot with **W**, then:

```
C:
CD \DMTEST
V640
NETTEST 192.168.2.192
```

Check: no garbage rows on any V640 screen (click all the sound buttons a few times), a clean exit, and NETTEST reaching `Done.`
The logs `V640.LOG` / `NETTEST.LOG` are written to `C:\DMTEST`; I'll read them from the image when the card is back.

---

## Round 2 (after the first run on 2026-09-29)

Round 1 findings:
- V640 refused to start graphics. With DOS=UMB, MS-DOS keeps a small block (the UMB link) at the very top of
  conventional memory, inside the 640x200 video RAM, so the memory claim didn't land exactly on 9000h. V640 now keeps that block
  safe during graphics mode, and it writes the whole memory chain to `V640.LOG`.
- NETTEST stopped right after its title line, inside the mTCP startup. It now prints each step
  (`reading mTCP settings...`, `starting the TCP/IP stack...`), so we can see which one hangs.

What to run (boot with **W**):

```
E:
CD \DMTEST
V640
NETTEST 192.168.2.192
```

If NETTEST hangs again, note the last line it printed. Then reboot with W and try once more with mTCP tracing on:

```
SET DEBUGGING=127
SET LOGFILE=C:\NETTRACE.TXT
E:
CD \DMTEST
NETTEST 192.168.2.192
```

Also try running it from C: instead of E: (copy first: `MD C:\DMTEST` then `COPY E:\DMTEST\*.* C:\DMTEST`).
Then bring the card back: `V640.LOG`, `NETTEST.LOG` and `C:\NETTRACE.TXT` (inside the C: image) tell me the rest.

---

## Round 1 instructions

Four things to prove on the real machine before building DeskMind on top of them.
The test files are in `D:\DMTEST\` on the SD card, which the Tandy sees as **`E:\DMTEST`** (PMDFS).
Each program writes a log (`V640.LOG`, `NETTEST.LOG`) into the folder it runs from. Bring the card back
to the PC afterwards, or just tell me what you saw.

## On this PC first

1. **Once:** run `setup-firewall.bat` (it asks for admin). It opens TCP 8286 to your local network only.
2. Run `START-MINDSERVER.bat server`. A console window opens with `MindServer 0.1 listening on 0.0.0.0:8286`.
   Leave it open. (ComfyUI and NInfer aren't needed for these tests.)

## Test 1: 640x200x16 video, mouse, sound (normal boot is fine)

```
E:
CD \DMTEST
V640
```

1. Text screen: check that it says **Tandy 1000 SL/TL/RL (Video II)** and **Reserve video memory: ok (segment 9000)**. Press a key.
2. You'll hear a short rising chime (Tandy 3-voice sound) and see **screen 1**: 16 colour bars numbered 0-15, a striped
   line pattern (left: thin coloured lines on every other row; right: 1-pixel vertical stripes), dither swatches and the
   character set. Things to look for:
   - Is **colour 6 brown or dark yellow** on your monitor?
   - Do the lines look like evenly spaced thin lines, and **not** four wide bands? (Wide bands would mean a different memory layout.)
3. Press **2**: the dithered Might and Magic picture (from `TEST640.RAW`). It should fill the screen at the right shape.
4. Press **3**: speed numbers (full-screen copy, clear, time per character).
5. Press **4**: mouse and sound. Move the mouse; you should see a white arrow and changing x/y. Click **Startup / Reply /
   Image / Error** to hear each effect (or press S R I E). **Sound** toggles sound off and on.
6. Press **5**: three transitions (wipe, blinds, dissolve) with their times at the bottom.
7. Press **Esc** (or Q). You should be back at a normal DOS prompt with a clean 80-column text screen.

If the picture is garbage or the machine hangs at step 2, try `V640 /320` (the 320x200x16 fallback) and tell me.

## Test 2: network (boot with **W**)

Reboot and press **W** at the "Normal or WiFi network" prompt. Then:

```
E:
CD \DMTEST
NETTEST 192.168.2.192
```

It runs five tests and ends with **ALL TESTS PASSED**. Key numbers are the download speed (bytes/s) and whether the
streamed lines arrive a few words at a time. It also overwrites `TEST640.RAW` / `TEST320.RAW` with fresh copies.
(A line "End: heap is corrupted!" at the very end is a known false alarm from the compiler's runtime; ignore it.)

Then run `V640` again in W mode. That checks the video memory claim still works with the network drivers loaded.

## Test 3: ComfyUI (on this PC, whenever the 4090 is free)

1. Start ComfyUI: `START-MINDSERVER.bat noqwen` (or `run_comfy_image.bat`).
2. `helper\.venv\Scripts\python helper\comfytest.py`
3. It generates one 1024x768 Krea2 image with the trimmed workflow (no SeedVR2), prints the time, and writes a
   dither comparison sheet to `helper\out\dithertest_comfy_<seed>.png`.

## What to report back

- Test 1: colour 6 brown/yellow, lines OK?, the numbers from screens 3 and 5, mouse OK?, sound OK?, clean exit?
- Test 2: PASSED or not, the download speed.
- Or just bring the SD card back: the logs are in `D:\DMTEST\`.
