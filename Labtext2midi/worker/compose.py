"""MIDI-GPT composition for DeskMind (generalised MIDI-GPT/compose-cracktro.py).

For each of a recipe's three eight-bar sections (A, B, break) a chord scaffold is written for
the three voices; MIDI-GPT then regenerates the lead and the bass in context (one-bar steps in
four-bar windows, attention masking: the settings that worked).  Theme A is generated twice and
the take with the more varied lead is kept.  Form, register/dynamic changes and the final
cadence are composed in code, as in Raster Rush.

Needs the MIDI-GPT environment (torch, midigpt) and an engine from load_engine().
"""
import copy

import recipes as R


def load_engine(checkpoint, device='cuda:0'):
    from midigpt.inference import InferenceEngine
    return InferenceEngine.from_checkpoint(str(checkpoint), device=device)


def _scaffold(rec, tonic, prog):
    from midigpt import Score, Track, Bar, Note
    lead, arp, bass = [], [], []
    steps = 1920 // rec.arp_step
    for numeral in prog:
        tones = R.chord_tones(tonic, numeral)
        lead.append(Bar(notes=[Note(pitch=tones[i] + rec.lead_octave, velocity=90, onset_ticks=e * 240, duration_ticks=d)
                               for i, e, d in rec.lead_pattern]))
        notes = []
        for j in range(steps):
            on = j * rec.arp_step
            lift = 12 if rec.arp_lift and on >= 960 else 0
            notes.append(Note(pitch=tones[rec.arp_pattern[j % len(rec.arp_pattern)]] + 12 + lift,
                              velocity=70 + (8 if on % 480 == 0 else 0), onset_ticks=on, duration_ticks=rec.arp_len))
        arp.append(Bar(notes=notes))
        root = tones[0] - 12
        if rec.bass == 'octave_quarters':
            bn = [Note(pitch=root + (12 if j % 2 else 0), velocity=95, onset_ticks=j * 480, duration_ticks=360) for j in range(4)]
        elif rec.bass == 'root_fifth_halves':
            bn = [Note(pitch=root + (7 if j else 0), velocity=95, onset_ticks=j * 960, duration_ticks=840) for j in range(2)]
        else:
            bn = [Note(pitch=root, velocity=95, onset_ticks=0, duration_ticks=1800)]
        bass.append(Bar(notes=bn))
    return Score(tracks=[Track(bars=lead, instrument=80), Track(bars=arp, instrument=81), Track(bars=bass, instrument=38)],
                 resolution=480, tempo=400000)


def _bar_key(bar, res):
    return tuple((round(n.onset_ticks * 480 / res), n.pitch) for n in bar.notes)


def changed_bars(score, scaffold, tid):
    """How many of the 8 bars MIDI-GPT changed (onsets and pitches) on track tid."""
    return sum(_bar_key(a, score.resolution) != _bar_key(b, scaffold.resolution)
               for a, b in zip(score.tracks[tid].bars, scaffold.tracks[tid].bars))


def _generate(engine, rec, tonic, section, seed, log):
    import torch
    from midigpt.inference import GenerationRequest, InferenceConfig, TrackPrompt
    prog = rec.progs[section]
    score = scaffold = _scaffold(rec, tonic, prog)
    for tid, (grid, maxdur, (lo, hi)) in sorted(rec.model.items()):
        log(f'{section}: {"lead" if tid == 0 else "bass"} (seed {seed})')
        attrs = {'min_polyphony': 0, 'max_polyphony': 0, 'max_note_duration': maxdur}
        bar_controls = {}
        for i, numeral in enumerate(prog):
            pcs = sorted({p % 12 for p in R.chord_tones(tonic, numeral)}) if tid == 2 else R.scale_pcs(tonic, rec.mode, numeral)
            bar_controls[i] = {'pitch_mask': {'pitch_classes': pcs, 'shape': {'type': 'uniform', 'min': lo, 'max': hi}}}
        controls = {'rhythm_mask': {'grid': {'unit': grid, 'strength': 0.9}}}
        prompts = [TrackPrompt(id=i, bars=list(range(8)) if i == tid else [],
                               attributes=attrs if i == tid else {}, controls=controls if i == tid else {},
                               bar_controls=bar_controls if i == tid else {}) for i in range(3)]
        cfg = InferenceConfig(model_dim=4, tracks_per_step=3, bars_per_step=1, mask_mode='attention',
                              seed=seed + tid * 71, temperature=rec.temperature, top_p=0.97, max_attempts=5,
                              temperature_escalation=1.1, novelty_check=False, density_hard_limit=24)
        with torch.inference_mode():
            score = engine.session(score, GenerationRequest(tracks=prompts, config=cfg)).run()
    score.changed = {tid: changed_bars(score, scaffold, tid) for tid in rec.model}
    return score


MIN_LEAD = 16                       # notes in an 8-bar section: fewer and the tune has gone missing


def _generate_ok(engine, rec, tonic, section, seed, log):
    """_generate, retried with new seeds (at most 3 tries) while the lead comes back too sparse."""
    for k in range(3):
        sc = _generate(engine, rec, tonic, section, seed + k * 7919, log)
        if _lead_variety(sc)[1] >= MIN_LEAD:
            break
        log(f'{section}: lead too sparse ({_lead_variety(sc)[1]} notes), trying again')
    return sc


def _lead_variety(score):
    notes = [n for b in score.tracks[0].bars for n in b.notes]
    return len({n.pitch for n in notes}), len(notes)


def compose(engine, spec, log=print):
    """Returns (arrangement for compile.py, report).  spec: style/key/tempo/energy/drums/length/title/seed."""
    s = R.resolve(spec)
    rec = R.RECIPES[s['style']]
    tonic, seed = s['tonic'], s['seed'] or 6502
    # Theme A twice (keep the more varied lead), then B and the break
    takes = [(_generate_ok(engine, rec, tonic, 'A', seed + k * 61499, log), seed + k * 61499) for k in range(2)]
    a, a_seed = max(takes, key=lambda x: _lead_variety(x[0]))
    src = {'A': a, 'B': _generate_ok(engine, rec, tonic, 'B', seed + 1987, log),
           'break': _generate_ok(engine, rec, tonic, 'break', seed + 1991, log)}
    voices = [[], [], []]
    sections = []
    bar0 = 0
    for name, source, bars, opts in R.form(s['bars']):
        sc = src[source]
        res = sc.resolution
        for tid in range(3):
            for k, i in enumerate(bars):
                bar = copy.deepcopy(sc.tracks[tid].bars[i])
                if tid == 0 and opts.get('mute_lead'):
                    continue
                notes = bar.notes[::2] if tid == 1 and opts.get('arp_sparse') else bar.notes
                cap = res * rec.lengths[tid]
                for n in notes:
                    p = min(100, n.pitch + opts.get('octave', 0)) if tid == 0 else n.pitch
                    v = max(1, min(127, round(n.velocity * opts.get('gain', 1.0))))
                    voices[tid].append(((bar0 + k) * 4 + n.onset_ticks / res, min(n.duration_ticks, cap) / res, p, v))
        sections.append({'name': name, 'start': bar0, 'bars': len(bars),
                         'drums': opts.get('drums', 'normal')})
        bar0 += len(bars)
    # Closing cadence in the last bar: lead down the tonic chord, accompaniment on it, bass on the root
    last = (bar0 - 1) * 4
    for v in voices:
        v[:] = [n for n in v if n[0] < last]
    third = 3 if rec.mode == 'minor' else 4
    lb = 64 + (tonic - 64) % 12
    voices[0] += [(last + j * 0.5, 1 / 3, lb + off, 94) for j, off in enumerate(rec.cadence)]
    ab = 55 + (tonic - 55) % 12
    triad = (ab, ab + third, ab + 7, ab + 12)
    steps = 1920 // rec.arp_step
    voices[1] += [(last + j * rec.arp_step / 480, rec.arp_len / 480 * 0.8, triad[j % 4], 75) for j in range(steps)]
    voices[2] += [(last, 4 - 1 / 480, 28 + (tonic - 28) % 12, 100)]
    arr = {'bpm': s['tempo'], 'bars': bar0, 'sections': sections, 'voices': voices, 'drums': s['drums'],
           'hats': rec.hats, 'gains': rec.gains}
    report = dict(s)
    report.update({'theme_a_seed': a_seed, 'lead_variety': _lead_variety(a),
                   'model_changed_bars': {k: {'lead': v.changed.get(0), 'bass': v.changed.get(2)} for k, v in src.items()},
                   'method': 'MIDI-GPT regenerated lead and bass over a composed chord scaffold; '
                             'accompaniment, form, dynamics, drums and the final cadence are composed in code.'})
    return arr, report


def write_midi(arr, path):
    """The arrangement as a three-track MIDI file (for listening on the PC)."""
    from symusic import Score, Track, Note, Tempo
    tpq = 480
    sc = Score(tpq)
    sc.tempos.append(Tempo(0, qpm=float(arr['bpm'])))
    for name, prog, voice in zip(('Tandy voice 1 - lead', 'Tandy voice 2 - accompaniment', 'Tandy voice 3 - bass'),
                                 (80, 81, 38), arr['voices']):
        tr = Track(name=name, program=prog)
        for on, ln, p, v in sorted(voice):
            tr.notes.append(Note(round(on * tpq), max(1, round(ln * tpq)), int(p), int(v)))
        sc.tracks.append(tr)
    sc.dump_midi(str(path))
