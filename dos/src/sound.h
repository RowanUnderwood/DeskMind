// DeskMind - Tandy 3-voice sound (SN76496 at port C0h), or the PC speaker on other PCs.
// Sequences play in the background from the INT 1Ch timer tick (18.2/s).

#ifndef SOUND_H
#define SOUND_H

#include <stdio.h>

struct SndNote {
  unsigned      hz1, hz2;    // voices 0 and 1 (0 = silent)
  unsigned char ticks;       // length in 55 ms ticks; 0 ends the sequence
  unsigned char vol;         // 0 = loudest .. 15 = off; decays 2 per tick
};

extern int snd_enabled;      // user setting; snd_play() does nothing if 0
extern int snd_mode;         // SNDTEST only: 0 = old writes, 1 = paced, 2 = paced + refresh (default)

void snd_init( void );
void snd_done( void );       // must be called before exit (unhooks INT 1Ch)
void snd_play( const SndNote *seq );
void snd_stop( void );
int  snd_busy( void );
void snd_poll( void );       // call often: safety nets (logged), see sound.cpp
void snd_settle( void );     // wait until the current sound has ended; call before disk work
void snd_music( int on );    // music.cpp's owner hook: 1 = a song owns the chip (effects are muted)

// Diagnostics for the stuck-note hunt: an event ring, dumped by snd_diag()
enum { SL_PLAY = 1, SL_END, SL_STOP, SL_WATCHDOG, SL_NET, SL_STALL, SL_USER, SL_DISK_BEGIN, SL_DISK_END, SL_SETTLE };
enum { SD_RESCAN, SD_DELETE, SD_CHAT_LOAD, SD_CHAT_SAVE, SD_DOWNLOAD, SD_CHAT_LIST };
extern volatile unsigned long snd_isr_count;    // calls of our INT 1Ch handler
void snd_log( int code, int arg );
void snd_force( int step );  // DeskMind F9: 2 = chip silence, 3 = speaker gate off, 4 = PIT 2 + multiplexer reset
void snd_diag( FILE *f );    // state + event ring, as text

// Built-in effects
extern const SndNote SND_STARTUP[];
extern const SndNote SND_REPLY[];
extern const SndNote SND_IMAGE[];
extern const SndNote SND_ERROR[];
extern const SndNote SND_CLICK[];

#endif
