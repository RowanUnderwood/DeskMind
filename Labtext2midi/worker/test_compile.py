"""Offline checks of compile.py (no GPU): Raster Rush's MIDI through the generic compiler.

    MIDI-GPT\\.venv\\Scripts\\python.exe worker\\test_compile.py

Checks: the stream validates, all three voices and the noise channel are used, the bass is
borrowed for at most 5 ticks at a time and restored, nothing sits below the chip's range, and
compile_fit shortens an over-long song instead of failing.  Without drums the stream must be
byte-identical to the hardware-confirmed outputs/tandy/RASTER.T3 (export-tandy.py's output).
"""
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import compile as C  # noqa: E402
import recipes  # noqa: E402
import t3  # noqa: E402

MIDI = HERE.parent / 'MIDI-GPT' / 'outputs' / 'cracktro' / 'RasterRush.mid'


def raster_arrangement():
    from symusic import Score
    s = Score(str(MIDI))
    tpq = s.ticks_per_quarter
    voices = [[(n.time / tpq, n.duration / tpq, n.pitch, n.velocity) for n in tr.notes] for tr in s.tracks]
    secs = []
    start = 0
    for name, _, bars, opts in recipes.form(48):
        n = len(bars)
        secs.append({'name': name, 'start': start, 'bars': n, 'drums': opts.get('drums', 'normal')})
        start += n
    return {'bpm': 150, 'bars': 48, 'sections': secs, 'voices': voices, 'drums': 'full', 'hats': 'eighths'}


def replay(data):
    """Kick windows: each starts with the kick's 350 Hz (no bass note is that high) and must end
    with a bass write 5 ticks later (the restore).  Returns the kick start ticks without one."""
    _, end, recs = t3.read_stream(data)
    writes = {}                                   # tick -> ch 2 dividers written on it
    latch = 0
    lo = 0
    for t, p in recs:
        for b in p:
            if b & 0x80:
                latch = (b >> 4) & 7
                lo = b & 15
            elif latch == 4:
                writes.setdefault(t, []).append(lo | ((b & 63) << 4))
    start = t3.divider(350)
    starts = [t for t, ds in writes.items() if start in ds]
    missing = [t for t in starts if t + 5 not in writes]
    return starts, missing


def main():
    arr = raster_arrangement()
    data, rep = C.compile_song(arr, 'full')
    print('raster full drums:', rep)
    assert rep['bytes'] < 32000 and all(rep['notes_per_voice'])
    assert rep['drum_hits'].get('kick') and rep['drum_hits'].get('snare') and rep['drum_hits'].get('hat')
    starts, missing = replay(data)
    assert len(starts) == rep['drum_hits']['kick'] and not missing, (len(starts), missing[:5])
    assert 0xE4 in data or 0xE5 in data
    t3.render_wav(data, HERE / 'test-raster.wav')
    tone_only, _ = C.compile_song(arr, 'none')
    proven = (HERE.parent / 'MIDI-GPT' / 'outputs' / 'tandy' / 'RASTER.T3').read_bytes()
    assert tone_only == proven, 'tone path no longer matches the TL/3-confirmed RASTER.T3'
    print('tone-only stream is byte-identical to the TL/3-confirmed RASTER.T3')
    for d in ('light', 'none'):
        _, r = C.compile_song(arr, d)
        print(f'raster {d}:', r['bytes'], 'bytes', r['drum_hits'])
    # Over-long: the same notes twice (96 bars) must be cut to fit
    long = dict(arr)
    long['voices'] = [v + [(on + 192, ln, p, vel) for on, ln, p, vel in v] for v in arr['voices']]
    long['sections'] = arr['sections'] + [dict(s, start=s['start'] + 48) for s in arr['sections']]
    long['bars'] = 96
    data, rep, cuts = C.compile_fit(long)
    print('96 bars ->', rep['bytes'], 'bytes', cuts)
    assert rep['bytes'] <= t3.MAX_BYTES and cuts
    print('compile tests passed')


if __name__ == '__main__':
    main()
