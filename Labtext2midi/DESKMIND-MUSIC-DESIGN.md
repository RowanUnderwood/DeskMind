# DeskMind music: a conversation that produces playable Tandy compositions

Design proposal, 2026-10-04. This is a reference for future implementation;
the integration described here has not been built or benchmarked.

## Recommended approach

Put the musical conversation in DeskMind's existing chat window. Add a music
service to MindServer, backed by a separate MIDI-GPT worker on the RTX 3090.
Build one small DOS playback module that can be linked into DeskMind,
SLIDES.EXE and an optional standalone MUSIC.EXE.

That gives us a coherent experience while keeping the heavy dependencies on the
PC. The separate worker also avoids putting PyTorch and MIDI-GPT's Python 3.11
environment into MindServer's documented Python 3.12/Qt environment.

Start with an integrated chat interface and offline companion playback. Decide
whether to enable playback inside DeskMind after measuring its worst-case free
conventional memory and testing sound timing alongside networking. A companion
player is a useful fallback, but a second DOS program does not automatically
solve memory pressure if DeskMind remains resident while executing it.

The central division of work is:

| Participant | Contribution |
|---|---|
| Human | Chooses direction, listens, selects themes, asks for changes and decides what to keep |
| Qwen | Interprets those requests, proposes musical choices and turns feedback into specific edits |
| MindServer | Maintains the composition, validates commands, builds arrangements and compiles hardware-safe playback |
| MIDI-GPT | Generates or regenerates bounded melodic/bass phrases in musical context |
| Tandy | Downloads finished music and plays it locally, alone or along with the slideshow. |

The Python backend should contain substantial musical logic, but it can be a
small collection of focused modules. We do not need Qwen to emit thousands of
MIDI events or program PSG registers. We need it to direct a reliable composing
system and explain the choices that the human can hear.

## What we can build on

DeskMind already has streamed chat, persistent transcripts, image generation
from chat, deferred asset downloads, a native GUI and a standalone slideshow.
MindServer already coordinates jobs, storage and local AI services. Music fits
that pattern naturally.

Our music experiments supply a working starting point:

- Raster Rush is a complete 48-bar, 150 BPM arrangement of about 77 seconds.
- MIDI-GPT generated its lead/bass material; explicit code provided harmony,
  arpeggios, form and the final cadence.
- Both the three-tone Tandy arrangement and the drum variant were confirmed
  by the user on real Tandy 1000 TL/3 hardware.
- The drum variant uses three tone channels plus fixed-rate noise. A kick
  borrows the bass channel for about 42 ms, then restores its current state.
- Its register stream is 24,781 bytes; the standalone player binary is 570
  bytes. The binary's file size is not its runtime memory footprint.

The installed Yellow checkpoint takes structured controls. Our genre system
should therefore translate styles into musical recipes, rather than pass an
English genre prompt to MIDI-GPT. The upstream project documents multitrack
infill and structured controls; our installed wheel's tested behavior is the
implementation baseline. [MIDI-GPT upstream documentation](https://github.com/Metacreation-Lab/MIDI-GPT)

## Make the user a participant through audible choices

The strongest experience is a short cycle: propose, hear, choose, revise. Avoid
making the user complete a long questionnaire before any music exists.

Example conversation, illustrating proposed behavior:

> User: Make a cracktro for these space pictures. Fast, but slightly mysterious.
>
> Qwen: I'll try a minor-key theme with quick arpeggios and a restrained drum
> pattern. I'll make two short themes so you can pick the direction.
>
> [Two eight-bar sketches appear, with Play A and Play B.]
>
> User: B. Keep that tune, but make the bass punchier and the drums less busy.
>
> Qwen: I'll keep B's melody, shorten the bass notes and remove alternate hats.
>
> [A new revision appears beside B; the original remains available.]
>
> User: Great. Make it a full song with a quieter middle, and use it for the show.
>
> Qwen: I'll bring the opening theme back after the quiet section and finish
> with a short ending. I'll attach the finished version to this slideshow.

Offer simple controls beside the result: Play, Stop, Keep, Compare, Undo and
Use in Slideshow. The prose stays conversational; the buttons make selection
and playback unambiguous. A compact text music card is enough on DOS: title,
duration, style, revision and a one-line change summary. No piano roll is needed
for the first version.

Useful requests should have predictable effects:

| Human request | Backend operation |
|---|---|
| Keep the tune, change the bass | Lock melody; regenerate or transform bass in selected bars |
| Less busy | Reduce a specified part's rhythmic activity; preserve the chosen theme |
| More energetic | Adjust tempo, articulation, arpeggio pattern or drum pattern within the preset |
| Make the ending stronger | Replace only the ending with a planned cadence |
| Keep A's melody with B's drums | Merge compatible parts using shared key, tempo and bar alignment |
| Make it shorter | Remove/reorder sections and rebuild transitions/endings |
| Try another tune | Generate a new phrase branch, preserving the arrangement brief |
| Undo that | Select the prior immutable revision; no regeneration |

When feedback is ambiguous, ask one useful question, such as “Is it the tune
or the drums that feel too busy?” Otherwise apply a sensible, visible default.
Small edits should not silently become a new song.

Qwen should explain the actual completed operation, using backend results.
It should not claim it listened to the preview. Initially it can inspect the
score's structure and measured properties, and rely on the human for taste.
Image vision can suggest a musical mood, but it is a proposal the user can
override, not a claim that there is one correct soundtrack for a picture.

## The composition backend

Use an intermediate composition document as the authoritative musical state.
Store the full document on the PC, with a small summary on the Tandy. It should
include:

- Project ID, immutable revision ID, parent revision and creation request.
- Selected preset and preset version; tempo, meter, key, section/bar map.
- Harmony, motifs, note events, articulation and drum pattern parameters.
- Locked parts/bars, selected candidate and current user preferences.
- Seeds per phrase/part, model/checkpoint identity and compiler version.
- Exported asset IDs/hashes, validation results and revision change summary.

Keep a stable project ID while revisions receive distinct eight-hex asset IDs.
Then “that one” can refer to a selected revision rather than whichever file was
last generated. Maintain composition state separately from the chat text:
DeskMind trims the displayed transcript of long chats, so it cannot be our only
record of locked melodies or the selected soundtrack. Supply Qwen a compact
current-state summary and resolve edits against an explicit base revision.

Suggested MindServer modules:

| Module | Responsibility |
|---|---|
| `music_spec.py` | Schemas, limits, defaults and validated edit operations |
| `music_presets.py` | Versioned styles, harmony/rhythm recipes and hardware voice roles |
| `music_arrange.py` | Scaffold, motifs, form, transitions, cadence and phrase reuse |
| `music_worker.py` | Queued calls to the separate 3090 generation process |
| `music_compile.py` | Target constraints, percussion, volume envelopes and T3 export |
| `music_render.py` | Preview from the exact exported register stream |
| `music_store.py` | Projects, revisions, metadata, assets and slideshow links |
| `music_tools.py` | Qwen action dispatch and concise structured results |

These are proposed module boundaries, not files that currently exist.

Refactor the successful scripts into these components. Do not expose the
current drum overlay as a general engine: it assumes Raster Rush's tempo,
length and section positions. The reusable version must calculate percussion,
kick ownership and envelopes from each composition's beat/bar map.

Generate a four- or eight-bar motif first. Assemble longer songs by reusing
chosen phrases, introducing controlled variations and generating additional
sections in bounded windows. Our tested one-bar infill steps in four-bar
context are a safer starting point than one enormous full-song request.

Separate cheap edits from model work. Tempo changes, attenuation, drum density,
section reuse and some articulation edits can be deterministic. A genuinely
new tune or bass phrase uses MIDI-GPT. Cache completed phrases and exports;
playing, undoing or choosing a previous candidate should be immediate.

Validation comes before publication: monophonic tone parts, positive durations,
hardware pitch range, timer quantization, legal register writes, stream size,
bounded bytes per playback tick, clean ending and unclipped preview. Reject or
repair a bad phrase within a bounded retry budget. Report failed generation
honestly and preserve the current good revision. Seeds support traceability;
store the actual resulting events because a seed alone does not guarantee
identical results across dependency/model changes.

## Genres should be recipes we can refine

Start with the proven cracktro recipe and two contrasting styles. Expand only
after listening tests establish useful differences. This is an initial palette
of proposed presets, not a claim that these styles are already implemented:

| Preset | Musical recipe | Tandy roles |
|---|---|---|
| Cracktro | Minor harmony, fast sixteenth arps, repeated hook, sparse break and return | Lead / arps / bass; noise drums |
| Adventure | Strong memorable melody, moderate pace, alternating accompaniment | Lead / accompaniment / bass; restrained noise |
| Dungeon | Slower minor/modal phrases, space between notes, unresolved middle | Lead / sparse harmony / bass; occasional noise |
| Space drift | Long phrases, slow changing arps, wide dynamic contrast | Lead / arps / bass; drums optional |
| Puzzle | Short repeating motifs, clear rhythm and bright harmony | Lead / rhythmic accompaniment / bass; light hats |
| March | Strong beat, short articulated notes, repeated phrases | Lead / accompaniment / bass; noise snare |

Each recipe defines allowable keys, tempo range, chord choices, rhythm grids,
pitch ranges, phrase length, repetition, drum patterns, envelopes and form.
Parameters such as energy, brightness and busyness map to those real musical
controls. Offer independent drum density and melodic activity; our Yellow
checkpoint's note-density attribute applies to drum tracks, not melodic tracks.
For melodic busyness use rhythm scaffolds, duration controls, postprocessing
and measured/rejected output rather than an unsupported attribute.

Hardware timbre remains square waves and noise. Names such as “space drift”
describe arrangement and mood; they do not promise pads, reverb or sampled
instruments. Preserve that expectation in both Qwen's explanations and previews.

## Give Qwen a small, explicit action vocabulary

Expose actions such as `propose_music`, `make_sketches`, `revise_music`,
`extend_music`, `select_revision` and `attach_music_to_show`. Commands use IDs,
enumerated choices, bounded values and an explicit base revision. The backend
resolves these to approved templates and musical operations.

Illustrative internal command:

```json
{
  "action": "revise_music",
  "project_id": "project-17",
  "base_revision": "8A21B04C",
  "request_id": "chat-42-turn-9",
  "edits": [
    {"op": "lock", "part": "lead", "bars": "all"},
    {"op": "set", "parameter": "bass_articulation", "value": "short"},
    {"op": "set", "parameter": "drum_density", "value": "sparse"}
  ]
}
```

Prefer native tool calls if the current NInfer/Qwen combination supports them
reliably; test that capability rather than assuming OpenAI-compatible HTTP
implies reliable tools. A bounded `<music>...</music>` action block can be a
fallback using the existing drawing pattern. MindServer must buffer and
validate complete actions, strip them from displayed text, reject unsupported
fields and deduplicate request IDs so a retried reply does not create two songs.
Do not execute generated Python, shell commands or arbitrary paths.

Return actual outcomes to Qwen: created revision, applied edits, limitations,
validation and available assets. Locked bars must be enforced by the backend
and compared after generation, rather than entrusted only to a prompt.

The current Qwen template requires one leading system message. Merge music
instructions into that message and supply project context through supported
conversation/tool messages; do not introduce a later system message. Load a
small style catalog plus the selected recipe instead of every preset in every
chat. The detailed numerical translation belongs in Python.

## Jobs, GPUs and the existing protocol

Keep Qwen on the 5090, ComfyUI on the 4090 and MIDI-GPT on the 3090. The music
worker must set `CUDA_VISIBLE_DEVICES` to the verified 3090 UUID before importing
torch and check the visible device. Use the installed local checkpoint and
environment. Do not reload the model for every phrase; give the worker explicit
load/unload controls and one generation job at a time initially.

MindServer should remain responsive while the worker runs. Queue requests,
track progress at real stage boundaries, and allow cancellation between steps.
An obsolete result must not replace a newer selected revision. Generation
latency and simultaneous service throughput still need measurement; report
stages rather than invented percentages or completion times. Reuse an already
running Qwen service; music should not trigger disruptive service restarts.

Extend the existing simple HTTP/CP437 protocol. Proposed additions could be:

- `POST /music/gen` starts a validated music job.
- `GET /music/job/<id>` reports queued/generating/arranging/compiling/ready.
- `GET /music/list` returns a bounded catalog of IDs, revisions and titles.
- `GET /music/<asset-id>` returns a finished T3 stream.
- `M <asset-id> <title>` identifies a ready music attachment in chat.

These are design sketches. Negotiate a protocol feature/version before sending
new record types to old clients. Keep JSON and detailed tool state on the PC.
Extend local transcript records to preserve music attachments and revision
selection as well as ordinary text.

DeskMind has one HTTP socket. Finish and close a streaming chat/job response
before downloading music, exactly as with pictures. Queue ready IDs, fetch one
asset at a time afterward, validate into a temporary file, then atomically make
it available to the library. Use eight-hex IDs as 8.3 filenames such as
`8A21B04C.T3`, with titles in a compact catalog. Enforce download limits and
verify length/hash; do not try to deliver timing-critical register writes over
WiFi. Playback is from a fully downloaded, validated file.

## DOS playback and memory: the real integration work

Use one shared `music.cpp`/assembly engine with a tiny API: load, play, stop,
position, finished and cleanup. DeskMind can call it from music cards;
SLIDES.EXE can call it for a saved soundtrack; MUSIC.EXE can provide offline
listening. Avoid a TSR: W mode has little upper-memory headroom and resident
audio adds timer, sound and cleanup complexity.

The current standalone COM plays events from its foreground loop while IRQ0
advances a counter. That is proven for standalone listening, but it is not yet
a background slideshow engine. Copying its loop into a GUI would delay notes
whenever a transition or network operation blocks.

For background playback, preload the validated stream and dispatch due PSG
writes in a bounded timer handler. No file access, DOS calls, allocation,
networking or GUI drawing belongs in that handler. Keep the timer/record state
in conventional memory; make EMS optional rather than required. Preserve BIOS
time by chaining the original timer at its normal cadence and restore all
changed state on exit/error. Audit any code that assumes INT 08h/1Ch still runs
at the original rate, and measure interrupt latency during blits and disk reads.

**Sound ownership is mandatory.** DeskMind's current `sound.cpp` hooks INT 1Ch
and refreshes all four channel volumes even when idle. That would overwrite
the music's state. Merely declining to play new UI chimes is insufficient.
Add an ownership mechanism that suspends effect refreshes while music owns
the PSG, prevents split/interleaved latch-data writes, and resumes cleanly
afterward. Initially suppress UI sounds during music. Mixing effects by briefly
stealing voices can be a later, deliberate feature.

DeskMind's recent notes also describe lost PSG writes and paced writes using
port-61 reads, with a hardware-test item still pending in those notes. Reuse a
single paced write routine and stress-test the music path under load. Our
standalone success does not prove simultaneous graphics/network playback.
Respect machine detection: never send Tandy port-C0 writes on a non-Tandy PC,
where DeskMind currently uses a PC-speaker fallback. Disable Tandy music there
initially; a separate one-voice export can be designed later. `/CGA` selects
video mode and should not by itself determine whether Tandy audio is present.

For the first implementation, budget a full-song buffer capped at about 32 KB,
plus the actual player code, state and stack. The current song needs about
24.2 KiB; these are asset measurements, not a measured DeskMind allocation
budget. Long songs may exceed the current T3P1 16-bit tick limit or buffer cap;
reject them clearly at first, then consider a versioned chunk format and
main-loop refill with an underrun policy if longer music becomes necessary.

Measure free memory with long chat buffers, large gallery/catalog, network
buffers, graphics reservation and slideshow buffer active. SLIDES already uses
one 64,000-byte picture buffer in Tandy mode, so adding music requires a combined
measurement. Record actual linked size, heap headroom and stack high-water use.
If DeskMind cannot reserve the song buffer comfortably, retain music creation
and selection in chat and hand playback to MUSIC.EXE after saving/suspending or
exiting DeskMind. DOS EXEC alone does not release the parent's allocations;
a suspend/resume launcher would need explicit memory release and sound cleanup.

## Music with SLIDES.EXE

Start with a soundtrack selection and optional loop. A proposed `/M filename`
switch or saved show manifest chooses a local T3 asset; a show should pin an
immutable revision so later composition edits do not change its soundtrack.
Music and slideshow remain usable without MindServer or WiFi once downloaded.

Use the same playback module in the Gallery slideshow and standalone SLIDES.
Keep the existing picture delay/effect behavior initially. Define music behavior
explicitly: play once or loop, mute/stop on slideshow exit, and cleanly handle
a missing or invalid track. Loop boundaries need an intentional musical ending
and restoration of initial PSG state, not just a pointer reset.

Later, let the backend suggest picture changes at section or bar boundaries.
Use the music clock as the source of elapsed soundtrack time. Because transitions
take measurable time, begin them with that latency in mind; precise beat-sync
needs testing and cannot be assumed from nominal picture delays. Store image
IDs, music revision and cue points in a show manifest. Avoid requiring users to
learn a timeline editor before they can make an accompanied slideshow.

## Suggested implementation order

1. **Reusable backend:** refactor the proven cracktro/PSG scripts; create project
   and revision storage, one versioned preset, locks and bounded edits. Check
   that old tone/drum artifacts remain reproducible and preserved.
2. **Background playback spike:** use the drum song in a shared DOS engine and
   SLIDES test build. Measure memory, interrupt load, write pacing and cleanup
   on the real TL/3 with transitions and file reads. Settle PSG ownership here.
3. **Chat composition MVP:** generate two short candidates, select one, make a
   targeted revision, undo, extend to a song and download/play it. Add protocol
   capabilities and transcript music records. Measure live job latency.
4. **Slideshow attachment:** pin a revision, play once/loop, demonstrate offline
   playback and verify exit paths. Keep standalone MUSIC.EXE as an option.
5. **Expand musical range:** add adventure/dungeon recipes, then other styles,
   based on actual listening feedback. Add richer cue timing only afterward.

Acceptance for the first release: a user can choose between audible themes,
retain a melody while changing accompaniment, undo without regenerating, and
attach the chosen song to a slideshow. Real TL/3 tests should combine music
with longest transitions, image loading, mouse input and network waits; verify
tempo, no stuck sounds, normal BIOS time, exit cleanup and memory headroom.
Also test worker failure/cancellation, malformed tool output, interrupted
downloads and reopening a long chat with composition state intact.

The recommendation is to invest first in **persistent composition state,
predictable edits and an honest listening/feedback loop**. Those features will
make the human and Qwen feel involved far more than a large list of genre names
or a single “generate song” command.

## Context and implementation references

- [DeskMind context](</H:/Dos Projects/DOS 286 generative AI applications/CLAUDE.md>)
- [DeskMind architecture plan](</H:/Dos Projects/DOS 286 generative AI applications/PLAN.md>)
- [Current DeskMind sound handler](</H:/Dos Projects/DOS 286 generative AI applications/dos/src/sound.cpp>)
- [Slideshow implementation](</H:/Dos Projects/DOS 286 generative AI applications/dos/src/slide.cpp>)
- [Our accumulated music findings](<H:/Dos Projects/DOS 286 generative AI applications/Labtext2midi/AGENTS.md>)
- [Cracktro composition script](<H:/Dos Projects/DOS 286 generative AI applications/Labtext2midi/MIDI-GPT/compose-cracktro.py>)
- [PSG exporter/preview](<H:/Dos Projects/DOS 286 generative AI applications/Labtext2midi/MIDI-GPT/export-tandy.py>)
- [Drum version exporter](<H:/Dos Projects/DOS 286 generative AI applications/Labtext2midi/MIDI-GPT/export-tandy-drums.py>)

Existing facts above come from those notes, inspected source and the user's
hardware confirmations. Module names, endpoints, controls and integration
choices are proposals. Memory capacity, generation latency and background
playback reliability remain implementation measurements to obtain.
