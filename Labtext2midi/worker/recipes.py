"""Song recipes for DeskMind's music worker.

A recipe turns a few user-level choices (style, key, tempo, energy, drums, length) into the
musical material compose.py hands to MIDI-GPT: chord progressions, a scaffold for the three
Tandy voices (lead / accompaniment / bass), what the model may regenerate, the song form and
the drum pattern.  `cracktro` is the proven Raster Rush recipe (user-confirmed on the real TL/3);
`adventure` and `dungeon` follow the same shape with different material.

Hardware stays three square waves plus noise: style names describe arrangement and mood,
not instruments.
"""
from dataclasses import dataclass, field

TONICS = {'C': 0, 'C#': 1, 'DB': 1, 'D': 2, 'D#': 3, 'EB': 3, 'E': 4, 'F': 5, 'F#': 6, 'GB': 6,
          'G': 7, 'G#': 8, 'AB': 8, 'A': 9, 'A#': 10, 'BB': 10, 'B': 11}
NAMES = ['C', 'C#', 'D', 'Eb', 'E', 'F', 'F#', 'G', 'Ab', 'A', 'Bb', 'B']

# Roman numeral -> (semitones above the tonic, 'M' major / 'm' minor triad)
DEGREES = {'i': (0, 'm'), 'I': (0, 'M'), 'bII': (1, 'M'), 'ii': (2, 'm'), 'III': (3, 'M'), 'iii': (4, 'm'),
           'iv': (5, 'm'), 'IV': (5, 'M'), 'v': (7, 'm'), 'V': (7, 'M'), 'VI': (8, 'M'), 'vi': (9, 'm'),
           'VII': (10, 'M'), 'bVII': (10, 'M')}
SCALES = {'minor': [0, 2, 3, 5, 7, 8, 10], 'major': [0, 2, 4, 5, 7, 9, 11]}

STYLES = ('cracktro', 'adventure', 'dungeon')
ENERGIES = ('low', 'medium', 'high')
DRUMS = ('none', 'light', 'full')
LENGTHS = {'short': 32, 'medium': 48, 'long': 64}


@dataclass
class Recipe:
    name: str
    mode: str                       # 'minor' or 'major'
    keys: tuple                     # tonic names that suit it
    bpm: tuple                      # (low, medium, high energy)
    progs: dict                     # 'A', 'B', 'break' -> 8 roman numerals
    # Scaffold (480 ticks per quarter): (chord-tone index, onset in eighths, length in ticks)
    lead_pattern: list
    lead_octave: int                # semitones above the chord voicing
    arp_step: int                   # ticks between accompaniment notes (120 = 16ths)
    arp_pattern: list               # chord-tone indices cycled; octave lift per half bar if arp_lift
    arp_lift: bool
    arp_len: int                    # note length in ticks
    bass: str                       # 'octave_quarters', 'root_fifth_halves', 'root_whole'
    # What MIDI-GPT regenerates: per track id, grid unit, max note duration bin (0 = 32nd .. 5 = whole), range
    model: dict
    temperature: float
    lengths: tuple                  # note-length caps in quarters for lead / accompaniment / bass
    drums_default: str
    hats: str                       # 'eighths', 'quarters', 'none'
    gains: tuple = (0.8, 0.35, 0.6)  # chip volume per voice (export-tandy.py)
    cadence: list = field(default_factory=lambda: [12, 7, 3, 0, 3, 7, 12, 12])   # lead offsets in the last bar


RECIPES = {
    'cracktro': Recipe(
        name='cracktro', mode='minor', keys=('A', 'D', 'E', 'C', 'G'), bpm=(140, 150, 160),
        progs={'A': ['i', 'VI', 'VII', 'v', 'i', 'VI', 'iv', 'V'],
               'B': ['VI', 'VII', 'i', 'III', 'VI', 'iv', 'V', 'V'],
               'break': ['i', 'i', 'VI', 'VI', 'iv', 'iv', 'V', 'V']},
        lead_pattern=[(i, j, 200) for j, i in enumerate((0, 1, 2, 1, 0, 1, 2, 1))],
        lead_octave=24, arp_step=120, arp_pattern=[0, 1, 2, 1], arp_lift=True, arp_len=100,
        bass='octave_quarters',
        model={0: ('eighth', 2, (60, 88)), 2: ('quarter', 3, (28, 48))},
        temperature=1.05, lengths=(0.48, 0.21, 0.8), drums_default='full', hats='eighths'),
    'adventure': Recipe(
        name='adventure', mode='major', keys=('C', 'D', 'F', 'G', 'Bb'), bpm=(104, 116, 128),
        progs={'A': ['I', 'V', 'vi', 'IV', 'I', 'V', 'IV', 'V'],
               'B': ['vi', 'IV', 'I', 'V', 'vi', 'IV', 'ii', 'V'],
               'break': ['IV', 'IV', 'I', 'I', 'ii', 'ii', 'V', 'V']},
        lead_pattern=[(0, 0, 400), (1, 2, 400), (2, 4, 400), (1, 6, 400)],
        lead_octave=24, arp_step=240, arp_pattern=[0, 2, 1, 2], arp_lift=False, arp_len=180,
        bass='root_fifth_halves',
        model={0: ('eighth', 3, (62, 86)), 2: ('quarter', 4, (31, 50))},
        temperature=0.98, lengths=(0.9, 0.4, 1.8), drums_default='light', hats='quarters',
        gains=(0.85, 0.3, 0.6), cadence=[12, 7, 4, 0, 4, 7, 12, 12]),
    'dungeon': Recipe(
        name='dungeon', mode='minor', keys=('D', 'E', 'A', 'C'), bpm=(72, 84, 96),
        progs={'A': ['i', 'i', 'VI', 'bII', 'i', 'iv', 'V', 'V'],
               'B': ['iv', 'iv', 'i', 'i', 'VI', 'bII', 'V', 'V'],
               'break': ['i', 'VI', 'i', 'VI', 'iv', 'iv', 'V', 'V']},
        lead_pattern=[(0, 0, 420), (2, 2, 420), (1, 4, 420), (2, 6, 300)],
        lead_octave=24, arp_step=480, arp_pattern=[0, 1, 2, 1], arp_lift=False, arp_len=300,
        bass='root_fifth_halves',
        model={0: ('eighth', 3, (57, 81)), 2: ('quarter', 4, (28, 47))},
        temperature=1.0, lengths=(1.2, 0.7, 1.9), drums_default='light', hats='none',
        gains=(0.8, 0.25, 0.65), cadence=[12, 10, 7, 3, 2, 3, 0, 0]),
}


def chord_tones(tonic, numeral):
    """Three MIDI pitches for a chord, root voiced between E2 (40) and D#3 (51)."""
    off, quality = DEGREES[numeral]
    root = 40 + (tonic + off - 40) % 12
    return [root, root + (3 if quality == 'm' else 4), root + 7]


def scale_pcs(tonic, mode, numeral):
    """Pitch classes the lead may use over a chord (minor keys raise the 7th over a major V)."""
    pcs = [(tonic + s) % 12 for s in SCALES[mode]]
    if mode == 'minor' and numeral == 'V':
        pcs = [p for p in pcs if p != (tonic + 10) % 12] + [(tonic + 11) % 12]
    if numeral == 'bII':
        pcs = [p for p in pcs if p != (tonic + 2) % 12] + [(tonic + 1) % 12]
    return sorted(pcs)


def resolve(spec):
    """Validated, clamped settings from a request (all fields optional).  Raises ValueError for a
    style that does not exist; everything else falls back to the recipe's defaults."""
    style = str(spec.get('style') or 'cracktro').lower()
    if style not in RECIPES:
        raise ValueError(f'unknown style {style!r}; one of {", ".join(STYLES)}')
    r = RECIPES[style]
    energy = str(spec.get('energy') or 'medium').lower()
    if energy not in ENERGIES:
        energy = 'medium'
    bpm = spec.get('tempo')
    try:
        bpm = int(bpm)
    except (TypeError, ValueError):
        bpm = r.bpm[ENERGIES.index(energy)]
    bpm = max(r.bpm[0] - 10, min(r.bpm[2] + 10, bpm))
    key = str(spec.get('key') or '').strip().upper().replace(' MINOR', '').replace(' MAJOR', '').replace('M', '')
    if key not in TONICS:
        key = r.keys[int(spec.get('seed') or 0) % len(r.keys)].upper()
    drums = str(spec.get('drums') or r.drums_default).lower()
    if drums not in DRUMS:
        drums = r.drums_default
    length = str(spec.get('length') or 'medium').lower()
    if length not in LENGTHS:
        length = 'medium'
    title = ' '.join(str(spec.get('title') or '').split())[:39] or f'{style.title()} in {NAMES[TONICS[key]]}'
    seed = int(spec.get('seed') or 0) & 0x7FFFFFFF
    return {'style': style, 'energy': energy, 'tempo': bpm, 'key': NAMES[TONICS[key]], 'tonic': TONICS[key],
            'mode': r.mode, 'drums': drums, 'length': length, 'bars': LENGTHS[length], 'title': title, 'seed': seed}


def form(length_bars):
    """Sections: (name, source, bar indices, options).  source is 'A', 'B' or 'break'."""
    intro = ('Intro', 'A', range(4), {'gain': 0.82, 'mute_lead': True, 'drums': 'sparse'})
    hook = ('Main theme', 'A', range(8), {})
    var = ('Theme, higher', 'A', range(8), {'gain': 1.04, 'octave': 12})
    b = ('Second theme', 'B', range(8), {'gain': 1.03})
    brk = ('Break', 'break', range(8), {'gain': 0.85, 'arp_sparse': True, 'drums': 'sparse'})
    final = ('Final theme', 'A', range(8), {'gain': 1.05})
    outro = ('Ending', 'A', range(4), {'gain': 0.85, 'cadence': True})
    if length_bars <= 32:
        return [intro, hook, b, final, outro]
    if length_bars <= 48:
        return [intro, hook, var, b, brk, final, outro]
    return [intro, hook, var, b, brk, final, ('Second theme again', 'B', range(8), {'gain': 1.04}),
            ('Final theme, higher', 'A', range(8), {'gain': 1.06, 'octave': 12}), outro]
