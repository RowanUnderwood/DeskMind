"""Build MODEM.T3: DeskMind's online startup sound, a condensed 56k modem handshake (~6 s).

    python tools\\make_modem.py            -> dos\\res\\MODEM.T3 and helper\\out\\modem-preview.wav

Three square-wave voices plus the noise channel of the Tandy chip, 120 Hz register stream
(Labtext2midi\\worker\\t3.py).  An impression of a V.90 connect, not a real modem signal:
dial tone, seven DTMF digits, one ring, the 2100 Hz answer tone with its 15 Hz wobble and
phase-reversal clicks, V.8 warbles, the "ding-bong", line-probing chirps and the training hiss.
Reproducible (fixed random seed).
"""
import random
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'Labtext2midi' / 'worker'))
import t3  # noqa: E402

DTMF_ROW = {'1': 697, '2': 697, '3': 697, '4': 770, '5': 770, '6': 770, '7': 852, '8': 852, '9': 852, '0': 941}
DTMF_COL = {'1': 1209, '2': 1336, '3': 1477, '4': 1209, '5': 1336, '6': 1477, '7': 1209, '8': 1336, '9': 1477, '0': 1336}


def build():
    rnd = random.Random(56000)
    b = t3.Builder()
    T = b.tick
    s = 0.15                                           # a breath of silence first

    def two(t0, t1, f1, f2, a1=3, a2=4):
        b.note(T(t0), T(t1), 0, f1, a1)
        b.note(T(t0), T(t1), 1, f2, a2)

    # Dial tone (350 + 440 Hz)
    two(s, s + 0.40, 350, 440, 4, 4)
    s += 0.48
    # Seven DTMF digits, 70 ms each with 50 ms gaps
    for d in '5550123':
        two(s, s + 0.07, DTMF_ROW[d], DTMF_COL[d], 3, 3)
        s += 0.12
    s += 0.20
    # One ring-back burst (440 + 480 Hz)
    two(s, s + 0.55, 440, 480, 5, 5)
    s += 0.75
    # Answer tone: 2100 Hz with the ANSam 15 Hz amplitude wobble and two phase-reversal clicks
    t0, t1 = T(s), T(s + 1.00)
    b.tone(t0, 0, 2100)
    for t in range(t0, t1):
        wob = (t - t0) % 8 < 4                         # ~15 Hz at 120 ticks/s
        click = (t - t0) in (54, 55)                   # a brief dip, 450 ms in
        b.vol(t, 0, 15 if click else (3 if wob else 5))
    b.vol(t1, 0, 15)
    s += 1.08
    # V.8 menus: two FSK channels warbling (980/1180 and 1650/1850 Hz)
    t0, t1 = T(s), T(s + 0.60)
    for t in range(t0, t1):
        b.tone(t, 0, rnd.choice((980, 1180)))
        b.tone(t, 1, rnd.choice((1650, 1850)))
        if t == t0:
            b.vol(t, 0, 4)
            b.vol(t, 1, 5)
    b.vol(t1, 0, 15)
    b.vol(t1, 1, 15)
    s += 0.66
    # "Ding" then the low "bong"
    two(s, s + 0.14, 2400, 1200, 3, 5)
    s += 0.16
    t0, t1 = T(s), T(s + 0.40)
    b.tone(t0, 0, 600)
    b.tone(t0, 1, 1200)
    for k, t in enumerate(range(t0, t1)):
        b.vol(t, 0, min(14, 2 + k // 6))               # decays like a struck bell
        b.vol(t, 1, min(14, 5 + k // 5))
    b.vol(t1, 0, 15)
    b.vol(t1, 1, 15)
    s += 0.46
    # Line probing: three voices jumping between comb tones
    t0, t1 = T(s), T(s + 0.42)
    comb = [150 * k for k in range(2, 26)]
    for t in range(t0, t1, 2):
        for ch in range(3):
            b.tone(t, ch, rnd.choice(comb))
    for ch in range(3):
        b.vol(t0, ch, 6)
        b.vol(t1, ch, 15)
    s += 0.46
    # Training: the hiss, louder then settling, with a faint carrier
    t0, t1 = T(s), T(s + 1.25)
    b.noise(t0, white=True, rate=t3.NOISE_HI)
    b.tone(t0, 2, 1800)
    for k, t in enumerate(range(t0, t1)):
        if k == 40:
            b.noise(t, white=True, rate=t3.NOISE_MID)
        b.vol(t, 3, 2 + (k % 3 == 0) + (k > 90) * 2)   # rough, then a little quieter
        if k % 12 == 0:
            b.vol(t, 2, 9 if (k // 12) % 2 else 11)
    b.vol(t1, 3, 15)
    b.vol(t1, 2, 15)
    s += 1.25
    return b.build(T(s))


def main():
    data = build()
    report = t3.validate(data)
    out = ROOT / 'dos' / 'res' / 'MODEM.T3'
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(data)
    wav = ROOT / 'helper' / 'out' / 'modem-preview.wav'
    wav.parent.mkdir(parents=True, exist_ok=True)
    t3.render_wav(data, wav)
    print(f'{out}: {report}')
    print(f'preview: {wav}')


if __name__ == '__main__':
    main()
