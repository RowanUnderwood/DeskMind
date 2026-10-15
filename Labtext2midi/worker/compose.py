"""MIDI-GPT composition for DeskMind (generalised MIDI-GPT/compose-cracktro.py).

For each of a recipe's three sections (A, B, break; 8 bars, 12 for a blues) a chord scaffold is
written for the three voices (patterns.py); MIDI-GPT then regenerates the parts the recipe names
(the lead, and for some recipes the bass) in context: one-bar steps in four-bar windows, attention
masking, the settings that worked.  Theme A is generated twice and the take with the more varied
lead is kept.  Form, register/dynamic changes and the final cadence are composed in code, as in
Raster Rush.  Meter (3/4, 6/8) goes to MIDI-GPT in each bar; swing is applied by compile.py.

The arrangement keeps every section's generated parts (`sources`) so a song can be revised later
without generating it again.

Needs the MIDI-GPT environment (torch, midigpt) and an engine from load_engine().
"""
import copy
import random

import patterns as P
import recipes as R

TPQ = 480


def load_engine(checkpoint, device='cuda:0'):
    from midigpt.inference import InferenceEngine
    return InferenceEngine.from_checkpoint(str(checkpoint), device=device)


def _ticks(beats):
    return int(round(beats * TPQ))


def scaffold_notes(rec, tonic, prog, seed=0):
    """The three scaffold voices for one section: per track, per bar, notes in beats."""
    bpb = rec.beats_per_bar
    rng = random.Random(seed)
    tracks = [[], [], []]
    for k, numeral in enumerate(prog):
        tones, pcs = R.chord(tonic, rec.mode, numeral)
        nxt = R.chord(tonic, rec.mode, prog[(k + 1) % len(prog)])[0][0]
        tracks[0].append(P.lead_bar(tones, rec.lead, bpb, _lead_pcs(rec, tonic, numeral)))
        tracks[1].append(P.acc_bar(tones, rec.acc, bpb, k, rng))
        tracks[2].append(P.bass_bar(tones, rec.bass, bpb, nxt, rng, pcs))
    return tracks


def _score(rec, tracks):
    from midigpt import Score, Track, Bar, Note
    num, den = rec.meter
    out = []
    for tid, bars in enumerate(tracks):
        out.append(Track(bars=[Bar(notes=[Note(pitch=p, velocity=v, onset_ticks=_ticks(on), duration_ticks=max(1, _ticks(ln)))
                                          for on, ln, p, v in bar],
                                   ts_numerator=num, ts_denominator=den, beat_length=rec.beats_per_bar)
                               for bar in bars], instrument=rec.instruments[tid]))
    return Score(tracks=out, resolution=TPQ, tempo=400000)


def _bar_key(bar, res):
    return tuple((round(n.onset_ticks * TPQ / res), n.pitch) for n in bar.notes)


def changed_bars(score, scaffold, tid):
    """How many bars MIDI-GPT changed (onsets and pitches) on track tid."""
    return sum(_bar_key(a, score.resolution) != _bar_key(b, scaffold.resolution)
               for a, b in zip(score.tracks[tid].bars, scaffold.tracks[tid].bars))


def _lead_pcs(rec, tonic, numeral):
    tones, pcs = R.chord(tonic, rec.mode, numeral)
    if rec.lead_scale:
        pcs = sorted({(tonic + s) % 12 for s in R.LEAD_SCALES[rec.lead_scale]} | {p % 12 for p in tones})
    return pcs


def _generate(engine, rec, tonic, section, seed, log):
    prog = R.prog(rec, section)
    scaffold = _score(rec, scaffold_notes(rec, tonic, prog, seed))
    score = _infill(engine, rec, tonic, prog, scaffold, rec.model, seed, log, section, rec.novelty)
    score.changed = {tid: changed_bars(score, scaffold, tid) for tid in rec.model}
    score.repaired = _repair(score, rec, tonic, prog, rec.model)
    return score


# Revisions can regenerate a part the recipe itself leaves to the scaffold (the bass of most v2 recipes)
DEFAULT_MODEL = {0: ('eighth', 3, (60, 86)), 2: ('quarter', 4, (28, 50))}


def _infill(engine, rec, tonic, prog, score, models, seed, log, label, novelty_first):
    """MIDI-GPT regenerates tracks `models` (tid -> grid, max duration bin, pitch range) of a section,
    one after another, with the other tracks as context."""
    import torch
    from midigpt.inference import GenerationRequest, InferenceConfig, TrackPrompt
    nbars = len(prog)
    for tid, (grid, maxdur, (lo, hi)) in sorted(models.items()):
        log(f'{label}: {"lead" if tid == 0 else "bass"} (seed {seed})')
        attrs = {'min_polyphony': 0, 'max_polyphony': 0, 'max_note_duration': maxdur}
        bar_controls = {}
        for i, numeral in enumerate(prog):
            pcs = sorted({p % 12 for p in R.chord(tonic, rec.mode, numeral)[0]}) if tid == 2 else _lead_pcs(rec, tonic, numeral)
            bar_controls[i] = {'pitch_mask': {'pitch_classes': pcs, 'shape': {'type': 'uniform', 'min': lo, 'max': hi}}}
        controls = {'rhythm_mask': {'grid': {'unit': grid, 'strength': 0.9}}}
        prompts = [TrackPrompt(id=i, bars=list(range(nbars)) if i == tid else [],
                               attributes=attrs if i == tid else {}, controls=controls if i == tid else {},
                               bar_controls=bar_controls if i == tid else {}) for i in range(3)]
        before = score
        for novelty in ((True, False) if novelty_first else (False,)):
            cfg = InferenceConfig(model_dim=4, tracks_per_step=3, bars_per_step=1, mask_mode='attention',
                                  seed=seed + tid * 71, temperature=rec.temperature, top_p=0.97,
                                  max_attempts=8 if novelty else 5, temperature_escalation=1.1,
                                  novelty_check=novelty, density_hard_limit=24)
            try:
                with torch.inference_mode():
                    score = engine.session(before, GenerationRequest(tracks=prompts, config=cfg)).run()
                break
            except RuntimeError as e:                    # every attempt copied the input somewhere
                log(f'{label}: novelty check gave up ({str(e)[:60]}...); without it')
    return score


def _repair(score, rec, tonic, prog, tids):
    """The pitch mask is not strictly hard: some notes (a few percent; more in jazz and blues) come back
    outside it.  Each is moved to the nearest allowed pitch (the lower one on a tie).  Returns how many."""
    fixed = 0
    for tid in tids:
        lo, hi = (rec.model.get(tid) or DEFAULT_MODEL[tid])[2]
        for i, bar in enumerate(score.tracks[tid].bars):
            numeral = prog[i]
            allowed = set(_lead_pcs(rec, tonic, numeral)) if tid == 0 else \
                {p % 12 for p in R.chord(tonic, rec.mode, numeral)[0]}
            for n in bar.notes:
                if n.pitch % 12 not in allowed:
                    n.pitch = min((n.pitch + d for d in (-1, 1, -2, 2, -3, 3) if (n.pitch + d) % 12 in allowed),
                                  key=lambda p: (abs(p - n.pitch), p))
                    fixed += 1
                # The range is a soft shape, not a limit: a note far outside it goes an octave in
                while n.pitch < lo - 2:
                    n.pitch += 12
                    fixed += 1
                while n.pitch > hi + 2:
                    n.pitch -= 12
                    fixed += 1
    return fixed


def _repeats(score):
    """Lead bars that are exact copies (onsets and pitches) of the bar before."""
    res = score.resolution
    bars = score.tracks[0].bars
    return sum(1 for a, b in zip(bars, bars[1:]) if b.notes and _bar_key(a, res) == _bar_key(b, res))


def _quality(score, rec):
    """(ok, sort key): enough lead notes, enough different pitches, not too many copied bars."""
    distinct, notes = _lead_variety(score)
    nbars = rec.section_bars
    rep = _repeats(score)
    ok = notes >= rec.min_lead * nbars // 8 and distinct >= 5 and rep <= nbars // 4
    return ok, (ok, -rep, min(distinct, 9), notes)


def _generate_ok(engine, rec, tonic, section, seed, log):
    """_generate, retried with new seeds (at most 4 tries) while the lead comes back too sparse, with too
    few different pitches, or copying bars; the best try is kept."""
    best = None
    for k in range(4):
        sc = _generate(engine, rec, tonic, section, seed + k * 7919, log)
        ok, key = _quality(sc, rec)
        if best is None or key > best[0]:
            best = (key, sc)
        if ok:
            break
        d, n = _lead_variety(sc)
        log(f'{section}: lead {n} notes, {d} pitches, {_repeats(sc)} copied bars; trying again')
    return best[1]


def _lead_variety(score):
    notes = [n for b in score.tracks[0].bars for n in b.notes]
    return len({n.pitch for n in notes}), len(notes)


def source_parts(score):
    """A generated section as plain data: per track, per bar, (onset, length, pitch, velocity) in beats."""
    res = score.resolution
    return [[[(n.onset_ticks / res, n.duration_ticks / res, n.pitch, n.velocity) for n in bar.notes]
             for bar in tr.bars] for tr in score.tracks]


def _tremolo(notes, step):
    out = []
    for on, ln, p, v in notes:
        if ln < step * 1.5:
            out.append((on, ln, p, v))
            continue
        k = 0
        while k * step < ln - step * 0.25:
            out.append((on + k * step, step * 0.8, p, v - (0 if k == 0 else 6)))
            k += 1
    return out


def arrange(rec, s, sources, kind=None):
    """The song from its generated sections: form, register, dynamics and the closing cadence.
    `sources`: section name -> source_parts().  Returns the arrangement for compile.py."""
    tonic = s['tonic']
    bpb = rec.beats_per_bar
    n = rec.section_bars
    kind = kind or ('sketch' if s.get('sketch') else rec.form)
    voices = [[], [], []]
    sections = []
    bar0 = 0
    for name, source, bars, opts in R.form(s['bars'], n, kind):
        parts = sources[source]
        for tid in range(3):
            if (tid == 0 and opts.get('mute_lead')) or (tid == 1 and opts.get('mute_acc')) or \
                    (tid == 2 and opts.get('mute_bass')):
                continue
            for k, i in enumerate(bars):
                notes = parts[tid][i]
                if tid == 1 and opts.get('arp_sparse'):
                    notes = notes[::2]
                cap = rec.lengths[tid]
                for on, ln, p, v in notes:
                    p = min(100, p + opts.get('octave', 0)) if tid == 0 else p
                    p += opts.get('transpose', 0)
                    v = max(1, min(127, round(v * opts.get('gain', 1.0))))
                    voices[tid].append(((bar0 + k) * bpb + on, min(ln, cap), p, v))
        sections.append({'name': name, 'start': bar0, 'bars': len(bars), 'drums': opts.get('drums', 'normal')})
        bar0 += len(bars)
    if rec.tremolo:
        voices[0] = _tremolo(voices[0], rec.tremolo)
    # Closing cadence in the last bar: lead down the tonic chord, accompaniment on it, bass on the root
    # (in the final key, after a key change)
    tonic = (tonic + R.form(s['bars'], n, kind)[-1][3].get('transpose', 0)) % 12
    last = (bar0 - 1) * bpb
    for v in voices:
        v[:] = [x for x in v if x[0] < last]
    third = 3 if rec.mode in ('minor', 'dorian', 'harmonic') else 4
    lb = 64 + (tonic - 64) % 12
    eighths = int(round(bpb * 2))
    cad = (list(rec.cadence) + [rec.cadence[-1]] * eighths)[:eighths]
    voices[0] += [(last + j * 0.5, 1 / 3, lb + off, 94) for j, off in enumerate(cad)]
    ab = 55 + (tonic - 55) % 12
    triad = (ab, ab + third, ab + 7, ab + 12)
    step = rec.acc.get('step', 0.5) if rec.acc['type'] in ('arp', 'alberti') else 0.5
    alen = rec.acc.get('len', step) if rec.acc['type'] == 'arp' else step
    voices[1] += [(last + j * step, alen * 0.8, triad[j % 4], 75) for j in range(int(round(bpb / step)))]
    voices[2] += [(last, bpb - 1 / 480, 28 + (tonic - 28) % 12, 100)]
    return {'bpm': s['tempo'], 'bars': bar0, 'beats_per_bar': bpb, 'meter': list(rec.meter), 'sections': sections,
            'voices': voices, 'drums': s['drums'], 'hats': rec.hats, 'gains': rec.gains,
            'drum_pattern': rec.drum_pattern, 'swing': rec.swing, 'style': rec.name, 'instruments': list(rec.instruments),
            'spec': s, 'sources': sources}


def compose(engine, spec, log=print, allowed=None):
    """Returns (arrangement for compile.py, report).  spec: style/key/tempo/energy/drums/length/title/seed,
    and sketch (theme A and a cadence only).  `allowed` = the styles accepted (recipes.resolve)."""
    s = R.resolve(spec, allowed)
    rec = R.RECIPES[s['style']]
    tonic, seed = s['tonic'], s['seed'] or 6502
    # Theme A twice (keep the more varied lead), then B and the break
    takes = [(_generate_ok(engine, rec, tonic, 'A', seed + k * 61499, log), seed + k * 61499) for k in range(2)]
    a, a_seed = max(takes, key=lambda x: _lead_variety(x[0]))
    src = {'A': a}
    if not s['sketch']:
        src['B'] = _generate_ok(engine, rec, tonic, 'B', seed + 1987, log)
        src['break'] = _generate_ok(engine, rec, tonic, 'break', seed + 1991, log)
        if any(x[1] == 'C' for x in R.form(s['bars'], rec.section_bars, rec.form)):
            src['C'] = _generate_ok(engine, rec, tonic, 'C', seed + 2003, log)
    arr = arrange(rec, s, {k: source_parts(v) for k, v in src.items()})
    arr['seeds'] = {'A': a_seed, 'B': seed + 1987, 'break': seed + 1991}
    report = dict(s)
    report.update({'theme_a_seed': a_seed, 'lead_variety': _lead_variety(a),
                   'model_changed_bars': {k: {'lead': v.changed.get(0), 'bass': v.changed.get(2)} for k, v in src.items()},
                   'repaired_notes': {k: v.repaired for k, v in src.items()},
                   'copied_bars': {k: _repeats(v) for k, v in src.items()},
                   'method': 'MIDI-GPT regenerated the recipe\'s parts over a composed chord scaffold; '
                             'accompaniment, form, dynamics, drums and the final cadence are composed in code.'})
    return arr, report


# ================================================================ revisions

PARTS = {'lead': 0, 'accompaniment': 1, 'bass': 2}
PART_NAMES = {v: k for k, v in PARTS.items()}
# Drum patterns that fit a meter (revisions cycle through these)
PATTERNS_BY_BEATS = {4.0: ['backbeat', 'rock', 'four_floor', 'shuffle', 'train', 'halftime', 'onedrop', 'march',
                           'bossa', 'jazz'],
                     3.0: ['waltz', 'jig']}


def _acc_variant(spec, k):
    """Another accompaniment from the same recipe: the pattern turned round, or an octave up."""
    s = dict(spec)
    if s.get('pattern'):
        p = list(s['pattern'])
        s['pattern'] = p[k % len(p):] + p[:k % len(p)]
    elif s['type'] == 'rhythm':
        hits = list(s['hits'])
        s['hits'] = [(on, hits[(j + k) % len(hits)][1]) for j, (on, _) in enumerate(hits)]
    else:
        s['octave'] = spec.get('octave', 12) + (12 if k % 2 else 0)
    if s == spec:
        s['octave'] = spec.get('octave', 12) + 12
    return s


def _parts_to_score(rec, parts):
    """A generated section (source_parts) back as a MIDI-GPT score."""
    return _score(rec, parts)


def arrangement_from_midi(midi_path, report):
    """A v1 song (made before revisions: no arrangement kept) rebuilt from its MIDI and report, for the
    revisions that need no model: drums, tempo and key.  No `sources`."""
    from symusic import Score
    s = dict(report)
    rec = R.RECIPES[s['style']]
    sc = Score(str(midi_path))
    tpq = sc.ticks_per_quarter
    voices = [[(n.time / tpq, n.duration / tpq, n.pitch, n.velocity) for n in tr.notes] for tr in sc.tracks[:3]]
    sections, bar0 = [], 0
    for name, _, bars, opts in R.form(s['bars'], rec.section_bars, rec.form):
        sections.append({'name': name, 'start': bar0, 'bars': len(bars), 'drums': opts.get('drums', 'normal')})
        bar0 += len(bars)
    s.setdefault('sketch', False)
    return {'bpm': s['tempo'], 'bars': bar0, 'beats_per_bar': rec.beats_per_bar, 'meter': list(rec.meter),
            'sections': sections, 'voices': voices, 'drums': s['drums'], 'hats': rec.hats, 'gains': rec.gains,
            'drum_pattern': rec.drum_pattern, 'swing': rec.swing, 'style': rec.name,
            'instruments': list(rec.instruments), 'spec': s}


def revise(engine, base, edits, seed, log=print):
    """A new revision of a song.  `base` = its arrangement; `edits` (all optional):
        regenerate   parts to write anew: 'lead', 'bass' (MIDI-GPT, the other parts as context) and/or
                     'accompaniment' (another pattern from the recipe).  Parts not named are locked:
                     they come back note for note (checked).
        drums        'none' | 'light' | 'full';  drum_pattern  a pattern name, or 'next'
        tempo        quarter notes per minute;  transpose  semitones (-7 .. 7)
        length       'short' .. 'epic' (a sketch becomes a full song: build)
    Returns (arrangement, report with 'change' = one line saying what was done)."""
    rec = R.RECIPES[base['style']]
    s = dict(base['spec'])
    s['sketch'] = bool(s.get('sketch'))
    regen = [PARTS[p] for p in edits.get('regenerate') or [] if p in PARTS]
    change = []
    sources = {k: [[[list(n) for n in bar] for bar in tr] for tr in v] for k, v in (base.get('sources') or {}).items()}
    if not sources and (regen or edits.get('length')):
        raise ValueError('this song was made before revisions: only its drums, tempo and key can change')
    k = max(-7, min(7, int(edits.get('transpose') or 0)))
    if k:
        auto_title = s.get('title') == f'{rec.name.title()} in {s.get("key")}'
        s['tonic'] = (s['tonic'] + k) % 12
        s['key'] = R.NAMES[s['tonic']]
        if auto_title:                           # the made-up title names the key: keep it true
            s['title'] = f'{rec.name.title()} in {s["key"]}'
        change.append(f'key {"up" if k > 0 else "down"} {abs(k)} to {s["key"]}')
        for v in sources.values():
            for tr in v:
                for bar in tr:
                    for n in bar:
                        n[2] += k
    if edits.get('tempo'):
        bpm = max(rec.bpm[0] - 20, min(rec.bpm[2] + 20, int(edits['tempo'])))
        if bpm != s['tempo']:
            change.append(f'tempo {s["tempo"]} -> {bpm}')
            s['tempo'] = bpm
    length = edits.get('length')
    if length in R.LENGTHS and (length != s['length'] or s['sketch']):
        change.append(f'{"built into a full song" if s["sketch"] else "length"}: {length}')
        s['length'] = length
        s['sketch'] = False
        s['bars'] = min(R.LENGTHS[length], int(R.MAX_SECONDS * s['tempo'] / 60 / rec.beats_per_bar))
    elif s['sketch'] and edits.get('build'):
        change.append('built into a full song')
        s['sketch'] = False
    if not sources:                              # a v1 song: cheap edits on its notes
        arr = dict(base, spec=s, bpm=s['tempo'])
        if k:
            arr['voices'] = [[(on, ln, p + k, v) for on, ln, p, v in voice] for voice in base['voices']]
    else:
        # Sections the (new) form needs that this song doesn't have yet: generated fresh
        kind = 'sketch' if s['sketch'] else rec.form
        needed = {x[1] for x in R.form(s['bars'], rec.section_bars, kind)}
        for sec in sorted(needed - set(sources)):
            log(f'new section {sec}')
            sources[sec] = source_parts(_generate_ok(engine, rec, s['tonic'], sec, seed + 1987 + len(sec), log))
            change.append(f'new {sec} section')
        model_tids = [t for t in regen if t != 1]
        for j, (sec, parts) in enumerate(sorted(sources.items())):
            prog = R.prog(rec, sec)
            if model_tids:
                models = {t: rec.model.get(t) or DEFAULT_MODEL[t] for t in model_tids}
                sc = _infill(engine, rec, s['tonic'], prog, _parts_to_score(rec, parts), models, seed + j * 101, log,
                             f'revise {sec}', True)
                _repair(sc, rec, s['tonic'], prog, model_tids)
                new = source_parts(sc)
                for t in model_tids:
                    parts[t] = [[list(n) for n in bar] for bar in new[t]]
            if 1 in regen:
                spec = _acc_variant(rec.acc, 1 + seed % 7)
                rng = random.Random(seed + j)
                parts[1] = [[list(n) for n in P.acc_bar(R.chord(s['tonic'], rec.mode, num)[0], spec,
                                                        rec.beats_per_bar, i, rng)] for i, num in enumerate(prog)]
        if regen:
            change.append('new ' + ' and '.join(PART_NAMES[t] for t in sorted(regen)))
        arr = arrange(rec, s, sources, kind)
        # Locked parts: note for note what the base had (after a key change, moved by it)
        for sec, parts in (base.get('sources') or {}).items():
            for t in range(3):
                if t in regen:
                    continue
                want = [[[n[0], n[1], n[2] + k, n[3]] for n in bar] for bar in parts[t]]
                have = [[list(n) for n in bar] for bar in sources[sec][t]]
                if want != have:
                    raise RuntimeError(f'locked {PART_NAMES[t]} of section {sec} changed')
    pattern = edits.get('drum_pattern')
    arr['drum_pattern'] = base.get('drum_pattern', rec.drum_pattern)
    if pattern:
        fam = PATTERNS_BY_BEATS.get(float(arr['beats_per_bar']), [])
        if pattern == 'next' and fam:
            cur = arr['drum_pattern']
            pattern = fam[(fam.index(cur) + 1) % len(fam)] if cur in fam else fam[0]
        if pattern in fam:
            arr['drum_pattern'] = pattern
            change.append(f'drum pattern {pattern}')
    if edits.get('drums') in R.DRUMS and edits['drums'] != base.get('drums'):
        arr['drums'] = edits['drums']
        arr['spec']['drums'] = edits['drums']
        change.append(f'drums {edits["drums"]}')
    report = dict(arr['spec'])
    report.update({'change': '; '.join(change) or 'no change', 'locked': [PART_NAMES[t] for t in range(3) if t not in regen],
                   'revision_seed': seed})
    return arr, report


def write_midi(arr, path):
    """The arrangement as a three-track MIDI file (for listening on the PC)."""
    from symusic import Score, Track, Note, Tempo, TimeSignature
    tpq = 480
    sc = Score(tpq)
    sc.tempos.append(Tempo(0, qpm=float(arr['bpm'])))
    num, den = arr.get('meter', (4, 4))
    sc.time_signatures.append(TimeSignature(0, int(num), int(den)))
    progs = arr.get('instruments') or (80, 81, 38)
    for name, prog, voice in zip(('Tandy voice 1 - lead', 'Tandy voice 2 - accompaniment', 'Tandy voice 3 - bass'),
                                 progs, arr['voices']):
        tr = Track(name=name, program=prog)
        for on, ln, p, v in sorted(voice):
            tr.notes.append(Note(round(on * tpq), max(1, round(ln * tpq)), int(p), int(v)))
        sc.tracks.append(tr)
    sc.dump_midi(str(path))
