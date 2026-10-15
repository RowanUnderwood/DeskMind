// DeskMind - background music: T3P1 register streams for the Tandy 3-voice chip (SN76496 at C0h),
// or the same streams on the PC speaker.
//
// T3P1 (made by MindServer's music worker and tools\make_modem.py; the format of the
// RASTER.T3 player that was proven on the real TL/3):
//   "T3P1", u16 PIT divisor, u16 end tick, then records:
//   u16 absolute tick (never decreasing), u8 count (1-32), count bytes for port C0h.
//   The last record is tick FFFFh with count 0.  Streams start and end with a mute record.
//   Ticks are 16 bits: at the usual 120 Hz no song is longer than 546 seconds.
//
// Playback runs from IRQ0 (INT 08h) at the stream's rate (about 120 Hz): the PIT is
// reprogrammed only while a song plays, and the BIOS handler is still called at 18.2 Hz on
// average, so BIOS ticks, mTCP and the INT 1Ch sound effects keep their time.
//
// A song of at most MUSIC_MAX bytes sits whole in one DOS block (the proven path).  A longer
// one streams: a MUSIC_RING-byte ring in one DOS block that music_poll() refills from the open
// file (the disk can't be read from an interrupt).  If the main loop is away so long that the
// ring runs dry, the song pauses, muted, and goes on when data comes (music_underruns() counts it).
//
// Speaker output (music_set_output): the handler runs MUSIC_SPK_MULT times per stream tick,
// decodes the chip writes into shadow registers and drives PIT channel 2 with one voice at a
// time: the lead on two of every three sub-ticks, the bass and accompaniment taking turns on
// the third, noise hits as short high clicks.  Labtext2midi\worker\t3.py speaker_schedule() is
// the reference this follows exactly (MUSTEST /SPK compares them).

#ifndef MUSIC_H
#define MUSIC_H

#define MUSIC_MAX  32000u                  // played from memory up to this size
#define MUSIC_RING 16384u                  // larger files stream through this ring (a power of two)
#define MUSIC_SPK_MULT 2

enum { MUS_OK = 0, MUS_NO_CHIP, MUS_NO_FILE, MUS_BAD, MUS_TOO_BIG, MUS_NO_MEM };
enum { MUSIC_CHIP = 0, MUSIC_SPEAKER = 1 };

int  music_available( void );              // 1 = something to play on (the chip, or the speaker)
void music_set_output( int out );          // MUSIC_CHIP (needs the Tandy chip) or MUSIC_SPEAKER; before music_play
int  music_output( void );                 // what songs play on: MUSIC_CHIP or MUSIC_SPEAKER
int  music_play( const char *path, int loop );   // MUS_*; stops any song first
void music_stop( void );                   // silence, PIT and IRQ0 back, buffer freed, file closed
void music_release( const char *path );    // stops the song if it streams from this file (before deleting it)
int  music_playing( void );                // 1 while a song is loaded (playing or just ended)
int  music_poll( void );                   // call often: refills a streaming song; 1 once when a non-looping song has ended
void music_done( void );                   // before exit (also registered with atexit)
unsigned music_length( void );             // seconds, of the song playing
unsigned music_position( void );           // seconds into it
unsigned music_underruns( void );          // times a streaming song ran dry and paused
int  music_streaming( void );              // 1 = the song plays from the file, 0 = from memory
const char *music_error( int rc );
// Checks a file's header without playing it: MUS_OK and its length in seconds
int  music_probe( const char *path, unsigned *seconds );

// Called with 1 before a song takes the chip or speaker and 0 after it gave it back (DeskMind's
// sound effects step aside: see snd_music in sound.cpp).  SLIDES.EXE leaves it 0.
extern void ( *music_owner_hook )( int on );

// MUSTEST only, while music_log is set: chip output logs every write as (u16 song tick, byte),
// 3 bytes each; speaker output logs every sub-tick's PIT count as a u16 (0 = speaker off)
extern unsigned char far *music_log;
extern unsigned music_log_max;
extern volatile unsigned music_log_n;
extern unsigned music_ring_cap;            // MUSTEST only: fill the ring only this far (MUSIC_RING normally)

#endif
