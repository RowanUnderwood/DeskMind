// SNDTEST - hunts the stuck-note bug on the real TL/3 (DeskMind 0.8.x).
//
//   SNDTEST [/D] [/N] [/Rn] [server-ip [port]]   (with an IP, N can load the network; port 8286)
//   /D and /N start with that load on; /Rn quits after n rounds (unattended tests).
//
// Plays DeskMind's own sound effects in random order with random gaps of 0-3 ticks, so
// new sounds often cut into running ones, as in real use.  After every 40 sounds it
// pauses for 1.5 s: any tone heard during a pause is a stuck note.  The loads keep
// running during the pauses.
//
//   0/1/2  chip writes: 0 = old (back to back), 1 = paced, 2 = paced + volume refresh
//   D      disk load: writes and deletes a temporary file and reads picture headers
//          (C:\DESKMIND\PICS), like a Gallery rescan
//   N      network load: back-to-back 1 MB downloads from MindServer's /test/bytes
//   Esc    quit (writes SNDTEST.LOG: the sound state and the last events)
//
// "miss" counts BIOS ticks that never reached our INT 1Ch handler.  Text mode only.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <io.h>
#include <dos.h>

#include "sound.h"
#include "sys.h"
#include "net.h"

static const SndNote *const s_fx[] = { SND_CLICK, SND_REPLY, SND_IMAGE, SND_ERROR, SND_STARTUP };
static int s_disk = 0, s_netOn = 0, s_haveNet = 0, s_quit = 0;
static long s_sounds = 0;
static int s_round = 0, s_maxRounds = 0;
static char s_buf[ 4096 ];
static const char *s_what = "";

// Ticks that never reached the handler: BIOS ticks counted minus handler calls, since the start
static unsigned long s_b0 = 0, s_i0 = 0;
static long missed( void ) {
  long m = (long)( ticks( ) - s_b0 ) - (long)( snd_isr_count - s_i0 );
  return m > 0 ? m : 0;
}

// ---------------------------------------------------------------- network load
static char s_host[40] = "";
static unsigned s_port = 8286;
static int s_conn = 0;                        // 0 none, 1 waiting for headers, 2 reading
static unsigned long s_bytes = 0;
static int s_netErr = 0;

static void net_step( void ) {
  if ( !s_haveNet ) return;
  if ( !s_netOn ) {
    if ( s_conn ) { http_close( ); s_conn = 0; }
    net_poll( );
    return;
  }
  if ( !s_conn ) {
    if ( http_open( s_host, s_port, "GET", "/test/bytes?n=1048576", 0, 0, 0 ) ) { s_netErr++; s_netOn = 0; return; }
    s_conn = 1;
  }
  if ( s_conn == 1 ) {
    int st = http_headers( );
    if ( st == NET_AGAIN ) return;
    if ( st != 200 ) { http_close( ); s_conn = 0; s_netErr++; s_netOn = 0; return; }
    s_conn = 2;
  }
  int n = http_read( s_buf, sizeof( s_buf ) );
  if ( n > 0 ) s_bytes += n;
  else if ( n != NET_AGAIN ) { http_close( ); s_conn = 0; if ( n != NET_DONE ) s_netErr++; }
}

// ---------------------------------------------------------------- disk load
// One small step per call, so the sounds and keys keep going: a 4K write to a temporary
// file (8 of them, then close and delete), then one picture header read.
static int s_tmp = -1, s_writes = 0;
static struct find_t s_ff;
static int s_scanning = 0;

static void disk_step( void ) {
  if ( !s_disk ) return;
  if ( s_writes < 8 ) {
    if ( s_tmp < 0 ) s_tmp = open( "SNDTEST.TMP", O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0666 );
    if ( s_tmp >= 0 ) write( s_tmp, s_buf, sizeof( s_buf ) );
    s_writes++;
    return;
  }
  if ( s_tmp >= 0 ) { close( s_tmp ); s_tmp = -1; remove( "SNDTEST.TMP" ); }
  unsigned rc = s_scanning ? _dos_findnext( &s_ff ) : _dos_findfirst( "C:\\DESKMIND\\PICS\\*.TPI", _A_NORMAL, &s_ff );
  s_scanning = 1;
  if ( rc ) { s_scanning = 0; s_writes = 0; return; }        // folder done (or none): write again
  char p[80];
  sprintf( p, "C:\\DESKMIND\\PICS\\%s", s_ff.name );
  int fd = open( p, O_RDONLY | O_BINARY );
  if ( fd >= 0 ) { read( fd, s_buf, 128 ); close( fd ); }
}

// ---------------------------------------------------------------- screen and keys
static void show( const char *what ) {
  s_what = what;
  // Under 80 columns, or the line wraps and \r no longer rewrites it in place
  printf( "\rM%d D:%-3s N:%-3s r%-4d s%-6ld %6luK miss %-4ld err %-2d %-17s",
          snd_mode, s_disk ? "on" : "off", s_netOn ? "on" : "off", s_round, s_sounds,
          s_bytes >> 10, missed( ), s_netErr, what );
  fflush( stdout );                              // no newline, so stdout would hold part of it back
}

static void keys( void ) {
  int k = kbd_get( );
  if ( !k ) return;
  if ( k == K_ESC ) s_quit = 1;
  else if ( k >= '0' && k <= '2' ) { snd_stop( ); snd_mode = k - '0'; }
  else if ( k == 'd' || k == 'D' ) s_disk = !s_disk;
  else if ( ( k == 'n' || k == 'N' ) && s_haveNet ) { s_netOn = !s_netOn; s_netErr = 0; }
  show( s_what );
}

// Waits n ticks, keeping the loads going and reading keys
static void wait( unsigned n ) {
  unsigned long end = ticks( ) + n, next = ticks( ) + 9;
  while ( ticks( ) < end && !s_quit ) {
    disk_step( );
    net_step( );
    keys( );
    if ( snd_mode >= 2 ) snd_poll( );
    if ( ticks( ) >= next ) { show( s_what ); next = ticks( ) + 9; }
  }
}

int main( int argc, char **argv ) {
  printf( "SNDTEST - stuck-note hunt.  0/1/2 = chip write mode, D = disk load, N = network load,\n"
          "Esc = quit.  Listen during each LISTEN pause: any tone then is a stuck note.\n" );
  int a = 1, netAtStart = 0;
  for ( ; a < argc && argv[a][0] == '/'; a++ ) {
    char c = argv[a][1] & ~0x20;
    if ( c == 'D' ) s_disk = 1;
    else if ( c == 'N' ) netAtStart = 1;
    else if ( c == 'R' ) s_maxRounds = atoi( argv[a] + 2 );
    else if ( c == 'Q' ) snd_enabled = 0;           // no sounds: the loads alone (crash hunting)
  }
  if ( a < argc ) {
    str_copy( s_host, argv[a], sizeof( s_host ) );
    if ( a + 1 < argc ) s_port = (unsigned)atoi( argv[a + 1] );
    // Same order as DeskMind: the TCP/IP stack hooks INT 1Ch before the sound does
    printf( "Starting the network (MindServer %s:%u)...\n", s_host, s_port );
    s_haveNet = ( net_init( ) == 0 );
    if ( !s_haveNet ) printf( "No network: %s.  N is off.\n", net_error( ) );
    else s_netOn = netAtStart;
  }
  else printf( "(No server IP given, so N is off.  Use: SNDTEST 192.168.x.x)\n" );
  printf( "\n" );
  srand( (unsigned)ticks( ) );
  snd_init( );
  s_b0 = ticks( );
  s_i0 = snd_isr_count;
  while ( !s_quit ) {
    s_round++;
    show( "playing" );
    for ( int i = 0; i < 40 && !s_quit; i++ ) {
      snd_play( s_fx[ rand( ) % 5 ] );
      s_sounds++;
      wait( rand( ) % 4 );
    }
    wait( 12 );                                  // let the last sound finish
    show( "LISTEN: quiet now" );
    wait( 27 );
    if ( s_maxRounds && s_round >= s_maxRounds ) s_quit = 1;
  }
  if ( s_tmp >= 0 ) { close( s_tmp ); remove( "SNDTEST.TMP" ); }
  if ( s_conn ) http_close( );
  FILE *f = fopen( "SNDTEST.LOG", "a" );
  if ( f ) {
    fprintf( f, "\n=== SNDTEST: %d rounds, %ld sounds, %luK downloaded, %ld ticks missed ===\n",
             s_round, s_sounds, s_bytes >> 10, missed( ) );
    snd_diag( f );
    fclose( f );
  }
  snd_done( );
  if ( s_haveNet ) net_done( );
  printf( "\nSNDTEST.LOG written.\n" );
  return 0;
}
