"""T3P1 register streams for the Tandy 1000 3-voice chip (SN76496 / PSSJ at port C0h).

The format of the RASTER.T3 player that was confirmed on the real TL/3, and of DeskMind's
music.cpp:

    "T3P1", u16 PIT divisor, u16 end tick, then records:
    u16 absolute tick (never decreasing), u8 count (1-32), count bytes written to port C0h.
    The last record is tick 0xFFFF with count 0.  Streams start and end with a mute record.

`Builder` collects register writes per tick; `read_stream` is the reference parser (from
MIDI-GPT/export-tandy.py); `render_wav` writes a preview from the exact register stream
(numpy is only needed there).  No MIDI-GPT or torch imports: MindServer and tools use it too.
"""
import math
import struct
from collections import defaultdict

PSG_CLOCK = 3_579_545
PIT_CLOCK = 1_193_182
PIT_DIV = 9943                       # 120.002 Hz, as the proven Raster Rush stream
MAX_BYTES = 30_000                   # DeskMind's buffer is 32,000; keep a margin
MUTE = bytes((0x9F, 0xBF, 0xDF, 0xFF))

# Noise control (register 6): bit 2 = white noise; bits 0-1 = rate (0 fastest .. 2 slowest,
# 3 follows tone 3).  Clock / 512, / 1024, / 2048.
NOISE_HI, NOISE_MID, NOISE_LO, NOISE_TONE3 = 0, 1, 2, 3


def divider(hz):
    """10-bit tone divider for a frequency (clamped to the chip's 1-1023)."""
    return max(1, min(1023, round(PSG_CLOCK / (32 * hz))))


def tone_hz(div):
    return PSG_CLOCK / (32 * div)


class Builder:
    """Register writes by tick.  Writes on one tick keep their order."""

    def __init__(self, pit_div=PIT_DIV):
        self.pit_div = pit_div
        self.rate = PIT_CLOCK / pit_div
        self.w = defaultdict(bytearray)
        self.end = 0

    def tick(self, seconds):
        return round(seconds * self.rate)

    def raw(self, t, *bs):
        self.w[t].extend(bs)
        self.end = max(self.end, t)

    def tone(self, t, ch, hz=None, div=None):
        d = div if div is not None else divider(hz)
        self.raw(t, 0x80 | (ch << 5) | (d & 15), (d >> 4) & 63)

    def vol(self, t, ch, att):
        self.raw(t, 0x90 | (ch << 5) | max(0, min(15, int(att))))

    def noise(self, t, white=True, rate=NOISE_HI):
        self.raw(t, 0xE0 | (4 if white else 0) | (rate & 3))

    def note(self, t0, t1, ch, hz, att, decay=()):
        """A tone from tick t0 to t1 (exclusive) at attenuation att; decay = ((fraction, extra), ...)."""
        if t1 <= t0:
            return
        self.tone(t0, ch, hz)
        self.vol(t0, ch, att)
        for frac, extra in decay:
            t = t0 + round((t1 - t0) * frac)
            if t0 < t < t1:
                self.vol(t, ch, min(14, att + extra))
        self.vol(t1, ch, 15)

    def build(self, end=None):
        """The stream; the final mute record goes at `end` (after any writes on that tick)."""
        end = max(self.end, end if end is not None else self.end)
        data = bytearray(b'T3P1' + struct.pack('<HH', self.pit_div, end))
        records = [(0, MUTE)] + [(t, bytes(self.w[t])) for t in sorted(self.w)] + [(end, MUTE)]
        for t, payload in records:
            for k in range(0, len(payload), 32):          # at most 32 bytes per record
                chunk = payload[k:k + 32]
                data += struct.pack('<HB', t, len(chunk)) + chunk
        data += struct.pack('<HB', 0xFFFF, 0)
        return bytes(data)


def read_stream(data):
    """(divisor, end tick, [(tick, payload)]); raises ValueError like the DOS player would refuse."""
    if data[:4] != b'T3P1':
        raise ValueError('Bad magic')
    divisor, end = struct.unpack_from('<HH', data, 4)
    if divisor < 2000:
        raise ValueError('Divisor below 2000')
    pos = 8
    records = []
    last = 0
    while pos < len(data):
        tick, count = struct.unpack_from('<HB', data, pos)
        pos += 3
        if tick == 65535 and count == 0:
            if pos != len(data):
                raise ValueError('Trailing data')
            return divisor, end, records
        if not 1 <= count <= 32 or tick < last or tick > end or pos + count > len(data):
            raise ValueError('Invalid event')
        records.append((tick, data[pos:pos + count]))
        pos += count
        last = tick
    raise ValueError('Missing terminator')


def validate(data, max_bytes=MAX_BYTES):
    """Checks what the DOS player and the chip need.  Returns a small report dict."""
    divisor, end, records = read_stream(data)
    if len(data) > max_bytes:
        raise ValueError(f'{len(data)} bytes, over {max_bytes}')
    if records[0] != (0, MUTE):
        raise ValueError('Does not start with the mute record')
    latch = None
    atts = [15] * 4
    per_tick = defaultdict(int)
    for tick, payload in records:
        per_tick[tick] += len(payload)
        for b in payload:
            if b & 0x80:
                latch = (b >> 4) & 7
                if latch & 1:
                    atts[latch >> 1] = b & 15
            else:
                if latch is None or latch & 1 or latch == 6:
                    raise ValueError(f'Data byte {b:02X} after latch {latch} at tick {tick}')
    if atts != [15] * 4:
        raise ValueError('Does not end muted')
    return {'bytes': len(data), 'records': len(records), 'end_tick': end, 'divisor': divisor,
            'seconds': round(end * divisor / PIT_CLOCK, 2), 'max_bytes_per_tick': max(per_tick.values())}


def _blep(np, phase, step):
    out = np.zeros_like(phase)
    m = phase < step
    x = phase[m] / step
    out[m] = 2 * x - x * x - 1
    m = phase > 1 - step
    x = (phase[m] - 1) / step
    out[m] = x * x + 2 * x + 1
    return out


def render_wav(data, wav_path, sr=44100):
    """Mono preview from the register stream (render_stream of export-tandy.py): band-limited
    squares and a PSSJ-style LFSR for noise.  Not cycle-accurate chip or speaker emulation."""
    import numpy as np
    import wave
    divisor, end, records = read_stream(data)
    tick_seconds = divisor / PIT_CLOCK
    audio = np.zeros(round(end * tick_seconds * sr), dtype=np.float64)
    divs = [1023] * 3
    atts = [15] * 4
    phases = [0.0] * 3
    latch = 0
    prev = 0
    noise_control = 0
    noise_phase = 0.0
    rng = 0x8000
    for tick, payload in records + [(end, b'')]:
        stop = min(len(audio), round(tick * tick_seconds * sr))
        size = stop - prev
        if size > 0:
            t = np.arange(size) / sr
            for ch in range(3):
                f = PSG_CLOCK / (32 * divs[ch])
                phase = np.mod(phases[ch] + f * t, 1)
                if atts[ch] != 15 and f < sr / 2:
                    sq = np.where(phase < 0.5, 1.0, -1.0)
                    sq += _blep(np, phase, f / sr) - _blep(np, np.mod(phase - 0.5, 1), f / sr)
                    audio[prev:stop] += sq * 10 ** (-atts[ch] / 10) / 3
                phases[ch] = (phases[ch] + size * f / sr) % 1
            rate = PSG_CLOCK / (512 * (1 << (noise_control & 3))) if (noise_control & 3) != 3 else PSG_CLOCK / (32 * divs[2])
            positions = noise_phase + np.arange(size) * rate / sr
            idx = np.floor(positions).astype(int)
            steps = int(math.floor(noise_phase + size * rate / sr))
            values = np.empty(steps + 1)
            values[0] = 1.0 if rng & 1 else -1.0
            for i in range(1, steps + 1):
                fb = bool(rng & 2) != ((not bool(rng & 32)) and bool(noise_control & 4))
                rng = (rng >> 1) | (0x8000 if fb else 0)
                values[i] = 1.0 if rng & 1 else -1.0
            if atts[3] != 15:
                audio[prev:stop] += values[idx] * 10 ** (-atts[3] / 10) / 3
            noise_phase = (noise_phase + size * rate / sr) % 1
            prev = stop
        for b in payload:
            if b & 0x80:
                latch = (b >> 4) & 7
                ch = latch // 2
                if latch % 2:
                    atts[ch] = b & 15
                elif ch < 3:
                    divs[ch] = (divs[ch] & 0x3F0) | (b & 15)
                else:
                    control = b & 7
                    if (control ^ noise_control) & 4:
                        rng = 0x8000
                    noise_control = control
            else:
                ch = latch // 2
                divs[ch] = max(1, (divs[ch] & 15) | ((b & 63) << 4))
    audio -= np.mean(audio)
    peak = float(np.max(np.abs(audio))) if len(audio) else 0.0
    audio *= 0.9 / max(peak, 0.001)
    pcm = np.round(np.clip(audio, -1, 1) * 32767).astype('<i2')
    with wave.open(str(wav_path), 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sr)
        w.writeframes(pcm.tobytes())
    return {'seconds': round(end * tick_seconds, 2), 'samples': len(pcm)}
