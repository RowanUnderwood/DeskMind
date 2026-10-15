// MUSTEST - unattended checks of music.cpp (the IRQ0 song player).
//
//   MUSTEST song.T3 [/S seconds] [/STALL seconds] [/SPK reference.SPK]
//
// 1. Plays the song (or its first n seconds with /S) with the write log on, then compares
//    every chip write and the song tick it happened on with the file's schedule.  A song over
//    32,000 bytes streams from the file; the schedule is read from the file in pieces too.
// 2. BIOS clock: ticks counted during playback against the song's own length.
// 3. INT 08h vector back after the song, after a stop half-way, and after a damaged file.
// /STALL n: the ring is only filled 1 KB deep and the main loop stops refilling for n seconds at
//    a time (a streamed song runs dry, pauses and goes on); the log must still match the schedule.
// /SPK ref: plays on the PC speaker and compares each sub-tick's PIT count with ref, made by
//    Labtext2midi\worker\t3.py:  python t3.py speaker song.T3 song.SPK  (u16 counts)
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

// The file's records, read in order through a small buffer
struct Reader {
  int h;
  unsigned char buf[ 1024 ];
  unsigned have, at;
};
static int rd_byte( Reader *r ) {
  if ( r->at == r->have ) {
    int got = read( r->h, r->buf, sizeof( r->buf ) );
    if ( got <= 0 ) return -1;
    r->have = got;
    r->at = 0;
  }
  return r->buf[ r->at++ ];
}
// Next record: 1 with tick, count and bytes; 0 at the end record (or a read error)
static int rd_record( Reader *r, unsigned *tick, unsigned *n, unsigned char *bytes ) {
  int a = rd_byte( r ), b = rd_byte( r ), c = rd_byte( r );
  if ( a < 0 || b < 0 || c < 0 ) return 0;
  *tick = a | ( b << 8 );
  *n = c;
  if ( *tick == 0xFFFF ) return 0;
  for ( unsigned k = 0; k < *n; k++ ) { int x = rd_byte( r ); if ( x < 0 ) return 0; bytes[k] = (unsigned char)x; }
  return 1;
}
static int rd_open( Reader *r, const char *path ) {
  r->h = open( path, O_RDONLY | O_BINARY );
  r->have = r->at = 0;
  if ( r->h < 0 ) return 1;
  lseek( r->h, 8, SEEK_SET );
  return 0;
}

int main( int argc, char *argv[] ) {
  if ( argc < 2 ) { printf( "MUSTEST song.T3 [/S seconds] [/STALL seconds] [/SPK reference.SPK]\n" ); return 1; }
  const char *path = argv[1], *spkRef = 0;
  unsigned limit = 0, stall = 0;
  for ( int i = 2; i < argc; i++ ) {
    if ( str_ieq( argv[i], "/S" ) && i + 1 < argc ) limit = atoi( argv[++i] );
    else if ( str_ieq( argv[i], "/STALL" ) && i + 1 < argc ) stall = atoi( argv[++i] );
    else if ( str_ieq( argv[i], "/SPK" ) && i + 1 < argc ) spkRef = argv[++i];
  }
  s_log = fopen( "MUSTEST.LOG", "a" );
  if ( s_log ) fprintf( s_log, "--- MUSTEST %s%s%s%s\n", path, limit ? " /S" : "", stall ? " /STALL" : "", spkRef ? " /SPK" : "" );
  if ( spkRef ) music_set_output( MUSIC_SPEAKER );
  if ( stall ) music_ring_cap = 1024;              // about 3 seconds of a busy song: a stall runs it dry

  // The expected schedule, straight from the file
  int h = open( path, O_RDONLY | O_BINARY );
  if ( h < 0 ) { say( 0, "cannot open %s", path ); return 1; }
  long size = filelength( h );
  unsigned char hdr[8];
  read( h, hdr, 8 );
  close( h );
  unsigned div = *(unsigned *)( hdr + 4 ), endTick = *(unsigned *)( hdr + 6 );
  unsigned long writes = 0;
  static unsigned char bytes[ 32 ];
  Reader rr;
  unsigned t, n;
  if ( rd_open( &rr, path ) ) { say( 0, "cannot read %s", path ); return 1; }
  while ( rd_record( &rr, &t, &n, bytes ) ) writes += n;
  close( rr.h );
  double rate = 1193182.0 / div;
  printf( "%s: %ld bytes (%s), divisor %u (%.3f Hz), %u ticks (%.1f s), %lu chip writes, %s\n",
          path, size, size > (long)MUSIC_MAX ? "streamed" : "in memory", div, rate, endTick, endTick / rate, writes,
          spkRef ? "PC speaker" : "Tandy chip" );

  unsigned lseg, lmax = 65520u;
  if ( !spkRef && writes * 3 < lmax ) lmax = (unsigned)( writes * 3 );
  if ( _dos_allocmem( ( lmax + 15 ) >> 4, &lseg ) ) { say( 0, "no memory for the log" ); return 1; }
  music_log = (unsigned char far *)MK_FP( lseg, 0 );
  music_log_max = lmax;
  music_log_n = 0;

  // 1 + 2: play
  Vec before = vec08( );
  unsigned long t0 = ticks( ), lastStall = ticks( );
  int rc = music_play( path, 0 );
  say( rc == MUS_OK, "music_play: %s (%s)", music_error( rc ), music_streaming( ) ? "streaming" : "from memory" );
  if ( rc ) return 1;
  say( vec08( ) != before, "IRQ0 hooked while playing" );
  int esc = 0, stopped = 0;
  for ( ;; ) {
    if ( stall && ticks( ) - lastStall >= 2 * 182ul / 10 ) {    // every 2 s: no refills for /STALL seconds
      unsigned long s0 = ticks( );
      while ( ticks( ) - s0 < stall * 182ul / 10 ) ;
      lastStall = ticks( );
    }
    if ( music_poll( ) ) break;
    if ( kbd_get( ) == K_ESC ) { esc = 1; music_stop( ); break; }
    if ( limit && ticks( ) - t0 >= limit * 182ul / 10 ) { stopped = 1; music_stop( ); break; }
  }
  unsigned long dt = ticks( ) - t0;
  unsigned under = music_underruns( );
  music_log = 0;
  say( !esc, "played to the end or the /S limit" );
  say( vec08( ) == before, "IRQ0 vector restored" );
  printf( "  underruns: %u\n", under );
  if ( s_log ) fprintf( s_log, "  underruns: %u\n", under );
  if ( stall ) say( under > 0, "/STALL made the stream run dry (%u times) and it went on", under );
  else if ( size > (long)MUSIC_MAX ) say( under == 0, "no underruns while the main loop kept polling" );

  unsigned char far *lg = (unsigned char far *)MK_FP( lseg, 0 );
  unsigned ln = music_log_n;
  if ( spkRef ) {
    // Speaker: every sub-tick's count against the reference
    int rh = open( spkRef, O_RDONLY | O_BINARY );
    if ( rh < 0 ) { say( 0, "cannot open %s", spkRef ); return 1; }
    static unsigned char ref[ 1024 ];
    unsigned k = 0, bad = 0, got = 0;
    while ( k < ln && ( got = read( rh, ref, sizeof( ref ) ) ) > 0 ) {
      for ( unsigned j = 0; j + 1 < got && k < ln; j += 2, k += 2 ) {
        unsigned want = ref[j] | ( ref[j+1] << 8 ), have = lg[k] | ( lg[k+1] << 8 );
        if ( want != have ) {
          if ( bad < 5 ) printf( "  sub-tick %u: count %u, expected %u\n", k / 2, have, want );
          bad++;
        }
      }
    }
    close( rh );
    say( bad == 0 && ln > 0, "%u speaker sub-ticks match the reference%s", ln / 2, ln >= lmax - 1 ? " (log full)" : "" );
  }
  else {
    // Chip: the write log against the schedule (up to where it stopped or the log filled)
    unsigned k = 0, bad = 0, lastTick = 0;
    rd_open( &rr, path );
    while ( k < ln && rd_record( &rr, &t, &n, bytes ) ) {
      for ( unsigned j = 0; j < n && k + 2 < ln; j++, k += 3 ) {
        unsigned lt = lg[k] | ( lg[k+1] << 8 );
        if ( lt != t || lg[k+2] != bytes[j] ) {
          if ( bad < 5 ) printf( "  write %u: tick %u byte %02X, expected tick %u byte %02X\n", k / 3, lt, lg[k+2], t, bytes[j] );
          bad++;
        }
        lastTick = t;
      }
    }
    close( rr.h );
    int full = (unsigned long)ln / 3 < writes && ln + 3 > lmax;
    if ( stopped || full ) say( bad == 0 && ln > 0, "%u writes up to tick %u match the file (%s)", ln / 3, lastTick,
                                full ? "log full" : "stopped by /S" );
    else say( bad == 0 && ln / 3 == writes, "%u of %lu writes, every one on its tick", ln / 3, writes );
  }

  // BIOS clock: the BIOS handler must still run at 18.2 Hz on average (not with stalls: the song pauses)
  if ( !stall ) {
    double expect = ( stopped ? limit : endTick / rate ) * 18.2065;
    say( !esc && dt + 2 >= expect && dt <= expect + 3, "BIOS ticks during playback %lu, expected about %.0f", dt, expect );
  }

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
