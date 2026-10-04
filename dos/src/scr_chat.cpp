// DeskMind - Chat screen.
//
// Messages live in a far text buffer; layout() turns them into display lines
// (word-wrapped text, and 5+1 rows per picture: each row draws a 10-pixel slice
// of the 160x50 thumbnail).  Streaming replies grow the last message; only the
// rows that changed are redrawn, and the panel scrolls with one memory move.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <dos.h>
#include <io.h>
#include <direct.h>

#include "app.h"
#include "scr.h"
#include "video.h"
#include "sound.h"
#include "sys.h"
#include "cfg.h"
#include "tpi.h"
#include "net.h"

// ---------------------------------------------------------------- data

enum { MK_USER, MK_AI, MK_PIC, MK_ERR };
enum { DL_TEXT, DL_PIC, DL_BLANK };

struct Msg {
  unsigned char kind;
  unsigned off, len;             // text in s_tx (MK_PIC: the title)
  char pic[9];                   // MK_USER: attached picture; MK_PIC: the drawn picture
  int firstLine;                 // first display line
};

struct DLine {
  unsigned off;                  // text offset in s_tx
  unsigned char len;             // characters on this line
  unsigned char type;            // DL_*
  unsigned char sub;             // DL_PIC: slice 0..4; DL_TEXT: 1 on a message's first line
  short msg;
};

#define TX_SIZE   40000u
#define MAX_MSGS  300
#define MAX_DL    1400
#define GUTTER    6               // "You:  " / "Qwen: "

static char far *s_tx = 0;
static unsigned  s_txLen = 0;
static Msg      *s_msgs = 0;
static int       s_nmsgs = 0;
static DLine far *s_dl = 0;
static int       s_ndl = 0;
static int       s_top = 0;
static char      s_chatId[9] = "";
static char      s_title[40] = "New chat";
static char      s_attach[9] = "";
static int       s_saved = 0;      // messages [0, s_saved) are in the .TCH file ...
static long      s_end = 0;        // ... which ends at byte s_end (new messages are appended there)
static int       s_warned = 0;     // "nearly full" notice shown for this chat
static int       s_cut = 0;        // text did not fit during this reply

// Panel geometry (screen coordinates)
#define WIN_Y    ( SCR_Y )
#define WIN_H    ( SCR_H - 1 )
#define PX       6
#define PY       ( WIN_Y + 14 )
#define PW       628
#define PH       ( 4 + ROWS * ROW_H )
#define ROWS     13
#define TEXT_X   ( PX + 4 )
#define COLS     ( ( PW - 20 ) / 8 )           // 76 columns
#define WRAP     ( COLS - GUTTER )

// Input row: a form with the edit field and buttons
static char s_input[ 400 ];
static Widget s_ws[5];
static Form s_form;
enum { ID_SEND = 1, ID_PIC, ID_CHATS, ID_NEW };

// ---------------------------------------------------------------- thumbnail cache

#define THUMBS 6
static struct { char id[9]; unsigned char far *px; int w, h; unsigned long used; } s_th[ THUMBS ];

static unsigned char far *thumb( const char *id, int *w, int *h ) {
  static unsigned long clock = 0;
  int slot = -1, oldest = 0;
  for ( int i = 0; i < THUMBS; i++ ) {
    if ( s_th[i].px && !strcmp( s_th[i].id, id ) ) { s_th[i].used = ++clock; *w = s_th[i].w; *h = s_th[i].h; return s_th[i].px; }
    if ( s_th[i].used < s_th[oldest].used ) oldest = i;
  }
  slot = oldest;
  if ( !s_th[slot].px ) s_th[slot].px = (unsigned char far *)_fmalloc( TPI_THUMB_MAX );
  if ( !s_th[slot].px ) return 0;
  char p[80];
  TpiHeader hd;
  app_pic_path( p, id );
  if ( tpi_thumb( p, s_th[slot].px, &hd ) ) { s_th[slot].id[0] = 0; return 0; }
  strcpy( s_th[slot].id, id );
  s_th[slot].w = hd.thumb_w; s_th[slot].h = hd.thumb_h;
  s_th[slot].used = ++clock;
  *w = hd.thumb_w; *h = hd.thumb_h;
  return s_th[slot].px;
}

// ---------------------------------------------------------------- text store

static int add_msg( int kind, const char *text, const char *pic ) {
  if ( s_nmsgs >= MAX_MSGS ) return -1;
  Msg &m = s_msgs[ s_nmsgs ];
  m.kind = (unsigned char)kind;
  m.off = s_txLen;
  m.len = 0;
  m.pic[0] = 0;
  if ( pic ) str_copy( m.pic, pic, sizeof( m.pic ) );
  m.firstLine = s_ndl;
  s_nmsgs++;
  if ( text ) {
    unsigned n = (unsigned)strlen( text );
    if ( s_txLen + n < TX_SIZE ) { _fmemcpy( s_tx + s_txLen, text, n ); s_txLen += n; m.len = n; }
    else s_cut = 1;
  }
  return s_nmsgs - 1;
}

// Append to the last message (it must be the last one in the buffer)
static void append_text( const char *t ) {
  if ( !s_nmsgs ) return;
  Msg &m = s_msgs[ s_nmsgs - 1 ];
  unsigned n = (unsigned)strlen( t );
  if ( s_txLen + n >= TX_SIZE ) { s_cut = 1; return; }
  _fmemcpy( s_tx + s_txLen, t, n );
  s_txLen += n;
  m.len += n;
}

// ---------------------------------------------------------------- layout

static void add_line( unsigned off, int len, int type, int sub, int msg ) {
  if ( s_ndl >= MAX_DL ) return;
  DLine far &d = s_dl[ s_ndl++ ];
  d.off = off; d.len = (unsigned char)len; d.type = (unsigned char)type; d.sub = (unsigned char)sub; d.msg = (short)msg;
}

// How full the chat is: 0 fine, 1 nearly full (warn), 2 full (no room for another reply).
// A long reply is a few thousand characters and a few dozen lines.
static int chat_room( void ) {
  if ( s_txLen > TX_SIZE - 6000u || s_nmsgs > MAX_MSGS - 6 || s_ndl > MAX_DL - 200 ) return 2;
  if ( s_txLen > TX_SIZE - 12000u || s_nmsgs > MAX_MSGS - 30 || s_ndl > MAX_DL - 400 ) return 1;
  return 0;
}

static void layout_text( int mi, unsigned off, unsigned len ) {
  // trim trailing newlines/spaces
  while ( len && ( s_tx[ off + len - 1 ] == '\n' || s_tx[ off + len - 1 ] == ' ' ) ) len--;
  unsigned pos = 0;
  int first = 1;
  do {
    unsigned start = pos, end = pos;
    while ( end < len && s_tx[ off + end ] != '\n' && end - start < WRAP ) end++;
    unsigned next = end;
    if ( end < len && s_tx[ off + end ] == '\n' ) next = end + 1;
    else if ( end < len ) {
      unsigned sp = end;
      while ( sp > start && s_tx[ off + sp - 1 ] != ' ' ) sp--;
      if ( sp > start ) { end = sp; next = sp; }
    }
    add_line( off + start, (int)( end - start ), DL_TEXT, first, mi );
    first = 0;
    pos = next;
  } while ( pos < len );
}

static void layout_from( int mi ) {
  if ( mi < 0 ) mi = 0;
  s_ndl = ( mi < s_nmsgs ) ? s_msgs[mi].firstLine : s_ndl;
  for ( int i = mi; i < s_nmsgs; i++ ) {
    Msg &m = s_msgs[i];
    m.firstLine = s_ndl;
    if ( m.kind == MK_PIC ) {
      for ( int k = 0; k < 5; k++ ) add_line( m.off, m.len, DL_PIC, k, i );
    }
    else {
      layout_text( i, m.off, m.len );
      if ( m.kind == MK_USER && m.pic[0] ) {
        for ( int k = 0; k < 5; k++ ) add_line( 0, 0, DL_PIC, k, i );
      }
    }
    add_line( 0, 0, DL_BLANK, 0, i );
  }
}

// ---------------------------------------------------------------- drawing

static int max_top( void ) { return s_ndl > ROWS ? s_ndl - ROWS : 0; }

static void draw_row( int r ) {
  int i = s_top + r;
  int y = PY + 2 + r * ROW_H;
  gui_mouse_hide( );
  vid_hline( PX + 2, PX + PW - 15, y, WHITE );
  vid_hline( PX + 2, PX + PW - 15, y + 9, WHITE );
  vid_fill( PX + 2, y + 1, 2, 8, WHITE );
  if ( i >= s_ndl ) {
    for ( int c = 0; c < COLS; c++ ) vid_char( TEXT_X + c * 8, y + 1, ' ', BLACK, WHITE );
    gui_mouse_show( );
    return;
  }
  DLine far &d = s_dl[i];
  Msg &m = s_msgs[ d.msg ];
  int c = 0;
  // gutter
  const char *lab = "      ";
  unsigned char lc = BLACK;
  if ( d.type == DL_TEXT && d.sub ) {
    if ( m.kind == MK_USER ) { lab = "You:  "; lc = BLUE; }
    else if ( m.kind == MK_AI ) { lab = "Qwen: "; lc = GREEN; }
    else if ( m.kind == MK_ERR ) { lab = "!     "; lc = RED; }
  }
  for ( ; c < GUTTER; c++ ) vid_char( TEXT_X + c * 8, y + 1, (unsigned char)lab[c], lc, WHITE );

  if ( d.type == DL_TEXT ) {
    unsigned char fg = m.kind == MK_USER ? BLUE : m.kind == MK_ERR ? RED : BLACK;
    for ( int k = 0; k < d.len && c < COLS; k++, c++ )
      vid_char( TEXT_X + c * 8, y + 1, (unsigned char)s_tx[ d.off + k ], fg, WHITE );
    for ( ; c < COLS; c++ ) vid_char( TEXT_X + c * 8, y + 1, ' ', BLACK, WHITE );
  }
  else if ( d.type == DL_PIC ) {
    const char *id = m.pic;
    int tw = 160, th = 50;
    unsigned char far *px = thumb( id, &tw, &th );
    int tx = TEXT_X + GUTTER * 8;
    if ( px ) vid_blit( tx, y, tw, ROW_H, px + d.sub * ROW_H * ( tw / 2 ), tw / 2 );
    else vid_fill( tx, y, 160, ROW_H, DGRAY );
    // caption to the right of the thumbnail
    int cx = ( tx + 168 - TEXT_X ) / 8;
    for ( c = GUTTER; c < cx; c++ ) ;
    char cap[60] = "";
    if ( d.sub == 1 ) {
      if ( m.kind == MK_PIC ) { unsigned n = m.len < 50 ? m.len : 50; _fmemcpy( cap, s_tx + m.off, n ); cap[n] = 0; }
      else strcpy( cap, "(attached picture)" );
    }
    else if ( d.sub == 3 ) strcpy( cap, px || app_have_pic( id ) ? "click to view" : "click to download" );
    unsigned char fg = d.sub == 3 ? DGRAY : BLACK;
    int k = 0;
    for ( c = cx; c < COLS; c++ ) vid_char( TEXT_X + c * 8, y + 1, (unsigned char)( cap[k] ? cap[k++] : ' ' ), fg, WHITE );
    vid_fill( tx + 160, y, TEXT_X + cx * 8 - tx - 160, ROW_H, WHITE );
  }
  else {
    for ( ; c < COLS; c++ ) vid_char( TEXT_X + c * 8, y + 1, ' ', BLACK, WHITE );
  }
  gui_mouse_show( );
}

static int s_sbN = -1, s_sbTop = -1;

static void draw_scroll( int force ) {
  if ( !force && s_sbN == s_ndl && s_sbTop == s_top ) return;
  s_sbN = s_ndl; s_sbTop = s_top;
  int x = PX + PW - 12, y = PY + 1, h = PH - 2;
  gui_mouse_hide( );
  vid_fill( x, y, 10, h, LGRAY );
  vid_fill( x, y, 2, h, DGRAY );
  if ( s_ndl > ROWS ) {
    int th = h * ROWS / s_ndl;
    if ( th < 6 ) th = 6;
    int ty = y + (int)( (long)( h - th ) * s_top / ( s_ndl - ROWS ) );   // long: 124 * 550 overflows an int
    vid_fill( x + 2, ty, 8, th, DGRAY );
    ui_bevel( x + 2, ty, 8, th, 1 );
  }
  gui_mouse_show( );
}

static void draw_panel( void ) {
  for ( int r = 0; r < ROWS; r++ ) draw_row( r );
  draw_scroll( 1 );
}

static void set_top( int top ) {
  if ( top > max_top( ) ) top = max_top( );
  if ( top < 0 ) top = 0;
  if ( top == s_top ) return;
  int d = top - s_top;
  s_top = top;
  if ( d > 0 && d < ROWS ) {
    gui_mouse_hide( );
    vid_scroll_up( PX + 2, PY + 2, PW - 16, ROWS * ROW_H, d * ROW_H, WHITE );
    gui_mouse_show( );
    for ( int r = ROWS - d; r < ROWS; r++ ) draw_row( r );
  }
  else draw_panel( );
  draw_scroll( 0 );
}

// After messages changed from display line `from` on: scroll to the end if we were there
static void refresh_from( int from, int follow ) {
  if ( follow && max_top( ) != s_top ) {
    int oldTop = s_top;
    set_top( max_top( ) );
    // rows that were already on screen but changed
    for ( int r = 0; r < ROWS; r++ ) {
      int i = s_top + r;
      if ( i >= from && i < oldTop + ROWS - ( s_top - oldTop ) + ( s_top - oldTop ) ) draw_row( r );
    }
  }
  else {
    for ( int r = 0; r < ROWS; r++ ) if ( s_top + r >= from ) draw_row( r );
  }
  draw_scroll( 0 );
}

static void draw_title( void ) {
  char t[70];
  sprintf( t, "Chat - %s", s_title );
  ui_window( 0, WIN_Y, 640, WIN_H, t, 1 );
  gui_mouse_hide( );
  ui_sunken( PX, PY, PW, PH, WHITE );
  gui_mouse_show( );
}

void chat_draw( void ) {
  draw_title( );
  draw_panel( );
  s_form.x = 0; s_form.y = WIN_Y;
  for ( int i = 0; i < s_form.n; i++ ) form_draw_widget( &s_form, i );
  if ( s_attach[0] ) app_status( "Picture %s is attached to your next message.", s_attach );
  else app_status( "F2 Chat  F3 Create  F4 Gallery  F5 Settings   PgUp/PgDn scroll, click a picture to view" );
}

// ---------------------------------------------------------------- files

static void chat_file( char *out, const char *id ) {
  sprintf( out, "%s\\%.8s.TCH", cfg.chats, id );
}

// Transcripts are read and written in binary with explicit CR LF, in 4K buffered chunks
// (8 MHz tests: the 512-byte default buffers and a seek per line made a long chat take 30 s).
static char s_fbuf[ 4096 ];

static void save_chat( void ) {
  if ( !s_chatId[0] ) return;
  mkdir( cfg.chats );
  char path[80];
  chat_file( path, s_chatId );
  // Messages never change once a reply has ended, so only the new ones are appended (a
  // long chat also keeps its older part, which isn't loaded, untouched in the file).
  FILE *f = 0;
  if ( s_end > 0 && ( f = fopen( path, "r+b" ) ) != 0 && fseek( f, s_end, SEEK_SET ) ) { fclose( f ); f = 0; }
  if ( !f ) {                                   // new chat (or the file went missing): all of it
    s_end = 0; s_saved = 0;
    f = fopen( path, "wb" );
  }
  if ( !f ) return;
  setvbuf( f, s_fbuf, _IOFBF, sizeof( s_fbuf ) );
  static char line[ 200 ];
  for ( int i = s_saved; i < s_nmsgs; i++ ) {
    Msg &m = s_msgs[i];
    if ( m.kind == MK_ERR ) continue;
    if ( m.kind == MK_PIC ) {
      unsigned n = m.len < 60 ? m.len : 60;
      _fmemcpy( line, s_tx + m.off, n ); line[n] = 0;
      fprintf( f, "!I %s %s\r\n", m.pic, line );
      continue;
    }
    fputs( m.kind == MK_USER ? ">U\r\n" : ">A\r\n", f );
    if ( m.kind == MK_USER && m.pic[0] ) fprintf( f, "!P %s\r\n", m.pic );
    unsigned k = 0;
    while ( k < m.len ) {
      // one paragraph per line, in chunks (it used to break paragraphs every 199 characters)
      unsigned n = 0;
      while ( k < m.len && s_tx[ m.off + k ] != '\n' && n < sizeof( line ) - 1 ) line[n++] = s_tx[ m.off + k++ ];
      line[n] = 0;
      fputs( line, f );
      if ( k < m.len && s_tx[ m.off + k ] != '\n' ) continue;
      if ( k < m.len ) k++;
      fputs( "\r\n", f );
    }
  }
  fflush( f );
  if ( !ferror( f ) ) {
    s_end = ftell( f );
    s_saved = s_nmsgs;
    chsize( fileno( f ), s_end );             // (only matters if the file was longer)
  }
  fclose( f );
}

static void clear_chat( void ) {
  s_txLen = 0; s_nmsgs = 0; s_ndl = 0; s_top = 0;
  s_chatId[0] = 0; s_attach[0] = 0;
  s_saved = 0; s_end = 0; s_warned = 0; s_cut = 0;
  strcpy( s_title, "New chat" );
}

// What a transcript costs in memory: text bytes, display lines (estimated), messages
struct Cost { long bytes, lines; int msgs; };

static int is_msg_start( const char *l ) {
  return ( l[0] == '>' && ( l[1] == 'U' || l[1] == 'A' ) && ( !l[2] || l[2] == '\r' || l[2] == '\n' ) ) ||
         !strncmp( l, "!I ", 3 );
}

// Line reader over fread chunks.  Watcom's fgets costs about 1000 cycles per character on an
// 8086 (8 MHz test: 2.4 s for 20 KB); memchr over a buffer is many times cheaper.
struct Rd { FILE *f; unsigned pos, len; };
static char s_rbuf[ 2048 ];

static void rd_open( Rd *r, FILE *f, long at ) {
  r->f = f; r->pos = r->len = 0;
  fseek( f, at, SEEK_SET );
}

// The next line (at most max-1 bytes of it) into out, without CR/LF.  Returns the bytes taken
// from the file (0 = end); *whole = 1 when the line ended (else the rest comes next call).
static unsigned rd_line( Rd *r, char *out, unsigned max, int *whole ) {
  unsigned n = 0, used = 0;
  *whole = 0;
  while ( n < max - 1 ) {
    if ( r->pos >= r->len ) {
      r->len = (unsigned)fread( s_rbuf, 1, sizeof( s_rbuf ), r->f );
      r->pos = 0;
      if ( !r->len ) break;
    }
    char *p = s_rbuf + r->pos;
    unsigned avail = r->len - r->pos;
    char *nl = (char *)memchr( p, '\n', avail );
    unsigned take = nl ? (unsigned)( nl - p ) + 1 : avail;
    if ( take > max - 1 - n ) { take = max - 1 - n; nl = 0; }
    memcpy( out + n, p, take );
    n += take; r->pos += take; used += take;
    if ( nl ) { *whole = 1; break; }
  }
  while ( n && ( out[ n - 1 ] == '\n' || out[ n - 1 ] == '\r' ) ) n--;
  out[n] = 0;
  return used;
}

// Walks a transcript (opened "rb") from `from` (a line start) to the end.  Returns the offset
// of the oldest message start from which the rest fits in *fit, the last start if none does,
// or -1 if there is no message start at all.  Only the last RING starts can qualify
// (fit->msgs is smaller), so they are kept in a ring with the cost up to each of them.
#define RING ( MAX_MSGS / 2 + 10 )
static long tch_fit( FILE *f, long from, const Cost *fit ) {
  static char line[ 512 ];
  static struct { long at; Cost cum; } ring[ RING ];
  int nring = 0, first = 0;
  Cost used = { 0, 0, 0 };
  int continued = 0, whole;
  long at = from;
  Rd r;
  rd_open( &r, f, from );
  unsigned n;
  while ( ( n = rd_line( &r, line, sizeof( line ), &whole ) ) != 0 ) {
    int start = !continued && is_msg_start( line );
    if ( start ) {
      int slot;
      if ( nring < RING ) slot = ( first + nring++ ) % RING;
      else { slot = first; first = ( first + 1 ) % RING; }
      ring[slot].at = at; ring[slot].cum = used;
    }
    at += n;
    long len = (long)strlen( line );
    if ( start ) {
      used.msgs++; used.lines++;                        // + the blank line after it
      if ( line[0] == '!' ) { used.lines += 5; used.bytes += len; }
    }
    else if ( !continued && !strncmp( line, "!P ", 3 ) ) used.lines += 5;
    else { used.bytes += len + 1; used.lines += len / WRAP + ( continued ? 0 : 1 ); }
    continued = !whole;
  }
  for ( int k = 0; k < nring; k++ ) {
    int i = ( first + k ) % RING;
    if ( used.bytes - ring[i].cum.bytes <= fit->bytes && used.lines - ring[i].cum.lines <= fit->lines &&
         used.msgs - ring[i].cum.msgs <= fit->msgs ) return ring[i].at;
  }
  return nring ? ring[ ( first + nring - 1 ) % RING ].at : -1;
}

// The chat's title: the first line of the first user message (near the start of the file)
static void tch_title( FILE *f ) {
  static char line[ 120 ];
  int afterUser = 0;
  rewind( f );
  for ( int i = 0; i < 20 && fgets( line, sizeof( line ), f ); i++ ) {
    line[ strcspn( line, "\r\n" ) ] = 0;
    if ( afterUser && line[0] && line[0] != '!' ) { str_copy( s_title, line, sizeof( s_title ) ); return; }
    afterUser = !strcmp( line, ">U" ) || ( afterUser && line[0] == '!' );
  }
}

// Reads messages from file offset `at` to the end
static void parse_from( FILE *f, long at ) {
  static char line[ 1024 ];
  int cur = -1;                 // current text message
  int firstUser = strcmp( s_title, "New chat" ) == 0;    // no title found by tch_title
  int continued = 0;            // the previous line was cut (buffer full): this is the rest
  int whole;
  Rd r;
  rd_open( &r, f, at );
  while ( rd_line( &r, line, sizeof( line ), &whole ) ) {
    if ( continued ) {          // rest of a long paragraph
      if ( cur >= 0 && cur == s_nmsgs - 1 ) append_text( line );
      continued = !whole;
      continue;
    }
    continued = !whole;
    if ( !strcmp( line, ">U" ) || !strcmp( line, ">A" ) ) {
      cur = add_msg( line[1] == 'U' ? MK_USER : MK_AI, 0, 0 );
      continue;
    }
    if ( !strncmp( line, "!P ", 3 ) && cur >= 0 ) { str_copy( s_msgs[cur].pic, line + 3, 9 ); continue; }
    if ( !strncmp( line, "!I ", 3 ) ) {
      char pid[9];
      str_copy( pid, line + 3, 9 );
      const char *t = strlen( line ) > 12 ? line + 12 : "";
      add_msg( MK_PIC, t, pid );
      cur = -1;
      continue;
    }
    if ( cur < 0 || cur != s_nmsgs - 1 ) continue;
    if ( s_msgs[cur].len ) append_text( "\n" );
    append_text( line );
    if ( firstUser && s_msgs[cur].kind == MK_USER && line[0] ) {
      str_copy( s_title, line, sizeof( s_title ) );
      firstUser = 0;
    }
  }
}

static int load_chat( const char *id ) {
  char path[80];
  chat_file( path, id );
  FILE *f = fopen( path, "rb" );
  if ( !f ) return 1;
  clear_chat( );
  str_copy( s_chatId, id, sizeof( s_chatId ) );
  tch_title( f );
  fseek( f, 0, SEEK_END );
  long size = ftell( f );
  // Most chats fit: read them in one pass.  A chat too long for the buffers opens at its
  // newest part (about half full, so there is room to go on); only the end of the file is
  // read, and the older part stays in the file (save_chat only appends).
  int trim = 1;
  if ( size <= (long)( TX_SIZE - 12000u ) ) {
    parse_from( f, 0 );
    layout_from( 0 );
    trim = s_cut || chat_room( ) != 0;
    if ( trim ) {                              // too many messages or lines after all
      char title[ sizeof( s_title ) ];
      strcpy( title, s_title );
      clear_chat( );
      str_copy( s_chatId, id, sizeof( s_chatId ) );
      strcpy( s_title, title );
    }
  }
  if ( trim ) {
    static const Cost half = { TX_SIZE / 2, MAX_DL / 2, MAX_MSGS / 2 };
    long from = size > (long)half.bytes ? size - (long)half.bytes : 0;
    if ( from ) {                              // move on to the next line start
      static char skip[ 512 ];
      Rd r;
      int whole = 0;
      unsigned n;
      rd_open( &r, f, from );
      while ( !whole && ( n = rd_line( &r, skip, sizeof( skip ), &whole ) ) != 0 ) from += n;
    }
    long head = tch_fit( f, from, &half );
    if ( head < 0 ) head = tch_fit( f, 0, &half );     // one huge message: search all of it
    if ( head < 0 ) head = 0;
    if ( head ) add_msg( MK_ERR, "(Earlier messages are saved, but not shown: this chat is long.)", 0 );
    parse_from( f, head );
    layout_from( 0 );
  }
  fclose( f );
  s_cut = 0;
  s_saved = s_nmsgs;                           // all of it is in the file already
  s_end = size;
  s_top = max_top( );
  return 0;
}

// ---------------------------------------------------------------- chat list dialog

struct ChatEntry { char id[9]; char title[48]; unsigned long when; };
static ChatEntry *s_list = 0;
static int s_nlist = 0, s_nchats = 0;          // shown / on disk
#define MAX_CHATS 100

static int cmp_chat( const void *a, const void *b ) {
  unsigned long x = ( (const ChatEntry *)a )->when, y = ( (const ChatEntry *)b )->when;
  return x < y ? 1 : x > y ? -1 : 0;
}

static const char far *chat_item( void *, int i ) { return s_list[i].title; }

static void scan_chats( void ) {
  if ( !s_list ) s_list = (ChatEntry *)malloc( sizeof( ChatEntry ) * MAX_CHATS );
  s_nlist = s_nchats = 0;
  if ( !s_list ) return;
  struct find_t ff;
  char pattern[80];
  sprintf( pattern, "%s\\*.TCH", cfg.chats );
  unsigned rc = _dos_findfirst( pattern, _A_NORMAL, &ff );
  while ( rc == 0 ) {
    // DOS lists files in folder order: past MAX_CHATS, a newer chat replaces the oldest one
    unsigned long when = ( (unsigned long)ff.wr_date << 16 ) | ff.wr_time;
    s_nchats++;
    int slot = s_nlist;
    if ( s_nlist == MAX_CHATS ) {
      slot = 0;
      for ( int i = 1; i < s_nlist; i++ ) if ( s_list[i].when < s_list[slot].when ) slot = i;
      if ( s_list[slot].when >= when ) { rc = _dos_findnext( &ff ); continue; }
    }
    ChatEntry &c = s_list[ slot ];
    c.when = when;
    memcpy( c.id, ff.name, 8 ); c.id[8] = 0;
    char *dot = strchr( c.id, '.' ); if ( dot ) *dot = 0;
    // title = first line of the first user message; date from the file
    char path[80], line[120];
    sprintf( path, "%s\\%s", cfg.chats, ff.name );
    FILE *f = fopen( path, "r" );
    strcpy( line, "(empty chat)" );
    if ( f ) {
      char buf[120];
      int nextIsUser = 0;
      while ( fgets( buf, sizeof( buf ), f ) ) {
        buf[ strcspn( buf, "\r\n" ) ] = 0;
        if ( nextIsUser && buf[0] != '!' ) { str_copy( line, buf, sizeof( line ) ); break; }
        nextIsUser = !strcmp( buf, ">U" );
      }
      fclose( f );
    }
    unsigned d = ff.wr_date;
    sprintf( c.title, "%02u-%02u %-38.38s", ( d >> 5 ) & 15, d & 31, line );
    if ( slot == s_nlist ) s_nlist++;
    rc = _dos_findnext( &ff );
  }
  qsort( s_list, s_nlist, sizeof( ChatEntry ), cmp_chat );
}

static void sync_to_server( void ) {
  if ( !app_net || !s_chatId[0] ) return;
  char file[80], path[40];
  chat_file( file, s_chatId );
  sprintf( path, "/chat/%s/sync", s_chatId );
  http_post_file( cfg.server, cfg.port, path, file, "text/plain" );
}

static void chats_dialog( void ) {
  scan_chats( );
  static Widget ws[5];
  memset( ws, 0, sizeof( ws ) );
  int w = 460, h = 150, x = ( ( 640 - w ) / 2 ) & ~1, y = 22;
  ws[0].type = W_LIST; ws[0].id = 1; ws[0].x = 4; ws[0].y = 4; ws[0].w = 440; ws[0].h = 4 + 10 * ROW_H;
  list_set( &ws[0], s_nlist, chat_item, 0 );
  ws[1].type = W_BUTTON; ws[1].id = 1; ws[1].x = 4;   ws[1].y = 112; ws[1].w = 80; ws[1].h = 14; ws[1].text = "Open"; ws[1].flags = WF_DEFAULT;
  ws[2].type = W_BUTTON; ws[2].id = 2; ws[2].x = 92;  ws[2].y = 112; ws[2].w = 80; ws[2].h = 14; ws[2].text = "Delete";
  ws[3].type = W_BUTTON; ws[3].id = 3; ws[3].x = 180; ws[3].y = 112; ws[3].w = 96; ws[3].h = 14; ws[3].text = "New chat";
  ws[4].type = W_BUTTON; ws[4].id = 4; ws[4].x = 284; ws[4].y = 112; ws[4].w = 80; ws[4].h = 14; ws[4].text = "Cancel"; ws[4].flags = WF_CANCEL;
  Form f;
  unsigned char far *save = ui_save( x, y, w + 6, h + 3 );
  static char ft[60];
  if ( s_nchats > s_nlist ) sprintf( ft, "Chats on this Tandy (newest %d of %d)", s_nlist, s_nchats );
  else strcpy( ft, "Chats on this Tandy" );
  form_init( &f, ft, x, y, w, h, ws, 5 );
  form_draw( &f );
  int r;
  for ( ;; ) {
    r = form_run( &f );
    if ( r == 2 && ws[0].sel >= 0 && s_nlist ) {
      ChatEntry &c = s_list[ ws[0].sel ];
      char q[100];
      sprintf( q, "Delete this chat?  %s", c.title + 6 );
      if ( msg_box( "Delete chat", q, "Yes|No" ) == 1 ) {
        char path[80];
        chat_file( path, c.id );
        remove( path );
        if ( app_net ) {
          char p[40];
          sprintf( p, "/chat/%s/del", c.id );
          if ( http_open( cfg.server, cfg.port, "POST", p, 0, 0, 0 ) == 0 ) {
            while ( http_headers( ) == NET_AGAIN ) ;
            http_close( );
          }
        }
        if ( !strcmp( c.id, s_chatId ) ) clear_chat( );
        scan_chats( );
        list_set( &ws[0], s_nlist, chat_item, 0 );
      }
      form_draw( &f );
      continue;
    }
    break;
  }
  ui_restore( x, y, w + 6, h + 3, save );
  if ( ( r == 1 ) && ws[0].sel >= 0 && s_nlist ) {
    if ( load_chat( s_list[ ws[0].sel ].id ) == 0 ) {
      app_status( "Syncing the chat with MindServer..." );
      sync_to_server( );
    }
  }
  else if ( r == 3 ) clear_chat( );
  chat_draw( );
}

// ---------------------------------------------------------------- sending

static int s_busy = 0;

static void new_ai_msg_if_needed( int *aiMsg ) {
  if ( *aiMsg < 0 ) {
    *aiMsg = add_msg( MK_AI, 0, 0 );
    layout_from( *aiMsg );
  }
}

static void send_message( void ) {
  if ( s_busy || !s_input[0] ) return;
  if ( !app_need_net( "Chatting" ) ) return;
  if ( chat_room( ) == 2 ) {
    int r;
    if ( s_chatId[0] )
      r = msg_box( "Chat full", "This chat is full.  Continue = hide the older messages (they stay saved) "
                                "and send.  New = start a new chat.", "Continue|New|Cancel" );
    else {
      r = msg_box( "Chat full", "This chat is full.  Start a new chat to go on.", "New|Cancel" );
      r = r == 1 ? 2 : 0;
    }
    if ( r == 1 ) {
      char keep[9];
      str_copy( keep, s_attach, sizeof( keep ) );          // load_chat clears the attachment
      save_chat( );
      load_chat( s_chatId );
      str_copy( s_attach, keep, sizeof( s_attach ) );
    }
    else if ( r == 2 ) clear_chat( );                        // (a fresh chat: the attachment goes too)
    else return;
    chat_draw( );
    if ( chat_room( ) == 2 ) { msg_box( "Chat full", "Still no room.  Start a new chat.", "OK" ); return; }
  }
  s_busy = 1;
  s_cut = 0;

  // Show the user's message
  int um = add_msg( MK_USER, s_input, s_attach[0] ? s_attach : 0 );
  if ( !s_chatId[0] ) str_copy( s_title, s_input, sizeof( s_title ) );
  layout_from( um );
  refresh_from( s_msgs[um].firstLine, 1 );

  // Make sure MindServer has an attached picture (it may have been deleted there)
  if ( s_attach[0] ) {
    char p[40];
    static char tmp[64];
    int st = 0;
    sprintf( p, "/img/%s/thumb", s_attach );
    long n = http_get_all( cfg.server, cfg.port, p, tmp, sizeof( tmp ), &st );
    (void)n;
    if ( st == 404 ) app_upload_pic( s_attach );
  }

  static char path[ 120 ];
  sprintf( path, "/chat?id=%s", s_chatId );
  if ( s_attach[0] ) sprintf( path + strlen( path ), "&img=%s", s_attach );
  if ( cfg.draw_enhance >= 0 ) sprintf( path + strlen( path ), "&draw_enhance=%d", cfg.draw_enhance ? 1 : 0 );
  static char body[ 400 ];
  str_copy( body, s_input, sizeof( body ) );
  s_input[0] = 0; s_ws[0].cur = 0; s_ws[0].top = 0;
  form_draw_widget( &s_form, 0 );
  s_attach[0] = 0;

  unsigned long saveTimeout = net_timeout_ms;
  net_timeout_ms = 120000ul;
  int rc = http_open( cfg.server, cfg.port, "POST", path, body, (unsigned)strlen( body ), "text/plain" );
  int aiMsg = -1, done = 0, replied = 0, cancelled = 0;
  static char pending[4][9];
  int npending = 0;
  static char line[ 1100 ];
  // No pointer while the reply streams in (it can't be used then anyway), so no drawing
  // can ever leave stale pointer pixels on the text (seen once as "jus}" for "just").
  gui_mouse_hide( );
  const char *state = "Sending...";
  if ( rc ) {
    add_msg( MK_ERR, net_error( ), 0 );
    done = 1;
  }
  else {
    int st;
    while ( ( st = http_headers( ) ) == NET_AGAIN ) { app_spin( state ); if ( kbd_get( ) == K_ESC ) { cancelled = 1; break; } }
    if ( !cancelled && st != 200 ) { add_msg( MK_ERR, st < 0 ? net_error( ) : "MindServer refused the message", 0 ); done = 1; }
  }
  state = "Qwen is thinking...  (Esc = stop)";
  while ( !done && !cancelled ) {
    int r = http_line( line, sizeof( line ) );
    if ( r == NET_AGAIN ) {
      app_spin( state );
      if ( kbd_get( ) == K_ESC ) cancelled = 1;
      continue;
    }
    if ( r != 1 ) {
      if ( r != NET_DONE ) add_msg( MK_ERR, net_error( ), 0 );
      break;
    }
    char code = line[0];
    const char *arg = line[1] ? line + 2 : "";
    if ( code == 'C' ) {
      if ( !s_chatId[0] ) { str_copy( s_chatId, arg, sizeof( s_chatId ) ); }
    }
    else if ( code == 'S' ) {
      if ( !strncmp( arg, "drawing", 7 ) ) {
        static char t[60];
        sprintf( t, "Drawing the picture... %s%%  (Esc = stop)", arg + 8 );
        state = t;
      }
      else state = "Qwen is thinking...  (Esc = stop)";
      app_spin( state );
    }
    else if ( code == 'T' || code == 'N' ) {
      int wasBottom = ( s_top == max_top( ) );
      new_ai_msg_if_needed( &aiMsg );
      if ( aiMsg != s_nmsgs - 1 ) { aiMsg = -1; new_ai_msg_if_needed( &aiMsg ); }
      if ( !replied ) { snd_play( SND_REPLY ); replied = 1; }
      int oldLast = s_ndl > 1 ? s_ndl - 2 : 0;             // the last text line may grow
      append_text( code == 'T' ? arg : "\n" );
      layout_from( aiMsg );
      refresh_from( oldLast, wasBottom );
      state = "Qwen is answering...  (Esc = stop)";
    }
    else if ( code == 'I' ) {
      // mTCP here has one connection, and the reply is still streaming on it: note the
      // picture now and download it after the reply ends (it used to open a second
      // connection, which broke the reply with "No answer from ...").
      char pid[9];
      str_copy( pid, arg, sizeof( pid ) );
      const char *title = strlen( arg ) > 9 ? arg + 9 : "";
      if ( npending < 4 ) str_copy( pending[ npending++ ], pid, 9 );
      int wasBottom = ( s_top == max_top( ) );
      int pm = add_msg( MK_PIC, title, pid );
      layout_from( pm );
      refresh_from( s_msgs[pm].firstLine, wasBottom );
      aiMsg = -1;
    }
    else if ( code == 'E' ) {
      int em = add_msg( MK_ERR, arg, 0 );
      layout_from( em );
      refresh_from( s_msgs[em].firstLine, 1 );
      snd_play( SND_ERROR );
    }
    else if ( code == 'D' ) done = 1;
  }
  http_close( );
  net_timeout_ms = saveTimeout;

  // Now that the reply's connection is closed, fetch the pictures it announced
  int got = 0;
  for ( int k = 0; k < npending; k++ ) {
    if ( app_have_pic( pending[k] ) ) continue;
    app_status( "Downloading the picture..." );
    if ( app_download_pic( pending[k] ) == 0 ) {
      got++;
      for ( int t = 0; t < THUMBS; t++ ) if ( !strcmp( s_th[t].id, pending[k] ) ) s_th[t].id[0] = 0;
    }
    else {
      // Say so (it used to fail silently, leaving a grey box); a click on it tries again
      static char t[160];
      sprintf( t, "Could not download the picture: %s.  Click it to try again.", net_error( ) );
      add_msg( MK_ERR, t, 0 );
      snd_play( SND_ERROR );
    }
  }
  if ( got ) gallery_rescan( );      // the Gallery lists PICS only when told to, as after Create
  if ( npending ) {
    draw_panel( );
    snd_play( SND_IMAGE );
  }
  if ( cancelled ) add_msg( MK_ERR, "Stopped.", 0 );
  if ( s_cut ) add_msg( MK_ERR, "Part of this reply did not fit: the chat is full.  Press Send again to continue.", 0 );
  else if ( chat_room( ) && !s_warned ) {
    add_msg( MK_ERR, "This chat is nearly full.  Soon the older messages will be hidden (they stay saved), "
                     "or press New for a fresh chat.", 0 );
    s_warned = 1;
  }
  if ( s_nmsgs && s_msgs[ s_nmsgs - 1 ].kind == MK_ERR ) {
    int first = s_nmsgs - 1;
    while ( first > 0 && s_msgs[ first - 1 ].kind == MK_ERR && s_msgs[ first - 1 ].firstLine >= s_ndl ) first--;
    layout_from( first );
    refresh_from( s_msgs[ first ].firstLine, 1 );
  }
  save_chat( );
  s_busy = 0;
  gui_mouse_show( );
  chat_draw( );
  app_status( s_chatId[0] ? "Saved as %s.TCH.  PgUp/PgDn scroll, click a picture to view." : "Ready.", s_chatId );
}

// ---------------------------------------------------------------- screen API

void chat_init( void ) {
  s_tx = (char far *)_fmalloc( TX_SIZE );
  s_msgs = (Msg *)malloc( sizeof( Msg ) * MAX_MSGS );
  s_dl = (DLine far *)_fmalloc( sizeof( DLine ) * MAX_DL );
  memset( s_th, 0, sizeof( s_th ) );
  memset( s_ws, 0, sizeof( s_ws ) );
  int by = PH + 3;
  s_ws[0].type = W_EDIT;   s_ws[0].x = PX - 6;  s_ws[0].y = by; s_ws[0].w = 368; s_ws[0].h = 14;
  s_ws[0].buf = s_input; s_ws[0].size = sizeof( s_input );
  s_ws[1].type = W_BUTTON; s_ws[1].id = ID_SEND;  s_ws[1].x = 370; s_ws[1].y = by; s_ws[1].w = 60; s_ws[1].h = 14; s_ws[1].text = "Send"; s_ws[1].flags = WF_DEFAULT;
  s_ws[2].type = W_BUTTON; s_ws[2].id = ID_PIC;   s_ws[2].x = 434; s_ws[2].y = by; s_ws[2].w = 64; s_ws[2].h = 14; s_ws[2].text = "Picture";
  s_ws[3].type = W_BUTTON; s_ws[3].id = ID_CHATS; s_ws[3].x = 502; s_ws[3].y = by; s_ws[3].w = 60; s_ws[3].h = 14; s_ws[3].text = "Chats";
  s_ws[4].type = W_BUTTON; s_ws[4].id = ID_NEW;   s_ws[4].x = 566; s_ws[4].y = by; s_ws[4].w = 56; s_ws[4].h = 14; s_ws[4].text = "New";
  form_init( &s_form, "", 0, WIN_Y, 640, WIN_H, s_ws, 5 );
  s_form.focus = 0;
  clear_chat( );
  mkdir( cfg.chats );
}

int chat_ready( void ) { return s_tx && s_msgs && s_dl; }

void chat_attach( const char *id ) {
  str_copy( s_attach, id, sizeof( s_attach ) );
}

void chat_new( void ) { clear_chat( ); chat_draw( ); }
void chat_open( void ) { chats_dialog( ); }

void chat_pick_attach( void ) {
  char id[9];
  if ( app_pick_picture( "Attach a picture to your next message", id ) ) {
    chat_attach( id );
    chat_draw( );
  }
}

int chat_event( Event *e ) {
  if ( e->type == EV_KEY ) {
    switch ( e->key ) {
      case K_PGUP: set_top( s_top - ( ROWS - 1 ) ); return CMD_NONE;
      case K_PGDN: set_top( s_top + ( ROWS - 1 ) ); return CMD_NONE;
      case K_UP:   set_top( s_top - 1 ); return CMD_NONE;
      case K_DOWN: set_top( s_top + 1 ); return CMD_NONE;
    }
  }
  if ( e->type == EV_DOWN && ui_hit( e->x, e->y, PX, PY, PW, PH ) ) {
    if ( e->x >= PX + PW - 12 ) {                   // scroll bar
      set_top( e->y < PY + PH / 2 ? s_top - ( ROWS - 1 ) : s_top + ( ROWS - 1 ) );
      return CMD_NONE;
    }
    int r = ( e->y - PY - 2 ) / ROW_H, i = s_top + r;
    if ( i >= 0 && i < s_ndl && s_dl[i].type == DL_PIC ) {
      Msg &m = s_msgs[ s_dl[i].msg ];
      if ( m.pic[0] ) {
        int had = app_have_pic( m.pic );
        app_view_pic( m.pic );
        if ( !had && app_have_pic( m.pic ) ) chat_draw( );   // now with its thumbnail
      }
    }
    return CMD_NONE;
  }
  int id = form_event( &s_form, e );
  switch ( id ) {
    case ID_SEND:  send_message( ); break;
    case ID_PIC:   chat_pick_attach( ); break;
    case ID_CHATS: chats_dialog( ); break;
    case ID_NEW:   chat_new( ); break;
  }
  return CMD_NONE;
}
