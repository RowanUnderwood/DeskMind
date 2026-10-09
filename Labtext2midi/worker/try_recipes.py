"""Compose every recipe with a few seeds and report how much MIDI-GPT changed (needs the 3090).

    MIDI-GPT\\.venv\\Scripts\\python.exe worker\\try_recipes.py [style ...] [--seeds 1,2,3] [--out DIR]

With --out, each song is written as <style>_<seed>.T3/.wav/.mid for listening.
"""
import argparse
import os
import sys
from pathlib import Path

os.environ['CUDA_VISIBLE_DEVICES'] = 'GPU-2271903e-28c4-d002-e41f-96471369c18a'
os.environ['HF_HUB_OFFLINE'] = '1'
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import compile as C  # noqa: E402
import compose  # noqa: E402
import recipes  # noqa: E402
import t3  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('styles', nargs='*', default=list(recipes.STYLES))
    ap.add_argument('--seeds', default='42,7,1234')
    ap.add_argument('--out')
    a = ap.parse_args()
    import torch
    assert torch.cuda.device_count() == 1 and '3090' in torch.cuda.get_device_name(0)
    eng = compose.load_engine(HERE.parent / 'MIDI-GPT' / 'models' / 'pretrained' / 'yellow_medium-final.safetensors')
    for st in a.styles:
        for seed in [int(x) for x in a.seeds.split(',')]:
            arr, rep = compose.compose(eng, {'style': st, 'seed': seed}, log=lambda m: None)
            data, srep, cuts = C.compile_fit(arr)
            print(f'{st:9} seed {seed:5}: {rep["key"]:2} {rep["tempo"]} bpm {srep["seconds"]:6.1f} s {srep["bytes"]:5} B '
                  f'changed {rep["model_changed_bars"]} lead variety {rep["lead_variety"]} {cuts}', flush=True)
            if a.out:
                o = Path(a.out)
                o.mkdir(parents=True, exist_ok=True)
                (o / f'{st}_{seed}.T3').write_bytes(data)
                t3.render_wav(data, o / f'{st}_{seed}.wav')
                compose.write_midi(arr, o / f'{st}_{seed}.mid')


if __name__ == '__main__':
    main()
