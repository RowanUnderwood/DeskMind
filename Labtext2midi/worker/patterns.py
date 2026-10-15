"""Scaffold patterns for the three Tandy voices: lead, accompaniment and bass, one bar at a time.

Pure Python (no MIDI-GPT): compose.py turns the notes into MIDI-GPT bars, and the genre lab and
tests use them directly.  A note is (onset_beats, length_beats, midi_pitch, velocity) with beats
counted in quarter notes from the start of the bar, so a 3/4 or 6/8 bar is 3 beats long.

Chords come from roman numerals read in the recipe's mode (recipes.chord), so 'VI' in a minor key
is the major chord on the flat sixth, as before.  The chip has one tone per voice: chords are always
broken (arpeggios, Alberti, boom-chick, stabs cycling the chord tones).
"""
import random

# ---------------------------------------------------------------- lead


def lead_bar(tones, spec, bpb, pcs=None):
    """The lead scaffold for one bar: spec = {'notes': [(index, onset, length)], 'octave', 'scalar'}.
    An index counts chord tones (beyond the chord it wraps up an octave: index 3 of a triad = the root an
    octave up), or with 'scalar' and the bar's lead pitch classes `pcs`, scale steps up from the chord root:
    a stepwise placeholder, so MIDI-GPT continues stepwise instead of in chord leaps."""
    out = []
    root = tones[0] + spec.get('octave', 24)
    steps = [p for p in range(root, root + 36) if p % 12 in set(pcs)] if spec.get('scalar') and pcs else None
    for i, on, ln in spec['notes']:
        if on >= bpb:
            continue
        p = steps[i] if steps else tones[i % len(tones)] + 12 * (i // len(tones)) + spec.get('octave', 24)
        out.append((on, min(ln, bpb - on), p, spec.get('vel', 90)))
    return out


# ---------------------------------------------------------------- accompaniment

def _tone(tones, i, lift=0):
    return tones[i % len(tones)] + 12 * (i // len(tones)) + lift


def acc_bar(tones, spec, bpb, bar_index=0, rng=None):
    """One bar of accompaniment.  spec['type']:
        arp       steps of `step` beats cycling `pattern`; `lift` raises the second half an octave
        alberti   low-high-mid-high in `step`s (classical)
        boomchick chord tones on the off-beats of the bar (beats 2 and 4), short
        offbeat   short stabs on every 'and' (reggae skank, techno stab), cycling the chord
        comp      jazz comping: stabs on 1 and the 'and' of 2 (Charleston), 3rd and 7th
        sustain   held chord tones, `step` beats each (pads)
        broken68  6/8: low-mid-high on each eighth of the two dotted-quarter beats
        oompah    waltz / stride: chord tones on every beat but the first
    `octave` shifts everything (default +12, as the original arpeggio)."""
    t = spec['type']
    oct_ = spec.get('octave', 12)
    vel = spec.get('vel', 70)
    out = []
    if t == 'arp':
        step, pat, ln = spec['step'], spec['pattern'], spec['len']
        n = int(round(bpb / step))
        for j in range(n):
            on = j * step
            lift = 12 if spec.get('lift') and on >= bpb / 2 else 0
            out.append((on, ln, _tone(tones, pat[j % len(pat)], oct_ + lift), vel + (8 if on % 1 == 0 else 0)))
    elif t == 'alberti':
        step = spec.get('step', 0.5)
        pat = (0, 2, 1, 2)
        n = int(round(bpb / step))
        for j in range(n):
            out.append((j * step, step * 0.9, _tone(tones, pat[j % 4], oct_), vel + (6 if j % 4 == 0 else 0)))
    elif t == 'boomchick':
        for b in range(1, int(bpb), 2):
            out.append((b, spec.get('len', 0.35), _tone(tones, 1 + (b // 2) % 2, oct_), vel))
    elif t == 'offbeat':
        ln = spec.get('len', 0.25)
        k = 0
        x = 0.5
        while x < bpb:
            out.append((x, ln, _tone(tones, spec.get('pattern', (1, 2))[k % len(spec.get('pattern', (1, 2)))], oct_), vel))
            x += spec.get('every', 1.0)
            k += 1
    elif t == 'comp':
        r = rng or random.Random(bar_index)
        hits = [(0, 1), (1.5, 2)] if bar_index % 2 == 0 else [(0.5, 1), (2.5, 2)]
        if r.random() < 0.4:
            hits.append((3.5, 1))
        for on, i in hits:
            if on < bpb:
                out.append((on, 0.4, _tone(tones, i, oct_), vel))
    elif t == 'sustain':
        step = spec.get('step', 2.0)
        pat = spec.get('pattern', (0, 1, 2, 1))
        j = 0
        x = 0.0
        while x < bpb:
            out.append((x, min(step, bpb - x) * 0.95, _tone(tones, pat[(bar_index * 2 + j) % len(pat)], oct_), vel))
            x += step
            j += 1
    elif t == 'broken68':
        pat = spec.get('pattern', (0, 1, 2))
        for j in range(int(round(bpb / 0.5))):
            out.append((j * 0.5, 0.45, _tone(tones, pat[j % len(pat)], oct_), vel + (8 if j % 3 == 0 else 0)))
    elif t == 'oompah':
        for b in range(1, int(bpb)):
            out.append((b, spec.get('len', 0.5), _tone(tones, 1 + (b - 1) % 2, oct_), vel))
    elif t == 'rhythm':
        for on, i in spec['hits']:
            if on < bpb:
                out.append((on, spec.get('len', 0.4), _tone(tones, i, oct_), vel))
    elif t == 'cliche':
        # The spy-film line over a chord: root, raised fifth, sixth, raised fifth (one per beat);
        # over a major chord the fifth itself instead of the raised one
        minor = (tones[1] - tones[0]) % 12 == 3
        steps = (0, 8, 9, 8) if minor else (0, 7, 9, 7)
        for b in range(int(bpb)):
            out.append((b, 0.9, tones[0] + oct_ + steps[b % 4], vel))
    else:
        raise ValueError(f'unknown accompaniment {t!r}')
    return out


# ---------------------------------------------------------------- bass

def bass_bar(tones, spec, bpb, next_root=None, rng=None, scale=None):
    """One bar of bass.  root = the chord root an octave below the voicing.  spec['type']:
        octave_quarters, root_fifth_halves, root_whole   (the original three)
        root_fifth     root on 1, fifth halfway (country, march, polka: the 'boom')
        octave_eighths driving root/octave eighths (rock, synthwave, techno uses offbeat)
        offbeat        root on every 'and' (techno)
        walking        quarter notes: root, chord/scale tones, chromatic approach to the next root
        boogie         swung eighths up and down root-3rd-5th-6th-b7 (blues)
        waltz          root on beat 1 only
        tumbao         bossa: root 1 (dotted quarter), fifth on the 'and' of 2, root on 3, fifth on the 'and' of 4
        reggae         one-drop bass: rests on 1, root on the 'and' of 1, fifth on 3
    """
    t = spec['type']
    root = tones[0] - 12
    vel = spec.get('vel', 95)
    if t == 'octave_quarters':
        return [(j, 0.75, root + (12 if j % 2 else 0), vel) for j in range(int(bpb))]
    if t == 'root_fifth_halves':
        return [(j * bpb / 2, bpb / 2 - 0.25, root + (7 if j else 0), vel) for j in range(2)]
    if t == 'root_whole':
        return [(0, bpb - 0.25, root, vel)]
    if t == 'root_fifth':
        return [(0, 0.8, root, vel), (bpb / 2, 0.8, root + 7 - (12 if root + 7 > 57 else 0), vel)]
    if t == 'octave_eighths':
        return [(j * 0.5, 0.4, root + (12 if spec.get('octaves') and j % 2 else 0), vel - (0 if j % 2 == 0 else 8))
                for j in range(int(bpb * 2))]
    if t == 'offbeat':
        return [(j + 0.5, 0.4, root, vel) for j in range(int(bpb))]
    if t == 'waltz':
        return [(0, 1.8, root, vel)]
    if t == 'tumbao':
        f = root + 7 - (12 if root + 7 > 57 else 0)
        return [(0, 1.4, root, vel), (1.5, 0.45, f, vel - 6), (2, 1.4, root, vel), (3.5, 0.45, f, vel - 6)][:4 if bpb >= 4 else 2]
    if t == 'reggae':
        return [(0.5, 0.9, root, vel), (1.5, 0.4, root + 7 - (12 if root + 7 > 57 else 0), vel - 6),
                (2.5, 0.9, root, vel)]
    if t == 'boogie':
        steps = (0, 4, 7, 9, 10, 9, 7, 4)
        return [(j * 0.5, 0.45, root + steps[j % 8], vel - (6 if j % 2 else 0)) for j in range(int(bpb * 2))]
    if t == 'walking':
        # Kept inside the chip's bass range (45-57: compile.py folds anything else by octaves,
        # which would turn a walking line into jumps)
        r = rng or random.Random(root)
        lo, hi = 45, 57
        first = lo + (root - lo) % 12
        pcs = {(p - root) % 12 for p in tones} | {(s - root) % 12 for s in (scale or ())}
        inrange = [p for p in range(lo, hi + 1) if (p - root) % 12 in pcs]
        notes = [first]
        for j in range(1, int(bpb)):
            if j == int(bpb) - 1 and next_root is not None:
                nr = lo + (next_root - lo) % 12
                appr = [x for x in (nr - 1, nr + 1) if lo <= x <= hi and x != notes[-1]]
                notes.append(r.choice(appr) if appr else nr)  # chromatic approach to the next root
                continue
            cands = [p for p in inrange if 0 < abs(p - notes[-1]) <= 5]
            notes.append(r.choice(cands) if cands else notes[-1])
        return [(j, 0.9, p, vel - (0 if j == 0 else 6)) for j, p in enumerate(notes)]
    raise ValueError(f'unknown bass {t!r}')
