"""DeskMind music worker: MIDI-GPT on the RTX 3090 behind a small local HTTP API.

    launch-music-worker.bat   (or: MIDI-GPT\\.venv\\Scripts\\python.exe worker\\music_worker.py [--port 8287])

    GET  /health   {"ok": true, "loaded": bool, "gpu": "...", "styles": [...], "busy": bool}
    POST /compose  JSON spec {style, key, tempo, energy, drums, length, title, seed, sketch}
                   -> {"ok": true, "title", "t3", "midi", "wav" (base64), "report", "arrangement"}
                      or {"ok": false, "error"}
    POST /revise   {"arrangement" (or "midi" + "report" of a v1 song), "edits", "seed"} -> the same
                   (compose.revise: regenerate parts, drums, tempo, transpose, length; report["change"])

--dev (the genre lab's instance on another port) accepts every candidate recipe, not only the
released styles, and the lab-only spec field "plain_instruments" (the scaffold keeps the original
GM programs, to measure what the genre instruments change).

MindServer (helper\\mindserver\\music.py) is the only client.  One composition at a time; the
model loads once at start.  The 3090 is chosen by UUID BEFORE torch is imported, and checked.
Listens on 127.0.0.1 only.
"""
import argparse
import base64
import json
import os
import sys
import tempfile
import threading
import time
import traceback
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

GPU_3090 = 'GPU-2271903e-28c4-d002-e41f-96471369c18a'
os.environ['CUDA_VISIBLE_DEVICES'] = GPU_3090
os.environ['CUDA_DEVICE_ORDER'] = 'PCI_BUS_ID'
os.environ['HF_HUB_OFFLINE'] = '1'

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import compile as C  # noqa: E402
import compose  # noqa: E402
import recipes  # noqa: E402
import t3  # noqa: E402

CHECKPOINT = HERE.parent / 'MIDI-GPT' / 'models' / 'pretrained' / 'yellow_medium-final.safetensors'

state = {'engine': None, 'gpu': '', 'error': '', 'busy': False, 'loaded_at': None, 'styles': recipes.STYLES}
lock = threading.Lock()


def log(msg):
    print(time.strftime('%H:%M:%S'), msg, flush=True)


def load():
    try:
        import torch
        if torch.cuda.device_count() != 1 or '3090' not in torch.cuda.get_device_name(0):
            raise RuntimeError(f'expected one RTX 3090, got {[torch.cuda.get_device_name(i) for i in range(torch.cuda.device_count())]}')
        state['gpu'] = torch.cuda.get_device_name(0)
        t0 = time.time()
        state['engine'] = compose.load_engine(CHECKPOINT)
        state['loaded_at'] = time.time()
        log(f'model loaded on {state["gpu"]} in {time.time() - t0:.1f} s')
    except Exception as e:                               # reported by /health
        state['error'] = f'{type(e).__name__}: {e}'
        log('model load failed: ' + state['error'])


def make(spec):
    """Compose, compile within the size limit, render the preview."""
    t0 = time.time()
    rec_saved = None
    if spec.get('plain_instruments') and state['styles'] is not recipes.STYLES:
        rec = recipes.RECIPES[str(spec.get('style')).lower()]
        rec_saved, rec.instruments = rec.instruments, (80, 81, 38)
    try:
        arr, report = compose.compose(state['engine'], spec, log=log, allowed=state['styles'])
    finally:
        if rec_saved:
            recipes.RECIPES[str(spec.get('style')).lower()].instruments = rec_saved
    return assets(arr, report, t0)


def assets(arr, report, t0):
    """Compile (within the size limit unless the song streams), MIDI and preview, as the reply."""
    data, rep, cuts = C.compile_fit(arr, None if arr['spec']['length'] in recipes.STREAMED else t3.MAX_BYTES)
    with tempfile.TemporaryDirectory() as d:
        mid, wav = Path(d) / 'song.mid', Path(d) / 'song.wav'
        compose.write_midi(arr, mid)
        t3.render_wav(data, wav)
        midi_b, wav_b = mid.read_bytes(), wav.read_bytes()
    report.update({'stream': rep, 'cuts': cuts, 'seconds': rep['seconds'], 'compose_seconds': round(time.time() - t0, 1),
                   'sections': [s['name'] for s in arr['sections']]})
    return {'ok': True, 'title': report['title'], 't3': base64.b64encode(data).decode(),
            'midi': base64.b64encode(midi_b).decode(), 'wav': base64.b64encode(wav_b).decode(), 'report': report,
            'arrangement': arr}


def make_revision(req):
    """A revision: req = {"arrangement": ..., "edits": {...}, "seed": n}; a v1 song sends
    {"midi": base64, "report": {...}} instead of the arrangement."""
    t0 = time.time()
    base = req.get('arrangement')
    if not base:
        with tempfile.TemporaryDirectory() as d:
            mid = Path(d) / 'old.mid'
            mid.write_bytes(base64.b64decode(req['midi']))
            base = compose.arrangement_from_midi(mid, req['report'])
    seed = int(req.get('seed') or int(time.time()) & 0xFFFF)
    arr, report = compose.revise(state['engine'], base, req.get('edits') or {}, seed, log=log)
    report['title'] = arr['spec'].get('title') or base['spec'].get('title') or report.get('title')
    return assets(arr, report, t0)


class Handler(BaseHTTPRequestHandler):
    def reply(self, obj, status=200):
        body = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == '/health':
            self.reply({'ok': state['engine'] is not None, 'loaded': state['engine'] is not None, 'gpu': state['gpu'],
                        'error': state['error'], 'busy': state['busy'], 'styles': list(state['styles']),
                        'descriptions': {k: recipes.RECIPES[k].desc for k in state['styles']},
                        'energies': list(recipes.ENERGIES), 'drums': list(recipes.DRUMS), 'lengths': list(recipes.LENGTHS)})
        else:
            self.reply({'ok': False, 'error': 'not found'}, 404)

    def do_POST(self):
        if self.path not in ('/compose', '/revise'):
            return self.reply({'ok': False, 'error': 'not found'}, 404)
        try:
            n = int(self.headers.get('Content-Length') or 0)
            spec = json.loads(self.rfile.read(n) or b'{}')
            if not isinstance(spec, dict):
                raise ValueError('spec must be a JSON object')
            if self.path == '/compose':
                recipes.resolve(spec, state['styles'])  # bad style etc. -> 400 before waiting
            elif not (spec.get('arrangement') or (spec.get('midi') and spec.get('report'))):
                raise ValueError('a revision needs the arrangement, or the MIDI and report of a v1 song')
        except Exception as e:
            return self.reply({'ok': False, 'error': str(e)}, 400)
        if state['engine'] is None:
            return self.reply({'ok': False, 'error': state['error'] or 'model still loading'}, 503)
        with lock:
            state['busy'] = True
            try:
                if self.path == '/compose':
                    log(f'compose {spec}')
                    out = make(spec)
                else:
                    log(f'revise {spec.get("edits")}')
                    out = make_revision(spec)
                    log(f'revised: {out["report"]["change"]}')
                log(f'done: {out["title"]}, {out["report"]["seconds"]} s, {out["report"]["stream"]["bytes"]} bytes, '
                    f'{out["report"]["compose_seconds"]} s to make')
                self.reply(out)
            except Exception as e:
                traceback.print_exc()
                self.reply({'ok': False, 'error': f'{type(e).__name__}: {e}'}, 500)
            finally:
                state['busy'] = False

    def log_message(self, fmt, *args):
        pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', type=int, default=8287)
    ap.add_argument('--dev', action='store_true', help='accept every candidate recipe (genre lab)')
    args = ap.parse_args()
    if args.dev:
        state['styles'] = recipes.CANDIDATES
    threading.Thread(target=load, daemon=True).start()
    srv = ThreadingHTTPServer(('127.0.0.1', args.port), Handler)
    log(f'music worker on http://127.0.0.1:{args.port} (loading the model)')
    srv.serve_forever()


if __name__ == '__main__':
    main()
