"""Arrangement -> T3P1 register stream for the Tandy chip (no MIDI-GPT or torch needed).

Generalises MIDI-GPT/export-tandy.py (tones, octave folding, volume envelopes) and
export-tandy-drums.py (white-noise hats/snare with envelope ownership, a 5-tick kick that
borrows the bass voice and restores it from a shadow copy), both confirmed on the real TL/3.
The drums follow the song's own bar map instead of Raster Rush's fixed positions.

An arrangement is a dict:
    bpm, bars,
    sections: [{'name', 'start', 'bars', 'drums': 'normal' | 'sparse' | 'none'}],
    voices:   three lists of (onset_beats, length_beats, midi_pitch, velocity)  (lead, accompaniment, bass)
    drums:    'none' | 'light' | 'full';  hats: 'eighths' | 'quarters' | 'none';  gains: 3 floats
optional (v2 recipes; the defaults give the original behaviour):
    beats_per_bar: quarter beats in a bar (4; 3 for 3/4 and 6/8)
    swing:        where an off-beat eighth falls (0.5 straight, 0.667 triplet swing)
    drum_pattern: a PATTERNS key, or 'backbeat' (the Raster Rush drums above)
"""
import math
from collections import defaultdict

import t3

MIN_PITCH = 45                      # A2: the chip's lowest tone is about 109 Hz
BASS_MAX = 57                       # the bass voice stays low (export-tandy.py)
KICK = ((0, 350, 2), (1, 245, 3), (2, 170, 5), (3, 125, 7), (4, 110, 10))   # tick, Hz, attenuation
NOISE = {'snare': (4, 14, 1), 'hat': (9, 3, 0), 'open_hat': (8, 9, 0),    # attenuation, length, rate
         'rim': (7, 5, 1), 'ride': (10, 8, 0)}

# Drum patterns for the v2 recipes: hits per bar as (beat, kind); 'sparse' is used for light drums
# and sparse sections, 'alt' for odd bars (two-bar patterns), 'fill' before a section change.
_E8 = [x * 0.5 for x in range(8)]
PATTERNS = {
    'none': {'normal': [], 'sparse': []},
    'four_floor': {'normal': [(0, 'kick'), (1, 'kick'), (2, 'kick'), (3, 'kick'), (1, 'snare'), (3, 'snare'),
                              (0.5, 'open_hat'), (1.5, 'open_hat'), (2.5, 'open_hat'), (3.5, 'open_hat')],
                   'sparse': [(0, 'kick'), (1, 'kick'), (2, 'kick'), (3, 'kick'),
                              (0.5, 'hat'), (1.5, 'hat'), (2.5, 'hat'), (3.5, 'hat')], 'fill': True},
    'shuffle': {'normal': [(0, 'kick'), (2, 'kick'), (1, 'snare'), (3, 'snare')] +
                          [(x, 'hat') for x in (0, 0.5, 1.5, 2, 2.5, 3.5)],
                'sparse': [(0, 'kick'), (2, 'kick'), (1, 'hat'), (3, 'hat')], 'fill': True},
    'jazz': {'normal': [(x, 'ride') for x in (0, 1, 1.5, 2, 3, 3.5)] + [(2.5, 'rim')],
             'sparse': [(1, 'ride'), (3, 'ride')]},
    'train': {'normal': [(0, 'kick'), (2, 'kick'), (1, 'snare'), (3, 'snare')] +
                        [(x, 'hat') for x in (0.5, 1.5, 2.5, 3.5)],
              'sparse': [(0, 'kick'), (2, 'kick'), (1, 'rim'), (3, 'rim')], 'fill': True},
    'march': {'normal': [(0, 'kick'), (2, 'kick'), (1, 'snare'), (2.5, 'snare'), (3, 'snare'), (3.5, 'snare')],
              'sparse': [(0, 'kick'), (2, 'kick'), (1, 'snare'), (3, 'snare')], 'fill': True},
    'onedrop': {'normal': [(2, 'kick'), (2, 'rim')] + [(x, 'hat') for x in (0, 0.5, 1, 1.5, 2.5, 3, 3.5)],
                'sparse': [(2, 'kick'), (2, 'rim')]},
    'rock': {'normal': [(0, 'kick'), (2, 'kick'), (2.5, 'kick'), (1, 'snare'), (3, 'snare')] +
                       [(x, 'hat') for x in (0, 0.5, 1.5, 2, 2.5, 3.5)],
             'sparse': [(0, 'kick'), (2, 'kick'), (1, 'snare'), (3, 'snare')], 'fill': True},
    'waltz': {'normal': [(0, 'kick'), (1, 'hat'), (2, 'hat')], 'sparse': [(0, 'kick')]},
    'jig': {'normal': [(0, 'kick'), (1.5, 'kick'), (0.5, 'hat'), (1, 'hat'), (1.5, 'rim'), (2, 'hat'), (2.5, 'hat')],
            'sparse': [(0, 'kick'), (1.5, 'kick')]},
    'bossa': {'normal': [(0, 'kick'), (1.5, 'kick'), (2, 'kick'), (3.5, 'kick'), (0, 'rim'), (1.5, 'rim'), (3, 'rim')] +
                        [(x, 'hat') for x in (0.5, 1, 2, 2.5)],
              'alt': [(0, 'kick'), (1.5, 'kick'), (2, 'kick'), (3.5, 'kick'), (1, 'rim'), (2.5, 'rim')] +
                     [(x, 'hat') for x in (0, 0.5, 1.5, 2, 3, 3.5)],
              'sparse': [(0, 'kick'), (2, 'kick'), (1.5, 'rim'), (3, 'rim')]},
    'halftime': {'normal': [(0, 'kick'), (2, 'snare'), (1, 'hat'), (3, 'hat')], 'sparse': [(0, 'kick'), (2, 'hat')]},
}


def swing_beat(x, swing):
    """A position in beats with swung eighths: an off-beat eighth moves to `swing` of the beat, swung
    sixteenths in proportion; on-beat and triplet positions stay where they are."""
    if swing == 0.5:
        return x
    b = math.floor(x + 1e-9)
    f = x - b
    if abs(f - 0.5) < 0.03:
        return b + swing
    if abs(f - 0.25) < 0.03:
        return b + swing / 2
    if abs(f - 0.75) < 0.03:
        return b + swing + (1 - swing) / 2
    return x


class Song:
    def __init__(self, arr):
        self.arr = arr
        self.b = t3.Builder()
        self.sec_per_beat = 60.0 / arr['bpm']
        self.bpb = arr.get('beats_per_bar', 4)
        self.swing = arr.get('swing', 0.5)

    def tick(self, beats):
        return round(beats * self.sec_per_beat * self.b.rate)

    def stick(self, beats):
        """tick() of a swung position."""
        return self.tick(swing_beat(beats, self.swing))


def fold(pitch, ch):
    while pitch < MIN_PITCH:
        pitch += 12
    if ch == 2:
        while pitch > BASS_MAX:
            pitch -= 12
    return pitch


def tone_events(song):
    """Per tick: ordered (order, ch, kind, value) for the three tone voices (export-tandy.py's rules)."""
    ev = defaultdict(list)
    stats = {'notes': [0, 0, 0], 'folded': 0, 'max_cents': 0.0}
    gains = song.arr.get('gains', (0.8, 0.35, 0.6))
    for ch, notes in enumerate(song.arr['voices']):
        notes = sorted(notes)
        for i, (on, length, pitch, vel) in enumerate(notes):
            p = fold(pitch, ch)
            stats['folded'] += p != pitch
            hz = 440 * 2 ** ((p - 69) / 12)
            div = t3.divider(hz)
            stats['max_cents'] = max(stats['max_cents'], abs(1200 * math.log2(t3.tone_hz(div) / hz)))
            start = song.stick(on)
            end = max(start + 1, song.stick(on + length))
            if i + 1 < len(notes):
                end = min(end, song.stick(notes[i + 1][0]))     # one voice: the next note cuts this one
            if end <= start:
                continue                                         # collapsed onto the next onset
            att = max(0, min(14, round(-10 * math.log10(max(1, vel) / 127 * gains[ch]))))
            ev[start].append((1, ch, 'on', (div, att, i)))
            n = end - start
            for frac, extra in ((0.4, 1), (0.75, 3)):
                t = start + round(n * frac)
                if start < t < end:
                    ev[t].append((2, ch, 'env', (min(14, att + extra), i)))
            ev[end].append((0, ch, 'off', (15, i)))
            stats['notes'][ch] += 1
    return ev, stats


def drum_hits(song, density):
    """[(beat, kind)] from the bar map: hats, backbeat snare, kicks, fills before section changes."""
    arr = song.arr
    if density == 'none':
        return []
    if arr.get('drum_pattern', 'backbeat') != 'backbeat':
        return pattern_hits(song, density)
    hats = arr.get('hats', 'eighths')
    light = density == 'light'
    hits = []
    fill_bars = {s['start'] + s['bars'] - 1 for s in arr['sections'][:-1]}
    for s in arr['sections']:
        mode = s.get('drums', 'normal')
        for bar in range(s['start'], s['start'] + s['bars']):
            if mode == 'none' or bar < 2 or bar == arr['bars'] - 1:
                continue
            b0 = bar * song.bpb
            sparse = mode == 'sparse' or light
            snares = [3] if sparse else [1, 3]
            if bar in fill_bars and not light:
                snares += [3.5, 3.75]
            hits += [(b0 + x, 'snare') for x in snares]
            if hats != 'none':
                step = 0.5 if hats == 'eighths' and not light else 1.0
                k = 0
                x = 0.0
                while x < 4:
                    if x not in snares and not (sparse and k % 2):
                        open_hat = x == 3.5 and bar % 4 == 3 and not light
                        hits.append((b0 + x, 'open_hat' if open_hat else 'hat'))
                    x += step
                    k += 1
            kicks = [0] if sparse else [0, 2]
            if not sparse and bar >= 4 and bar % 2 == 1:
                kicks.append(2.75)
            hits += [(b0 + x, 'kick') for x in kicks]
    return hits


def pattern_hits(song, density):
    """drum_hits for a PATTERNS drum pattern (v2 recipes), on the song's own bar map and meter.
    Like the original: no drums in the first two bars or the last one."""
    arr = song.arr
    pat = PATTERNS[arr['drum_pattern']]
    bpb = song.bpb
    light = density == 'light'
    fill_bars = {s['start'] + s['bars'] - 1 for s in arr['sections'][:-1]}
    hits = []
    for s in arr['sections']:
        mode = s.get('drums', 'normal')
        for bar in range(s['start'], s['start'] + s['bars']):
            if mode == 'none' or bar < 2 or bar == arr['bars'] - 1:
                continue
            if mode == 'sparse' or light:
                bar_hits = pat['sparse']
            else:
                bar_hits = pat['alt'] if bar % 2 and 'alt' in pat else pat['normal']
            bar_hits = [(x, k) for x, k in bar_hits if x < bpb]
            if bar in fill_bars and pat.get('fill') and not light:
                bar_hits = [(x, k) for x, k in bar_hits if x < bpb - 0.5 or k == 'kick'] + \
                           [(bpb - 0.5, 'snare'), (bpb - 0.25, 'snare')]
            hits += [(bar * bpb + x, k) for x, k in bar_hits]
    return hits


def compile_song(arr, density=None, max_bytes=t3.MAX_BYTES):
    """Returns (T3 bytes, report)."""
    song = Song(arr)
    density = density or arr.get('drums', 'full')
    ev, stats = tone_events(song)
    # Drums: kick windows borrow the bass voice (ch 2); noise hits own the noise voice
    kick_start = {}
    noise_on = defaultdict(list)
    counts = defaultdict(int)
    for beat, kind in drum_hits(song, density):
        t = song.stick(beat)
        counts[kind] += 1
        if kind == 'kick':
            kick_start[t] = True
        else:
            noise_on[t].append(kind)
    borrowed = set()
    kick_writes = defaultdict(list)
    for k in kick_start:
        for off, hz, att in KICK:
            d = t3.divider(hz)
            kick_writes[k + off].append(bytes((0xC0 | (d & 15), (d >> 4) & 63, 0xD0 | att)))
            borrowed.add(k + off)
    noise_env = defaultdict(list)
    for t, kinds in noise_on.items():
        kind = 'snare' if 'snare' in kinds else kinds[0]          # one noise voice: the snare wins
        noise_on[t] = [kind]
        att, length, _ = NOISE[kind]
        for off, extra in ((1, 2), (length // 2, 5), (length - 1, 8), (length, 15)):
            noise_env[t + off].append((t, min(15, att + extra)))
    ticks = sorted(set(ev) | set(kick_writes) | set(noise_on) | set(noise_env) |
                   {t + 1 for t in borrowed if t + 1 not in borrowed})
    b = song.b
    active = [-1, -1, -1]
    shadow = [1023, 15]                    # the bass voice's real state while a kick borrows it
    noise_owner = -1
    noise_ctl = -1
    for t in ticks:
        bass_out = t not in borrowed
        for order, ch, kind, val in sorted(ev.get(t, [])):
            if kind == 'on':
                div, att, idx = val
                active[ch] = idx
                if ch == 2:
                    shadow[:] = [div, att]
                if ch != 2 or bass_out:
                    b.tone(t, ch, div=div)
                    b.vol(t, ch, att)
            else:
                att, idx = val
                if active[ch] != idx:
                    continue
                if kind == 'off':
                    active[ch] = -1
                if ch == 2:
                    shadow[1] = att
                if ch != 2 or bass_out:
                    b.vol(t, ch, att)
        if t - 1 in borrowed and bass_out:                       # kick over: the bass as it is now
            b.tone(t, 2, div=shadow[0])
            b.vol(t, 2, shadow[1])
        for w in kick_writes.get(t, []):
            b.raw(t, *w)
        for owner, att in noise_env.get(t, []):
            if owner == noise_owner:
                b.raw(t, 0xF0 | att)
        for kind in noise_on.get(t, []):
            att, _, rate = NOISE[kind]
            noise_owner = t
            if noise_ctl != (4 | rate):
                b.noise(t, white=True, rate=rate)
                noise_ctl = 4 | rate
            b.raw(t, 0xF0 | att)
    end = song.tick(arr['bars'] * song.bpb)
    data = b.build(end)
    report = t3.validate(data, max_bytes)
    report.update({'notes_per_voice': stats['notes'], 'octave_folded': stats['folded'],
                   'max_pitch_error_cents': round(stats['max_cents'], 2), 'drums': density,
                   'drum_hits': dict(counts)})
    return data, report


def compile_fit(arr, max_bytes=t3.MAX_BYTES):
    """Compiles within max_bytes (None: a streamed song, no size limit) and the tick limit: drums thin
    out first (full -> light -> none), then the song loses its middle sections.  Returns (data, report,
    notes about what was cut)."""
    cuts = []
    order = {'full': ['full', 'light', 'none'], 'light': ['light', 'none'], 'none': ['none']}[arr.get('drums', 'full')]
    while True:
        last_err = None
        for d in order:
            try:
                data, rep = compile_song(arr, d, max_bytes)
                if d != order[0]:
                    cuts.append(f'drums {order[0]} -> {d} to fit')
                return data, rep, cuts
            except ValueError as e:
                if 'over' not in str(e):
                    raise
                last_err = e
        if len(arr['sections']) <= 3:
            raise ValueError(f'song does not fit: {last_err}')
        arr = drop_section(arr)
        cuts.append(f'shortened to {arr["bars"]} bars to fit')


def drop_section(arr):
    """The arrangement without its second-to-last full section (keeps intro, themes and ending)."""
    secs = arr['sections']
    k = len(secs) - 2
    s = secs[k]
    bpb = arr.get('beats_per_bar', 4)
    a, n = s['start'] * bpb, s['bars'] * bpb
    voices = [[(on - n if on >= a + n else on, ln, p, v) for on, ln, p, v in voice if not a <= on < a + n]
              for voice in arr['voices']]
    new_secs = []
    for i, x in enumerate(secs):
        if i == k:
            continue
        x = dict(x)
        if i > k:
            x['start'] -= s['bars']
        new_secs.append(x)
    out = dict(arr)
    out.update(voices=voices, sections=new_secs, bars=arr['bars'] - s['bars'])
    return out
