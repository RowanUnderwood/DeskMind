// MUSTEST - unattended checks of music.cpp (the IRQ0 song player).
//
//   MUSTEST song.T3 [/S seconds]
//
// 1. Plays the song (or its first n seconds with /S) with the write log on, then compares
//    every chip write and the song tick it happened on with the file's schedule.
// 2. BIOS clock: ticks counted during playback against the song's own length.
// 3. INT 08h vector back after the song, after a stop half-way, and after a damaged file.
// Prints PASS/FAIL lines and appends them to MUSTEST.LOG.  Esc stops early (a FAIL).

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <dos.h>
#include <io.h>
#include <fcntl.h>

#include "music.h"
#include "sys.h"

static FILE *s_log = 0;
static int s_fails = 0;

static void say( int ok, const char *fmt, ... ) {
  char t[200];
  va_list a;
  va_start( a, fmt );
  vsprintf( t, fmt, a );
  va_end( a );
  if ( !ok ) s_fails++;
  printf( "%s %s\n", ok ? "PASS" : "FAIL", t );
  if ( s_log ) fprintf( s_log, "%s %s\n", ok ? "PASS" : "FAIL", t );
}

typedef void (__interrupt __far *Vec)( void );
static Vec vec08( void ) { return *(Vec far *)MK_FP( 0, 8 * 4 ); }

int main( int argc, char *argv[] ) {
  if ( argc < 2 ) { printf( "MUSTEST song.T3 [/S seconds]\n" ); return 1; }
  const char *path = argv[1];
  unsigned limit = 0;
  for ( int i = 2; i < argc; i++ ) if ( str_ieq( argv[i], "/S" ) && i + 1 < argc ) limit = atoi( argv[++i] );
  s_log = fopen( "MUSTEST.LOG", "a" );
  if ( s_log ) fprintf( s_log, "--- MUSTEST %s%s\n", path, limit ? " (limited)" : "" );

  // The expected schedule, straight from the file
  int h = open( path, O_RDONLY | O_BINARY );
  if ( h < 0 ) { say( 0, "cannot open %s", path ); return 1; }
  long size = filelength( h );
  unsigned seg;
  if ( size > (long)MUSIC_MAX || _dos_allocmem( (unsigned)( ( size + 15 ) >> 4 ), &seg ) ) { say( 0, "file too big" ); return 1; }
  unsigned char far *f = (unsigned char far *)MK_FP( seg, 0 );
  read( h, f, (unsigned)size );
  close( h );
  unsigned div = *(unsigned far *)( f + 4 ), endTick = *(unsigned far *)( f + 6 );
  unsigned long writes = 0;
  for ( unsigned pos = 8; *(unsigned far *)( f + pos ) != 0xFFFF; pos += 3 + f[ pos + 2 ] ) writes += f[ pos + 2 ];
  double rate = 1193182.0 / div;
  printf( "%s: %ld bytes, divisor %u (%.3f Hz), %u ticks (%.1f s), %lu chip writes\n",
          path, size, div, rate, endTick, endTick / rate, writes );

  unsigned lseg, lmax = (unsigned)( writes * 3 > 65520ul ? 65520u : writes * 3 );
  if ( _dos_allocmem( ( lmax + 15 ) >> 4, &lseg ) ) { say( 0, "no memory for the log" ); return 1; }
  music_log = (unsigned char far *)MK_FP( lseg, 0 );
  music_log_max = lmax;
  music_log_n = 0;

  // 1 + 2: play
  Vec before = vec08( );
  unsigned long t0 = ticks( );
  int rc = music_play( path, 0 );
  say( rc == MUS_OK, "music_play: %s", music_error( rc ) );
  if ( rc ) return 1;
  say( vec08( ) != before, "IRQ0 hooked while playing" );
  int esc = 0, stopped = 0;
  for ( ;; ) {
    if ( music_poll( ) ) break;
    if ( kbd_get( ) == K_ESC ) { esc = 1; music_stop( ); break; }
    if ( limit && ticks( ) - t0 >= limit * 182ul / 10 ) { stopped = 1; music_stop( ); break; }
  }
  unsigned long dt = ticks( ) - t0;
  music_log = 0;
  say( !esc, "played to the end or the /S limit" );
  say( vec08( ) == before, "IRQ0 vector restored" );

  // Compare the write log with the schedule (up to where it stopped)
  unsigned n = music_log_n, k = 0, bad = 0;
  unsigned char far *lg = (unsigned char far *)MK_FP( lseg, 0 );
  unsigned lastTick = 0;
  for ( unsigned pos = 8; *(unsigned far *)( f + pos ) != 0xFFFF && k < n; pos += 3 + f[ pos + 2 ] ) {
    unsigned t = *(unsigned far *)( f + pos );
    for ( unsigned j = 0; j < f[ pos + 2 ] && k < n; j++, k += 3 ) {
      unsigned lt = lg[k] | ( lg[k+1] << 8 );
      if ( lt != t || lg[k+2] != f[ pos + 3 + j ] ) {
        if ( bad < 5 ) printf( "  write %u: tick %u byte %02X, expected tick %u byte %02X\n", k / 3, lt, lg[k+2], t, f[ pos + 3 + j ] );
        bad++;
      }
      lastTick = t;
    }
  }
  if ( stopped ) say( bad == 0 && n > 0, "%u writes up to tick %u match the file (stopped by /S)", n / 3, lastTick );
  else say( bad == 0 && n / 3 == writes, "%u of %lu writes, every one on its tick", n / 3, writes );

  // BIOS clock: the BIOS handler must still run at 18.2 Hz on average
  double expect = ( stopped ? limit : endTick / rate ) * 18.2065;
  say( !esc && dt + 2 >= expect && dt <= expect + 3, "BIOS ticks during playback %lu, expected about %.0f", dt, expect );

  // 3: stop half-way, damaged file
  rc = music_play( path, 1 );
  wait_ticks( 18 );
  int was = music_playing( );
  music_stop( );
  say( rc == 0 && was && !music_playing( ) && vec08( ) == before, "looping song stopped after 1 s, vector restored" );
  FILE *b = fopen( "MUSBAD.T3", "wb" );
  if ( b ) { fwrite( "T3P1\x2B\x26\x10\x00\x00\x00\x05\x9F", 1, 12, b ); fclose( b ); }
  rc = music_play( "MUSBAD.T3", 0 );
  say( rc == MUS_BAD && vec08( ) == before, "damaged file refused (%s)", music_error( rc ) );
  remove( "MUSBAD.T3" );
  rc = music_play( "NOSUCH.T3", 0 );
  say( rc == MUS_NO_FILE && vec08( ) == before, "missing file refused" );

  printf( "%s\n", s_fails ? "MUSTEST FAILED" : "MUSTEST PASSED" );
  if ( s_log ) { fprintf( s_log, "%s\n", s_fails ? "MUSTEST FAILED" : "MUSTEST PASSED" ); fclose( s_log ); }
  return s_fails ? 1 : 0;
}
