// DeskMind - background music: T3P1 register streams for the Tandy 3-voice chip (SN76496 at C0h).
//
// T3P1 (made by MindServer's music worker and tools\make_modem.py; the format of the
// RASTER.T3 player that was proven on the real TL/3):
//   "T3P1", u16 PIT divisor, u16 end tick, then records:
//   u16 absolute tick (never decreasing), u8 count (1-32), count bytes for port C0h.
//   The last record is tick FFFFh with count 0.  Streams start and end with a mute record.
//
// Playback runs from IRQ0 (INT 08h) at the stream's rate (about 120 Hz): the PIT is
// reprogrammed only while a song plays, and the BIOS handler is still called at 18.2 Hz on
// average, so BIOS ticks, mTCP and the INT 1Ch sound effects keep their time.
// The whole song sits in one DOS block (at most MUSIC_MAX bytes), freed by music_stop().

#ifndef MUSIC_H
#define MUSIC_H

#define MUSIC_MAX 32000u

enum { MUS_OK = 0, MUS_NO_CHIP, MUS_NO_FILE, MUS_BAD, MUS_TOO_BIG, MUS_NO_MEM };

int  music_available( void );              // 1 = a Tandy sound chip to play on
int  music_play( const char *path, int loop );   // MUS_*; stops any song first
void music_stop( void );                   // silence, PIT and IRQ0 back, buffer freed
int  music_playing( void );                // 1 while a song is loaded (playing or just ended)
int  music_poll( void );                   // call often: 1 once when a non-looping song has ended (it is stopped then)
void music_done( void );                   // before exit (also registered with atexit)
unsigned music_length( void );             // seconds, of the song playing
unsigned music_position( void );           // seconds into it
const char *music_error( int rc );
// Checks a file without playing it: MUS_OK and its length in seconds
int  music_probe( const char *path, unsigned *seconds );

// Called with 1 before a song takes the chip and 0 after it gave it back (DeskMind's sound
// effects step aside: see snd_music in sound.cpp).  SLIDES.EXE leaves it 0.
extern void ( *music_owner_hook )( int on );

// MUSTEST only: every chip write as (u16 song tick, byte), 3 bytes each, while music_log is set
extern unsigned char far *music_log;
extern unsigned music_log_max;
extern volatile unsigned music_log_n;

#endif
