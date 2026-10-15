"""Song recipes for DeskMind's music worker.

A recipe turns a few user-level choices (style, key, tempo, energy, drums, length) into the
musical material compose.py hands to MIDI-GPT: chord progressions, a scaffold for the three
Tandy voices (lead / accompaniment / bass; patterns.py), what the model may regenerate, the song
form, the meter, swing and the drum pattern (compile.py).  `cracktro` is the proven Raster Rush
recipe (user-confirmed on the real TL/3); `adventure` and `dungeon` follow the same shape.

The Yellow MIDI-GPT checkpoint has no genre control (its encoder has no genre_groups).  A genre
is therefore what the recipe builds: harmony, rhythm, meter, swing, accompaniment and bass
patterns, drums, form, and the General MIDI programs of the scaffold tracks (the one lever the
model itself reads).  New genres ship only after genre_lab.py and a blind listening test.

Hardware stays three square waves plus noise: style names describe arrangement and mood,
not instruments.
"""
from dataclasses import dataclass, field

TONICS = {'C': 0, 'C#': 1, 'DB': 1, 'D': 2, 'D#': 3, 'EB': 3, 'E': 4, 'F': 5, 'F#': 6, 'GB': 6,
          'G': 7, 'G#': 8, 'AB': 8, 'A': 9, 'A#': 10, 'BB': 10, 'B': 11}
NAMES = ['C', 'C#', 'D', 'Eb', 'E', 'F', 'F#', 'G', 'Ab', 'A', 'Bb', 'B']

# Diatonic modes the roman numerals are read in ('VI' in minor = the major chord on the flat sixth)
MODES = {'major': [0, 2, 4, 5, 7, 9, 11], 'minor': [0, 2, 3, 5, 7, 8, 10], 'dorian': [0, 2, 3, 5, 7, 9, 10],
         'mixolydian': [0, 2, 4, 5, 7, 9, 10], 'harmonic': [0, 2, 3, 5, 7, 8, 11], 'lydian': [0, 2, 4, 6, 7, 9, 11]}
# Lead scales that replace the diatonic one (the chord's own tones are always added)
LEAD_SCALES = {'blues': [0, 3, 5, 6, 7, 10], 'major_pent': [0, 2, 4, 7, 9], 'minor_pent': [0, 3, 5, 7, 10]}
SCALES = MODES                                     # old name

ROMAN = {'I': 0, 'II': 1, 'III': 2, 'IV': 3, 'V': 4, 'VI': 5, 'VII': 6}
# Chord suffix -> intervals above the root and the scale degree each one stands for (third = 2 ...)
QUALITY = {
    '': None, '7': None, 'M7': ((0, 0), (4, 2), (7, 4), (11, 6)), '6': None,
    'o': ((0, 0), (3, 2), (6, 4)), 'o7': ((0, 0), (3, 2), (6, 4), (9, 6)), 'h7': ((0, 0), (3, 2), (6, 4), (10, 6)),
    '+': ((0, 0), (4, 2), (8, 4)), 'sus4': ((0, 0), (5, 3), (7, 4)), '5': ((0, 0), (7, 4)),
}

STYLES_V1 = ('cracktro', 'adventure', 'dungeon')
ENERGIES = ('low', 'medium', 'high')
DRUMS = ('none', 'light', 'full')
LENGTHS = {'short': 32, 'medium': 48, 'long': 64, 'extended': 112, 'epic': 208}
STREAMED = ('extended', 'epic')     # longer than DeskMind keeps in memory: played from disk
MAX_SECONDS = 520                   # the 16-bit tick limit is 546 s at 120 Hz; keep a margin


def parse_chord(symbol):
    """'bVII7' -> (accidental -1, degree 6, upper True, suffix '7')."""
    s = symbol
    acc = 0
    while s[:1] in ('b', '#'):
        acc += -1 if s[0] == 'b' else 1
        s = s[1:]
    i = 0
    while i < len(s) and s[i].upper() in 'IV':
        i += 1
    rom, suffix = s[:i], s[i:]
    if rom.upper() not in ROMAN or suffix not in QUALITY:
        raise ValueError(f'bad chord {symbol!r}')
    return acc, ROMAN[rom.upper()], rom.isupper(), suffix


def _intervals(upper, suffix):
    if QUALITY[suffix] is not None:
        return QUALITY[suffix]
    third = 4 if upper else 3
    out = [(0, 0), (third, 2), (7, 4)]
    if suffix == '7':
        out.append((10, 6))                       # dominant 7th on a major chord, minor 7th on a minor one
    elif suffix == '6':
        out.append((9, 5))
    return tuple(out)


def chord(tonic, mode, symbol):
    """(pitches, lead pitch classes) for a chord in a key.  Pitches: root voiced between E2 (40) and
    D#3 (51), then its intervals.  Lead pitch classes: the mode's scale with each chord tone put in
    place of the scale note it stands for (the raised 7th over V in minor, the flat 2nd of bII ...)."""
    acc, deg, upper, suffix = parse_chord(symbol)
    scale = MODES[mode]
    root_pc = (tonic + scale[deg] + acc) % 12
    root = 40 + (root_pc - 40) % 12
    ivs = _intervals(upper, suffix)
    pcs = [(tonic + s) % 12 for s in scale]
    for iv, role in ivs:
        diatonic = (tonic + scale[(deg + role) % 7]) % 12
        actual = (root_pc + iv) % 12
        if actual != diatonic and diatonic in pcs:
            pcs[pcs.index(diatonic)] = actual
    return [root + iv for iv, _ in ivs], sorted(set(pcs))


def chord_tones(tonic, numeral, mode='minor'):
    """The chord's pitches (the old three-note API for triads)."""
    return chord(tonic, mode, numeral)[0]


def scale_pcs(tonic, mode, numeral):
    return chord(tonic, mode, numeral)[1]


@dataclass
class Recipe:
    name: str
    desc: str                       # one line for Qwen and the listening quiz
    mode: str                       # MODES key the numerals are read in
    keys: tuple                     # tonic names that suit it
    bpm: tuple                      # quarter notes per minute for (low, medium, high) energy
    progs: dict                     # 'A', 'B', 'break' -> one chord symbol per bar (8, or 12 for blues)
    lead: dict                      # patterns.lead_bar spec
    acc: dict                       # patterns.acc_bar spec
    bass: dict                      # patterns.bass_bar spec
    # What MIDI-GPT regenerates: per track id, grid unit, max note duration bin (0 = 32nd .. 5 = whole), range
    model: dict
    temperature: float
    lengths: tuple                  # note-length caps in quarters for lead / accompaniment / bass
    drums_default: str
    drum_pattern: str = 'backbeat'  # compile.PATTERNS ('backbeat' = the original Raster Rush drums)
    hats: str = 'eighths'           # for 'backbeat': 'eighths', 'quarters', 'none'
    gains: tuple = (0.8, 0.35, 0.6)  # chip volume per voice (export-tandy.py)
    cadence: list = field(default_factory=lambda: [12, 7, 3, 0, 3, 7, 12, 12])   # lead offsets, eighths of the last bar
    meter: tuple = (4, 4)
    swing: float = 0.5              # where an off-beat eighth falls (0.5 straight, 0.667 triplet swing)
    lead_scale: str = ''            # LEAD_SCALES key, else the mode with the chord's tones
    instruments: tuple = (80, 81, 38)   # GM programs of the scaffold tracks (MIDI-GPT reads them)
    form: str = 'song'              # 'song' or 'build' (layered, techno)
    min_lead: int = 16              # fewest lead notes per 8 bars before a section is generated again
    tremolo: float = 0.0            # > 0: long lead notes become repeated notes this many beats apart (surf)
    # MIDI-GPT's novelty check: a regenerated bar identical to the scaffold is rejected and sampled again
    # (the v1 recipes ran without it; the genre lab showed it is what makes the model actually compose:
    # without it 2 of 6 cracktro sketches kept the placeholder arpeggio in most bars)
    novelty: bool = True

    @property
    def beats_per_bar(self):
        return self.meter[0] * 4 / self.meter[1]

    @property
    def section_bars(self):
        return len(self.progs['A'])


def _lead(notes, octave=24, scalar=False):
    return {'notes': notes, 'octave': octave, 'scalar': scalar}


RECIPES = {
    # ---- v1: cracktro is the TL/3-confirmed Raster Rush recipe; the scaffold is unchanged (ticks -> beats)
    'cracktro': Recipe(
        name='cracktro', desc='fast demo-scene music: minor key, quick arpeggios, driving beat',
        mode='minor', keys=('A', 'D', 'E', 'C', 'G'), bpm=(140, 150, 160),
        progs={'A': ['i', 'VI', 'VII', 'v', 'i', 'VI', 'iv', 'V'],
               'B': ['VI', 'VII', 'i', 'III', 'VI', 'iv', 'V', 'V'],
               'break': ['i', 'i', 'VI', 'VI', 'iv', 'iv', 'V', 'V']},
        lead=_lead([(i, j * 0.5, 200 / 480) for j, i in enumerate((0, 1, 2, 1, 0, 1, 2, 1))]),
        acc={'type': 'arp', 'step': 0.25, 'pattern': [0, 1, 2, 1], 'lift': True, 'len': 100 / 480},
        bass={'type': 'octave_quarters'},
        model={0: ('eighth', 2, (60, 88)), 2: ('quarter', 3, (28, 48))},
        temperature=1.05, lengths=(0.48, 0.21, 0.8), drums_default='full', hats='eighths'),
    'adventure': Recipe(
        name='adventure', desc='a bright, memorable game melody at a moderate pace, major key',
        mode='major', keys=('C', 'D', 'F', 'G', 'Bb'), bpm=(104, 116, 128),
        progs={'A': ['I', 'V', 'vi', 'IV', 'I', 'V', 'IV', 'V'],
               'B': ['vi', 'IV', 'I', 'V', 'vi', 'IV', 'ii', 'V'],
               'break': ['IV', 'IV', 'I', 'I', 'ii', 'ii', 'V', 'V']},
        lead=_lead([(0, 0, 400 / 480), (1, 1, 400 / 480), (2, 2, 400 / 480), (1, 3, 400 / 480)]),
        acc={'type': 'arp', 'step': 0.5, 'pattern': [0, 2, 1, 2], 'lift': False, 'len': 180 / 480},
        bass={'type': 'root_fifth_halves'},
        model={0: ('eighth', 3, (62, 86)), 2: ('quarter', 4, (31, 50))},
        temperature=0.98, lengths=(0.9, 0.4, 1.8), drums_default='light', hats='quarters',
        gains=(0.85, 0.3, 0.6), cadence=[12, 7, 4, 0, 4, 7, 12, 12]),
    'dungeon': Recipe(
        name='dungeon', desc='slow, dark and mysterious, minor key, sparse',
        mode='minor', keys=('D', 'E', 'A', 'C'), bpm=(72, 84, 96),
        progs={'A': ['i', 'i', 'VI', 'bII', 'i', 'iv', 'V', 'V'],
               'B': ['iv', 'iv', 'i', 'i', 'VI', 'bII', 'V', 'V'],
               'break': ['i', 'VI', 'i', 'VI', 'iv', 'iv', 'V', 'V']},
        lead=_lead([(0, 0, 420 / 480), (2, 1, 420 / 480), (1, 2, 420 / 480), (2, 3, 300 / 480)]),
        acc={'type': 'arp', 'step': 1.0, 'pattern': [0, 1, 2, 1], 'lift': False, 'len': 300 / 480},
        bass={'type': 'root_fifth_halves'},
        model={0: ('eighth', 3, (57, 81)), 2: ('quarter', 4, (28, 47))},
        temperature=1.0, lengths=(1.2, 0.7, 1.9), drums_default='light', hats='none',
        gains=(0.8, 0.25, 0.65), cadence=[12, 10, 7, 3, 2, 3, 0, 0]),

    # ---- v2 candidates (genre_lab.py decides which ship)
    'blues': Recipe(
        name='blues', desc='a 12-bar shuffle blues: swung rhythm, boogie bass, bluesy melody',
        mode='mixolydian', keys=('E', 'A', 'G', 'C'), bpm=(76, 92, 108), swing=0.667, lead_scale='blues',
        progs={'A': ['I7', 'IV7', 'I7', 'I7', 'IV7', 'IV7', 'I7', 'I7', 'V7', 'IV7', 'I7', 'V7'],
               'B': ['I7', 'IV7', 'I7', 'I7', 'IV7', 'IV7', 'I7', 'I7', 'V7', 'IV7', 'I7', 'I7'],
               'break': ['I7', 'I7', 'I7', 'I7', 'IV7', 'IV7', 'I7', 'I7', 'V7', 'IV7', 'I7', 'V7']},
        lead=_lead([(2, 0, 0.5), (1, 0.5, 0.5), (0, 1, 1.5), (3, 3, 1)]),
        acc={'type': 'offbeat', 'pattern': (1, 3), 'every': 1.0, 'len': 0.3},
        bass={'type': 'boogie'},
        model={0: ('eighth_triplet', 3, (60, 84))},
        temperature=1.0, lengths=(1.5, 0.35, 0.5), drums_default='light', drum_pattern='shuffle',
        gains=(0.85, 0.3, 0.6), cadence=[12, 10, 7, 3, 4, 0, 0, 0], instruments=(29, 0, 33), min_lead=12),
    'jazz': Recipe(
        name='jazz', desc='swing jazz: ii-V-I changes, walking bass, swung ride cymbal',
        mode='major', keys=('F', 'Bb', 'C', 'Eb'), bpm=(120, 140, 168), swing=0.667,
        progs={'A': ['ii7', 'V7', 'IM7', 'IM7', 'ii7', 'V7', 'IM7', 'VI7'],
               'B': ['iii7', 'VI7', 'ii7', 'V7', 'iii7', 'VI7', 'ii7', 'V7'],
               'break': ['IM7', 'vi7', 'ii7', 'V7', 'IM7', 'vi7', 'ii7', 'V7']},
        lead=_lead([(0, 0, 0.5), (1, 0.5, 0.5), (2, 1, 0.5), (3, 1.5, 0.5), (2, 2, 1), (1, 3, 1)]),
        acc={'type': 'comp'},
        bass={'type': 'walking'},
        model={0: ('eighth', 3, (60, 86))},
        temperature=1.0, lengths=(1.5, 0.4, 0.9), drums_default='light', drum_pattern='jazz',
        gains=(0.85, 0.3, 0.6), cadence=[12, 11, 7, 4, 2, 0, 0, 0], instruments=(66, 0, 32)),
    'country': Recipe(
        name='country', desc='country two-step: boom-chick rhythm, alternating bass, fiddle-style tune',
        mode='major', keys=('G', 'D', 'A', 'C', 'E'), bpm=(104, 120, 140), swing=0.58, lead_scale='major_pent',
        progs={'A': ['I', 'I', 'IV', 'IV', 'V', 'V', 'I', 'I'],
               'B': ['IV', 'IV', 'I', 'I', 'V', 'V', 'I', 'V'],
               'break': ['vi', 'vi', 'IV', 'IV', 'I', 'I', 'V', 'V']},
        lead=_lead([(0, 0, 0.5), (1, 0.5, 0.5), (2, 1, 1), (1, 2, 0.5), (0, 2.5, 0.5), (1, 3, 1)]),
        acc={'type': 'boomchick', 'len': 0.3},
        bass={'type': 'root_fifth'},
        model={0: ('eighth', 3, (62, 86))},
        temperature=0.98, lengths=(1.0, 0.3, 0.8), drums_default='light', drum_pattern='train',
        gains=(0.85, 0.3, 0.6), cadence=[12, 9, 7, 4, 2, 0, 0, 0], instruments=(110, 25, 33)),
    'classical': Recipe(
        name='classical', desc='classical, Mozart-like: Alberti bass, graceful melody, clear cadences',
        mode='major', keys=('C', 'G', 'D', 'F', 'Bb'), bpm=(96, 112, 126),
        progs={'A': ['I', 'IV', 'V', 'I', 'vi', 'ii', 'V', 'I'],
               'B': ['vi', 'iii', 'IV', 'I', 'ii', 'V', 'I', 'V'],
               'break': ['I', 'V', 'vi', 'iii', 'IV', 'I', 'ii', 'V']},
        lead=_lead([(0, 0, 1), (1, 1, 0.5), (2, 1.5, 0.5), (3, 2, 1), (2, 3, 1)]),
        acc={'type': 'alberti', 'step': 0.25},
        bass={'type': 'root_fifth_halves'},
        model={0: ('eighth', 3, (62, 88))},
        temperature=0.95, lengths=(1.5, 0.24, 1.8), drums_default='none', drum_pattern='none',
        gains=(0.85, 0.3, 0.6), cadence=[12, 11, 12, 7, 4, 2, 0, 0], instruments=(40, 0, 42)),
    'waltz': Recipe(
        name='waltz', desc='a waltz in 3/4: oom-pah-pah accompaniment, sweeping melody',
        mode='major', keys=('D', 'G', 'F', 'Bb'), bpm=(132, 150, 168), meter=(3, 4),
        progs={'A': ['I', 'I', 'V7', 'V7', 'V7', 'V7', 'I', 'I'],
               'B': ['IV', 'IV', 'I', 'I', 'ii', 'V7', 'I', 'I'],
               'break': ['vi', 'vi', 'ii', 'ii', 'V7', 'V7', 'I', 'V7']},
        lead=_lead([(0, 0, 1.5), (1, 1.5, 0.5), (2, 2, 1)]),
        acc={'type': 'oompah', 'len': 0.5},
        bass={'type': 'waltz'},
        model={0: ('eighth', 4, (62, 88))},
        temperature=0.98, lengths=(2.5, 0.5, 1.8), drums_default='none', drum_pattern='waltz',
        gains=(0.85, 0.3, 0.6), cadence=[7, 4, 2, 0, 0, 0], instruments=(40, 0, 42), min_lead=10),
    'techno': Recipe(
        name='techno', desc='techno: four-on-the-floor kick, off-beat stabs, hypnotic repeating riff',
        mode='minor', keys=('A', 'F', 'D', 'G'), bpm=(124, 130, 138), form='build',
        progs={'A': ['i', 'i', 'i', 'i', 'VI', 'VI', 'VII', 'VII'],
               'B': ['i', 'i', 'iv', 'iv', 'i', 'i', 'v', 'v'],
               'break': ['VI', 'VI', 'VI', 'VI', 'i', 'i', 'i', 'i']},
        lead=_lead([(0, 0, 0.25), (0, 0.5, 0.25), (2, 0.75, 0.25), (1, 1.5, 0.5), (0, 2, 0.25), (0, 2.5, 0.25),
                    (3, 2.75, 0.25), (2, 3.5, 0.5)]),
        acc={'type': 'offbeat', 'pattern': (2, 1), 'every': 1.0, 'len': 0.2},
        bass={'type': 'offbeat'},
        model={0: ('sixteenth', 2, (60, 84))},
        temperature=1.0, lengths=(0.5, 0.2, 0.4), drums_default='full', drum_pattern='four_floor',
        gains=(0.8, 0.35, 0.65), cadence=[12, 12, 7, 7, 3, 3, 0, 0], instruments=(81, 81, 38)),
    'chiptune': Recipe(
        name='chiptune', desc='8-bit console chiptune: bright major key, very fast arpeggios, bouncy bass',
        mode='major', keys=('C', 'G', 'D', 'A', 'E'), bpm=(150, 160, 172),
        progs={'A': ['I', 'V', 'vi', 'IV', 'I', 'V', 'IV', 'V'],
               'B': ['IV', 'V', 'iii', 'vi', 'ii', 'V', 'I', 'I'],
               'break': ['vi', 'vi', 'IV', 'IV', 'ii', 'ii', 'V', 'V']},
        lead=_lead([(0, 0, 0.25), (1, 0.25, 0.25), (2, 0.5, 0.5), (4, 1, 0.5), (3, 1.5, 0.5), (2, 2, 1), (0, 3, 1)],
                   scalar=True),
        acc={'type': 'arp', 'step': 0.25, 'pattern': [0, 1, 2, 3], 'lift': False, 'len': 0.2},
        bass={'type': 'octave_eighths', 'octaves': True},
        model={0: ('sixteenth', 2, (64, 86))},
        temperature=1.0, lengths=(0.6, 0.2, 0.4), drums_default='full', hats='eighths',
        gains=(0.8, 0.3, 0.6), cadence=[12, 7, 4, 0, 4, 7, 12, 12], instruments=(80, 81, 38)),
    'rock': Recipe(
        name='rock', desc='hard rock: power chords chugging in eighths, driving bass, heavy backbeat',
        mode='minor', keys=('E', 'A', 'D', 'G'), bpm=(116, 132, 150),
        progs={'A': ['i5', 'i5', 'VI5', 'VII5', 'i5', 'i5', 'VI5', 'VII5'],
               'B': ['iv5', 'iv5', 'i5', 'i5', 'VI5', 'VII5', 'i5', 'i5'],
               'break': ['VI5', 'VI5', 'VII5', 'VII5', 'iv5', 'iv5', 'V5', 'V5']},
        lead=_lead([(0, 0, 1), (1, 1, 0.5), (0, 1.5, 0.5), (2, 2, 1.5), (1, 3.5, 0.5)], scalar=True),
        acc={'type': 'arp', 'step': 0.5, 'pattern': [0, 1], 'lift': False, 'len': 0.4},
        bass={'type': 'octave_eighths'},
        model={0: ('eighth', 3, (60, 86))},
        temperature=1.0, lengths=(1.5, 0.4, 0.4), drums_default='full', drum_pattern='rock',
        gains=(0.85, 0.35, 0.65), cadence=[12, 10, 7, 5, 3, 0, 0, 0], instruments=(30, 29, 34)),
    'march': Recipe(
        name='march', desc='a brass-band march: strong beat, dotted rhythms, oom-pah, snare rolls',
        mode='major', keys=('Bb', 'F', 'Eb', 'C'), bpm=(108, 116, 124),
        progs={'A': ['I', 'IV', 'I', 'V', 'I', 'IV', 'V', 'I'],
               'B': ['V', 'V', 'I', 'I', 'V', 'V', 'I', 'V'],
               'break': ['IV', 'IV', 'I', 'I', 'ii', 'V', 'I', 'V']},
        lead=_lead([(0, 0, 0.75), (1, 0.75, 0.25), (2, 1, 1), (3, 2, 0.75), (2, 2.75, 0.25), (1, 3, 1)]),
        acc={'type': 'offbeat', 'pattern': (1, 2), 'every': 1.0, 'len': 0.3},
        bass={'type': 'root_fifth'},
        model={0: ('sixteenth', 3, (62, 88))},
        temperature=0.95, lengths=(1.5, 0.3, 0.8), drums_default='full', drum_pattern='march',
        gains=(0.85, 0.3, 0.6), cadence=[12, 7, 4, 7, 12, 12, 12, 12], instruments=(56, 60, 58)),
    'reggae': Recipe(
        name='reggae', desc='reggae: laid-back off-beat skank, one-drop drums, syncopated bass',
        mode='major', keys=('G', 'A', 'C', 'D'), bpm=(70, 80, 92), swing=0.56,
        progs={'A': ['I', 'I', 'IV', 'IV', 'I', 'I', 'V', 'V'],
               'B': ['vi', 'IV', 'I', 'V', 'vi', 'IV', 'I', 'V'],
               'break': ['IV', 'IV', 'I', 'I', 'IV', 'IV', 'V', 'V']},
        lead=_lead([(0, 0.5, 1), (1, 1.5, 0.5), (2, 2, 1.5), (1, 3.5, 0.5)], scalar=True),
        acc={'type': 'offbeat', 'pattern': (1, 2), 'every': 1.0, 'len': 0.18},
        bass={'type': 'reggae'},
        model={0: ('eighth', 3, (60, 84))},
        temperature=0.98, lengths=(1.5, 0.2, 0.9), drums_default='full', drum_pattern='onedrop',
        gains=(0.85, 0.3, 0.65), cadence=[12, 9, 7, 4, 2, 0, 0, 0], instruments=(22, 27, 33), min_lead=12),
    'jig': Recipe(
        name='jig', desc='a Celtic jig in 6/8: running triplet-feel melody, lilting accompaniment',
        mode='major', keys=('D', 'G', 'A', 'E'), bpm=(150, 165, 180), meter=(6, 8),
        progs={'A': ['I', 'I', 'V', 'V', 'I', 'I', 'IV', 'V'],
               'B': ['vi', 'vi', 'IV', 'V', 'I', 'IV', 'V', 'I'],
               'break': ['IV', 'IV', 'I', 'I', 'V', 'V', 'I', 'V']},
        lead=_lead([(0, 0, 0.5), (1, 0.5, 0.5), (2, 1, 0.5), (1, 1.5, 0.5), (0, 2, 0.5), (2, 2.5, 0.5)]),
        acc={'type': 'broken68'},
        bass={'type': 'root_fifth'},
        model={0: ('eighth', 2, (62, 88))},
        temperature=0.98, lengths=(1.0, 0.45, 1.3), drums_default='light', drum_pattern='jig',
        gains=(0.85, 0.3, 0.6), cadence=[12, 7, 4, 0, 0, 0], instruments=(110, 24, 32)),
    'ragtime': Recipe(
        name='ragtime', desc='piano ragtime: syncopated melody over a stride left hand',
        mode='major', keys=('C', 'F', 'G', 'Bb'), bpm=(88, 100, 112),
        progs={'A': ['I', 'I', 'VI7', 'VI7', 'II7', 'V7', 'I', 'V7'],
               'B': ['IV', 'IV', 'I', 'I', 'II7', 'II7', 'V7', 'V7'],
               'break': ['vi', 'III7', 'vi', 'III7', 'II7', 'V7', 'I', 'I']},
        lead=_lead([(0, 0, 0.25), (1, 0.25, 0.5), (2, 0.75, 0.25), (1, 1, 0.5), (3, 1.5, 0.5), (2, 2, 0.25),
                    (0, 2.25, 0.5), (1, 2.75, 0.75), (2, 3.5, 0.5)], scalar=True),
        acc={'type': 'boomchick', 'len': 0.4},
        bass={'type': 'root_fifth'},
        model={0: ('sixteenth', 2, (62, 90))},
        temperature=1.0, lengths=(0.8, 0.4, 0.8), drums_default='none', drum_pattern='none',
        gains=(0.85, 0.35, 0.6), cadence=[12, 7, 4, 0, 4, 7, 12, 12], instruments=(0, 0, 0)),
    'bossa': Recipe(
        name='bossa', desc='bossa nova: gentle Brazilian groove, jazzy major-7th chords, syncopated guitar',
        mode='major', keys=('F', 'C', 'G', 'D'), bpm=(120, 132, 144),
        progs={'A': ['IM7', 'IM7', 'II7', 'II7', 'ii7', 'V7', 'IM7', 'V7'],
               'B': ['IVM7', 'IVM7', 'iv7', 'bVII7', 'iii7', 'VI7', 'ii7', 'V7'],
               'break': ['IM7', 'vi7', 'ii7', 'V7', 'IM7', 'vi7', 'ii7', 'V7']},
        lead=_lead([(1, 0, 1.5), (2, 1.5, 1), (3, 2.5, 1.5)]),
        acc={'type': 'rhythm', 'hits': [(0, 1), (1, 2), (1.5, 1), (2.5, 3), (3, 2)], 'len': 0.4},
        bass={'type': 'tumbao'},
        model={0: ('eighth', 4, (60, 86))},
        temperature=0.98, lengths=(2.0, 0.4, 1.4), drums_default='light', drum_pattern='bossa',
        gains=(0.85, 0.3, 0.6), cadence=[11, 9, 7, 4, 2, 0, 0, 0], instruments=(73, 24, 32), min_lead=10),
    'lullaby': Recipe(
        name='lullaby', desc='a gentle lullaby in 6/8: slow, soft, rocking harp-like arpeggios',
        mode='major', keys=('F', 'C', 'G', 'D'), bpm=(84, 96, 108), meter=(6, 8),
        progs={'A': ['I', 'IV', 'I', 'V', 'I', 'IV', 'V', 'I'],
               'B': ['vi', 'IV', 'I', 'V', 'vi', 'IV', 'V', 'V'],
               'break': ['IV', 'I', 'IV', 'I', 'ii', 'V', 'I', 'I']},
        lead=_lead([(0, 0, 1), (1, 1, 0.5), (2, 1.5, 1.5)]),
        acc={'type': 'broken68', 'vel': 55},
        bass={'type': 'root_whole'},
        model={0: ('eighth', 4, (62, 84))},
        temperature=0.95, lengths=(2.5, 0.45, 2.8), drums_default='none', drum_pattern='none',
        gains=(0.75, 0.25, 0.5), cadence=[7, 4, 2, 0, 0, 0], instruments=(10, 46, 32), min_lead=8),
    'synthwave': Recipe(
        name='synthwave', desc='80s synthwave: moody minor chords, pulsing octave bass, sixteenth arpeggios',
        mode='minor', keys=('A', 'E', 'F#', 'C'), bpm=(90, 100, 112),
        progs={'A': ['i', 'VI', 'III', 'VII', 'i', 'VI', 'III', 'VII'],
               'B': ['VI', 'VII', 'i', 'i', 'VI', 'VII', 'III', 'III'],
               'break': ['iv', 'iv', 'VI', 'VI', 'iv', 'iv', 'VII', 'VII']},
        lead=_lead([(0, 0, 1.5), (1, 1.5, 0.5), (2, 2, 2)]),
        acc={'type': 'arp', 'step': 0.25, 'pattern': [0, 1, 2, 3], 'lift': False, 'len': 0.2},
        bass={'type': 'octave_eighths', 'octaves': True},
        model={0: ('eighth', 4, (60, 86))},
        temperature=0.98, lengths=(2.5, 0.2, 0.4), drums_default='full', drum_pattern='rock',
        gains=(0.85, 0.28, 0.6), cadence=[12, 10, 7, 3, 0, 0, 0, 0], instruments=(81, 90, 38), min_lead=10),
    'surf': Recipe(
        name='surf', desc='surf rock / spy theme: tremolo-picked minor melody, chromatic guitar line, fast beat',
        mode='harmonic', keys=('E', 'A', 'D'), bpm=(150, 165, 180), tremolo=0.25,
        progs={'A': ['i', 'i', 'iv', 'iv', 'i', 'i', 'V', 'V'],
               'B': ['VI', 'VI', 'V', 'V', 'i', 'i', 'V', 'V'],
               'break': ['i', 'i', 'i', 'i', 'iv', 'iv', 'V', 'V']},
        lead=_lead([(0, 0, 1), (2, 1, 1), (1, 2, 1), (0, 3, 1)]),
        acc={'type': 'cliche'},
        bass={'type': 'octave_eighths'},
        model={0: ('quarter', 4, (60, 84))},
        temperature=1.0, lengths=(2.0, 0.9, 0.4), drums_default='full', drum_pattern='rock',
        gains=(0.85, 0.3, 0.6), cadence=[12, 11, 7, 3, 0, 0, 0, 0], instruments=(27, 26, 34), min_lead=8),
    'space': Recipe(
        name='space', desc='space drift: slow, floating, long notes over slowly turning arpeggios',
        mode='dorian', keys=('D', 'A', 'E', 'C'), bpm=(64, 72, 80),
        progs={'A': ['i', 'i', 'VII', 'VII', 'IV', 'IV', 'i', 'i'],
               'B': ['iv', 'iv', 'i', 'i', 'VII', 'VII', 'III', 'III'],
               'break': ['VII', 'VII', 'VII', 'VII', 'i', 'i', 'i', 'i']},
        lead=_lead([(0, 0, 2), (2, 2, 2)]),
        acc={'type': 'arp', 'step': 0.5, 'pattern': [0, 1, 2, 3, 2, 1], 'lift': False, 'len': 0.45, 'vel': 60},
        bass={'type': 'root_whole'},
        model={0: ('quarter', 5, (60, 84))},
        temperature=0.95, lengths=(4.0, 0.45, 3.8), drums_default='none', drum_pattern='halftime',
        gains=(0.8, 0.25, 0.55), cadence=[12, 12, 7, 7, 0, 0, 0, 0], instruments=(89, 88, 38), min_lead=6),
    'puzzle': Recipe(
        name='puzzle', desc='a cheerful puzzle-game tune: short bouncy staccato motifs, bright and light',
        mode='major', keys=('C', 'F', 'G', 'D'), bpm=(112, 120, 132),
        progs={'A': ['I', 'vi', 'ii', 'V', 'I', 'vi', 'ii', 'V'],
               'B': ['IV', 'V', 'iii', 'vi', 'ii', 'V', 'I', 'I'],
               'break': ['vi', 'vi', 'ii', 'ii', 'IV', 'IV', 'V', 'V']},
        lead=_lead([(0, 0, 0.25), (1, 0.5, 0.25), (2, 1, 0.25), (1, 1.5, 0.25), (3, 2, 0.5), (2, 3, 0.25),
                    (1, 3.5, 0.25)], scalar=True),
        acc={'type': 'rhythm', 'hits': [(0, 0), (1, 1), (2, 2), (3, 1)], 'len': 0.2},
        bass={'type': 'root_fifth'},
        model={0: ('eighth', 2, (64, 88))},
        temperature=1.0, lengths=(0.3, 0.2, 0.5), drums_default='light', hats='quarters',
        gains=(0.8, 0.3, 0.6), cadence=[12, 7, 4, 0, 4, 7, 12, 12], instruments=(12, 11, 32)),
}

# The styles MindServer and Qwen may use.  New ones are added here only after genre_lab.py and a
# blind listening test (DESKMIND-MUSIC-DESIGN.md); CANDIDATES lists every recipe for the lab.
STYLES = STYLES_V1
CANDIDATES = tuple(RECIPES)


def resolve(spec, allowed=None):
    """Validated, clamped settings from a request (all fields optional).  Raises ValueError for a
    style that does not exist; everything else falls back to the recipe's defaults.  `allowed` =
    the styles accepted (default STYLES; the lab passes CANDIDATES)."""
    allowed = allowed or STYLES
    style = str(spec.get('style') or 'cracktro').lower()
    if style not in RECIPES or style not in allowed:
        raise ValueError(f'unknown style {style!r}; one of {", ".join(allowed)}')
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
    bars = min(LENGTHS[length], int(MAX_SECONDS * bpm / 60 / r.beats_per_bar))   # slow songs get fewer bars
    seed = int(spec.get('seed') or 0) & 0x7FFFFFFF
    return {'style': style, 'energy': energy, 'tempo': bpm, 'key': NAMES[TONICS[key]], 'tonic': TONICS[key],
            'mode': r.mode, 'drums': drums, 'length': length, 'bars': bars, 'title': title, 'seed': seed,
            'sketch': bool(spec.get('sketch'))}


def prog(rec, source):
    """A section source's chords: 'C' (the third theme of long songs) is a new tune over B's chords."""
    return rec.progs.get(source) or rec.progs['B']


def form(length_bars, n=8, kind='song'):
    """Sections: (name, source, bar indices, options).  source is 'A', 'B' or 'break'; n = bars per
    section (8, or 12 for a blues); length_bars picks the size (32 short, 48 medium, 64 long), as
    with 8-bar sections.  kind 'build' layers the parts in (techno); 'sketch' = theme A and an ending."""
    if kind == 'sketch':
        return [('Theme', 'A', range(n), {}), ('Ending', 'A', range(1), {'cadence': True, 'gain': 0.85})]
    intro = ('Intro', 'A', range(4), {'gain': 0.82, 'mute_lead': True, 'drums': 'sparse'})
    hook = ('Main theme', 'A', range(n), {})
    var = ('Theme, higher', 'A', range(n), {'gain': 1.04, 'octave': 12})
    b = ('Second theme', 'B', range(n), {'gain': 1.03})
    brk = ('Break', 'break', range(n), {'gain': 0.85, 'arp_sparse': True, 'drums': 'sparse'})
    final = ('Final theme', 'A', range(n), {'gain': 1.05})
    outro = ('Ending', 'A', range(4), {'gain': 0.85, 'cadence': True})
    if length_bars > 64:
        return _long_form(length_bars, n, kind)
    if kind == 'build':
        intro = ('Intro', 'A', range(4), {'mute_lead': True, 'mute_acc': True, 'gain': 0.9})
        groove = ('Groove', 'A', range(n), {'mute_lead': True})
        brk = ('Breakdown', 'break', range(n), {'gain': 0.9, 'drums': 'none', 'mute_bass': True})
        if length_bars <= 32:
            return [intro, groove, hook, final, outro]
        if length_bars <= 48:
            return [intro, groove, hook, b, brk, final, outro]
        return [intro, groove, hook, b, brk, final, ('Second theme again', 'B', range(n), {'gain': 1.04}), outro]
    if length_bars <= 32:
        return [intro, hook, b, final, outro]
    if length_bars <= 48:
        return [intro, hook, var, b, brk, final, outro]
    return [intro, hook, var, b, brk, final, ('Second theme again', 'B', range(n), {'gain': 1.04}),
            ('Final theme, higher', 'A', range(n), {'gain': 1.06, 'octave': 12}), outro]


def _long_form(length_bars, n, kind):
    """Songs longer than 'long' (played from disk): the long form, then a third theme (C), the themes again
    with variations, and a key change up a whole step for the last themes and the ending."""
    brk_opts = {'gain': 0.9, 'drums': 'none', 'mute_bass': True} if kind == 'build' else         {'gain': 0.85, 'arp_sparse': True, 'drums': 'sparse'}
    if kind == 'build':
        secs = [('Intro', 'A', range(4), {'mute_lead': True, 'mute_acc': True, 'gain': 0.9}),
                ('Groove', 'A', range(n), {'mute_lead': True}), ('Main theme', 'A', range(n), {})]
    else:
        secs = [('Intro', 'A', range(4), {'gain': 0.82, 'mute_lead': True, 'drums': 'sparse'}),
                ('Main theme', 'A', range(n), {}), ('Theme, higher', 'A', range(n), {'gain': 1.04, 'octave': 12})]
    secs += [('Second theme', 'B', range(n), {'gain': 1.03}), ('Break', 'break', range(n), brk_opts),
             ('Main theme', 'A', range(n), {'gain': 1.05})]
    more = [('Third theme', 'C', range(n), {'gain': 1.02}), ('Third theme, higher', 'C', range(n), {'gain': 1.04, 'octave': 12}),
            ('Second theme again', 'B', range(n), {'gain': 1.03}), ('Break', 'break', range(n), brk_opts),
            ('Main theme', 'A', range(n), {}), ('Third theme again', 'C', range(n), {'gain': 1.03})]
    ending = [('Main theme, new key', 'A', range(n), {'gain': 1.05, 'transpose': 2}),
              ('Final theme, higher', 'A', range(n), {'gain': 1.06, 'octave': 12, 'transpose': 2}),
              ('Ending', 'A', range(4), {'gain': 0.85, 'cadence': True, 'transpose': 2})]
    total = sum(len(b) for _, _, b, _ in secs + ending)
    k = 0
    while total + n <= length_bars:
        secs.append(more[k % len(more)])
        total += n
        k += 1
    return secs + ending
