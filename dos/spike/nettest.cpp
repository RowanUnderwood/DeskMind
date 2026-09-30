// NETTEST - DeskMind Phase 0 network test (PicoMEM WiFi + mTCP -> MindServer).
//
//   NETTEST <server-ip> [port]      default port 8286
//
// Boot the Tandy with W (WiFi).  Results go to the screen and NETTEST.LOG.
// Also downloads the test image to TEST640.RAW / TEST320.RAW for V640.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <malloc.h>
#include <dos.h>
#include <conio.h>

#include "net.h"

static FILE *g_log = 0;

static void logf( const char *fmt, ... ) {
  va_list ap;
  va_start( ap, fmt ); vprintf( fmt, ap ); va_end( ap );
  if ( g_log ) { va_start( ap, fmt ); vfprintf( g_log, fmt, ap ); va_end( ap ); fflush( g_log ); _dos_commit( fileno( g_log ) ); }
}

static unsigned long ticks( void ) {
  return *(volatile unsigned long far *)MK_FP( 0x40, 0x6C );
}

static const char *heap_state( void ) {
  switch ( _heapchk( ) ) {
    case _HEAPOK:       return "ok";
    case _HEAPEMPTY:    return "empty";
    case _HEAPBADBEGIN: return "BAD BEGIN";
    case _HEAPBADNODE:  return "BAD NODE";
  }
  return "?";
}

static const char *g_host;
static unsigned    g_port = 8286;

static int test_ping( void ) {
  static char buf[512];
  int status = 0;
  long n = http_get_all( g_host, g_port, "/ping", buf, sizeof( buf ) - 1, &status );
  if ( n < 0 ) { logf( "  FAILED: %s\n", net_error( ) ); return 1; }
  buf[n] = 0;
  logf( "  HTTP %d, %ld bytes:\n%s\n", status, n, buf );
  return status != 200;
}

static int test_speed( char far *buf, unsigned size, int runs ) {
  char path[40];
  sprintf( path, "/test/bytes?n=%u", size );
  unsigned long totalBytes = 0, totalTicks = 0;
  for ( int i = 0; i < runs; i++ ) {
    int status = 0;
    unsigned long t0 = ticks( );
    long n = http_get_all( g_host, g_port, path, buf, size, &status );
    unsigned long t1 = ticks( );
    if ( n < 0 ) { logf( "  run %d FAILED: %s\n", i + 1, net_error( ) ); return 1; }
    // Check the pattern: byte k = k & 255
    unsigned bad = 0;
    for ( unsigned k = 0; k < (unsigned)n; k++ ) if ( (unsigned char)buf[k] != ( k & 255 ) ) bad++;
    logf( "  run %d: %ld bytes in %lu ms%s\n", i + 1, n, ( t1 - t0 ) * 55ul, bad ? "  DATA ERRORS!" : "" );
    totalBytes += n; totalTicks += ( t1 - t0 );
  }
  if ( totalTicks == 0 ) totalTicks = 1;
  logf( "  Average: %lu bytes/s (%lu KB/s)\n", totalBytes * 182ul / ( totalTicks * 10ul ),
        totalBytes * 182ul / ( totalTicks * 10ul ) / 1024ul );
  return 0;
}

static int test_stream( void ) {
  // Simulates a chat reply: the server sends coded lines with pauses
  if ( http_open( g_host, g_port, "GET", "/test/stream", 0, 0, 0 ) ) {
    logf( "  FAILED: %s\n", net_error( ) ); return 1;
  }
  int st;
  while ( ( st = http_headers( ) ) == NET_AGAIN ) ;
  if ( st != 200 ) { logf( "  HTTP status %d %s\n", st, st < 0 ? net_error( ) : "" ); http_close( ); return 1; }
  unsigned long t0 = ticks( );
  char line[200];
  int lines = 0;
  for ( ;; ) {
    int rc = http_line( line, sizeof( line ) );
    if ( rc == 1 ) {
      lines++;
      logf( "  +%4lu ms  %s\n", ( ticks( ) - t0 ) * 55ul, line );
    }
    else if ( rc == NET_AGAIN ) continue;
    else if ( rc == NET_DONE ) break;
    else { logf( "  FAILED: %s\n", net_error( ) ); http_close( ); return 1; }
  }
  http_close( );
  logf( "  %d lines streamed\n", lines );
  return 0;
}

static int test_echo( void ) {
  static char body[2000];
  for ( unsigned i = 0; i < sizeof( body ); i++ ) body[i] = (char)( 'A' + i % 26 );
  if ( http_open( g_host, g_port, "POST", "/test/echo", body, sizeof( body ), "text/plain" ) ) {
    logf( "  FAILED: %s\n", net_error( ) ); return 1;
  }
  int st;
  while ( ( st = http_headers( ) ) == NET_AGAIN ) ;
  static char back[2100];
  unsigned got = 0;
  for ( ;; ) {
    int rc = http_read( back + got, sizeof( back ) - got );
    if ( rc > 0 ) got += rc;
    else if ( rc == NET_AGAIN ) continue;
    else break;
  }
  http_close( );
  int same = ( got == sizeof( body ) ) && !memcmp( back, body, got );
  logf( "  HTTP %d, sent %u, got %u back: %s\n", st, (unsigned)sizeof( body ), got, same ? "match" : "MISMATCH" );
  return !same;
}

static int test_image( char far *buf, const char *path, const char *file, unsigned size ) {
  int status = 0;
  unsigned long t0 = ticks( );
  long n = http_get_all( g_host, g_port, path, buf, size, &status );
  unsigned long t1 = ticks( );
  if ( n < 0 ) { logf( "  FAILED: %s\n", net_error( ) ); return 1; }
  if ( status != 200 || n != (long)size ) { logf( "  HTTP %d, %ld bytes (expected %u)\n", status, n, size ); return 1; }
  FILE *f = fopen( file, "wb" );
  if ( !f ) { logf( "  Could not write %s\n", file ); return 1; }
  static char tmp[1024];
  for ( unsigned off = 0; off < size; off += sizeof( tmp ) ) {
    unsigned k = size - off < sizeof( tmp ) ? size - off : sizeof( tmp );
    _fmemcpy( tmp, buf + off, k );
    fwrite( tmp, 1, k, f );
  }
  fclose( f );
  logf( "  %u bytes in %lu ms -> %s\n", size, ( t1 - t0 ) * 55ul, file );
  return 0;
}

int main( int argc, char *argv[] ) {
  if ( argc < 2 ) {
    printf( "Usage: NETTEST <server-ip> [port]\n" );
    return 1;
  }
  g_host = argv[1];
  if ( argc > 2 ) g_port = (unsigned)atoi( argv[2] );

  g_log = fopen( "NETTEST.LOG", "a" );
  logf( "\n=== DeskMind NETTEST -> %s:%u ===\n", g_host, g_port );

  logf( "MTCPCFG=%s\n", getenv( "MTCPCFG" ) ? getenv( "MTCPCFG" ) : "(not set)" );
  logf( "Starting mTCP (reads MTCPCFG, hooks the packet driver)...\n" );
  net_verbose = 1;
  if ( net_init( ) ) { logf( "Network: %s\n", net_error( ) ); return 1; }
  logf( "mTCP started.\n" );
  char ip[20]; net_my_ip( ip );
  logf( "My IP address: %s\n", ip );

  char far *buf = (char far *)_fmalloc( 64000u );
  if ( !buf ) { logf( "Out of memory\n" ); net_done( ); return 1; }

  int fails = 0;
  logf( "[heap %s]\n", heap_state( ) );
  logf( "1. Ping\n" );                 fails += test_ping( );
  logf( "[heap %s]\n", heap_state( ) );
  logf( "2. Download speed (64000 bytes x 3)\n" ); fails += test_speed( buf, 64000u, 3 );
  logf( "[heap %s]\n", heap_state( ) );
  logf( "3. Streamed reply (like a chat answer)\n" ); fails += test_stream( );
  logf( "[heap %s]\n", heap_state( ) );
  logf( "4. Upload + echo (2000 bytes)\n" ); fails += test_echo( );
  logf( "[heap %s]\n", heap_state( ) );
  logf( "5. Test images from the dither pipeline\n" );
  fails += test_image( buf, "/test/image?mode=640", "TEST640.RAW", 64000u );
  fails += test_image( buf, "/test/image?mode=320", "TEST320.RAW", 32000u );
  logf( "[heap %s]\n", heap_state( ) );

  logf( "\n%s (%d failed)\n", fails ? "SOME TESTS FAILED" : "ALL TESTS PASSED", fails );
  _ffree( buf );
  logf( "[heap %s after free]\n", heap_state( ) );
  logf( "Stopping mTCP...\n" );
  net_done( );
  logf( "mTCP stopped. Closing the log...\n" );
  if ( g_log ) fclose( g_log );
  printf( "Done.\n" );
  return fails ? 1 : 0;
}
