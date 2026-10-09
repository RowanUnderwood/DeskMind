// DeskMind - background music from IRQ0.  See music.h.
//
// THIS FILE IS COMPILED WITH -zu (see the makefile): irq0() runs on whatever stack was
// interrupted (DOS, the disk BIOS, the packet driver), so its data must be reached through DS.
// The handler makes no DOS or BIOS calls and allocates nothing.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dos.h>
#include <conio.h>
#include <i86.h>
#include <io.h>
#include <fcntl.h>

#include "music.h"
#include "video.h"

void ( *music_owner_hook )( int on ) = 0;
unsigned char far *music_log = 0;
unsigned music_log_max = 0;
volatile unsigned music_log_n = 0;

static void (__interrupt __far *s_old08)( void ) = 0;
static void (__interrupt __far *s_old23)( void ) = 0;
static unsigned s_seg = 0;                         // DOS block holding the song
static unsigned char far *s_buf = 0;
static unsigned s_size = 0;
static unsigned s_div = 0;                         // PIT divisor (1193182 / rate)
static unsigned s_endTick = 0;
static volatile unsigned s_pos = 0;                // offset of the next record
static volatile unsigned s_tick = 0;               // song tick (rate ticks since the start)
static volatile unsigned s_frac = 0;               // PIT clocks towards the next BIOS tick
static volatile unsigned char s_state = 0;         // 0 idle, 1 playing, 2 ended
static unsigned char s_loop = 0;
static unsigned char s_old61 = 0, s_set61 = 0;
static int s_atexit = 0;

// The SN76496 needs about 9 us per byte; ISA port reads take about 1 us on any CPU
static void psg( unsigned char b ) {
  outp( 0xC0, b );
  for ( int i = 0; i < 10; i++ ) inp( 0x61 );
}

static void mute_all( void ) {
  psg( 0x9F ); psg( 0xBF ); psg( 0xDF ); psg( 0xFF );
}

static void __interrupt __far irq0( void ) {
  if ( s_state == 1 ) {
    for ( ;; ) {
      unsigned t = *(unsigned far *)( s_buf + s_pos );
      if ( t == 0xFFFF ) {                         // end of the stream (it ended with a mute record)
        if ( s_loop ) { s_pos = 8; s_tick = 0xFFFF; }   // wraps to 0 below: tick 0 plays next time
        else s_state = 2;
        break;
      }
      if ( t > s_tick ) break;
      unsigned char n = s_buf[ s_pos + 2 ];
      const unsigned char far *p = s_buf + s_pos + 3;
      for ( unsigned char k = 0; k < n; k++ ) {
        psg( p[k] );
        if ( music_log && music_log_n + 3 <= music_log_max ) {      // MUSTEST only
          unsigned i = music_log_n;
          music_log[i] = (unsigned char)s_tick; music_log[i+1] = (unsigned char)( s_tick >> 8 ); music_log[i+2] = p[k];
          music_log_n = i + 3;
        }
      }
      s_pos += 3 + n;
    }
    s_tick++;
  }
  // The BIOS handler (clock, INT 1Ch, its own end-of-interrupt) every 65536 PIT clocks
  unsigned f = s_frac;
  s_frac = f + s_div;
  if ( s_frac < f ) _chain_intr( s_old08 );
  outp( 0x20, 0x20 );
}

// The IRQ0 vector, read and written directly with interrupts off (a DOS call could enable them)
typedef void (__interrupt __far *Vec)( void );
static Vec get08( void ) { return *(Vec far *)MK_FP( 0, 8 * 4 ); }
static void set08( Vec v ) { *(Vec far *)MK_FP( 0, 8 * 4 ) = v; }

// Ctrl-C while a song plays: ignored (DOS would end the program with IRQ0 still ours)
static void __interrupt __far break23( void ) { }

int music_available( void ) {
  return vid_detect( ) != VT_OTHER;               // the same test as sound.cpp's Tandy chip
}

const char *music_error( int rc ) {
  switch ( rc ) {
    case MUS_OK:      return "OK";
    case MUS_NO_CHIP: return "Music needs the Tandy sound chip";
    case MUS_NO_FILE: return "Could not read the song file";
    case MUS_BAD:     return "The song file is damaged (not a T3P1 stream)";
    case MUS_TOO_BIG: return "The song is too long (over 32,000 bytes)";
    case MUS_NO_MEM:  return "Not enough memory for the song";
  }
  return "?";
}

// The checks of the proven RASTER.COM player and export-tandy.py's read_stream
static int check( const unsigned char far *b, unsigned size, unsigned *div, unsigned *endTick ) {
  if ( size < 11 || _fmemcmp( b, "T3P1", 4 ) ) return MUS_BAD;
  unsigned d = *(const unsigned far *)( b + 4 );
  if ( d < 2000 ) return MUS_BAD;
  unsigned pos = 8, last = 0;
  for ( ;; ) {
    if ( pos + 3 > size ) return MUS_BAD;
    unsigned t = *(const unsigned far *)( b + pos );
    unsigned n = b[ pos + 2 ];
    pos += 3;
    if ( t == 0xFFFF ) {
      if ( n != 0 || pos != size ) return MUS_BAD;
      break;
    }
    if ( n < 1 || n > 32 || t < last || pos + n > size ) return MUS_BAD;
    last = t;
    pos += n;
  }
  *div = d;
  *endTick = *(const unsigned far *)( b + 6 );
  if ( *endTick < last ) *endTick = last;
  return MUS_OK;
}

static unsigned secs( unsigned ticks, unsigned div ) {
  return (unsigned)( ( (unsigned long)ticks * div + 596591ul ) / 1193182ul );
}

int music_probe( const char *path, unsigned *seconds ) {
  int h = open( path, O_RDONLY | O_BINARY );
  if ( h < 0 ) return MUS_NO_FILE;
  static unsigned char hdr[8];
  int got = read( h, hdr, 8 );
  long size = filelength( h );
  close( h );
  if ( got != 8 || memcmp( hdr, "T3P1", 4 ) ) return MUS_BAD;
  if ( size > (long)MUSIC_MAX ) return MUS_TOO_BIG;
  unsigned d = *(unsigned *)( hdr + 4 ), e = *(unsigned *)( hdr + 6 );
  if ( d < 2000 ) return MUS_BAD;
  if ( seconds ) *seconds = secs( e, d );
  return MUS_OK;
}

void music_stop( void ) {
  if ( !s_old08 ) return;
  _disable( );
  s_state = 0;
  outp( 0x43, 0x36 );                              // PIT channel 0: mode 3, divisor 65536 (18.2 Hz)
  outp( 0x40, 0 );
  outp( 0x40, 0 );
  set08( s_old08 );
  s_old08 = 0;
  _enable( );
  mute_all( );
  if ( s_old23 ) { _dos_setvect( 0x23, s_old23 ); s_old23 = 0; }
  if ( s_set61 ) { outp( 0x61, ( inp( 0x61 ) & ~0x60 ) | ( s_old61 & 0x60 ) ); s_set61 = 0; }
  if ( s_seg ) { _dos_freemem( s_seg ); s_seg = 0; s_buf = 0; }
  if ( music_owner_hook ) music_owner_hook( 0 );
}

void music_done( void ) { music_stop( ); }

int music_play( const char *path, int loop ) {
  music_stop( );
  if ( !music_available( ) ) return MUS_NO_CHIP;
  int h = open( path, O_RDONLY | O_BINARY );
  if ( h < 0 ) return MUS_NO_FILE;
  long size = filelength( h );
  if ( size > (long)MUSIC_MAX ) { close( h ); return MUS_TOO_BIG; }
  if ( size < 11 ) { close( h ); return MUS_BAD; }
  if ( _dos_allocmem( (unsigned)( ( size + 15 ) >> 4 ), &s_seg ) ) { s_seg = 0; close( h ); return MUS_NO_MEM; }
  s_buf = (unsigned char far *)MK_FP( s_seg, 0 );
  int bad = read( h, s_buf, (unsigned)size ) != (int)size;
  close( h );
  int rc = bad ? MUS_NO_FILE : check( s_buf, (unsigned)size, &s_div, &s_endTick );
  if ( rc ) { _dos_freemem( s_seg ); s_seg = 0; s_buf = 0; return rc; }
  s_size = (unsigned)size;
  if ( !s_atexit ) { atexit( music_done ); s_atexit = 1; }

  if ( music_owner_hook ) music_owner_hook( 1 );   // sound effects step aside (and go quiet)
  s_old61 = inp( 0x61 );
  if ( ( s_old61 & 0x60 ) != 0x60 ) { outp( 0x61, s_old61 | 0x60 ); s_set61 = 1; }   // chip to the speaker
  mute_all( );
  s_old23 = _dos_getvect( 0x23 );
  _dos_setvect( 0x23, break23 );
  s_pos = 8;
  s_tick = 0;
  s_frac = 0;
  s_loop = (unsigned char)( loop != 0 );
  _disable( );
  s_old08 = get08( );
  set08( irq0 );
  outp( 0x43, 0x34 );                              // PIT channel 0: mode 2 at the song's rate
  outp( 0x40, s_div & 0xFF );
  outp( 0x40, s_div >> 8 );
  s_state = 1;
  _enable( );
  return MUS_OK;
}

int music_playing( void ) { return s_old08 != 0; }

int music_poll( void ) {
  if ( s_old08 && s_state == 2 ) { music_stop( ); return 1; }
  return 0;
}

unsigned music_length( void ) { return s_old08 ? secs( s_endTick, s_div ) : 0; }

unsigned music_position( void ) {
  if ( !s_old08 ) return 0;
  unsigned t = s_tick;
  if ( t > s_endTick ) t = s_endTick;
  return secs( t, s_div );
}
