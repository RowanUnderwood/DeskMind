// DeskMind - background music from IRQ0.  See music.h.
//
// THIS FILE IS COMPILED WITH -zu (see the makefile): irq0() runs on whatever stack was
// interrupted (DOS, the disk BIOS, the packet driver), so its data must be reached through DS.
// The handler makes no DOS or BIOS calls and allocates nothing.
//
// The song's bytes are read through one window: s_buf[ offset & s_mask ].  From memory the whole
// file is there (s_mask = FFFFh, offsets 8 .. size); streaming, s_buf is the ring (s_mask =
// MUSIC_RING - 1) and offsets count bytes since the start (16-bit, wrapping).  s_rd is the next
// record (the handler moves it), s_wr the end of the bytes that are there (music_poll moves it).

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

#define SPK_AUDIBLE 12                             // the same rules as t3.py speaker_schedule()
#define SPK_DRUM    9

void ( *music_owner_hook )( int on ) = 0;
unsigned char far *music_log = 0;
unsigned music_log_max = 0;
volatile unsigned music_log_n = 0;
unsigned music_ring_cap = MUSIC_RING;

static void (__interrupt __far *s_old08)( void ) = 0;
static void (__interrupt __far *s_old23)( void ) = 0;
static unsigned s_seg = 0;                         // DOS block holding the song or the ring
static unsigned char far *s_buf = 0;
static unsigned s_mask = 0xFFFF;
static volatile unsigned s_rd = 0, s_wr = 0;       // see above
static unsigned s_div = 0;                         // PIT divisor of the stream (1193182 / rate)
static unsigned s_pitDiv = 0;                      // what channel 0 runs at (s_div / MULT on the speaker)
static unsigned s_endTick = 0;
static volatile unsigned s_tick = 0;               // song tick (rate ticks since the start)
static volatile unsigned s_frac = 0;               // PIT clocks towards the next BIOS tick
static volatile unsigned char s_state = 0;         // 0 idle, 1 playing, 2 ended
static volatile unsigned char s_stall = 0;         // streaming ran dry: paused, muted
static volatile unsigned s_under = 0;
static unsigned char s_loop = 0, s_stream = 0;
static unsigned char s_old61 = 0, s_set61 = 0;
static int s_atexit = 0;
static int s_out = -1;                             // MUSIC_CHIP / MUSIC_SPEAKER; -1 = not chosen yet
static unsigned char s_spk = 0;                    // this song plays on the speaker
static unsigned char s_mult = 1, s_sub = 0;
static int s_fh = -1;                              // streaming: the open song file
static long s_fpos = 0, s_fsize = 0;
static char s_path[80] = "";

// Speaker: shadow chip registers and the sub-tick counters of speaker_schedule()
static unsigned s_sdiv[3];
static unsigned char s_satt[4], s_latch = 0, s_hit = 0, s_drum = 0, s_ph3 = 0;
static unsigned s_n = 0, s_k3 = 0, s_lfsr = 0xACE1u, s_cnt = 0;

static int is_tandy( void ) { return vid_detect( ) != VT_OTHER; }

// The SN76496 needs about 9 us per byte; ISA port reads take about 1 us on any CPU
static void psg( unsigned char b ) {
  outp( 0xC0, b );
  for ( int i = 0; i < 10; i++ ) inp( 0x61 );
}

static void mute_all( void ) {
  psg( 0x9F ); psg( 0xBF ); psg( 0xDF ); psg( 0xFF );
}

// ---------------------------------------------------------------- speaker

static void spk_out( unsigned c ) {
  if ( c == s_cnt ) return;                        // reprogramming the same pitch would click
  if ( !c ) outp( 0x61, inp( 0x61 ) & ~3 );        // gate off
  else {
    outp( 0x42, c & 0xFF );
    outp( 0x42, c >> 8 );
    if ( !s_cnt ) outp( 0x61, inp( 0x61 ) | 3 );
  }
  s_cnt = c;
}

static void spk_byte( unsigned char b ) {
  if ( b & 0x80 ) {
    s_latch = ( b >> 4 ) & 7;
    if ( s_latch & 1 ) {
      unsigned char a = b & 15;
      s_satt[ s_latch >> 1 ] = a;
      if ( s_latch == 7 && a <= SPK_DRUM ) { unsigned char h = a <= 5 ? 2 : 1; if ( h > s_hit ) s_hit = h; }
    }
    else if ( s_latch < 6 ) s_sdiv[ s_latch >> 1 ] = ( s_sdiv[ s_latch >> 1 ] & 0x3F0 ) | ( b & 15 );
  }
  else if ( s_latch < 6 && !( s_latch & 1 ) ) {
    unsigned d = ( s_sdiv[ s_latch >> 1 ] & 15 ) | ( ( b & 63 ) << 4 );
    s_sdiv[ s_latch >> 1 ] = d ? d : 1;
  }
}

static void spk_sub( void ) {
  unsigned c;
  if ( s_stall || s_state != 1 ) c = 0;
  else if ( s_drum ) {
    s_drum--;
    s_lfsr = ( s_lfsr >> 1 ) ^ ( ( s_lfsr & 1 ) ? 0xB400u : 0 );
    c = 150 + ( s_lfsr & 511 );
  }
  else {
    unsigned char o[2], no = 0;
    if ( s_satt[2] <= SPK_AUDIBLE ) o[ no++ ] = 2;
    if ( s_satt[1] <= SPK_AUDIBLE ) o[ no++ ] = 1;
    int ch = -1;
    if ( s_satt[0] <= SPK_AUDIBLE ) ch = ( no && s_ph3 == 2 ) ? o[ no == 2 ? ( s_k3 & 1 ) : 0 ] : 0;
    else if ( no ) ch = o[ no == 2 ? ( s_n & 1 ) : 0 ];
    c = ch >= 0 ? ( s_sdiv[ ch ] << 5 ) / 3 : 0;
  }
  s_n++;
  if ( ++s_ph3 == 3 ) { s_ph3 = 0; s_k3++; }
  if ( music_log && music_log_n + 2 <= music_log_max ) {                 // MUSTEST only
    unsigned i = music_log_n;
    music_log[i] = (unsigned char)c; music_log[i+1] = (unsigned char)( c >> 8 );
    music_log_n = i + 2;
  }
  spk_out( c );
}

// ---------------------------------------------------------------- the stream

static unsigned char rb( unsigned off ) { return s_buf[ off & s_mask ]; }

static void out_byte( unsigned char b ) {
  if ( s_spk ) { spk_byte( b ); return; }
  psg( b );
  if ( music_log && music_log_n + 3 <= music_log_max ) {                 // MUSTEST only
    unsigned i = music_log_n;
    music_log[i] = (unsigned char)s_tick; music_log[i+1] = (unsigned char)( s_tick >> 8 ); music_log[i+2] = b;
    music_log_n = i + 3;
  }
}

static void dry( void ) {
  if ( s_stall ) return;
  s_stall = 1;
  s_under++;
  if ( s_spk ) spk_out( 0 ); else mute_all( );
}

// One stream tick: every record due now (the records of RASTER.COM / export-tandy.py)
static void stream_tick( void ) {
  for ( ;; ) {
    unsigned avail = s_wr - s_rd;
    if ( avail < 3 ) { dry( ); return; }
    unsigned t = rb( s_rd ) | ( (unsigned)rb( s_rd + 1 ) << 8 );
    if ( t == 0xFFFF ) {                           // end of the stream (it ended with a mute record)
      s_rd += 3;
      if ( s_loop ) { if ( !s_stream ) s_rd = 8; s_tick = 0xFFFF; }   // wraps to 0 below: tick 0 plays next time
      else s_state = 2;
      break;
    }
    if ( t > s_tick ) break;
    unsigned char n = rb( s_rd + 2 );
    if ( avail < 3u + n ) { dry( ); return; }
    for ( unsigned char k = 0; k < n; k++ ) out_byte( rb( s_rd + 3 + k ) );
    s_rd += 3 + n;
  }
  s_stall = 0;
  if ( s_hit ) { s_drum = s_hit; s_hit = 0; }
  s_tick++;
}

static void __interrupt __far irq0( void ) {
  if ( s_state == 1 ) {
    if ( ++s_sub >= s_mult ) { s_sub = 0; stream_tick( ); }
    if ( s_spk ) spk_sub( );
  }
  // The BIOS handler (clock, INT 1Ch, its own end-of-interrupt) every 65536 PIT clocks
  unsigned f = s_frac;
  s_frac = f + s_pitDiv;
  if ( s_frac < f ) _chain_intr( s_old08 );
  outp( 0x20, 0x20 );
}

// The IRQ0 vector, read and written directly with interrupts off (a DOS call could enable them)
typedef void (__interrupt __far *Vec)( void );
static Vec get08( void ) { return *(Vec far *)MK_FP( 0, 8 * 4 ); }
static void set08( Vec v ) { *(Vec far *)MK_FP( 0, 8 * 4 ) = v; }

// Ctrl-C while a song plays: ignored (DOS would end the program with IRQ0 still ours)
static void __interrupt __far break23( void ) { }

// ---------------------------------------------------------------- the rest runs in the main program

static int chosen( void ) {
  if ( s_out < 0 ) s_out = is_tandy( ) ? MUSIC_CHIP : MUSIC_SPEAKER;
  return s_out;
}

void music_set_output( int out ) { s_out = out == MUSIC_SPEAKER ? MUSIC_SPEAKER : MUSIC_CHIP; }
int  music_output( void ) { return chosen( ); }
int  music_available( void ) { return chosen( ) == MUSIC_SPEAKER || is_tandy( ); }
unsigned music_underruns( void ) { return s_under; }
int  music_streaming( void ) { return s_old08 && s_stream; }

const char *music_error( int rc ) {
  switch ( rc ) {
    case MUS_OK:      return "OK";
    case MUS_NO_CHIP: return "Music needs the Tandy sound chip (or the PC speaker setting)";
    case MUS_NO_FILE: return "Could not read the song file";
    case MUS_BAD:     return "The song file is damaged (not a T3P1 stream)";
    case MUS_TOO_BIG: return "The song file is too big";
    case MUS_NO_MEM:  return "Not enough memory for the song";
  }
  return "?";
}

// The checks of the proven RASTER.COM player and export-tandy.py's read_stream, over a file read
// in pieces (so a streamed song is checked whole before it starts).  *lastTick = the last record's.
static int check_file( int h, long size, unsigned d, unsigned *lastTick ) {
  static unsigned char chunk[ 2048 ];
  unsigned have = 0, at = 0;                       // bytes in chunk, read position in it
  long pos = 8, last = 0;
  if ( d < 2000 ) return MUS_BAD;
  lseek( h, 8, SEEK_SET );
  for ( ;; ) {
    unsigned char hdr[3];
    for ( int k = 0; k < 3; k++ ) {
      if ( at == have ) { int got = read( h, chunk, sizeof( chunk ) ); if ( got <= 0 ) return MUS_BAD; have = got; at = 0; }
      hdr[k] = chunk[ at++ ];
    }
    pos += 3;
    unsigned t = hdr[0] | ( (unsigned)hdr[1] << 8 ), n = hdr[2];
    if ( t == 0xFFFF ) {
      if ( n != 0 || pos != size ) return MUS_BAD;
      break;
    }
    if ( n < 1 || n > 32 || t < last || pos + n > size ) return MUS_BAD;
    last = t;
    pos += n;
    while ( n ) {                                  // skip the payload
      if ( at == have ) { int got = read( h, chunk, sizeof( chunk ) ); if ( got <= 0 ) return MUS_BAD; have = got; at = 0; }
      unsigned k = have - at < n ? have - at : n;
      at += k;
      n -= k;
    }
  }
  *lastTick = (unsigned)last;
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
  close( h );
  if ( got != 8 || memcmp( hdr, "T3P1", 4 ) ) return MUS_BAD;
  unsigned d = *(unsigned *)( hdr + 4 ), e = *(unsigned *)( hdr + 6 );
  if ( d < 2000 ) return MUS_BAD;
  if ( seconds ) *seconds = secs( e, d );
  return MUS_OK;
}

static void close_file( void ) {
  if ( s_fh >= 0 ) { close( s_fh ); s_fh = -1; }
}

// Streaming: tops the ring up from the file (at most 4 pieces of up to 4 KB per call); a looping
// song starts the file again after its end record, which the handler reads straight on
static void refill( void ) {
  if ( !s_stream || s_fh < 0 ) return;
  for ( int piece = 0; piece < 4; piece++ ) {
    unsigned used = s_wr - s_rd;
    unsigned cap = music_ring_cap <= MUSIC_RING ? music_ring_cap : MUSIC_RING;
    if ( used >= cap ) return;
    unsigned room = cap - used;
    if ( room < 512 && cap > 1024 ) return;        // wait for a worthwhile piece
    if ( s_fpos >= s_fsize ) {
      if ( !s_loop ) { close_file( ); return; }
      lseek( s_fh, 8, SEEK_SET );
      s_fpos = 8;
    }
    unsigned off = s_wr & s_mask;
    unsigned n = room;
    if ( n > MUSIC_RING - off ) n = MUSIC_RING - off;
    if ( n > 4096 ) n = 4096;
    if ( (long)n > s_fsize - s_fpos ) n = (unsigned)( s_fsize - s_fpos );
    int got = read( s_fh, s_buf + off, n );
    if ( got <= 0 ) { close_file( ); return; }    // the handler pauses at the gap: never plays garbage
    s_fpos += got;
    s_wr += got;                                   // one word write: the handler sees all of it or none
  }
}

void music_stop( void ) {
  if ( !s_old08 ) { close_file( ); return; }
  _disable( );
  s_state = 0;
  outp( 0x43, 0x36 );                              // PIT channel 0: mode 3, divisor 65536 (18.2 Hz)
  outp( 0x40, 0 );
  outp( 0x40, 0 );
  set08( s_old08 );
  s_old08 = 0;
  _enable( );
  if ( s_spk ) {
    outp( 0x61, ( inp( 0x61 ) & ~0x63 ) | ( s_old61 & 0x60 ) );   // gate off; Tandy multiplexer as it was
    s_cnt = 0;
  }
  else {
    mute_all( );
    if ( s_set61 ) { outp( 0x61, ( inp( 0x61 ) & ~0x60 ) | ( s_old61 & 0x60 ) ); s_set61 = 0; }
  }
  if ( s_old23 ) { _dos_setvect( 0x23, s_old23 ); s_old23 = 0; }
  close_file( );
  if ( s_seg ) { _dos_freemem( s_seg ); s_seg = 0; s_buf = 0; }
  s_path[0] = 0;
  if ( music_owner_hook ) music_owner_hook( 0 );
}

void music_done( void ) { music_stop( ); }

void music_release( const char *path ) {
  if ( s_old08 && s_path[0] && !stricmp( s_path, path ) ) music_stop( );
}

int music_play( const char *path, int loop ) {
  music_stop( );
  int spk = chosen( ) == MUSIC_SPEAKER;
  if ( !spk && !is_tandy( ) ) return MUS_NO_CHIP;
  int h = open( path, O_RDONLY | O_BINARY );
  if ( h < 0 ) return MUS_NO_FILE;
  long size = filelength( h );
  unsigned char hdr[8];
  if ( size < 11 || read( h, hdr, 8 ) != 8 || memcmp( hdr, "T3P1", 4 ) ) { close( h ); return MUS_BAD; }
  unsigned d = *(unsigned *)( hdr + 4 ), e = *(unsigned *)( hdr + 6 ), last = 0;
  int stream = size > (long)MUSIC_MAX;
  if ( size > 4000000L ) { close( h ); return MUS_TOO_BIG; }
  int rc = check_file( h, size, d, &last );
  if ( rc ) { close( h ); return rc; }
  unsigned paras = stream ? MUSIC_RING >> 4 : (unsigned)( ( size + 15 ) >> 4 );
  if ( _dos_allocmem( paras, &s_seg ) ) { s_seg = 0; close( h ); return MUS_NO_MEM; }
  s_buf = (unsigned char far *)MK_FP( s_seg, 0 );
  s_div = d;
  s_endTick = e < last ? last : e;
  s_stream = (unsigned char)stream;
  if ( stream ) {
    s_mask = MUSIC_RING - 1;
    s_rd = s_wr = 0;
    s_fh = h;
    s_fsize = size;
    lseek( h, 8, SEEK_SET );
    s_fpos = 8;
    s_loop = (unsigned char)( loop != 0 );
    for ( int k = 0; k < 4; k++ ) refill( );       // fill the ring before the first tick
  }
  else {
    s_mask = 0xFFFF;
    lseek( h, 0, SEEK_SET );
    int bad = read( h, s_buf, (unsigned)size ) != (int)size;
    close( h );
    if ( bad ) { _dos_freemem( s_seg ); s_seg = 0; s_buf = 0; return MUS_NO_FILE; }
    s_rd = 8;
    s_wr = (unsigned)size;
  }
  strncpy( s_path, path, sizeof( s_path ) - 1 );
  s_path[ sizeof( s_path ) - 1 ] = 0;
  if ( !s_atexit ) { atexit( music_done ); s_atexit = 1; }

  if ( music_owner_hook ) music_owner_hook( 1 );   // sound effects step aside (and go quiet)
  s_spk = (unsigned char)spk;
  s_mult = spk ? MUSIC_SPK_MULT : 1;
  s_sub = (unsigned char)( s_mult - 1 );           // the first interrupt plays tick 0
  s_pitDiv = spk ? s_div / MUSIC_SPK_MULT : s_div;
  s_old61 = inp( 0x61 );
  if ( spk ) {
    for ( int c = 0; c < 3; c++ ) s_sdiv[c] = 1023;
    s_satt[0] = s_satt[1] = s_satt[2] = s_satt[3] = 15;
    s_latch = s_hit = s_drum = s_ph3 = 0;
    s_n = s_k3 = s_cnt = 0;
    s_lfsr = 0xACE1u;
    // Gate off; on a Tandy the sound multiplexer (bits 5-6) to the timer.  Bits 5-6 are left alone
    // elsewhere: on an XT they are keyboard and parity controls.
    unsigned char v = s_old61 & ~3;
    if ( is_tandy( ) ) v &= ~0x60;
    outp( 0x61, v );
    outp( 0x43, 0xB6 );                            // PIT channel 2: mode 3, square wave
  }
  else {
    s_set61 = 0;
    if ( ( s_old61 & 0x60 ) != 0x60 ) { outp( 0x61, s_old61 | 0x60 ); s_set61 = 1; }   // chip to the speaker
    mute_all( );
  }
  s_old23 = _dos_getvect( 0x23 );
  _dos_setvect( 0x23, break23 );
  s_tick = 0;
  s_frac = 0;
  s_stall = 0;
  s_under = 0;
  if ( !stream ) s_loop = (unsigned char)( loop != 0 );
  _disable( );
  s_old08 = get08( );
  set08( irq0 );
  outp( 0x43, 0x34 );                              // PIT channel 0: mode 2 at the song's rate
  outp( 0x40, s_pitDiv & 0xFF );
  outp( 0x40, s_pitDiv >> 8 );
  s_state = 1;
  _enable( );
  return MUS_OK;
}

int music_playing( void ) { return s_old08 != 0; }

int music_poll( void ) {
  if ( !s_old08 ) return 0;
  if ( s_state == 2 ) { music_stop( ); return 1; }
  refill( );
  return 0;
}

unsigned music_length( void ) { return s_old08 ? secs( s_endTick, s_div ) : 0; }

unsigned music_position( void ) {
  if ( !s_old08 ) return 0;
  unsigned t = s_tick;
  if ( t > s_endTick ) t = s_endTick;
  return secs( t, s_div );
}
