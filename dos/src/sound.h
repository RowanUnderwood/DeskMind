// DeskMind - Tandy 3-voice sound (SN76496 at port C0h).
// Sequences play in the background from the INT 1Ch timer tick (18.2/s).

#ifndef SOUND_H
#define SOUND_H

struct SndNote {
  unsigned      hz1, hz2;    // voices 0 and 1 (0 = silent)
  unsigned char ticks;       // length in 55 ms ticks; 0 ends the sequence
  unsigned char vol;         // 0 = loudest .. 15 = off; decays 2 per tick
};

extern int snd_enabled;      // user setting; snd_play() does nothing if 0

void snd_init( void );
void snd_done( void );       // must be called before exit (unhooks INT 1Ch)
void snd_play( const SndNote *seq );
void snd_stop( void );
int  snd_busy( void );

// Built-in effects
extern const SndNote SND_STARTUP[];
extern const SndNote SND_REPLY[];
extern const SndNote SND_IMAGE[];
extern const SndNote SND_ERROR[];
extern const SndNote SND_CLICK[];

#endif
