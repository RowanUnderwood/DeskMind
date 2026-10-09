# Project purpose

Use MIDI-GPT on the modern Windows workstation to compose short complete
pieces and cracktro music, then adapt/export it for Tandy 1000 TL/3 three-voice
sound. Generation happens on the RTX 3090; the Tandy only plays exported data.
Keep this file current as new setup details, failures, workarounds, or hardware
results are learned. Distinguish observed results from assumptions.

## DeskMind integration (implemented 2026-10-09, DeskMind 0.9.0)

The design below was built in a staged MVP (no revisions/locks/undo yet). Our code is in `worker/` (tracked by
the DeskMind repo; MIDI-GPT/, .python/ and MidiPlayer6/ are git-ignored there):
`t3.py` (format, Builder, read/validate, WAV preview), `recipes.py` (cracktro, adventure, dungeon),
`compose.py` (generalised compose-cracktro.py), `compile.py` (generalised export-tandy.py + bar-map drums),
`music_worker.py` (HTTP on 127.0.0.1:8287; `launch-music-worker.bat`), `test_compile.py`, `try_recipes.py`.
Observed: with drums off, compile.py reproduces outputs/tandy/RASTER.T3 byte for byte; model load 0.9 s,
a song 3-6 s on the 3090. The original scripts and outputs are unchanged. DeskMind side: see the DeskMind CLAUDE.md.
The venv's pyvenv.cfg home was updated to the moved .python folder (backup pyvenv.cfg.bak-20261009).
Not yet heard on the real TL/3 inside DeskMind (only DOSBox/86Box register-level checks).

## DeskMind integration proposal

`DESKMIND-MUSIC-DESIGN.md` contains the 2026-10-04 reference proposal requested
by the user: DeskMind chat/Qwen directs a structured MindServer composition
backend and separate 3090 MIDI-GPT worker; shared DOS playback for DeskMind,
SLIDES and an optional offline companion. This is a design, not implemented.
Important inspected-source issue: DeskMind's current sound.cpp refreshes all
four PSG volumes even when idle, so integrated music needs exclusive sound
ownership. Our standalone foreground player also needs a background dispatch
design for slideshow transitions. Measure actual runtime memory before deciding
whether playback stays inside DeskMind. Read the design for context and staged
implementation; preserve the external DeskMind project unless asked to edit it.

## Workspace and preserved files

- Workspace: `H:\Dos Projects\DOS 286 generative AI applications\Labtext2midi` (moved 2026-10-09 from `H:\Labtext2midi`;
  historical name; Text2midi was removed at the user's request). Older generated reports and
  DOSBox `.conf` files in outputs/ still name the old path: historical records, not live paths.
- This folder sits inside the DeskMind repo. MIDI-GPT/ (its own upstream git clone), .python/ and
  MidiPlayer6/ are git-ignored there; DeskMind's music worker lives in worker/ (tracked).
- `MIDI-GPT/`: active source checkout, local environment, model, scripts, songs.
- `.python/`: shared Python 3.11.15 runtime required by MIDI-GPT's environment;
  retain it. A junction with the shorter Python version name points at this runtime.
- `launch-midi-gpt-3090.cmd`: root launcher; delegates to MIDI-GPT's launcher.
- `MidiPlayer6/`: user-installed Falcosoft player and Reality GM/GS sound bank;
  preserve its files/settings.
- Do not reinstall Text2midi or delete generated songs without an explicit request.

## Environment and GPU

MIDI-GPT uses `.venv\Scripts\python.exe` inside its folder, Python 3.11.15,
the Windows wheel `midigpt[inference]==0.3.4`, and `torch==2.5.1+cu124`.
The cloned source commit at installation was
`89621cc1f4db2a53ffe29c12578e949c730de0ab`; runtime imports the installed wheel.
`requirements-installed.txt` records dependencies.

GPU inventory at setup: 5090 (index 0), 4090 (index 1), 3090 (index 2).
Always select the 3090 by UUID, BEFORE importing torch:
`CUDA_VISIBLE_DEVICES=GPU-2271903e-28c4-d002-e41f-96471369c18a`.
It appears as `cuda:0` inside the process. Check one visible GPU, name contains
RTX 3090, and load the model explicitly on `cuda:0`. Never rely on index 2.

Completed local model: `models/pretrained/yellow_medium-final.safetensors`.
Use `InferenceEngine.from_checkpoint(..., device="cuda:0")`; offline generation
works. The parent Text2midi model/downloader/cache/environment no longer exist.
The MIDI-GPT environment has its own working Hugging Face CLI if needed.

## What generation taught us

- MIDI-GPT accepts structured musical controls, not natural-language captions.
- Yellow supports 4/8-bar context windows. `model_dim` is context bars, not
  vocabulary size. Use `mask_mode="attention"`; no released model has MaskBar.
- `note_density` levels 0-9 apply to DRUM tracks in this checkpoint. Applying
  density to a melodic track causes a validation error.
- Melodic `min_polyphony`/`max_polyphony` are quantized levels: 0 means ONE
  simultaneous voice, not zero notes. Note-duration bins are 32nd, 16th,
  eighth, quarter, half, whole (indices 0-5).
- Set `tracks_per_step=3` when each generated part should hear both other
  parts. Context may overflow; four-bar windows and one-bar steps work well.
- Empty `bars=[]` with `autoregressive=True` produced zero planner steps in
  this wheel. Explicit whole-track AR targets can exhaust context on a trio.
  One-bar infill targets over a scaffold have been more reliable.
- A novelty check can reject a perfectly usable repeated drum pattern;
  disabling it for that case is reasonable. Retain silence checks.
- A melodic minimum-duration override gave questionable achievable-range
  warnings during full infill. Maximum-duration controls worked in our tests.
- `Score.tempo` is MICROSECONDS per quarter note, not BPM.
- Model output can change the resolution (often 1920 TPQ). Use the returned
  resolution when composing, concatenating or exporting notes.
- `midigpt.Score.to_midi()` writes MIDI. `symusic.Score.dump_midi()` writes
  MIDI; symusic has no `to_midi()` method.
- File paths passed to the launcher resolve relative to the MIDI-GPT folder.

## Existing musical artifacts and provenance

- `compose-song.py` -> `outputs/complete-song/Windowlight.mid`, WAV and MP3:
  48 bars, 108 BPM, piano/fingered bass/drums, ~1:47. User liked this piece.
  MIDI-GPT generated voices from a chord/rhythm scaffold; form/repetitions,
  dynamics and final tonic cadence were composed explicitly.
- `compose-cracktro.py` -> `outputs/cracktro/RasterRush.mid`, WAV and MP3:
  48 bars, 150 BPM, ~1:17, three monophonic voices. User said this direction
  is perfect for their project. Model generated lead/bass; fast arpeggios,
  arrangement and closing cadence were composed. Seed 68000 selected for
  theme A after mechanical pitch-diversity comparison with seed 6502.
- Raster Rush's original preview is custom pulse/triangle synthesis with
  stereo echo. It is NOT authentic Tandy audio. MIDI programs alone do not
  preserve those sounds in another player's sound bank.
- Bass in original Raster Rush includes notes below the Tandy tone range.
- Generation reports and eight-bar tests accompany each piece.
- We validated MIDI structure and waveform clipping, but did NOT audition
  audio ourselves. Do not imply subjective listening or hardware testing.

## Tandy export constraints

Target the standard PSSJ/SN76496-compatible PSG path: three monophonic
50%-duty square-wave tone channels, plus optional noise (unused initially).
No triangle waveform, arbitrary pulse width, hardware envelope or stereo.
For standard 3.579545 MHz clock, tone frequency is clock/(32*divider), with
10-bit divider 1-1023. Minimum normal tone is about 109.35 Hz; A2 (MIDI 45)
is a practical floor. Fold low bass notes upward by octaves, preserving pitch
class, and quantify pitch error after divider rounding.

Volume has 15 audible attenuation levels in 2 dB steps and level 15 is mute.
Tone registers use a latch byte with low four divider bits, then six high
bits. Use PSG port 0xC0 on the TL/3. The DOS player must restore any timer
vector/rate it changes and mute channels on normal exit or Escape.
Do not assume an emulator proves TL/3 speaker routing or exact chip behaviour.

Technical references:
- https://dosbox-x.com/doxygen/html/sn76496_8cpp_source.html
- https://dosbox-x.com/doxygen/html/tandy__sound_8cpp_source.html
- https://www.agidev.com/articles/agispec/agispecs-9.html

## Verification and tool choices

For musical changes, verify three tracks, positive note lengths, legal pitches,
expected tempo/duration, per-channel monophony, and no clipping in the preview.
For hardware exports, validate register bounds/stream ordering, player cleanup,
and emulator playback; clearly distinguish this from real TL/3 validation.
Preserve source MIDI and prior previews; put Tandy variants in a separate folder.

`ffmpeg` is available. `render-midi.ps1` uses the player's 32-bit BASS DLLs and
must run under SysWOW64 Windows PowerShell. A local portable NASM assembler is
available in `MIDI-GPT/tools/nasm-3.01`; no global assembler install is needed.
DOSBox 0.74-3 is installed at `C:\Program Files (x86)\DOSBox-0.74-3\DOSBox.exe`.
Use project-local emulator configs and hidden launches for automated checks.

## Tandy proof of concept (2026-10-04)

`MIDI-GPT/export-tandy.py` adapts original Raster Rush into `outputs/tandy/`.
The register-driven mono preview uses three 50%-duty squares; no noise, DAC,
echo, pulse-width modulation or triangle. It is hardware-constrained but
not cycle-accurate. 125 bass notes were octave-folded to MIDI 45-56.
Pitch rounding error is at most ~4.16 cents, onset error ~1.42 ms.

T3P1 format: 4-byte magic, little-endian uint16 PIT divisor and end tick;
each record is absolute uint16 tick, byte count, then C0h register writes;
FFFFh/0 ends the stream. This song uses PIT divisor 9943 (~120.0022 Hz).
The stream is 17,692 bytes. `tandy-player.asm` builds an 8086-compatible,
570-byte `RASTER.COM`; reads `RASTER.T3` from the working directory.
It validates buffer/record bounds, supports Escape, mutes all channels on
exit, restores IRQ0, and restores standard DOS timer mode/rate. It expects
standard DOS timer operation before launch; no TSR/game integration yet.

`verify-player.asm` is a DOS child-execution harness for return status,
IRQ0 vector restoration and BIOS-clock advancement. Three-second playback,
Escape, and malformed-stream tests passed in DOSBox 0.74-3 Tandy mode with
286 CPU emulation. Full-song playback also passed the same harness checks.
`package-tandy.py` validates all four logs and builds `outputs/tandy/RASTER.ZIP`
with RASTER.COM, RASTER.T3, RASTER.ASM and README.TXT. Validation report contains
stream/player SHA-256 hashes. On 2026-10-04 the user confirmed successful playback
on a real Tandy 1000 TL/3 and said the emulated audio was "spot on". This is
user-reported hardware validation, supported by a photo of PLAYBACK COMPLETE;
it is not an independent measurement. `outputs/tandy/hardware-validation.json`
records the tested artifact hashes; packaging only carries that confirmation
forward when the hashes still match. The player makes no model-specific routing changes.

## Percussion direction

The PSG has three tone channels plus a fourth noise channel. Fixed-rate white
noise with short volume envelopes can provide hats and snare-like hits while
retaining all three musical voices. The noise clock has three fixed settings;
the fourth follows tone channel 3 and would couple percussion to the bass.
Prefer fixed noise rates for the first percussion experiment. A pitched kick
can briefly borrow the bass oscillator for a descending pitch envelope.
No DAC is required for this approach. The DOS player already forwards raw PSG
writes. `export-tandy.py` now renders fixed-rate PSSJ-style LFSR noise as well
as tones. Noise uses the NCR/PSSJ XNOR feedback with taps 1/5 and mask 0x8000;
changing the white/periodic mode resets the LFSR, changing rate alone does not.
The preview's noise counter phase is approximate, not cycle-accurate.
Do not claim a specific Zeliard drum implementation without verifying its driver.

`export-tandy-drums.py` overlays explicitly composed percussion on the existing
hardware-confirmed Raster Rush stream, preserving the original files. Outputs
are in `outputs/tandy-drums/`: MP3/WAV preview, RASTER.T3/COM and RASTER.ZIP.
48 bars at 150 BPM: hats, backbeat snare, section fills and kick; sparse break.
One noise channel means coincident hats yield to snare. Envelope ownership
prevents old hit tails from cutting off retriggered hits. Each kick borrows
tone 3 for five PIT ticks (~41.67 ms); bass state keeps advancing in shadow
and is restored afterward. No extra tone polyphony, DAC or samples.
The new stream is 24,781 bytes, below the player's 32,000-byte buffer limit.
`verify-tandy-drums.py` checks lead/arp register preservation, bass restoration,
noise clock independence, final silence and DOS test logs before packaging.
Player binary is unchanged. On 2026-10-04 the user confirmed this drum version
on real Tandy 1000 TL/3 hardware and reported a positive result. Both the original
tone arrangement and the percussion variant now have user-reported hardware
validation. Noise hats/snare plus brief bass-channel kick borrowing are proven
workable in this setup. `outputs/tandy-drums/hardware-validation.json` records
the exact tested stream/player hashes; retain confirmation only while they match.
Drum version full-song and six-second tests passed in DOSBox Tandy/286 mode,
including IRQ0 restoration, return status and BIOS-clock advancement. Register
comparison confirms lead/arp unchanged and bass overrides last at most five
ticks; all four channels finish muted. Preview has no clipping. Regression
render of the original tone-only stream remains byte-identical to its prior WAV.
