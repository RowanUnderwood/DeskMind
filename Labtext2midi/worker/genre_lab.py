"""Genre lab: are the candidate recipes musically valid, and do they sound different?

    MIDI-GPT\\.venv\\Scripts\\python.exe worker\\genre_lab.py run    [--url http://127.0.0.1:8288] [--styles a,b]
                                                               [--seeds 6] [--full 2] [--plain 3] [--out DIR]
    MIDI-GPT\\.venv\\Scripts\\python.exe worker\\genre_lab.py report DIR

`run` asks a music worker started with --dev (every candidate recipe; the live worker on 8287 is
not touched) for, per style: `seeds` sketches (theme A + cadence), `full` medium-length songs, and
`plain` sketches with the scaffold's original GM programs (does the genre's instrument choice change
what MIDI-GPT writes?).  Everything is saved under Labtext2midi/out/genre_lab/<time>/, then `report`
writes:

  report.html   validity gates per piece, features, leave-one-out nearest-centroid accuracy with a
                confusion matrix (all features, and the lead alone = what the model wrote), separation
                per genre, the instrument check, and links to every preview
  quiz/         quiz.html + anonymous clips: 3 sketches per genre in random order; the listener names
                the genre of each; the page scores it and gives a results JSON to paste back

The recipe decides tempo, meter, chords and drums, so the all-feature accuracy is high by
construction.  The listening quiz is the gate (DESKMIND-MUSIC-DESIGN.md): a genre ships when at
least 5 of 6 sketches pass validity and the listener names it in at least 2 of its 3 clips.
"""
import argparse
import base64
import html
import json
import math
import random
import shutil
import sys
import time
from collections import Counter, defaultdict
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import compile as K  # noqa: E402
import compose as C  # noqa: E402
import recipes as R  # noqa: E402

OUT = HERE.parent / 'out' / 'genre_lab'

# Validity limits, calibrated on music known to be good: Raster Rush (TL/3-confirmed) has 24% stepwise
# motion and a 24-semitone range, Windowlight (liked) 8% and 26, the v1 chat songs 17-55% and up to 34
# (with octave sections).  Stepwise >= 5% only rejects a lead that is nothing but broken chords.
GATES = {'notes per bar': 12, 'copied bars': 0.25}           # copied bars: share of the section's bars
GATE_OVERRIDES = {'chiptune': {'notes per bar': 16}, 'surf': {'notes per bar': 24},   # tremolo repeats
                  'techno': {'copied bars': 0.5}}             # a repeating riff is the point
DRUM_KINDS = ('kick', 'snare', 'hat', 'open_hat', 'rim', 'ride')


# ================================================================ generating

def _http(url, body=None, timeout=600):
    import urllib.error
    import urllib.request
    req = urllib.request.Request(url, data=None if body is None else json.dumps(body).encode(),
                                 headers={'Content-Type': 'application/json'})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.loads(r.read())
    except urllib.error.HTTPError as e:          # the worker answers errors as JSON too
        return json.loads(e.read() or b'{}')


def run(args):
    url = args.url.rstrip('/')
    h = _http(url + '/health', timeout=10)
    if not h.get('ok'):
        sys.exit(f'worker not ready: {h}')
    styles = args.styles.split(',') if args.styles else list(h['styles'])
    missing = [s for s in styles if s not in h['styles']]
    if missing:
        sys.exit(f'the worker does not offer {missing} (start it with --dev)')
    out = Path(args.out) if args.out else OUT / time.strftime('%Y%m%d-%H%M%S')
    out.mkdir(parents=True, exist_ok=True)
    jobs = []
    for st in styles:
        jobs += [(st, 'sketch', 100 + k, {'sketch': True}) for k in range(args.seeds)]
        jobs += [(st, 'full', 500 + k, {'length': 'medium'}) for k in range(args.full)]
        if tuple(R.RECIPES[st].instruments) != (80, 81, 38):
            jobs += [(st, 'plain', 100 + k, {'sketch': True, 'plain_instruments': True}) for k in range(args.plain)]
    print(f'{len(jobs)} pieces from {url} into {out}', flush=True)
    t0 = time.time()
    for n, (st, kind, seed, extra) in enumerate(jobs, 1):
        d = out / st
        d.mkdir(exist_ok=True)
        base = d / f'{kind}_{seed}'
        if base.with_suffix('.json').exists():
            continue
        spec = dict(extra, style=st, seed=seed)
        r = _http(url + '/compose', spec)
        if not r.get('ok'):
            print(f'  {st} {kind} {seed}: ERROR {r.get("error")}', flush=True)
            base.with_suffix('.json').write_text(json.dumps({'spec': spec, 'error': r.get('error')}))
            continue
        for ext in ('t3', 'wav', 'midi'):
            (base.parent / f'{base.name}.{"mid" if ext == "midi" else ext}').write_bytes(base64.b64decode(r[ext]))
        base.with_suffix('.json').write_text(json.dumps({'spec': spec, 'report': r['report'], 'arrangement': r['arrangement']}))
        rep = r['report']
        print(f'  [{n}/{len(jobs)}] {st:10} {kind:6} {seed}: {rep["key"]:3} {rep["tempo"]} bpm {rep["seconds"]:6.1f} s '
              f'{rep["stream"]["bytes"]:6} B {rep["compose_seconds"]:4.1f} s', flush=True)
    print(f'done in {time.time() - t0:.0f} s', flush=True)
    report(out)


# ================================================================ measuring

def bar_map(arr, rec):
    """Song bar -> (section source, bar index in it), from the same form compose used."""
    s = arr['spec']
    kind = 'sketch' if s.get('sketch') else rec.form
    out = []
    for _, src, bars, _ in R.form(s['bars'], rec.section_bars, kind):
        out += [(src, i) for i in bars]
    return out


def measure(arr, rep, rec):
    """Validity checks and a feature vector for one piece."""
    bpb = arr['beats_per_bar']
    sw = arr.get('swing', 0.5)
    tonic = arr['spec']['tonic']
    bmap = bar_map(arr, rec)
    last_bar = arr['bars'] - 1
    lead = sorted(tuple(x) for x in arr['voices'][0] if x[0] < last_bar * bpb)   # without the cadence bar
    strong = {4.0: (0, 2), 3.0: (0, 1.5) if arr['meter'][1] == 8 else (0,)}[bpb]
    in_scale = ct_strong = n_strong = 0
    for on, ln, p, v in lead:
        bar = int(on // bpb)
        src, i = bmap[bar]
        numeral = R.prog(rec, src)[i]
        # A power chord (root + fifth) implies the mode's triad: its third is a chord tone too
        tones = {x % 12 for x in R.chord(tonic, rec.mode, numeral[:-1] if numeral.endswith('5') else numeral)[0]}
        in_scale += (p % 12) in set(C._lead_pcs(rec, tonic, numeral))
        pos = on - bar * bpb
        if any(abs(pos - s) < 0.01 for s in strong):
            n_strong += 1
            ct_strong += (p % 12) in tones
    n = max(1, len(lead))
    pitches = [x[2] for x in lead]
    ivs = [b - a for a, b in zip(pitches, pitches[1:])]
    gaps = [(b[0] - (a[0] + a[1])) / bpb for a, b in zip(lead, lead[1:])]
    lead_bars = max(1, len({int(x[0] // bpb) for x in lead}))
    sec_bars = rec.section_bars
    step = sum(abs(i) <= 2 for i in ivs) / max(1, len(ivs))
    span = max(pitches or [0]) - min(pitches or [0])
    copied = (rep.get('copied_bars') or {}).get('A', 0)
    changed = (rep.get('model_changed_bars') or {}).get('A', {}).get('lead') or 0
    stream = rep['stream']
    g = dict(GATES, **GATE_OVERRIDES.get(rec.name, {}))
    checks = {
        'in scale': (in_scale / n, in_scale / n >= 0.9),
        'chord tones on strong beats': (ct_strong / max(1, n_strong), n_strong == 0 or ct_strong / n_strong >= 0.5),
        'notes per bar': (len(lead) / lead_bars, 0.75 <= len(lead) / lead_bars <= g['notes per bar']),
        'longest gap (bars)': (max(gaps or [0]), max(gaps or [0]) <= 2.0),
        'distinct pitches': (len(set(pitches)), len(set(pitches)) >= 5),
        'stepwise motion': (step, step >= 0.05),
        'range (semitones)': (span, 5 <= span <= 36),
        'model changed bars': (changed / sec_bars, changed >= sec_bars / 2),
        'copied bars': (copied, copied <= sec_bars * g['copied bars']),
        'repaired notes': ((rep.get('repaired_notes') or {}).get('A', 0), True),
        'stream ok': (stream['bytes'], stream['bytes'] <= 30000 and stream['max_bytes_per_tick'] <= 32),
    }
    return checks, features(arr, rec)


def _hist(values, edges):
    h = [0] * (len(edges) + 1)
    for x in values:
        k = 0
        while k < len(edges) and x > edges[k]:
            k += 1
        h[k] += 1
    t = max(1, sum(h))
    return [c / t for c in h]


def features(arr, rec):
    """(all features, lead-only features).  Timing is measured after swing."""
    bpb = arr['beats_per_bar']
    sw = arr.get('swing', 0.5)
    tonic = arr['spec']['tonic']
    beats = max(1, arr['bars'] * bpb)
    swung = [[(K.swing_beat(x[0], sw), x[1], x[2]) for x in sorted(tuple(v) for v in voice)] for voice in arr['voices']]
    lead = swung[0]
    offs = [x[0] % 1 for v in swung[:2] for x in v if 0.4 < x[0] % 1 < 0.85]
    measured_swing = sum(offs) / len(offs) if offs else 0.5
    iois = [b[0] - a[0] for a, b in zip(lead, lead[1:])]
    ivs = [abs(b[2] - a[2]) for a, b in zip(lead, lead[1:])]
    pcs = [0] * 12
    for x in lead:
        pcs[(x[2] - tonic) % 12] += 1
    pcs = [c / max(1, len(lead)) for c in pcs]
    sync = sum(x[0] % 1 > 0.01 for x in lead) / max(1, len(lead))
    lead_f = ([len(lead) / beats, sum(x[1] for x in lead) / max(1, len(lead)), sync] +
              _hist(iois, (0.26, 0.4, 0.55, 0.8, 1.1, 1.6)) + _hist(ivs, (0, 2, 4, 7)) + pcs)
    song = K.Song(arr)
    hits = Counter(k for _, k in K.drum_hits(song, arr.get('drums', 'full')))
    drums = [hits.get(k, 0) / max(1, arr['bars']) / 4 for k in DRUM_KINDS]
    bass = swung[2]
    bsteps = [abs(b[2] - a[2]) for a, b in zip(bass, bass[1:])]
    all_f = ([arr['bpm'] / 160, bpb / 4, measured_swing * 2] +
             [len(v) / beats for v in swung] + [sum(x[1] for x in v) / max(1, len(v)) for v in swung] +
             [sum(s <= 2 for s in bsteps) / max(1, len(bsteps))] + drums + lead_f)
    return all_f, lead_f


# ================================================================ statistics

def _zscore(vectors):
    n = len(vectors[0])
    mean = [sum(v[i] for v in vectors) / len(vectors) for i in range(n)]
    sd = [math.sqrt(sum((v[i] - mean[i]) ** 2 for v in vectors) / len(vectors)) or 1.0 for i in range(n)]
    return [[(v[i] - mean[i]) / sd[i] for i in range(n)] for v in vectors]


def _dist(a, b):
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)))


def loo(labels, vectors):
    """Leave-one-out nearest-centroid: (accuracy, confusion {true: Counter(pred)})."""
    z = _zscore(vectors)
    conf = defaultdict(Counter)
    ok = 0
    for i, (lab, v) in enumerate(zip(labels, z)):
        cents = {}
        for g in set(labels):
            members = [z[j] for j in range(len(z)) if labels[j] == g and j != i]
            if members:
                cents[g] = [sum(c) / len(members) for c in zip(*members)]
        pred = min(cents, key=lambda g: _dist(v, cents[g]))
        conf[lab][pred] += 1
        ok += pred == lab
    return ok / len(labels), conf


def separation(labels, vectors):
    """Per genre: nearest other genre and (centroid distance / mean spread inside the two)."""
    z = _zscore(vectors)
    groups = defaultdict(list)
    for lab, v in zip(labels, z):
        groups[lab].append(v)
    cent = {g: [sum(c) / len(m) for c in zip(*m)] for g, m in groups.items()}
    spread = {g: sum(_dist(v, cent[g]) for v in m) / len(m) for g, m in groups.items()}
    out = {}
    for g in groups:
        near = min((o for o in groups if o != g), key=lambda o: _dist(cent[g], cent[o]))
        out[g] = (near, _dist(cent[g], cent[near]) / max(1e-6, (spread[g] + spread[near]) / 2))
    return out


# ================================================================ report

def load(out):
    pieces = []
    for f in sorted(out.glob('*/*.json')):
        if f.parent.name not in R.RECIPES:          # quiz/ and anything else that isn't a genre
            continue
        j = json.loads(f.read_text())
        if 'arrangement' not in j:
            pieces.append({'style': f.parent.name, 'kind': f.stem.split('_')[0], 'seed': f.stem.split('_')[1],
                           'error': j.get('error'), 'file': f})
            continue
        st = f.parent.name
        rec = R.RECIPES[st]
        checks, (all_f, lead_f) = measure(j['arrangement'], j['report'], rec)
        pieces.append({'style': st, 'kind': f.stem.split('_')[0], 'seed': f.stem.split('_')[1], 'file': f,
                       'checks': checks, 'valid': all(ok for _, ok in checks.values()), 'all': all_f, 'lead': lead_f,
                       'report': j['report'], 'arr': j['arrangement']})
    return pieces


def _conf_table(conf, names):
    rows = ['<table class="conf"><tr><th>true \\ guessed</th>' + ''.join(f'<th>{n[:6]}</th>' for n in names) + '</tr>']
    for t in names:
        cells = []
        tot = max(1, sum(conf[t].values()))
        for p in names:
            c = conf[t][p]
            a = c / tot
            col = f'rgba(40,140,60,{a:.2f})' if t == p else f'rgba(200,60,40,{a:.2f})'
            cells.append(f'<td style="background:{col}">{c or ""}</td>')
        rows.append(f'<tr><th>{t}</th>{"".join(cells)}</tr>')
    return '\n'.join(rows) + '</table>'


def report(out):
    out = Path(out)
    pieces = load(out)
    good = [p for p in pieces if 'error' not in p]
    sketches = [p for p in good if p['kind'] == 'sketch']
    names = sorted({p['style'] for p in good})
    acc_all, conf_all = loo([p['style'] for p in sketches], [p['all'] for p in sketches])
    acc_lead, conf_lead = loo([p['style'] for p in sketches], [p['lead'] for p in sketches])
    sep_lead = separation([p['style'] for p in sketches], [p['lead'] for p in sketches])
    # Instrument check: the same seeds with and without the genre's GM programs (lead notes changed?)
    inst = {}
    for st in names:
        plain = {p['seed']: p for p in good if p['style'] == st and p['kind'] == 'plain'}
        if not plain:
            continue
        diffs = []
        for p in sketches:
            q = plain.get(p['seed']) if p['style'] == st else None
            if q:
                a = {(round(x[0], 3), x[2]) for x in p['arr']['voices'][0]}
                b = {(round(x[0], 3), x[2]) for x in q['arr']['voices'][0]}
                diffs.append(1 - len(a & b) / max(1, len(a | b)))
        inst[st] = (sum(diffs) / len(diffs) if diffs else None, len(diffs), R.RECIPES[st].instruments)
    summary = {}
    for st in names:
        sk = [p for p in sketches if p['style'] == st]
        summary[st] = {'valid': sum(p['valid'] for p in sk), 'n': len(sk),
                       'errors': sum(1 for p in pieces if p['style'] == st and 'error' in p),
                       'lead_acc': sum(conf_lead[st][st] for _ in [0]) / max(1, sum(conf_lead[st].values())),
                       'nearest': sep_lead.get(st, ('-', 0))}
    write_report(out, pieces, names, summary, acc_all, conf_all, acc_lead, conf_lead, inst)
    write_quiz(out, sketches, names)
    js = {'all_feature_accuracy': acc_all, 'lead_accuracy': acc_lead, 'genres': summary,
          'instruments': {k: v[0] for k, v in inst.items()}}
    (out / 'summary.json').write_text(json.dumps(js, indent=1, default=str))
    print(f'all-feature LOO accuracy {acc_all:.0%}, lead-only {acc_lead:.0%}')
    for st in names:
        s = summary[st]
        i = inst.get(st)
        print(f'  {st:10} valid {s["valid"]}/{s["n"]}  lead-only recognised {s["lead_acc"]:.0%}  nearest {s["nearest"][0]} '
              f'(separation {s["nearest"][1]:.1f})' + (f'  instrument changes {i[0]:.0%} of lead notes' if i and i[0] is not None else '')
              + (f'  ERRORS {s["errors"]}' if s['errors'] else ''))
    print('report:', out / 'report.html')
    print('quiz:  ', out / 'quiz' / 'quiz.html')


CSS = """body{font:14px system-ui,sans-serif;margin:24px;max-width:1200px;color:#222}
table{border-collapse:collapse;margin:8px 0 20px}td,th{border:1px solid #ccc;padding:3px 6px;text-align:right}
th{background:#f3f3f3}td.l,th.l{text-align:left}.bad{color:#b00;font-weight:600}.ok{color:#080}
table.conf td{width:26px;text-align:center}h2{margin-top:32px}audio{height:28px;vertical-align:middle}"""


def write_report(out, pieces, names, summary, acc_all, conf_all, acc_lead, conf_lead, inst):
    h = [f'<!doctype html><meta charset="utf-8"><title>Genre lab</title><style>{CSS}</style>',
         f'<h1>Genre lab: {html.escape(out.name)}</h1>',
         '<p>Sketches = theme A + cadence. Validity gates are checked on every piece. The <b>listening quiz</b> '
         '(quiz/quiz.html) decides what ships: valid in at least 5 of 6 sketches and named in at least 2 of 3 clips.</p>',
         '<h2>Per genre</h2><table><tr><th class="l">genre</th><th class="l">recipe</th><th>valid sketches</th>'
         '<th>lead-only recognised</th><th class="l">nearest genre (lead)</th><th>separation</th>'
         '<th>instrument effect</th><th class="l">GM programs</th></tr>']
    for st in names:
        s = summary[st]
        i = inst.get(st)
        v = f'<span class="{"ok" if s["valid"] >= 5 else "bad"}">{s["valid"]}/{s["n"]}</span>'
        h.append(f'<tr><td class="l"><b>{st}</b></td><td class="l">{html.escape(R.RECIPES[st].desc)}</td><td>{v}</td>'
                 f'<td>{s["lead_acc"]:.0%}</td><td class="l">{s["nearest"][0]}</td><td>{s["nearest"][1]:.1f}</td>'
                 f'<td>{"" if not i or i[0] is None else f"{i[0]:.0%}"}</td><td class="l">{R.RECIPES[st].instruments}</td></tr>')
    h.append('</table><p>Separation = distance between genre centroids / average spread inside them (lead-only '
             'features, z-scored); below about 1.5 the two genres\' melodies overlap. Instrument effect = share of '
             'lead notes that differ when the same seed is generated with the plain programs (80, 81, 38).</p>')
    h.append(f'<h2>Nearest-centroid recognition, leave-one-out</h2><p>All features: <b>{acc_all:.0%}</b> '
             f'(high by construction: the recipe sets tempo, meter and drums). Lead only (what MIDI-GPT wrote): '
             f'<b>{acc_lead:.0%}</b>.</p><h3>Lead only</h3>' + _conf_table(conf_lead, names) +
             '<h3>All features</h3>' + _conf_table(conf_all, names))
    h.append('<h2>Every piece</h2><table><tr><th class="l">genre</th><th>kind</th><th>seed</th><th>key</th><th>bpm</th>'
             '<th>seconds</th><th>bytes</th>' + ''.join(f'<th>{k}</th>' for k in
                                                         next(p for p in pieces if 'checks' in p)['checks']) +
             '<th class="l">listen</th></tr>')
    for p in pieces:
        if 'error' in p:
            h.append(f'<tr><td class="l">{p["style"]}</td><td>{p["kind"]}</td><td>{p["seed"]}</td>'
                     f'<td colspan="20" class="l bad">{html.escape(str(p["error"]))}</td></tr>')
            continue
        r = p['report']
        cells = ''.join(f'<td class="{"" if ok else "bad"}">{v:.2f}</td>' if isinstance(v, float) else
                        f'<td class="{"" if ok else "bad"}">{v}</td>' for v, ok in p['checks'].values())
        rel = p['file'].relative_to(out).with_suffix('.wav').as_posix()
        h.append(f'<tr><td class="l">{p["style"]}</td><td>{p["kind"]}</td><td>{p["seed"]}</td><td>{r["key"]}</td>'
                 f'<td>{r["tempo"]}</td><td>{r["seconds"]}</td><td>{r["stream"]["bytes"]}</td>{cells}'
                 f'<td class="l"><audio controls preload="none" src="{rel}"></audio></td></tr>')
    h.append('</table>')
    (out / 'report.html').write_text('\n'.join(h), encoding='utf-8')


QUIZ = r"""<!doctype html><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Genre quiz</title>
<style>
:root{--bg:#fafaf7;--fg:#1d1d1b;--muted:#6b6b66;--line:#ddd;--ok:#1d7a3a;--bad:#b3261e;--card:#fff}
@media (prefers-color-scheme:dark){:root{--bg:#161615;--fg:#ecebe6;--muted:#a3a29b;--line:#3a3936;--ok:#6fcf8a;--bad:#ff8a80;--card:#1f1f1d}}
body{background:var(--bg);color:var(--fg);font:15px/1.45 system-ui,sans-serif;margin:0 auto;max-width:860px;padding:16px}
h1{font-size:22px;margin:8px 0}p{color:var(--muted)}
.clip{display:flex;flex-wrap:wrap;align-items:center;gap:10px;padding:10px 12px;border:1px solid var(--line);
 border-radius:8px;margin:8px 0;background:var(--card)}
.clip b{width:64px}audio{flex:1;min-width:220px;height:32px}select{font:inherit;padding:4px}
.res{font-weight:600}.ok{color:var(--ok)}.bad{color:var(--bad)}
button{font:inherit;padding:8px 14px;border-radius:6px;border:1px solid var(--line);background:var(--card);color:var(--fg);cursor:pointer}
textarea{width:100%;height:160px;font:12px monospace;background:var(--card);color:var(--fg);border:1px solid var(--line)}
table{border-collapse:collapse;margin:10px 0}td,th{border:1px solid var(--line);padding:3px 8px;text-align:left}
dl{display:grid;grid-template-columns:max-content 1fr;gap:2px 12px;color:var(--muted)}dt{font-weight:600;color:var(--fg)}
</style>
<h1>Which genre is it?</h1>
<p>Each clip is a short sketch as the Tandy plays it (three square-wave voices plus noise drums), in random order.
Pick the genre you hear; use "not sure" rather than guessing wildly. Answers are kept in this browser, so you can
stop and come back. Press <b>Show results</b> at the end and paste the results text back to Claude.</p>
<details><summary>The genres</summary><dl id="legend"></dl></details>
<div id="clips"></div>
<p><button id="score">Show results</button> <span id="count"></span></p>
<div id="out"></div>
<script>
const CLIPS = __CLIPS__;                 // [{id, file}]
const NAMES = __NAMES__;                 // {genre: description}
const KEY = JSON.parse(atob("__KEY__")); // {id: genre}
const STORE = "genre-quiz-__RUN__";
let ans = {};
try { ans = JSON.parse(localStorage.getItem(STORE) || "{}"); } catch (e) {}
const save = () => { try { localStorage.setItem(STORE, JSON.stringify(ans)); } catch (e) {} count(); };
const legend = document.getElementById("legend");
for (const [g, d] of Object.entries(NAMES)) legend.insertAdjacentHTML("beforeend", `<dt>${g}</dt><dd>${d}</dd>`);
const box = document.getElementById("clips");
CLIPS.forEach((c, i) => {
  const opts = ['<option value="">choose...</option>', '<option value="?">not sure</option>']
    .concat(Object.keys(NAMES).map(g => `<option value="${g}">${g}</option>`)).join("");
  box.insertAdjacentHTML("beforeend", `<div class="clip"><b>Clip ${i + 1}</b>
    <audio controls preload="none" src="${c.file}"></audio><select data-id="${c.id}">${opts}</select>
    <span class="res" id="r${c.id}"></span></div>`);
});
box.querySelectorAll("select").forEach(s => { s.value = ans[s.dataset.id] || ""; s.onchange = () => { ans[s.dataset.id] = s.value; save(); }; });
function count() { document.getElementById("count").textContent = `${Object.values(ans).filter(Boolean).length} of ${CLIPS.length} answered`; }
count();
document.getElementById("score").onclick = () => {
  const per = {};
  for (const g of Object.keys(NAMES)) per[g] = { right: 0, n: 0, guesses: {} };
  for (const c of CLIPS) {
    const truth = KEY[c.id], a = ans[c.id] || "";
    per[truth].n++;
    if (a === truth) per[truth].right++;
    else if (a) per[truth].guesses[a] = (per[truth].guesses[a] || 0) + 1;
    const r = document.getElementById("r" + c.id);
    r.textContent = a === truth ? "right" : `it was ${truth}`;
    r.className = "res " + (a === truth ? "ok" : "bad");
  }
  let t = "<table><tr><th>genre</th><th>named</th><th>heard as</th></tr>";
  for (const [g, p] of Object.entries(per)) {
    const other = Object.entries(p.guesses).map(([k, v]) => `${k}${v > 1 ? " x" + v : ""}`).join(", ");
    t += `<tr><td>${g}</td><td class="${p.right >= 2 ? "ok" : "bad"}">${p.right} of ${p.n}</td><td>${other}</td></tr>`;
  }
  t += "</table>";
  const results = { run: "__RUN__", answers: CLIPS.map(c => ({ clip: c.id, truth: KEY[c.id], answer: ans[c.id] || "" })) };
  document.getElementById("out").innerHTML = t + "<p>Results to paste back:</p><textarea readonly>" +
    JSON.stringify(results) + "</textarea>";
};
</script>
"""


def write_quiz(out, sketches, names, per=3):
    q = out / 'quiz'
    if q.exists():
        shutil.rmtree(q)
    (q / 'clips').mkdir(parents=True)
    rng = random.Random(out.name)
    clips = []
    for st in names:
        sk = sorted((p for p in sketches if p['style'] == st), key=lambda p: (not p['valid'], p['seed']))
        clips += [(st, p) for p in sk[:per]]
    rng.shuffle(clips)
    meta, key = [], {}
    for i, (st, p) in enumerate(clips, 1):
        cid = f'c{i:02d}'
        shutil.copyfile(p['file'].with_suffix('.wav'), q / 'clips' / f'{cid}.wav')
        meta.append({'id': cid, 'file': f'clips/{cid}.wav'})
        key[cid] = st
    page = (QUIZ.replace('__CLIPS__', json.dumps(meta))
            .replace('__NAMES__', json.dumps({n: R.RECIPES[n].desc for n in names}))
            .replace('__KEY__', base64.b64encode(json.dumps(key).encode()).decode())
            .replace('__RUN__', out.name))
    (q / 'quiz.html').write_text(page, encoding='utf-8')
    (q / 'key.json').write_text(json.dumps({'key': key, 'sources': {cid: str(p['file']) for cid, (_, p) in
                                                                     zip(key, clips)}}, indent=1))


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd', required=True)
    r = sub.add_parser('run')
    r.add_argument('--url', default='http://127.0.0.1:8288')
    r.add_argument('--styles')
    r.add_argument('--seeds', type=int, default=6)
    r.add_argument('--full', type=int, default=2)
    r.add_argument('--plain', type=int, default=3)
    r.add_argument('--out')
    p = sub.add_parser('report')
    p.add_argument('dir')
    a = ap.parse_args()
    if a.cmd == 'run':
        run(a)
    else:
        report(a.dir)


if __name__ == '__main__':
    main()
