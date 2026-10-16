// DeskMind - HTTP/1.0 client over mTCP.  See net.h.
// Socket handling follows mTCP's HTGET (GPLv3, Michael B. Brutman).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <bios.h>
#include <conio.h>
#include <dos.h>

#include "types.h"
#include "timer.h"
#include "trace.h"
#include "utils.h"
#include "packet.h"
#include "arp.h"
#include "tcp.h"
#include "tcpsockm.h"
#include "udp.h"
#include "dns.h"

#include "net.h"

unsigned long net_connect_ms = 10000ul;
void ( *net_wait_hook )( void ) = 0;
void ( *net_event_hook )( int ) = 0;
#define RECV_BUFFER        8192

unsigned long net_timeout_ms = 30000ul;
int           net_verbose = 0;
char          net_token[40] = "";
char          net_log_path[80] = "";

static volatile uint8_t s_break = 0;
static int         s_up = 0;
static TcpSocket  *s_sock = 0;
static char        s_err[100] = "";

static char        s_buf[1024];        // staging buffer (headers, lines)
static unsigned    s_len = 0, s_pos = 0;
static int         s_hdrDone = 0;
static int         s_status = 0;
static long        s_clen = -1;
static clockTicks_t s_last;

static void __interrupt __far break_handler( void ) { s_break = 1; }

static void set_err( const char *msg ) {
  strncpy( s_err, msg, sizeof( s_err ) - 1 );
  s_err[ sizeof( s_err ) - 1 ] = 0;
}

// Local port for each new connection.  It starts at a random point (BIOS ticks plus the
// PIT counter) and counts up, so no port repeats within a run and runs don't repeat each
// other.  rand() was never seeded, so every run used the same ports in the same order,
// and a restart could reuse a port the PC still held from a closed connection.
static uint16_t next_port( void ) {
  static uint16_t port = 0;
  if ( !port ) {
    outp( 0x43, 0 );
    unsigned pit = inp( 0x40 ); pit |= inp( 0x40 ) << 8;
    unsigned tick = *(unsigned far *)MK_FP( 0x40, 0x6C );
    port = 2048 + (uint16_t)( ( tick ^ pit ) % 30000u );
  }
  if ( ++port >= 32048u ) port = 2048;
  return port;
}

const char *net_error( void ) { return s_err; }

// One line per network failure in net_log_path (when set): the time, the error, what the
// request was, and the stack's counters.  Real-TL/3 WiFi drops are intermittent; this is
// what tells a lost SYN, a reset from the PC and a full receive buffer apart afterwards.
static char s_what[60] = "";             // "GET /music/job/12?wait=1"

static void net_log( const char *extra ) {
  if ( !net_log_path[0] ) return;
  FILE *f = fopen( net_log_path, "a" );
  if ( !f ) return;
  struct dosdate_t d; struct dostime_t t;
  _dos_getdate( &d ); _dos_gettime( &t );
  fprintf( f, "%04u-%02u-%02u %02u:%02u:%02u %s | %s | %s | rx %lu drop %lu retx %lu lowfree %u\n",
           d.year, d.month, d.day, t.hour, t.minute, t.second, s_err, s_what, extra ? extra : "",
           Packets_received, Packets_dropped, Tcp::Packets_Retransmitted, (unsigned)Buffer_lowFreeCount );
  fclose( f );
}

static void fail( const char *msg, const char *extra ) {
  set_err( msg );
  net_log( extra );
}

// The socket went to CLOSED under us: say who ended it.
static void reset_err( void ) {
  uint8_t why = s_sock ? s_sock->getCloseReason( ) : 0;
  if ( why == 1 )      fail( "Connection reset by the PC", "rst" );
  else if ( why == 4 ) fail( "The network dropped the connection", "no ack after resends" );
  else                 fail( "Connection reset", 0 );
}

int net_init( void ) {
  if ( s_up ) return 0;
  if ( net_verbose ) { printf( "  reading mTCP settings...\n" ); fflush( stdout ); }
  if ( Utils::parseEnv( ) != 0 ) {
    set_err( "mTCP settings not found (MTCPCFG). Boot with W for WiFi." );
    return NET_ERROR;
  }
  if ( net_verbose ) { printf( "  starting the TCP/IP stack (packet driver int 0x%02X)...\n", Utils::getPacketInt( ) ); fflush( stdout ); }
  if ( Utils::initStack( 1, TCP_SOCKET_RING_SIZE, break_handler, break_handler ) ) {
    set_err( "No packet driver found. Boot with W for WiFi." );
    return NET_ERROR;
  }
  s_up = 1;
  return 0;
}

void net_done( void ) {
  if ( !s_up ) return;
  if ( s_sock ) http_close( );
  Utils::endStack( );
  s_up = 0;
}

void net_my_ip( char *out ) {
  sprintf( out, "%d.%d.%d.%d", MyIpAddr[0], MyIpAddr[1], MyIpAddr[2], MyIpAddr[3] );
}

void net_poll( void ) {
  if ( !s_up ) return;
  PACKET_PROCESS_MULT( 5 );
  Arp::driveArp( );
  Tcp::drivePackets( );
  if ( net_wait_hook ) net_wait_hook( );
}

static int esc_pressed( void ) {
  if ( _bios_keybrd( _KEYBRD_READY ) ) {
    unsigned k = _bios_keybrd( _KEYBRD_READ );
    if ( ( k & 0xFF ) == 27 ) return 1;
  }
  return 0;
}


// ---------------------------------------------------------------- resolve

static int parse_ip( const char *s, IpAddr_t addr ) {
  unsigned a, b, c, d;
  char tail;
  if ( sscanf( s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail ) != 4 ) return 0;
  if ( a > 255 || b > 255 || c > 255 || d > 255 ) return 0;
  addr[0] = a; addr[1] = b; addr[2] = c; addr[3] = d;
  return 1;
}

static int resolve( const char *host, IpAddr_t addr ) {
  if ( parse_ip( host, addr ) ) return 0;
  char name[80];
  strncpy( name, host, sizeof( name ) - 1 ); name[ sizeof( name ) - 1 ] = 0;
  if ( Dns::resolve( name, addr, 1 ) < 0 ) { set_err( "DNS error" ); return NET_ERROR; }
  clockTicks_t start = TIMER_GET_CURRENT( );
  while ( Dns::isQueryPending( ) ) {
    net_poll( );
    Dns::drivePendingQuery( );
    if ( Timer_diff( start, TIMER_GET_CURRENT( ) ) > TIMER_MS_TO_TICKS( 10000ul ) ) break;
  }
  if ( Dns::resolve( name, addr, 0 ) != 0 ) { set_err( "Could not resolve the server name" ); return NET_ERROR; }
  return 0;
}


// ---------------------------------------------------------------- request

static int send_all( const char far *data, unsigned len ) {
  unsigned sent = 0;
  clockTicks_t start = TIMER_GET_CURRENT( );
  while ( sent < len ) {
    net_poll( );
    int16_t rc = s_sock->send( (uint8_t *)( data + sent ), len - sent );
    if ( rc > 0 ) { sent += rc; start = TIMER_GET_CURRENT( ); }
    else if ( rc < 0 ) { if ( s_sock->isClosed( ) ) reset_err( ); else fail( "Send failed", 0 ); return NET_ERROR; }
    if ( Timer_diff( start, TIMER_GET_CURRENT( ) ) > TIMER_MS_TO_TICKS( net_timeout_ms ) ) {
      fail( "Send timed out", 0 ); return NET_TIMEOUT;
    }
  }
  return 0;
}

// Connect s_sock to addr:port.  mTCP starts every socket at its maximum round-trip estimate
// (10 s), so it resends a lost SYN only after 10 s: one dropped packet on the WiFi used to
// fail the whole connect.  Instead a new attempt (new socket, new port) starts every 3 s
// (a third of net_connect_ms when that is shorter) until net_connect_ms runs out.  A refused
// attempt (reset: nothing listens on the port) is tried once more after half a second, then
// it fails at once, as before: MindServer is simply not running.
#define ATTEMPT_MS 3000ul

static int tcp_connect( const char *host, IpAddr_t addr, unsigned port ) {
  clockTicks_t start = TIMER_GET_CURRENT( );
  unsigned long attempt = net_connect_ms / 3 < ATTEMPT_MS ? net_connect_ms / 3 : ATTEMPT_MS;
  int tries = 0, refused = 0;
  for ( ;; ) {
    s_sock = TcpSocketMgr::getSocket( );
    if ( !s_sock ) { fail( "No free socket", 0 ); return NET_ERROR; }
    if ( s_sock->setRecvBuffer( RECV_BUFFER ) ) {
      TcpSocketMgr::freeSocket( s_sock ); s_sock = 0;
      fail( "Out of memory for the socket", 0 );
      return NET_ERROR;
    }
    tries++;
    if ( s_sock->connectNonBlocking( next_port( ), addr, port ) ) {
      TcpSocketMgr::freeSocket( s_sock ); s_sock = 0;
      fail( "Connect failed", 0 );
      return NET_ERROR;
    }
    clockTicks_t at = TIMER_GET_CURRENT( );
    int closed = 0;
    for ( ;; ) {
      net_poll( );
      if ( s_sock->isConnectComplete( ) ) {
        if ( tries > 1 ) {
          char x[40];
          sprintf( x, "connected on try %d", tries );
          set_err( "(recovered)" );
          net_log( x );
        }
        return 0;
      }
      if ( s_sock->isClosed( ) ) { closed = 1; break; }
      if ( Timer_diff( at, TIMER_GET_CURRENT( ) ) > TIMER_MS_TO_TICKS( attempt ) ) break;
      if ( Timer_diff( start, TIMER_GET_CURRENT( ) ) > TIMER_MS_TO_TICKS( net_connect_ms ) ) break;
    }
    refused += closed;
    http_close( );
    if ( refused >= 2 ) break;
    if ( Timer_diff( start, TIMER_GET_CURRENT( ) ) > TIMER_MS_TO_TICKS( net_connect_ms ) ) break;
    if ( closed ) {
      clockTicks_t w = TIMER_GET_CURRENT( );
      while ( Timer_diff( w, TIMER_GET_CURRENT( ) ) < TIMER_MS_TO_TICKS( 500ul ) ) net_poll( );
    }
  }
  sprintf( s_err, "No answer from %s:%u - is MindServer running?", host, port );
  char x[40];
  sprintf( x, "%d tries, %d refused", tries, refused );
  net_log( x );
  return NET_ERROR;
}

int http_open( const char *host, unsigned port, const char *method, const char *path,
               const char far *body, unsigned bodyLen, const char *contentType ) {
  if ( !s_up ) { set_err( "Network not started" ); return NET_ERROR; }
  sprintf( s_what, "%.6s %.50s", method, path );
  if ( s_sock ) http_close( );
  if ( net_event_hook ) net_event_hook( 1 );

  IpAddr_t addr;
  if ( resolve( host, addr ) ) return NET_ERROR;

  if ( tcp_connect( host, addr, port ) ) return NET_ERROR;

  char req[400];
  int n = sprintf( req, "%s %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: DeskMind/0.1\r\nConnection: close\r\n",
                   method, path, host );
  if ( net_token[0] ) n += sprintf( req + n, "X-Token: %s\r\n", net_token );
  if ( body ) {
    n += sprintf( req + n, "Content-Type: %s\r\nContent-Length: %u\r\n",
                  contentType ? contentType : "text/plain", bodyLen );
  }
  n += sprintf( req + n, "\r\n" );
  if ( send_all( req, n ) ) { http_close( ); return NET_ERROR; }
  if ( body && bodyLen ) {
    if ( send_all( body, bodyLen ) ) { http_close( ); return NET_ERROR; }
  }

  s_len = s_pos = 0;
  s_hdrDone = 0; s_status = 0; s_clen = -1;
  s_last = TIMER_GET_CURRENT( );
  return 0;
}


// ---------------------------------------------------------------- response

// Pull what the socket has into the staging buffer.  Returns bytes added,
// 0 if none, NET_ERROR on a socket error.
static int fill( void ) {
  if ( s_pos > 0 && s_pos == s_len ) s_pos = s_len = 0;
  if ( s_pos > 0 && s_len == sizeof( s_buf ) ) {
    memmove( s_buf, s_buf + s_pos, s_len - s_pos );
    s_len -= s_pos; s_pos = 0;
  }
  if ( s_len == sizeof( s_buf ) ) return 0;
  net_poll( );
  int16_t rc = s_sock->recv( (uint8_t *)( s_buf + s_len ), sizeof( s_buf ) - s_len );
  if ( rc < 0 ) { reset_err( ); return NET_ERROR; }
  if ( rc > 0 ) { s_len += rc; s_last = TIMER_GET_CURRENT( ); }
  return rc;
}

static int idle_state( void ) {
  if ( s_sock->isRemoteClosed( ) && !s_sock->recvDataWaiting( ) ) return NET_DONE;
  if ( Timer_diff( s_last, TIMER_GET_CURRENT( ) ) > TIMER_MS_TO_TICKS( net_timeout_ms ) ) {
    char x[40];
    sprintf( x, "silent %lu s, %u bytes buffered", net_timeout_ms / 1000ul, s_len );
    fail( "The server stopped answering", x );
    return NET_TIMEOUT;
  }
  return NET_AGAIN;
}

int http_headers( void ) {
  if ( !s_sock ) return NET_ERROR;
  if ( s_hdrDone ) return s_status;
  int rc = fill( );
  if ( rc < 0 ) return rc;

  // Find the blank line
  unsigned end = 0;
  for ( unsigned i = s_pos; i + 1 < s_len; i++ ) {
    if ( s_buf[i] == '\n' && s_buf[i+1] == '\n' ) { end = i + 2; break; }
    if ( i + 3 < s_len && s_buf[i] == '\r' && s_buf[i+1] == '\n' && s_buf[i+2] == '\r' && s_buf[i+3] == '\n' ) {
      end = i + 4; break;
    }
  }
  if ( !end ) {
    if ( s_len == sizeof( s_buf ) ) { set_err( "Response headers too long" ); return NET_ERROR; }
    int st = idle_state( );
    if ( st == NET_DONE ) { fail( "The server closed the connection", "before the headers" ); return NET_ERROR; }
    return st;
  }

  // Status line: HTTP/1.x NNN
  s_buf[ end - 1 ] = 0;
  char *p = strchr( s_buf + s_pos, ' ' );
  s_status = p ? atoi( p + 1 ) : 0;

  // Content-Length
  for ( char *line = s_buf + s_pos; line && *line; ) {
    if ( !strnicmp( line, "Content-Length:", 15 ) ) s_clen = atol( line + 15 );
    line = strchr( line, '\n' );
    if ( line ) line++;
  }
  s_pos = end;
  s_hdrDone = 1;
  return s_status ? s_status : NET_ERROR;
}

long http_content_length( void ) { return s_clen; }

int http_read( char far *buf, unsigned max ) {
  if ( !s_sock || !s_hdrDone ) return NET_ERROR;
  if ( s_pos < s_len ) {
    unsigned n = s_len - s_pos;
    if ( n > max ) n = max;
    _fmemcpy( buf, s_buf + s_pos, n );
    s_pos += n;
    return n;
  }
  net_poll( );
  if ( max > 32767 ) max = 32767;
  int16_t rc = s_sock->recv( (uint8_t *)buf, max );
  if ( rc < 0 ) { reset_err( ); return NET_ERROR; }
  if ( rc > 0 ) { s_last = TIMER_GET_CURRENT( ); return rc; }
  return idle_state( );
}

int http_line( char *line, unsigned max ) {
  if ( !s_sock || !s_hdrDone ) return NET_ERROR;
  for ( ;; ) {
    for ( unsigned i = s_pos; i < s_len; i++ ) {
      if ( s_buf[i] == '\n' ) {
        unsigned n = i - s_pos;
        if ( n && s_buf[ i - 1 ] == '\r' ) n--;
        if ( n >= max ) n = max - 1;
        memcpy( line, s_buf + s_pos, n );
        line[n] = 0;
        s_pos = i + 1;
        return 1;
      }
    }
    // No newline yet.  A full buffer means an over-long line: return part.
    if ( s_pos == 0 && s_len == sizeof( s_buf ) ) {
      unsigned n = max - 1 < s_len ? max - 1 : s_len;
      memcpy( line, s_buf, n ); line[n] = 0;
      s_pos = n;
      return 1;
    }
    int rc = fill( );
    if ( rc < 0 ) return rc;
    if ( rc == 0 ) {
      int st = idle_state( );
      if ( st == NET_DONE && s_pos < s_len ) {
        unsigned n = s_len - s_pos;
        if ( n >= max ) n = max - 1;
        memcpy( line, s_buf + s_pos, n ); line[n] = 0;
        s_pos = s_len;
        return 1;
      }
      return st;
    }
  }
}

void http_close( void ) {
  if ( !s_sock ) return;
  if ( net_event_hook ) net_event_hook( 0 );
  s_sock->close( );
  TcpSocketMgr::freeSocket( s_sock );
  s_sock = 0;
}

void url_encode( char *out, unsigned size, const char *s ) {
  static const char hex[] = "0123456789ABCDEF";
  unsigned n = 0;
  for ( ; *s && n + 4 < size; s++ ) {
    unsigned char c = (unsigned char)*s;
    if ( ( c >= 'A' && c <= 'Z' ) || ( c >= 'a' && c <= 'z' ) || ( c >= '0' && c <= '9' ) || c == '-' || c == '_' || c == '.' ) {
      out[n++] = c;
    }
    else {
      out[n++] = '%'; out[n++] = hex[ c >> 4 ]; out[n++] = hex[ c & 15 ];
    }
  }
  out[n] = 0;
}

int http_post_file( const char *host, unsigned port, const char *path, const char *file, const char *contentType ) {
  FILE *f = fopen( file, "rb" );
  if ( !f ) { set_err( "Could not open the file" ); return NET_ERROR; }
  fseek( f, 0, SEEK_END );
  long size = ftell( f );
  fseek( f, 0, SEEK_SET );
  if ( size < 0 || size > 1000000l ) { fclose( f ); set_err( "File too large" ); return NET_ERROR; }

  // Headers first (http_open with no body), then the file in pieces
  if ( !s_up ) { fclose( f ); set_err( "Network not started" ); return NET_ERROR; }
  IpAddr_t addr;
  if ( resolve( host, addr ) ) { fclose( f ); return NET_ERROR; }
  if ( s_sock ) http_close( );
  sprintf( s_what, "POST %.50s", path );
  if ( tcp_connect( host, addr, port ) ) { fclose( f ); return NET_ERROR; }
  char req[400];
  int n = sprintf( req, "POST %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: DeskMind/0.1\r\nConnection: close\r\n", path, host );
  if ( net_token[0] ) n += sprintf( req + n, "X-Token: %s\r\n", net_token );
  n += sprintf( req + n, "Content-Type: %s\r\nContent-Length: %ld\r\n\r\n",
                contentType ? contentType : "application/octet-stream", size );
  int rc = send_all( req, n );
  static char buf[ 2048 ];
  while ( !rc ) {
    size_t k = fread( buf, 1, sizeof( buf ), f );
    if ( k == 0 ) break;
    rc = send_all( buf, (unsigned)k );
  }
  fclose( f );
  if ( rc ) { http_close( ); return rc; }

  s_len = s_pos = 0; s_hdrDone = 0; s_status = 0; s_clen = -1;
  s_last = TIMER_GET_CURRENT( );
  int st;
  while ( ( st = http_headers( ) ) == NET_AGAIN ) ;
  // drain and close
  while ( st > 0 ) {
    int r = http_read( buf, sizeof( buf ) );
    if ( r != NET_AGAIN && r <= 0 ) break;
  }
  http_close( );
  return st;
}

long http_get_file( const char *host, unsigned port, const char *path, const char *file,
                    net_progress_fn progress, int *status ) {
  if ( http_open( host, port, "GET", path, 0, 0, 0 ) ) return NET_ERROR;
  int st;
  while ( ( st = http_headers( ) ) == NET_AGAIN ) {
    if ( progress && progress( 0, -1 ) ) { http_close( ); set_err( "Cancelled" ); return NET_ERROR; }
  }
  if ( status ) *status = st;
  if ( st != 200 ) {
    if ( st > 0 ) sprintf( s_err, "The server answered %d", st );
    http_close( );
    return NET_ERROR;
  }
  FILE *f = fopen( file, "wb" );
  if ( !f ) { http_close( ); set_err( "Could not create the file" ); return NET_ERROR; }
  static char buf[ 4096 ];
  unsigned long got = 0;
  long rc = 0;
  for ( ;; ) {
    int n = http_read( buf, sizeof( buf ) );
    if ( n > 0 ) {
      if ( fwrite( buf, 1, n, f ) != (size_t)n ) { set_err( "Disk full?" ); rc = NET_ERROR; break; }
      got += n;
      if ( progress && progress( got, s_clen ) ) { set_err( "Cancelled" ); rc = NET_ERROR; break; }
    }
    else if ( n == NET_AGAIN ) {
      if ( progress && progress( got, s_clen ) ) { set_err( "Cancelled" ); rc = NET_ERROR; break; }
    }
    else if ( n == NET_DONE ) break;
    else { rc = n; break; }
  }
  if ( fclose( f ) && !rc ) { set_err( "Disk error" ); rc = NET_ERROR; }
  http_close( );
  if ( rc == 0 && s_clen >= 0 && (long)got != s_clen ) { set_err( "Download incomplete" ); rc = NET_ERROR; }
  if ( rc ) { remove( file ); return rc; }
  return (long)got;
}

long http_get_all( const char *host, unsigned port, const char *path,
                   char far *buf, unsigned long max, int *status ) {
  if ( http_open( host, port, "GET", path, 0, 0, 0 ) ) return NET_ERROR;
  int st;
  while ( ( st = http_headers( ) ) == NET_AGAIN ) {
    if ( esc_pressed( ) ) { http_close( ); set_err( "Cancelled" ); return NET_ERROR; }
  }
  if ( status ) *status = st;
  if ( st < 0 ) { http_close( ); return st; }
  unsigned long got = 0;
  for ( ;; ) {
    unsigned want = ( max - got > 32767ul ) ? 32767u : (unsigned)( max - got );
    if ( want == 0 ) break;
    int rc = http_read( buf + (unsigned)got, want );
    if ( rc > 0 ) got += rc;
    else if ( rc == NET_AGAIN ) {
      if ( esc_pressed( ) ) { http_close( ); set_err( "Cancelled" ); return NET_ERROR; }
    }
    else if ( rc == NET_DONE ) break;
    else { http_close( ); return rc; }
  }
  http_close( );
  return (long)got;
}
