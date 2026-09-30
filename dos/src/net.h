// DeskMind - HTTP/1.0 client over mTCP (one connection at a time).
//
// All calls poll the TCP/IP stack; none block for long.  The caller runs
// its own loop (drawing, keyboard, "thinking..." animation) and keeps
// calling http_read / http_line until they return NET_DONE or an error.

#ifndef NET_H
#define NET_H

#define NET_AGAIN    0     // nothing yet, call again
#define NET_DONE    -1     // connection closed, all data read
#define NET_ERROR   -2     // connection failed or reset
#define NET_TIMEOUT -3     // nothing received for net_timeout_ms

extern unsigned long net_timeout_ms;     // default 30000
extern int net_verbose;                  // 1 = net_init prints its steps (text mode only)
extern char net_token[40];               // sent as X-Token when set
extern unsigned long net_connect_ms;     // connect timeout, default 10000
extern void ( *net_wait_hook )( void );  // called while waiting (keep the mouse pointer alive)

int  net_init( void );                   // 0 = ok (needs MTCPCFG + packet driver)
void net_done( void );
const char *net_error( void );           // text of the last error
void net_my_ip( char *out );             // "a.b.c.d"
void net_poll( void );

// Start a request.  body may be 0.  Returns 0 or NET_ERROR.
int  http_open( const char *host, unsigned port, const char *method, const char *path,
                const char far *body, unsigned bodyLen, const char *contentType );
// Wait for the status line and headers.  Returns the HTTP status (200 ...),
// NET_AGAIN while waiting, or an error.
int  http_headers( void );
long http_content_length( void );        // -1 if the server did not send one
// Body bytes: >0 = count, else NET_AGAIN / NET_DONE / error
int  http_read( char far *buf, unsigned max );
// One body line without CR/LF: 1 = got a line, else NET_AGAIN / NET_DONE / error.
// A last line without a newline is returned at close.
int  http_line( char *line, unsigned max );
void http_close( void );

// Download a GET response straight into a file (any size).  progress(bytes, total or -1)
// is called as data arrives; returning non-zero from it cancels.  Returns bytes or an error.
typedef int ( *net_progress_fn )( unsigned long done, long total );
long http_get_file( const char *host, unsigned port, const char *path, const char *file,
                    net_progress_fn progress, int *status );

// POST a file as the request body (any size, streamed from disk).  Returns the HTTP status or an error.
int  http_post_file( const char *host, unsigned port, const char *path, const char *file, const char *contentType );

// Percent-encode s for a URL query value
void url_encode( char *out, unsigned size, const char *s );

// Convenience: whole request into a buffer (blocking, with Esc to abort).
// Returns the byte count or an error code.
long http_get_all( const char *host, unsigned port, const char *path,
                   char far *buf, unsigned long max, int *status );

#endif
