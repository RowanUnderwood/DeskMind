"""Revisions on the 3090 (compose.revise): every edit, and locked parts coming back note for note.

    MIDI-GPT\\.venv\\Scripts\\python.exe worker\\test_revise.py [style]

1. a sketch is built into a full song: theme A unchanged, B and break new
2. new lead: accompaniment and bass unchanged, the lead different, still in the mask
3. new accompaniment: lead and bass unchanged
4. key up 2, faster, next drum pattern: notes moved by exactly 2, the rest unchanged
5. a v1 song (MIDI + report, no sources): drums and key work, a new lead is refused
Each result compiles within the limits.
"""
import glob
import json
import os
import sys
from pathlib import Path

os.environ['CUDA_VISIBLE_DEVICES'] = 'GPU-2271903e-28c4-d002-e41f-96471369c18a'
os.environ['HF_HUB_OFFLINE'] = '1'
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import compile as C  # noqa: E402
import compose  # noqa: E402
import recipes as R  # noqa: E402
import t3  # noqa: E402


def part(arr, sec, t):
    return [[tuple(n) for n in bar] for bar in arr['sources'][sec][t]]


def check(arr):
    data, rep, cuts = C.compile_fit(arr, None if arr['spec']['length'] in R.STREAMED else t3.MAX_BYTES)
    return rep


def main():
    style = sys.argv[1] if len(sys.argv) > 1 else 'blues'
    eng = compose.load_engine(HERE.parent / 'MIDI-GPT' / 'models' / 'pretrained' / 'yellow_medium-final.safetensors')
    q = lambda m: None
    sk, _ = compose.compose(eng, {'style': style, 'seed': 31, 'sketch': True}, log=q, allowed=R.CANDIDATES)
    sk = json.loads(json.dumps(sk))                                  # as MindServer stores it
    print('sketch:', sk['bars'], 'bars', check(sk)['seconds'], 's')

    full, rep = compose.revise(eng, sk, {'length': 'medium'}, 5, log=q)
    assert part(full, 'A', 0) == part(sk, 'A', 0) and part(full, 'A', 2) == part(sk, 'A', 2)
    assert {'B', 'break'} <= set(full['sources']) and not full['spec']['sketch']
    print('1 build:', rep['change'], '|', full['bars'], 'bars', check(full)['seconds'], 's')
    full = json.loads(json.dumps(full))

    lead, rep = compose.revise(eng, full, {'regenerate': ['lead']}, 6, log=q)
    for sec in full['sources']:
        assert part(lead, sec, 1) == part(full, sec, 1) and part(lead, sec, 2) == part(full, sec, 2), sec
    changed = sum(part(lead, s, 0) != part(full, s, 0) for s in full['sources'])
    assert changed >= 2, changed
    rec = R.RECIPES[style]
    for sec, parts in lead['sources'].items():
        for i, bar in enumerate(parts[0]):
            allowed = set(compose._lead_pcs(rec, full['spec']['tonic'], R.prog(rec, sec)[i]))
            assert all(n[2] % 12 in allowed for n in bar), (sec, i)
    print('2 new lead:', rep['change'], '| locked', rep['locked'], '| sections with a new lead:', changed)

    acc, rep = compose.revise(eng, full, {'regenerate': ['accompaniment']}, 7, log=q)
    for sec in full['sources']:
        assert part(acc, sec, 0) == part(full, sec, 0) and part(acc, sec, 2) == part(full, sec, 2)
    assert any(part(acc, s, 1) != part(full, s, 1) for s in full['sources'])
    print('3 new accompaniment:', rep['change'])

    moved, rep = compose.revise(eng, full, {'transpose': 2, 'tempo': full['spec']['tempo'] + 8, 'drum_pattern': 'next'}, 8, log=q)
    for sec in full['sources']:
        for t in range(3):
            assert part(moved, sec, t) == [[(a, b, p + 2, v) for a, b, p, v in bar] for bar in part(full, sec, t)]
    assert moved['bpm'] == full['bpm'] + 8 and moved['drum_pattern'] != full['drum_pattern']
    print('4 key/tempo/drums:', rep['change'], '|', check(moved)['seconds'], 's')

    v1 = sorted(glob.glob(str(HERE.parent.parent / 'helper' / 'data' / 'music' / '*.mid')))
    if v1:
        m = json.load(open(v1[0][:-4] + '.json', encoding='utf-8'))
        old = compose.arrangement_from_midi(v1[0], m.get('report', m))
        new, rep = compose.revise(eng, old, {'drums': 'none', 'transpose': -2}, 9, log=q)
        print('5 v1 song:', Path(v1[0]).stem, rep['change'], '|', check(new)['seconds'], 's')
        try:
            compose.revise(eng, old, {'regenerate': ['lead']}, 9, log=q)
            raise AssertionError('a v1 song accepted a new lead')
        except ValueError as e:
            print('  new lead refused:', e)
    print('revision tests passed')


if __name__ == '__main__':
    main()
