// DeskMind - DeskMate-style GUI.  See gui.h.

#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <dos.h>

#include "gui.h"
#include "video.h"
#include "mouse.h"
#include "sound.h"
#include "sys.h"

void ( *gui_idle )( void ) = 0;

static int s_mouse = 0;
static int s_hide = 0;
static int s_mx = 320, s_my = 100, s_mb = 0;
static unsigned long s_lastClick = 0;
static int s_lcx = -100, s_lcy = -100;

// ====================================================================== events

int gui_init( void ) {
  s_mouse = mouse_init( ) != 0;
  if ( s_mouse ) {
    mouse_read( &s_mx, &s_my, &s_mb );
    cursor_show( s_mx, s_my );
  }
  s_hide = 0;
  return 0;
}

void gui_done( void ) {
  if ( s_mouse ) cursor_hide( );
}

int gui_has_mouse( void ) { return s_mouse; }

void gui_mouse_hide( void ) {
  if ( s_mouse && s_hide++ == 0 ) cursor_hide( );
}

void gui_mouse_show( void ) {
  if ( !s_mouse || s_hide == 0 ) return;
  if ( --s_hide == 0 ) cursor_show( s_mx, s_my );
}

// Keeps the pointer moving during long operations (network waits call this).
void gui_pump( void ) {
  if ( !s_mouse ) return;
  static unsigned long last = 0;
  unsigned long t = ticks( );
  if ( t == last ) return;
  last = t;
  int x, y, b;
  mouse_read( &x, &y, &b );
  if ( x != s_mx || y != s_my ) {
    s_mx = x; s_my = y;
    if ( s_hide == 0 ) cursor_move( x, y );
  }
}

void form_set_focus( Form *f, int i );

int gui_poll( Event *e ) {
  if ( gui_idle ) gui_idle( );
  e->type = EV_NONE;
  e->key = 0;
  e->x = s_mx; e->y = s_my; e->buttons = s_mb;
  int k = kbd_get( );
  if ( k ) { e->type = EV_KEY; e->key = k; return EV_KEY; }
  if ( !s_mouse ) return EV_NONE;

  int x, y, b;
  mouse_read( &x, &y, &b );
  e->x = x; e->y = y; e->buttons = b;
  if ( x != s_mx || y != s_my ) {
    s_mx = x; s_my = y;
    if ( s_hide == 0 ) cursor_move( x, y );
    if ( b & MB_LEFT ) e->type = EV_MOVE;
  }
  if ( ( b & MB_LEFT ) && !( s_mb & MB_LEFT ) ) {
    unsigned long t = ticks( );
    int dbl = ( t - s_lastClick < 9 ) && ( x - s_lcx < 6 && s_lcx - x < 6 ) && ( y - s_lcy < 4 && s_lcy - y < 4 );
    e->type = EV_DOWN;
    e->key = dbl ? 2 : 1;
    s_lastClick = dbl ? 0 : t;
    s_lcx = x; s_lcy = y;
  }
  else if ( !( b & MB_LEFT ) && ( s_mb & MB_LEFT ) ) {
    e->type = EV_UP;
  }
  s_mb = b;
  return e->type;
}

// ====================================================================== drawing

int ui_hit( int x, int y, int rx, int ry, int rw, int rh ) {
  return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

void ui_desktop( void ) {
  gui_mouse_hide( );
  // A light dither of blue and cyan feels less flat than a solid colour
  for ( int y = WORK_Y; y < STATUS_Y; y++ ) {
    unsigned char far *p = vid_line_ptr( y );
    unsigned char v = ( y & 1 ) ? 0x11 : 0x11;
    _fmemset( p, v, vid_pitch );
  }
  gui_mouse_show( );
}

void ui_bevel( int x, int y, int w, int h, int raised ) {
  unsigned char hi = raised ? WHITE : DGRAY, lo = raised ? DGRAY : WHITE;
  vid_hline( x, x + w - 1, y, hi );
  vid_fill( x, y, 2, h, hi );
  vid_hline( x, x + w - 1, y + h - 1, lo );
  vid_fill( x + w - 2, y, 2, h, lo );
}

void ui_sunken( int x, int y, int w, int h, unsigned char fill ) {
  vid_fill( x + 2, y + 1, w - 4, h - 2, fill );
  ui_bevel( x, y, w, h, 0 );
}

int ui_text_fit( int x, int y, int w, const char far *s, unsigned char fg, int bg ) {
  int n = w / 8;
  return vid_text_n( x, y, s, n, fg, bg );
}

void ui_window( int x, int y, int w, int h, const char *title, int active ) {
  gui_mouse_hide( );
  vid_fill( x + w, y + 3, 6, h, BLACK );                 // shadow
  vid_fill( x + 6, y + h, w, 3, BLACK );
  vid_fill( x, y, w, h, LGRAY );
  vid_rect( x, y, w, h, BLACK );
  vid_vline( x + 1, y, y + h - 1, BLACK );
  vid_vline( x + w - 2, y, y + h - 1, BLACK );
  vid_fill( x + 2, y + 1, w - 4, 10, active ? BLUE : DGRAY );
  if ( title && *title ) {
    int tw = (int)strlen( title ) * 8;
    if ( tw > w - 16 ) tw = w - 16;
    int tx = ( x + ( w - tw ) / 2 ) & ~1;
    ui_text_fit( tx, y + 2, w - 16, title, active ? WHITE : LGRAY, -1 );
  }
  vid_hline( x + 2, x + w - 3, y + 11, BLACK );
  gui_mouse_show( );
}

void ui_button( int x, int y, int w, int h, const char *label, int flags ) {
  gui_mouse_hide( );
  if ( flags & BF_DEFAULT ) {
    vid_rect( x, y, w, h, BLACK );
    vid_vline( x + 1, y, y + h - 1, BLACK );
    vid_vline( x + w - 2, y, y + h - 1, BLACK );
    x += 2; y += 1; w -= 4; h -= 2;
  }
  vid_fill( x, y, w, h, LGRAY );
  ui_bevel( x, y, w, h, !( flags & BF_PRESSED ) );
  int tw = (int)strlen( label ) * 8;
  int tx = ( x + ( w - tw ) / 2 + ( ( flags & BF_PRESSED ) ? 2 : 0 ) ) & ~1;
  int ty = y + ( h - 8 ) / 2 + ( ( flags & BF_PRESSED ) ? 1 : 0 );
  unsigned char fg = ( flags & BF_DISABLED ) ? DGRAY : ( flags & BF_FOCUS ) ? BLUE : BLACK;
  ui_text_fit( tx, ty, w - 4, label, fg, -1 );
  if ( flags & BF_FOCUS ) vid_hline( tx, tx + tw - 1, ty + 9 < y + h - 1 ? ty + 9 : y + h - 2, BLUE );
  gui_mouse_show( );
}

void ui_status( const char *text ) {
  gui_mouse_hide( );
  vid_fill( 0, STATUS_Y, vid_w, 200 - STATUS_Y, LGRAY );
  vid_hline( 0, vid_w - 1, STATUS_Y, DGRAY );
  ui_text_fit( 4, STATUS_Y + 1, vid_w - 8, text, BLACK, -1 );
  gui_mouse_show( );
}

unsigned char far *ui_save( int x, int y, int w, int h ) {
  unsigned char far *buf = (unsigned char far *)_fmalloc( vid_save_size( w, h ) );
  if ( buf ) {
    gui_mouse_hide( );
    vid_save( x, y, w, h, buf );
    gui_mouse_show( );
  }
  return buf;
}

void ui_restore( int x, int y, int w, int h, unsigned char far *buf ) {
  if ( !buf ) return;
  gui_mouse_hide( );
  vid_restore( x, y, w, h, buf );
  gui_mouse_show( );
  _ffree( buf );
}

// ====================================================================== memo layout

#define MAX_LINES 256
static unsigned s_starts[ MAX_LINES ];

// Word-wraps s into lines of at most cols characters; returns the line count.
static int wrap( const char far *s, int cols, unsigned *starts, int max ) {
  unsigned pos = 0;
  int n = 0;
  if ( cols < 1 ) cols = 1;
  for ( ;; ) {
    if ( n >= max ) break;
    starts[ n++ ] = pos;
    unsigned end = pos;
    while ( s[end] && s[end] != '\n' && end - pos < (unsigned)cols ) end++;
    if ( !s[end] ) break;
    if ( s[end] == '\n' ) { pos = end + 1; continue; }
    // line is full: break after the last space, if any
    unsigned sp = end;
    while ( sp > pos && s[ sp - 1 ] != ' ' ) sp--;
    pos = ( sp > pos ) ? sp : end;
    if ( !s[pos] ) break;
  }
  return n;
}

static int line_len( const char far *s, unsigned *starts, int n, int i ) {
  unsigned end = ( i + 1 < n ) ? starts[ i + 1 ] : (unsigned)_fstrlen( s );
  int len = (int)( end - starts[i] );
  while ( len > 0 && ( s[ starts[i] + len - 1 ] == '\n' ) ) len--;
  return len;
}

static int line_of( unsigned cur, unsigned *starts, int n ) {
  int i = n - 1;
  while ( i > 0 && starts[i] > cur ) i--;
  return i;
}

// ====================================================================== widgets

int form_client_x( Form *f ) { return f->x + 6; }
int form_client_y( Form *f ) { return f->y + 14; }

void form_init( Form *f, const char *title, int x, int y, int w, int h, Widget *ws, int n ) {
  f->title = title;
  f->x = x & ~1; f->y = y; f->w = ( w + 1 ) & ~1; f->h = h;
  f->w_ = ws; f->n = n;
  f->focus = -1;
  f->dirty = 0;
  for ( int i = 0; i < n; i++ ) {
    Widget &w_ = ws[i];
    if ( w_.type != W_LABEL && !( w_.flags & ( WF_HIDDEN | WF_DISABLED ) ) && f->focus < 0 ) f->focus = i;
  }
}

static int focusable( Widget &w ) {
  return w.type != W_LABEL && !( w.flags & ( WF_HIDDEN | WF_DISABLED ) );
}

void sb_thumb( int h, int total, int visible, int top, int *ty, int *th ) {
  // long math throughout: 130 * 500 overflows an int (the 0.7.1 bug)
  int t = total > visible ? (int)( (long)h * visible / total ) : h;
  if ( t < 6 ) t = 6;
  if ( t > h ) t = h;
  *th = t;
  *ty = total > visible ? (int)( (long)( h - t ) * top / ( total - visible ) ) : 0;
}

void ui_scrollbar( int x, int y, int h, int total, int visible, int top ) {
  vid_fill( x, y, 10, h, LGRAY );
  vid_fill( x, y, 2, h, DGRAY );
  if ( total <= visible ) return;
  int ty, th;
  sb_thumb( h, total, visible, top, &ty, &th );
  vid_fill( x + 2, y + ty, 8, th, DGRAY );
  ui_bevel( x + 2, y + ty, 8, th, 1 );
}

static int sb_clamp( int top, int total, int visible ) {
  if ( top > total - visible ) top = total - visible;
  if ( top < 0 ) top = 0;
  return top;
}

void sb_track( int x, int y, int h, int total, int visible, int top, int page,
               int mx, int my, sb_apply_fn apply, void *ctx ) {
  (void)x; (void)mx;
  if ( total <= visible ) return;
  int ty, th;
  sb_thumb( h, total, visible, top, &ty, &th );
  Event e;
  if ( my >= y + ty && my < y + ty + th ) {
    // Drag the box.  Each redraw finishes before the next poll, which then reads where the
    // pointer is now, so a slow redraw skips positions instead of falling behind.
    int grab = my - ( y + ty ), span = h - th, pending = 0;
    unsigned long moved = ticks( );
    for ( ;; ) {
      gui_poll( &e );
      if ( e.type == EV_UP ) break;
      if ( e.type == EV_MOVE && span > 0 ) {
        long off = (long)( e.y - grab - y );
        int t = sb_clamp( (int)( ( off * ( total - visible ) + span / 2 ) / span ), total, visible );
        if ( t != top ) { top = t; apply( ctx, top, 1 ); pending = 1; }
        moved = ticks( );
      }
      else if ( pending && ticks( ) - moved >= 2 ) { apply( ctx, top, 0 ); pending = 0; }   // the pointer paused
    }
    if ( pending ) apply( ctx, top, 0 );
    return;
  }
  // Above or below the box: a page now, then repeat while held until the box reaches the pointer
  int dir = my < y + ty ? -1 : 1;
  unsigned long start = ticks( ), last = start;
  int py = my;
  for ( int first = 1;; first = 0 ) {
    if ( first || ( ticks( ) - start >= 6 && ticks( ) != last ) ) {
      last = ticks( );
      sb_thumb( h, total, visible, top, &ty, &th );
      int reached = dir < 0 ? py >= y + ty : py < y + ty + th;
      int t = sb_clamp( top + dir * page, total, visible );
      if ( !reached && t != top ) { top = t; apply( ctx, top, 0 ); }
    }
    gui_poll( &e );
    if ( e.type == EV_UP ) break;
    py = e.y;
  }
}

static int s_full = 0;         // 1 = draw widgets from scratch (form_draw), 0 = update in place
#define MEMO_ROWS 16
#define MEMO_COLS 80

static void draw_edit( Widget &w, int cx, int cy, int focused ) {
  if ( s_full ) ui_sunken( cx, cy, w.w, w.h, WHITE );
  int cols = ( w.w - 8 ) / 8;
  unsigned len = (unsigned)_fstrlen( w.buf );
  if ( w.cur > len ) w.cur = len;
  if ( w.cur < w.top ) w.top = w.cur;
  if ( w.cur >= w.top + cols ) w.top = w.cur - cols + 1;
  // No clearing: every cell is redrawn opaquely (unchanged cells look identical, so
  // nothing flickers), and only the two cursor lines above and below the text are wiped.
  int ty = cy + ( w.h - 8 ) / 2;
  for ( int c = 0; c < cols; c++ ) {
    unsigned k = w.top + c;
    vid_char( cx + 4 + c * 8, ty, (unsigned char)( k < len ? w.buf[k] : ' ' ), BLACK, WHITE );
  }
  vid_fill( cx + 4, ty - 1, cols * 8 + 2, 1, WHITE );
  vid_fill( cx + 4, ty + 8, cols * 8 + 2, 1, WHITE );
  vid_fill( cx + 4 + cols * 8, ty - 1, 2, 10, WHITE );
  if ( focused && !( w.flags & WF_READONLY ) ) {
    int px = cx + 4 + (int)( w.cur - w.top ) * 8;
    vid_fill( px, ty + 8, 8, 1, BLUE );
    vid_fill( px, ty - 1, 2, 10, BLUE );
  }
}

static void draw_scrollbar_if( Widget &w, int x, int y, int h, int total, int visible, int top ) {
  if ( !s_full && w.sbn == total && w.sbtop == top ) return;
  ui_scrollbar( x, y, h, total, visible, top );
  w.sbn = total; w.sbtop = top;
}

static int memo_cols( Widget &w ) { int c = ( w.w - 18 ) / 8; return c > MEMO_COLS ? MEMO_COLS : c; }
static int memo_rows( Widget &w ) { int r = ( w.h - 4 ) / ROW_H; return r > MEMO_ROWS ? MEMO_ROWS : r; }

// What the memo currently shows, so a keystroke only redraws the cells that changed
static const Widget *s_mw = 0;
static unsigned char s_mcache[ MEMO_ROWS ][ MEMO_COLS ];
static int s_mCurR = -1, s_mCurC = -1;

static void draw_memo( Widget &w, int cx, int cy, int focused ) {
  int cols = memo_cols( w ), rows = memo_rows( w );
  if ( s_full || s_mw != &w ) {
    ui_sunken( cx, cy, w.w, w.h, WHITE );
    memset( s_mcache, 0xFF, sizeof( s_mcache ) );
    s_mw = &w; s_mCurR = -1;
    w.sbn = -1;
  }
  int n = wrap( w.buf, cols, s_starts, MAX_LINES );
  int cl = line_of( w.cur, s_starts, n );
  if ( cl < (int)w.top ) w.top = cl;
  if ( cl >= (int)w.top + rows ) w.top = cl - rows + 1;
  if ( (int)w.top > n - 1 ) w.top = n > rows ? n - rows : 0;

  // wipe the old cursor bar and make sure its cell is redrawn
  if ( s_mCurR >= 0 ) {
    vid_fill( cx + 4 + s_mCurC * 8, cy + 2 + s_mCurR * ROW_H, 2, 10, WHITE );
    if ( s_mCurC < cols ) s_mcache[ s_mCurR ][ s_mCurC ] = 0xFF;
    s_mCurR = -1;
  }
  for ( int r = 0; r < rows; r++ ) {
    int i = (int)w.top + r;
    const char far *line = ( i < n ) ? w.buf + s_starts[i] : "";
    int len = ( i < n ) ? line_len( w.buf, s_starts, n, i ) : 0;
    for ( int c = 0; c < cols; c++ ) {
      unsigned char ch = (unsigned char)( c < len ? line[c] : ' ' );
      if ( s_mcache[r][c] != ch ) {
        vid_char( cx + 4 + c * 8, cy + 3 + r * ROW_H, ch, BLACK, WHITE );
        s_mcache[r][c] = ch;
      }
    }
  }
  draw_scrollbar_if( w, cx + w.w - 12, cy + 1, w.h - 2, n, rows, (int)w.top );
  if ( focused && !( w.flags & WF_READONLY ) ) {
    int row = cl - (int)w.top;
    int col = (int)( w.cur - s_starts[cl] );
    if ( col > cols ) col = cols;
    vid_fill( cx + 4 + col * 8, cy + 2 + row * ROW_H, 2, 10, BLUE );
    s_mCurR = row; s_mCurC = col;
  }
}

static int list_rows( Widget &w ) { return ( w.h - 4 ) / ROW_H; }

static void draw_list( Widget &w, int cx, int cy, int focused ) {
  if ( s_full ) { ui_sunken( cx, cy, w.w, w.h, WHITE ); w.sbn = -1; }
  int rows = list_rows( w );
  if ( w.sel >= w.count ) w.sel = w.count - 1;
  if ( w.sel < 0 && w.count ) w.sel = 0;
  if ( !( w.flags & WF_FREEVIEW ) ) {
    if ( w.sel >= 0 && w.sel < w.ltop ) w.ltop = w.sel;
    if ( w.sel >= w.ltop + rows ) w.ltop = w.sel - rows + 1;
  }
  if ( w.ltop > w.count - rows ) w.ltop = w.count - rows > 0 ? w.count - rows : 0;
  if ( w.ltop < 0 ) w.ltop = 0;
  int iw = w.w - 18;
  int cols = ( iw - 2 ) / 8;
  for ( int r = 0; r < rows; r++ ) {
    int i = w.ltop + r;
    int y = cy + 2 + r * ROW_H;
    int sel = ( i == w.sel && i < w.count );
    unsigned char bg = sel ? ( focused ? BLUE : DGRAY ) : WHITE;
    unsigned char fg = sel ? WHITE : BLACK;
    // Opaque, padded text over the whole row: rows that did not change look identical
    const char far *t = ( i < w.count && w.item ) ? w.item( w.ctx, i ) : "";
    vid_hline( cx + 2, cx + 3 + iw, y, bg );
    vid_hline( cx + 2, cx + 3 + iw, y + 9, bg );
    vid_fill( cx + 2, y + 1, 2, 8, bg );
    int c = 0;
    for ( ; c < cols && t[c]; c++ ) vid_char( cx + 4 + c * 8, y + 1, (unsigned char)t[c], fg, bg );
    for ( ; c < cols; c++ ) vid_char( cx + 4 + c * 8, y + 1, ' ', fg, bg );
    vid_fill( cx + 4 + cols * 8, y + 1, iw - cols * 8, 8, bg );
  }
  draw_scrollbar_if( w, cx + w.w - 12, cy + 1, w.h - 2, w.count, rows, w.ltop );
}

void form_draw_widget( Form *f, int i ) {
  Widget &w = f->w_[i];
  if ( w.flags & WF_HIDDEN ) return;
  int cx = form_client_x( f ) + w.x, cy = form_client_y( f ) + w.y;
  int focused = ( f->focus == i );
  gui_mouse_hide( );
  switch ( w.type ) {
    case W_LABEL:
      vid_fill( cx, cy, w.w, w.h, LGRAY );
      ui_text_fit( cx, cy + ( w.h - 8 ) / 2, w.w, w.text, w.fg ? w.fg : BLACK, -1 );
      break;
    case W_BUTTON: {
      int fl = ( w.flags & WF_DEFAULT ) ? BF_DEFAULT : 0;
      if ( focused ) fl |= BF_FOCUS;
      if ( w.flags & WF_DISABLED ) fl |= BF_DISABLED;
      ui_button( cx, cy, w.w, w.h, w.text, fl );
      break;
    }
    case W_CHECK:
      vid_fill( cx, cy, w.w, w.h, LGRAY );
      ui_sunken( cx, cy + 1, 14, 9, WHITE );
      if ( w.checked ) vid_char( cx + 4, cy + 1, 0xFB, BLACK, -1 );   // CP437 check mark
      ui_text_fit( cx + 20, cy + 1, w.w - 20, w.text, focused ? BLUE : BLACK, -1 );
      break;
    case W_EDIT: draw_edit( w, cx, cy, focused ); break;
    case W_MEMO: draw_memo( w, cx, cy, focused ); break;
    case W_LIST: draw_list( w, cx, cy, focused ); break;
  }
  gui_mouse_show( );
}

void form_draw( Form *f ) {
  gui_mouse_hide( );
  ui_window( f->x, f->y, f->w, f->h, f->title, 1 );
  s_full = 1;
  for ( int i = 0; i < f->n; i++ ) form_draw_widget( f, i );
  s_full = 0;
  gui_mouse_show( );
}

void list_set( Widget *w, int count, list_item_fn item, void *ctx ) {
  w->count = count; w->item = item; w->ctx = ctx;
  w->flags &= ~WF_FREEVIEW;
  if ( w->sel >= count ) w->sel = count - 1;
  if ( w->sel < 0 && count ) w->sel = 0;
}

void memo_to_end( Widget *w ) {
  w->cur = (unsigned)_fstrlen( w->buf );
}

static void set_focus( Form *f, int i ) {
  if ( i == f->focus ) return;
  int old = f->focus;
  f->focus = i;
  if ( old >= 0 ) form_draw_widget( f, old );
  if ( i >= 0 ) form_draw_widget( f, i );
}

void form_set_focus( Form *f, int i ) { set_focus( f, i ); }

static void move_focus( Form *f, int dir ) {
  if ( f->n == 0 ) return;
  int i = f->focus;
  for ( int k = 0; k < f->n; k++ ) {
    i = ( i + dir + f->n ) % f->n;
    if ( focusable( f->w_[i] ) ) { set_focus( f, i ); return; }
  }
}

// ---------------------------------------------------------------- text editing

static int edit_key( Widget &w, int k ) {
  if ( w.flags & WF_READONLY ) return 0;
  unsigned len = (unsigned)_fstrlen( w.buf );
  if ( w.cur > len ) w.cur = len;
  if ( k == K_LEFT )  { if ( w.cur ) w.cur--; return 1; }
  if ( k == K_RIGHT ) { if ( w.cur < len ) w.cur++; return 1; }
  if ( k == K_BS ) {
    if ( !w.cur ) return 1;
    _fmemmove( w.buf + w.cur - 1, w.buf + w.cur, len - w.cur + 1 );
    w.cur--;
    return 2;
  }
  if ( k == K_DEL ) {
    if ( w.cur < len ) _fmemmove( w.buf + w.cur, w.buf + w.cur + 1, len - w.cur );
    return 2;
  }
  if ( k >= 32 && k <= 254 && k != 127 ) {
    if ( len + 1 >= w.size ) { snd_play( SND_CLICK ); return 1; }
    _fmemmove( w.buf + w.cur + 1, w.buf + w.cur, len - w.cur + 1 );
    w.buf[ w.cur++ ] = (char)k;
    return 2;
  }
  return 0;
}

static int edit_handle( Widget &w, int k ) {
  unsigned len = (unsigned)_fstrlen( w.buf );
  if ( k == K_HOME ) { w.cur = 0; return 1; }
  if ( k == K_END )  { w.cur = len; return 1; }
  return edit_key( w, k );
}

static int memo_handle( Widget &w, int k ) {
  int cols = memo_cols( w ), rows = memo_rows( w );
  int n = wrap( w.buf, cols, s_starts, MAX_LINES );
  int cl = line_of( w.cur, s_starts, n );
  int col = (int)( w.cur - s_starts[cl] );
  int target = -1;
  if ( k == K_UP )   target = cl - 1;
  if ( k == K_DOWN ) target = cl + 1;
  if ( k == K_PGUP ) target = cl - rows;
  if ( k == K_PGDN ) target = cl + rows;
  if ( target != -1 || k == K_UP || k == K_PGUP ) {
    if ( target < 0 ) target = 0;
    if ( target > n - 1 ) target = n - 1;
    int ll = line_len( w.buf, s_starts, n, target );
    w.cur = s_starts[target] + ( col < ll ? col : ll );
    return 1;
  }
  if ( k == K_HOME ) { w.cur = s_starts[cl]; return 1; }
  if ( k == K_END )  { w.cur = s_starts[cl] + line_len( w.buf, s_starts, n, cl ); return 1; }
  return edit_key( w, k );
}

static int list_handle( Widget &w, int k ) {
  int rows = list_rows( w );
  int old = w.sel;
  if ( k == K_UP ) w.sel--;
  else if ( k == K_DOWN ) w.sel++;
  else if ( k == K_PGUP ) w.sel -= rows;
  else if ( k == K_PGDN ) w.sel += rows;
  else if ( k == K_HOME ) w.sel = 0;
  else if ( k == K_END ) w.sel = w.count - 1;
  else return 0;
  w.flags &= ~WF_FREEVIEW;                 // keys bring the view back to the selection
  if ( w.sel >= w.count ) w.sel = w.count - 1;
  if ( w.sel < 0 ) w.sel = 0;
  return old != w.sel ? 1 : 1;
}

// ---------------------------------------------------------------- events

// The list whose scroll bar is being tracked (sb_track's callback gets only the widget)
static Form *s_sbForm = 0;
static int s_sbIdx = 0;

static void list_scrolled( void *ctx, int top, int ) {
  Widget &w = *(Widget *)ctx;
  w.ltop = top;
  w.flags |= WF_FREEVIEW;                  // the highlight stays where it is
  form_draw_widget( s_sbForm, s_sbIdx );
}

static int cancel_id( Form *f ) {
  for ( int i = 0; i < f->n; i++ )
    if ( f->w_[i].type == W_BUTTON && ( f->w_[i].flags & WF_CANCEL ) ) return f->w_[i].id;
  return -1;
}

static int default_id( Form *f ) {
  for ( int i = 0; i < f->n; i++ )
    if ( f->w_[i].type == W_BUTTON && ( f->w_[i].flags & WF_DEFAULT ) && !( f->w_[i].flags & WF_DISABLED ) )
      return f->w_[i].id;
  return 0;
}

static void flash_button( Form *f, int i ) {
  Widget &w = f->w_[i];
  int cx = form_client_x( f ) + w.x, cy = form_client_y( f ) + w.y;
  ui_button( cx, cy, w.w, w.h, w.text, BF_PRESSED | ( ( w.flags & WF_DEFAULT ) ? BF_DEFAULT : 0 ) );
  snd_play( SND_CLICK );
  wait_ticks( 2 );
  form_draw_widget( f, i );
}

// Tracks a mouse press on a button until release; 1 if released inside
static int track_button( Form *f, int i ) {
  Widget &w = f->w_[i];
  int cx = form_client_x( f ) + w.x, cy = form_client_y( f ) + w.y;
  int base = ( w.flags & WF_DEFAULT ) ? BF_DEFAULT : 0;
  int inside = 1;
  ui_button( cx, cy, w.w, w.h, w.text, base | BF_PRESSED );
  Event e;
  for ( ;; ) {
    gui_poll( &e );
    if ( e.type == EV_UP ) break;
    int now = ui_hit( e.x, e.y, cx, cy, w.w, w.h );
    if ( now != inside ) {
      inside = now;
      ui_button( cx, cy, w.w, w.h, w.text, base | ( inside ? BF_PRESSED : BF_FOCUS ) );
    }
  }
  form_draw_widget( f, i );
  if ( inside ) snd_play( SND_CLICK );
  return inside;
}

int form_event( Form *f, Event *e ) {
  if ( e->type == EV_KEY ) {
    int k = e->key;
    Widget *fw = ( f->focus >= 0 ) ? &f->w_[ f->focus ] : 0;
    if ( k == K_TAB ) { move_focus( f, 1 ); e->type = EV_NONE; return 0; }
    if ( k == K_SHTAB ) { move_focus( f, -1 ); e->type = EV_NONE; return 0; }
    if ( k == K_ESC ) { e->type = EV_NONE; return cancel_id( f ); }
    if ( fw ) {
      int r = 0;
      if ( fw->type == W_BUTTON && ( k == K_ENTER || k == ' ' ) ) {
        flash_button( f, f->focus );
        e->type = EV_NONE;
        return fw->id;
      }
      if ( fw->type == W_CHECK && ( k == ' ' || k == K_ENTER ) ) {
        fw->checked = !fw->checked;
        f->dirty = 1;
        form_draw_widget( f, f->focus );
        e->type = EV_NONE;
        return 0;
      }
      if ( fw->type == W_LIST && k == K_ENTER && fw->count > 0 ) { e->type = EV_NONE; return fw->id; }
      if ( fw->type == W_EDIT ) r = edit_handle( *fw, k );
      else if ( fw->type == W_MEMO ) r = memo_handle( *fw, k );
      else if ( fw->type == W_LIST ) r = list_handle( *fw, k );
      if ( r ) {
        if ( r == 2 ) f->dirty = 1;
        form_draw_widget( f, f->focus );
        e->type = EV_NONE;
        return 0;
      }
    }
    if ( k == K_ENTER ) {
      int d = default_id( f );
      if ( d ) {
        for ( int i = 0; i < f->n; i++ ) if ( f->w_[i].id == d && f->w_[i].type == W_BUTTON ) flash_button( f, i );
        e->type = EV_NONE;
        return d;
      }
    }
    return 0;
  }

  if ( e->type == EV_DOWN ) {
    int bx = form_client_x( f ), by = form_client_y( f );
    for ( int i = 0; i < f->n; i++ ) {
      Widget &w = f->w_[i];
      if ( !focusable( w ) ) continue;
      int cx = bx + w.x, cy = by + w.y;
      if ( !ui_hit( e->x, e->y, cx, cy, w.w, w.h ) ) continue;
      int dbl = ( e->key == 2 );
      e->type = EV_NONE;
      set_focus( f, i );
      switch ( w.type ) {
        case W_BUTTON:
          return track_button( f, i ) ? w.id : 0;
        case W_CHECK:
          w.checked = !w.checked;
          f->dirty = 1;
          form_draw_widget( f, i );
          return 0;
        case W_EDIT: {
          int c = ( e->x - cx - 4 ) / 8;
          if ( c < 0 ) c = 0;
          w.cur = w.top + c;
          form_draw_widget( f, i );
          return 0;
        }
        case W_MEMO: {
          if ( e->x >= cx + w.w - 12 ) {                        // scroll bar: page
            int rows = memo_rows( w );
            int n = wrap( w.buf, memo_cols( w ), s_starts, MAX_LINES ), ty, th;
            sb_thumb( w.h - 2, n, rows, (int)w.top, &ty, &th );
            memo_handle( w, e->y < cy + 1 + ty ? K_PGUP : K_PGDN );
            (void)rows;
            form_draw_widget( f, i );
            return 0;
          }
          int n = wrap( w.buf, memo_cols( w ), s_starts, MAX_LINES );
          int line = (int)w.top + ( e->y - cy - 2 ) / ROW_H;
          if ( line > n - 1 ) line = n - 1;
          if ( line < 0 ) line = 0;
          int c = ( e->x - cx - 4 ) / 8, ll = line_len( w.buf, s_starts, n, line );
          if ( c < 0 ) c = 0;
          w.cur = s_starts[line] + ( c < ll ? c : ll );
          form_draw_widget( f, i );
          return 0;
        }
        case W_LIST: {
          if ( e->x >= cx + w.w - 12 ) {
            s_sbForm = f; s_sbIdx = i;
            sb_track( cx + w.w - 12, cy + 1, w.h - 2, w.count, list_rows( w ), w.ltop, list_rows( w ) - 1,
                      e->x, e->y, list_scrolled, &w );
            return 0;
          }
          int r = ( e->y - cy - 2 ) / ROW_H;
          int idx = w.ltop + r;
          if ( idx >= 0 && idx < w.count ) {
            int same = ( idx == w.sel );
            w.sel = idx;
            w.flags &= ~WF_FREEVIEW;
            form_draw_widget( f, i );
            if ( dbl && same ) return w.id;
          }
          return 0;
        }
      }
      return 0;
    }
  }
  return 0;
}

int form_run( Form *f ) {
  Event e;
  for ( ;; ) {
    gui_poll( &e );
    if ( e.type == EV_NONE ) continue;
    int r = form_event( f, &e );
    if ( r ) return r;
  }
}

// ====================================================================== dialogs

static int count_parts( const char *s, char sep ) {
  int n = 1;
  for ( ; *s; s++ ) if ( *s == sep ) n++;
  return n;
}

static void part( const char *s, char sep, int idx, char *out, int size ) {
  while ( idx > 0 && *s ) { if ( *s++ == sep ) idx--; }
  int n = 0;
  while ( *s && *s != sep && n < size - 1 ) out[n++] = *s++;
  out[n] = 0;
}

int msg_box( const char *title, const char *text, const char *buttons ) {
  static Widget ws[ 16 ];
  static char labels[ 4 ][ 16 ];
  static char lines[ 10 ][ 72 ];
  unsigned starts[ 10 ];
  int nlines = wrap( text, 64, starts, 10 );
  int maxw = 0;
  for ( int i = 0; i < nlines; i++ ) {
    int len = line_len( text, starts, nlines, i );
    if ( len > 71 ) len = 71;
    memcpy( lines[i], text + starts[i], len );
    lines[i][len] = 0;
    if ( len > maxw ) maxw = len;
  }
  int nb = count_parts( buttons, '|' );
  if ( nb > 4 ) nb = 4;
  int bw = 72, gap = 12;
  for ( int i = 0; i < nb; i++ ) {           // wide enough for the longest label
    char l[16];
    part( buttons, '|', i, l, sizeof( l ) );
    int need = (int)strlen( l ) * 8 + 24;
    if ( need > bw ) bw = ( need + 1 ) & ~1;
  }
  int w = maxw * 8 + 32;
  int need = nb * bw + ( nb - 1 ) * gap + 24;
  if ( w < need ) w = need;
  if ( w < 200 ) w = 200;
  int h = 14 + nlines * ROW_H + 30;
  int x = ( ( vid_w - w ) / 2 ) & ~1, y = ( 200 - h ) / 2;

  int n = 0;
  for ( int i = 0; i < nlines; i++ ) {
    memset( &ws[n], 0, sizeof( Widget ) );
    ws[n].type = W_LABEL; ws[n].x = 10; ws[n].y = 4 + i * ROW_H; ws[n].w = w - 28; ws[n].h = ROW_H;
    ws[n].text = lines[i];
    n++;
  }
  int bx = ( w - 12 - ( nb * bw + ( nb - 1 ) * gap ) ) / 2;
  for ( int i = 0; i < nb; i++ ) {
    part( buttons, '|', i, labels[i], sizeof( labels[i] ) );
    memset( &ws[n], 0, sizeof( Widget ) );
    ws[n].type = W_BUTTON; ws[n].id = i + 1;
    ws[n].x = ( bx + i * ( bw + gap ) ) & ~1; ws[n].y = 8 + nlines * ROW_H; ws[n].w = bw; ws[n].h = 14;
    ws[n].text = labels[i];
    ws[n].flags = ( i == 0 ? WF_DEFAULT : 0 ) | ( i == nb - 1 && nb > 1 ? WF_CANCEL : 0 );
    n++;
  }
  Form f;
  unsigned char far *save = ui_save( x, y, w + 6, h + 3 );
  form_init( &f, title, x, y, w, h, ws, n );
  form_draw( &f );
  int r = form_run( &f );
  ui_restore( x, y, w + 6, h + 3, save );
  return r < 0 ? 0 : r;
}

int input_box( const char *title, const char *prompt, char *buf, unsigned size ) {
  static Widget ws[4];
  memset( ws, 0, sizeof( ws ) );
  int w = 440, h = 64;
  int x = ( ( vid_w - w ) / 2 ) & ~1, y = 60;
  ws[0].type = W_LABEL; ws[0].x = 8;  ws[0].y = 2;  ws[0].w = w - 28; ws[0].h = ROW_H; ws[0].text = prompt;
  ws[1].type = W_EDIT;  ws[1].x = 8;  ws[1].y = 14; ws[1].w = w - 28; ws[1].h = 13;
  ws[1].buf = buf; ws[1].size = size; ws[1].cur = (unsigned)strlen( buf );
  ws[2].type = W_BUTTON; ws[2].id = 1; ws[2].x = w - 196; ws[2].y = 32; ws[2].w = 80; ws[2].h = 14;
  ws[2].text = "OK"; ws[2].flags = WF_DEFAULT;
  ws[3].type = W_BUTTON; ws[3].id = 2; ws[3].x = w - 104; ws[3].y = 32; ws[3].w = 80; ws[3].h = 14;
  ws[3].text = "Cancel"; ws[3].flags = WF_CANCEL;
  Form f;
  unsigned char far *save = ui_save( x, y, w + 6, h + 3 );
  form_init( &f, title, x, y, w, h, ws, 4 );
  form_draw( &f );
  int r = form_run( &f );
  ui_restore( x, y, w + 6, h + 3, save );
  return r == 1;
}

int memo_box( const char *title, const char *prompt, char far *buf, unsigned size ) {
  static Widget ws[4];
  memset( ws, 0, sizeof( ws ) );
  int w = 600, h = 130;
  int x = ( ( vid_w - w ) / 2 ) & ~1, y = 30;
  ws[0].type = W_LABEL; ws[0].x = 8; ws[0].y = 2; ws[0].w = w - 28; ws[0].h = ROW_H; ws[0].text = prompt;
  ws[1].type = W_MEMO;  ws[1].x = 8; ws[1].y = 14; ws[1].w = w - 28; ws[1].h = 4 + 7 * ROW_H;
  ws[1].buf = buf; ws[1].size = size; ws[1].cur = (unsigned)_fstrlen( buf );
  ws[2].type = W_BUTTON; ws[2].id = 1; ws[2].x = w - 196; ws[2].y = 94; ws[2].w = 80; ws[2].h = 14;
  ws[2].text = "OK"; ws[2].flags = WF_DEFAULT;
  ws[3].type = W_BUTTON; ws[3].id = 2; ws[3].x = w - 104; ws[3].y = 94; ws[3].w = 80; ws[3].h = 14;
  ws[3].text = "Cancel"; ws[3].flags = WF_CANCEL;
  Form f;
  unsigned char far *save = ui_save( x, y, w + 6, h + 3 );
  form_init( &f, title, x, y, w, h, ws, 4 );
  form_draw( &f );
  int r = form_run( &f );
  ui_restore( x, y, w + 6, h + 3, save );
  return r == 1;
}

static unsigned char far *s_busySave = 0;
static int s_bx, s_by, s_bw, s_bh;

void busy_begin( const char *title, const char *text ) {
  s_bw = 360; s_bh = 40;
  s_bx = ( ( vid_w - s_bw ) / 2 ) & ~1; s_by = 76;
  s_busySave = ui_save( s_bx, s_by, s_bw + 6, s_bh + 3 );
  ui_window( s_bx, s_by, s_bw, s_bh, title, 1 );
  busy_text( text );
}

void busy_text( const char *text ) {
  gui_mouse_hide( );
  vid_fill( s_bx + 4, s_by + 16, s_bw - 8, 20, LGRAY );
  ui_text_fit( s_bx + 12, s_by + 22, s_bw - 24, text, BLACK, -1 );
  gui_mouse_show( );
}

void busy_end( void ) {
  if ( s_busySave ) ui_restore( s_bx, s_by, s_bw + 6, s_bh + 3, s_busySave );
  s_busySave = 0;
}

// ====================================================================== menu bar

static const Menu *s_menus = 0;
static int s_nmenus = 0;
static const char *s_right = "";

static int title_x( int i ) {
  int x = 8;
  for ( int k = 0; k < i; k++ ) x += (int)strlen( s_menus[k].title ) * 8 + 16;
  return x;
}

void menubar_set( const Menu *menus, int n, const char *rightText ) {
  s_menus = menus; s_nmenus = n; s_right = rightText ? rightText : "";
}

static void draw_title( int i, int active ) {
  int x = title_x( i ) - 6, w = (int)strlen( s_menus[i].title ) * 8 + 12;
  vid_fill( x, 0, w, MENU_H - 1, active ? BLUE : LGRAY );
  vid_text( x + 6, 1, s_menus[i].title, active ? WHITE : BLACK, -1 );
  vid_hline( x + 6, x + 13, 9, active ? WHITE : RED );          // Alt+first letter
}

void menubar_draw( void ) {
  gui_mouse_hide( );
  vid_fill( 0, 0, vid_w, MENU_H - 1, LGRAY );
  vid_hline( 0, vid_w - 1, MENU_H - 1, BLACK );
  for ( int i = 0; i < s_nmenus; i++ ) draw_title( i, 0 );
  int rw = (int)strlen( s_right ) * 8;
  vid_text( ( vid_w - rw - 8 ) & ~1, 1, s_right, DGRAY, -1 );
  gui_mouse_show( );
}

int menubar_hit( int x, int y ) {
  if ( y >= MENU_H ) return -1;
  for ( int i = 0; i < s_nmenus; i++ ) {
    int tx = title_x( i ) - 6, tw = (int)strlen( s_menus[i].title ) * 8 + 12;
    if ( x >= tx && x < tx + tw ) return i;
  }
  return -1;
}

int menubar_key( int key ) {
  for ( int i = 0; i < s_nmenus; i++ ) {
    char c = s_menus[i].title[0];
    if ( c >= 'a' && c <= 'z' ) c -= 32;
    if ( key == K_ALT( c ) ) return i;
  }
  return -1;
}

static void drop_geom( int m, int *x, int *y, int *w, int *h ) {
  int maxl = 0;
  for ( int i = 0; i < s_menus[m].n; i++ ) {
    int l = (int)strlen( s_menus[m].items[i].label );
    if ( l > maxl ) maxl = l;
  }
  *x = ( title_x( m ) - 6 ) & ~1;
  *y = MENU_H;
  *w = ( maxl * 8 + 28 ) & ~1;
  *h = s_menus[m].n * ROW_H + 4;
  if ( *x + *w + 6 > vid_w ) *x = ( vid_w - *w - 6 ) & ~1;
}

static void draw_item( int m, int i, int sel ) {
  int x, y, w, h;
  drop_geom( m, &x, &y, &w, &h );
  const MenuItem &it = s_menus[m].items[i];
  int iy = y + 2 + i * ROW_H;
  gui_mouse_hide( );
  if ( it.label[0] == '-' ) {
    vid_fill( x + 2, iy, w - 4, ROW_H, WHITE );
    vid_hline( x + 6, x + w - 7, iy + ROW_H / 2, DGRAY );
  }
  else {
    vid_fill( x + 2, iy, w - 4, ROW_H, sel ? BLUE : WHITE );
    vid_text( x + 12, iy + 1, it.label, sel ? WHITE : BLACK, -1 );
  }
  gui_mouse_show( );
}

static int next_item( int m, int i, int dir ) {
  int n = s_menus[m].n;
  for ( int k = 0; k < n; k++ ) {
    i = ( i + dir + n ) % n;
    if ( s_menus[m].items[i].label[0] != '-' ) return i;
  }
  return i;
}

int menubar_run( int start, const Event *ev ) {
  int m = start;
  int mouseMode = 0;
  if ( m < 0 && ev ) { m = menubar_hit( ev->x, ev->y ); mouseMode = 1; }
  if ( m < 0 ) return 0;
  int sel = mouseMode ? -1 : next_item( m, -1, 1 );
  int result = 0;

  for ( ;; ) {
    int x, y, w, h;
    drop_geom( m, &x, &y, &w, &h );
    unsigned char far *save = ui_save( x, y, w + 6, h + 3 );
    gui_mouse_hide( );
    draw_title( m, 1 );
    vid_fill( x + w, y + 3, 6, h, BLACK );
    vid_fill( x + 6, y + h, w, 3, BLACK );
    vid_fill( x, y, w, h, WHITE );
    vid_rect( x, y, w, h, BLACK );
    vid_vline( x + 1, y, y + h - 1, BLACK );
    vid_vline( x + w - 2, y, y + h - 1, BLACK );
    for ( int i = 0; i < s_menus[m].n; i++ ) draw_item( m, i, i == sel );
    gui_mouse_show( );

    int switchTo = -1, done = 0;
    Event e;
    while ( !done ) {
      gui_poll( &e );
      if ( e.type == EV_KEY ) {
        int k = e.key;
        if ( k == K_ESC ) { done = 1; }
        else if ( k == K_UP || k == K_DOWN ) {
          int ns = next_item( m, sel < 0 ? ( k == K_UP ? 0 : -1 ) : sel, k == K_UP ? -1 : 1 );
          if ( sel >= 0 ) draw_item( m, sel, 0 );
          sel = ns; draw_item( m, sel, 1 );
        }
        else if ( k == K_LEFT ) { switchTo = ( m + s_nmenus - 1 ) % s_nmenus; done = 1; }
        else if ( k == K_RIGHT ) { switchTo = ( m + 1 ) % s_nmenus; done = 1; }
        else if ( k == K_ENTER && sel >= 0 ) { result = s_menus[m].items[sel].id; done = 1; }
        else {
          int alt = menubar_key( k );
          if ( alt >= 0 && alt != m ) { switchTo = alt; done = 1; }
          else if ( k >= 'a' && k <= 'z' || k >= 'A' && k <= 'Z' ) {
            char c = (char)( k >= 'a' ? k - 32 : k );
            for ( int i = 0; i < s_menus[m].n; i++ ) {
              char f = s_menus[m].items[i].label[0];
              if ( f >= 'a' ) f -= 32;
              if ( f == c ) { result = s_menus[m].items[i].id; done = 1; break; }
            }
          }
        }
      }
      else if ( e.type == EV_DOWN || e.type == EV_MOVE || e.type == EV_UP ) {
        int t = menubar_hit( e.x, e.y );
        if ( t >= 0 && t != m && e.type != EV_UP ) { switchTo = t; mouseMode = 1; done = 1; continue; }
        if ( ui_hit( e.x, e.y, x, y + 2, w, h - 4 ) ) {
          int i = ( e.y - y - 2 ) / ROW_H;
          if ( i >= 0 && i < s_menus[m].n && s_menus[m].items[i].label[0] != '-' ) {
            if ( i != sel ) {
              if ( sel >= 0 ) draw_item( m, sel, 0 );
              sel = i; draw_item( m, sel, 1 );
            }
            if ( e.type == EV_UP || ( e.type == EV_DOWN && !mouseMode ) ) { result = s_menus[m].items[i].id; done = 1; }
          }
        }
        else if ( e.type == EV_DOWN && t < 0 ) { done = 1; }        // click elsewhere closes
        else if ( e.type == EV_UP && t == m ) { mouseMode = 0; }     // released on the title: stay open
        else if ( e.type == EV_UP && mouseMode ) { done = 1; }       // released outside
      }
    }
    ui_restore( x, y, w + 6, h + 3, save );
    gui_mouse_hide( ); draw_title( m, 0 ); gui_mouse_show( );
    if ( switchTo < 0 ) break;
    m = switchTo;
    sel = mouseMode ? -1 : next_item( m, -1, 1 );
  }
  if ( result ) snd_play( SND_CLICK );
  return result;
}
