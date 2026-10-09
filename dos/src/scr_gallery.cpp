// DeskMind - Gallery screen: thumbnail grid or list, view / rename / delete / ask Qwen / sync.

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <dos.h>
#include <io.h>

#include "app.h"
#include "scr.h"
#include "video.h"
#include "sound.h"
#include "sys.h"
#include "cfg.h"
#include "tpi.h"
#include "net.h"
#include "slide.h"
#include "jukebox.h"

#define WIN_Y  SCR_Y
#define WIN_H  ( SCR_H - 1 )
#define MAX_PICS 500             // SLIDES.EXE shows up to 500 too

struct GPic { char id[9]; char title[40]; unsigned short year; unsigned char month, day, hour, minute; };
static GPic *s_p = 0;
static int s_n = 0;
static int s_total = 0;                // pictures on disk (more than s_n when the list is full)
static int s_sel = 0;
static int s_grid = 1;
static unsigned char far *s_thumb = 0;
char gallery_ask_id[9] = "";
static char s_note[120] = "";        // an action's result, shown by the next gallery_draw

// Grid geometry: 3 x 2 cells of 200 x 70
#define GX0   20
#define GY0   ( WIN_Y + 16 )
#define CW    204
#define CH    68
#define GCOLS 3
#define GROWS 2
#define PER_PAGE ( GCOLS * GROWS )
// The grid's scroll bar, one step per page, right of the cells (they end at x 620)
#define SBX   622
#define SBY   ( GY0 - 3 )
#define SBH   ( GROWS * CH - 2 )

static Widget s_ws[8];
static Form s_form;
enum { ID_LIST = 100, ID_VIEW, ID_RENAME, ID_DELETE, ID_ASK, ID_SYNC, ID_MODE, ID_SLIDES };

// ---------------------------------------------------------------- data

static long pic_key( const GPic *p ) {
  return ( (long)p->year << 20 ) | ( (long)p->month << 16 ) | ( (long)p->day << 11 ) | ( p->hour << 6 ) | p->minute;
}

static int cmp_newest( const void *a, const void *b ) {
  long kx = pic_key( (const GPic *)a ), ky = pic_key( (const GPic *)b );
  return kx < ky ? 1 : kx > ky ? -1 : 0;
}

static const char far *g_item( void *, int i );

void gallery_rescan( void ) {
  if ( !s_p ) return;
  snd_settle( );                                 // see save_chat in scr_chat.cpp
  snd_log( SL_DISK_BEGIN, SD_RESCAN );
  s_n = 0;
  s_total = 0;
  struct find_t ff;
  char pattern[80];
  sprintf( pattern, "%s\\*.TPI", cfg_pics( ) );
  unsigned rc = _dos_findfirst( pattern, _A_NORMAL, &ff );
  int oldest = -1;                               // when full: the entry a newer picture replaces
  while ( rc == 0 ) {
    char path[80];
    sprintf( path, "%s\\%s", cfg_pics( ), ff.name );
    TpiHeader h;
    if ( tpi_header( path, &h ) == 0 ) {
      GPic q;
      tpi_id( &h, q.id );
      str_copy( q.title, h.title[0] ? h.title : ff.name, sizeof( q.title ) );
      q.year = h.year; q.month = h.month; q.day = h.day; q.hour = h.hour; q.minute = h.minute;
      s_total++;
      // DOS lists files in folder order, so past MAX_PICS keep the newest ones
      if ( s_n < MAX_PICS ) s_p[ s_n++ ] = q;
      else {
        if ( oldest < 0 )
          for ( int i = oldest = 0; i < s_n; i++ ) if ( pic_key( &s_p[i] ) < pic_key( &s_p[oldest] ) ) oldest = i;
        if ( pic_key( &q ) > pic_key( &s_p[oldest] ) ) { s_p[oldest] = q; oldest = -1; }
      }
    }
    rc = _dos_findnext( &ff );
  }
  qsort( s_p, s_n, sizeof( GPic ), cmp_newest );
  if ( s_sel >= s_n ) s_sel = s_n - 1;
  if ( s_sel < 0 ) s_sel = 0;
  list_set( &s_ws[0], s_n, g_item, 0 );     // (passing 0 here once blanked the list view's titles)
  snd_log( SL_DISK_END, SD_RESCAN );
}

static const char far *g_item( void *, int i ) {
  static char t[70];
  GPic &p = s_p[i];
  sprintf( t, "%04u-%02u-%02u %02u:%02u  %-38.38s", p.year, p.month, p.day, p.hour, p.minute, p.title );
  return t;
}

// ---------------------------------------------------------------- drawing

static void draw_cell( int i, int sel ) {
  int page = s_sel / PER_PAGE;
  int k = i - page * PER_PAGE;
  if ( k < 0 || k >= PER_PAGE ) return;
  int x = GX0 + ( k % GCOLS ) * CW, y = GY0 + ( k / GCOLS ) * CH;
  gui_mouse_hide( );
  vid_fill( x - 4, y - 3, CW - 8, CH - 2, sel ? BLUE : LGRAY );
  if ( i < s_n ) {
    char p[80];
    TpiHeader h;
    app_pic_path( p, s_p[i].id );
    vid_fill( x, y, 164, 52, BLACK );
    if ( s_thumb && tpi_thumb( p, s_thumb, &h ) == 0 ) tpi_draw_thumb( x + 2, y + 1, s_thumb, h.thumb_w, h.thumb_h );
    ui_text_fit( x, y + 54, CW - 12, s_p[i].title, sel ? WHITE : BLACK, -1 );
  }
  gui_mouse_show( );
}

static int grid_pages( void ) { return ( s_n + PER_PAGE - 1 ) / PER_PAGE; }

// The page counter and the scroll bar (cheap: drawn alone while the bar is dragged)
static void draw_page_info( int page ) {
  char t[40];
  int pages = grid_pages( );
  if ( s_total > s_n ) sprintf( t, "Page %d of %d  (newest %d of %d)", pages ? page + 1 : 0, pages, s_n, s_total );
  else sprintf( t, "Page %d of %d  (%d pictures)", pages ? page + 1 : 0, pages, s_n );
  gui_mouse_hide( );
  vid_fill( GX0 - 4, GY0 + GROWS * CH, 400, 10, LGRAY );
  vid_text( GX0, GY0 + GROWS * CH, t, DGRAY, -1 );
  // In a white sunken frame like Chat's and the lists' (on the grey window the track was invisible)
  ui_sunken( SBX - 2, SBY - 1, 14, SBH + 2, WHITE );
  ui_scrollbar( SBX, SBY, SBH, pages, 1, page );
  gui_mouse_show( );
}

static void draw_grid( void ) {
  int page = s_sel / PER_PAGE;
  for ( int k = 0; k < PER_PAGE; k++ ) draw_cell( page * PER_PAGE + k, page * PER_PAGE + k == s_sel );
  draw_page_info( page );
}

// sb_track callback: the selection moves to the same place on the new page, like PgDn.
// While the bar is dragged only the counter and bar follow; the six thumbnails (read from
// disk) are drawn when the pointer pauses or the button is released.
static void grid_scrolled( void *, int page, int dragging ) {
  if ( dragging ) { draw_page_info( page ); return; }
  int i = page * PER_PAGE + s_sel % PER_PAGE;
  if ( i >= s_n ) i = s_n - 1;
  if ( i < 0 ) return;
  s_sel = i;
  draw_grid( );
}

static void draw_list_preview( void ) {
  int px = 456, py = WIN_Y + 20;
  gui_mouse_hide( );
  ui_sunken( px - 4, py - 2, 168, 54, BLACK );
  vid_fill( px - 6, py + 54, 176, 20, LGRAY );
  if ( s_sel < s_n ) {
    char p[80];
    TpiHeader h;
    app_pic_path( p, s_p[ s_sel ].id );
    if ( s_thumb && tpi_thumb( p, s_thumb, &h ) == 0 ) tpi_draw_thumb( px, py, s_thumb, h.thumb_w, h.thumb_h );
    vid_text( px - 4, py + 56, s_p[ s_sel ].id, DGRAY, -1 );
  }
  gui_mouse_show( );
}

void gallery_draw( void ) {
  s_ws[0].flags = s_grid ? WF_HIDDEN : 0;
  s_ws[0].sel = s_sel;
  s_ws[0].flags &= ~WF_FREEVIEW;
  s_ws[6].text = s_grid ? "List" : "Grid";
  if ( s_grid && s_form.focus == 0 ) s_form.focus = 1;
  form_draw( &s_form );
  if ( s_grid ) draw_grid( ); else draw_list_preview( );
  if ( s_note[0] ) { app_status( "%s", s_note ); s_note[0] = 0; }
  else if ( s_total > s_n )
    app_status( "Showing the newest %d of %d pictures.  Delete some to see the rest.", s_n, s_total );
  else
    app_status( "%d pictures.  Arrows/PgUp/PgDn move.  Enter or double-click views.  F2 Chat  F3 Create", s_n );
}

static void select( int i ) {
  if ( i < 0 ) i = 0;
  if ( i >= s_n ) i = s_n - 1;
  if ( i < 0 || i == s_sel ) return;
  int oldPage = s_sel / PER_PAGE;
  int old = s_sel;
  s_sel = i;
  if ( !s_grid ) { s_ws[0].sel = i; s_ws[0].flags &= ~WF_FREEVIEW; form_draw_widget( &s_form, 0 ); draw_list_preview( ); return; }
  if ( i / PER_PAGE != oldPage ) draw_grid( );
  else { draw_cell( old, 0 ); draw_cell( i, 1 ); }
}

// ---------------------------------------------------------------- actions

// The result of Rename or Delete: gallery_draw() follows them and would overwrite a plain app_status
static void note( const char *fmt, ... ) {
  va_list a;
  va_start( a, fmt );
  vsprintf( s_note, fmt, a );
  va_end( a );
}

// The last network error as a sentence (some already end in '?' or '.')
static const char *err_sentence( void ) {
  static char e[100];
  str_copy( e, net_error( ), sizeof( e ) - 1 );
  int n = (int)strlen( e );
  if ( n && e[n-1] != '.' && e[n-1] != '?' ) { e[n] = '.'; e[n+1] = 0; }
  return e;
}

static void do_rename( void ) {
  if ( s_sel >= s_n ) return;
  GPic &p = s_p[ s_sel ];
  char buf[40];
  str_copy( buf, p.title, sizeof( buf ) );
  if ( !input_box( "Rename", "New title for this picture:", buf, sizeof( buf ) ) || !buf[0] ) return;
  char path[80];
  app_pic_path( path, p.id );
  if ( tpi_set_title( path, buf ) ) { msg_box( "Rename", "Could not change the file.", "OK" ); return; }
  str_copy( p.title, buf, sizeof( p.title ) );
  // The other mode's copy gets the new title too (MindServer renames all of its copies)
  char twin[80];
  int twinBad = app_twin_path( twin, p.id ) && tpi_set_title( twin, buf );
  // Keep MindServer's title in step (the ID never changes).  404 = MindServer doesn't have it.
  if ( app_net ) {
    char pth[40];
    sprintf( pth, "/img/%s/title", p.id );
    busy_begin( "Rename", "Telling MindServer..." );
    int st = app_post_short( pth, buf );
    busy_end( );
    if ( st != 200 && st != 404 ) {
      char t[240];
      if ( st < 0 ) sprintf( t, "%s Renamed on this %s only; MindServer keeps the old title.", err_sentence( ), app_pc( ) );
      else          sprintf( t, "Renamed on this %s only. MindServer said %d, so it keeps the old title.", app_pc( ), st );
      msg_box( "Rename", t, "OK" );
    }
  }
  if ( twinBad ) note( "Renamed, but not the %s copy.  The file stays %s.TPI.", cfg_cga ? "Tandy" : "CGA", p.id );
  else note( "Renamed.  The file stays %s.TPI.", p.id );
}

static void do_delete( void ) {
  if ( s_sel >= s_n ) return;
  GPic &p = s_p[ s_sel ];
  char q[160];                            // title is up to 39 characters
  if ( app_net ) sprintf( q, "Delete \"%s\"?  Delete = only on this %s.  Everywhere = on MindServer too.", p.title, app_pc( ) );
  else           sprintf( q, "Delete \"%s\" from this %s?  (MindServer keeps its copy.)", p.title, app_pc( ) );
  int r = msg_box( "Delete picture", q, app_net ? "Delete|Everywhere|Cancel" : "Delete|Cancel" );
  if ( r == 0 || ( app_net ? r == 3 : r == 2 ) ) return;
  // Everywhere: ask MindServer first.  Deleting here first would lose nothing on the server, and
  // the next Sync would bring the picture back.  404 = MindServer doesn't have it: fine.
  int here = 0;
  if ( app_net && r == 2 ) {
    char pth[40];
    sprintf( pth, "/img/%s/del", p.id );
    busy_begin( "Delete picture", "Deleting it on MindServer..." );
    int st = app_post_short( pth, 0 );
    busy_end( );
    if ( st != 200 && st != 404 ) {
      char t[320];
      if ( st < 0 ) sprintf( t, "%s Delete \"%s\" only on this %s? Sync brings it back while MindServer has it.", err_sentence( ), p.title, app_pc( ) );
      else          sprintf( t, "MindServer said %d. Delete \"%s\" only on this %s? Sync brings it back while MindServer has it.", st, p.title, app_pc( ) );
      if ( msg_box( "Delete picture", t, "Delete here|Cancel" ) != 1 ) return;
      here = 1;
    }
  }
  char path[80];
  app_pic_path( path, p.id );
  snd_settle( );
  snd_log( SL_DISK_BEGIN, SD_DELETE );
  int bad = remove( path );
  snd_log( SL_DISK_END, SD_DELETE );
  if ( bad ) { msg_box( "Delete picture", "Could not delete the file.", "OK" ); return; }
  if ( here || r == 1 ) note( "Deleted %s on this %s.", p.id, app_pc( ) );
  else {
    // Everywhere: also the copy synced in the other mode, or it would linger there
    char twin[80];
    const char *other = cfg_cga ? "Tandy" : "CGA";
    if ( !app_twin_path( twin, p.id ) ) note( "Deleted %s here and on MindServer.", p.id );
    else if ( remove( twin ) ) note( "Deleted %s here and on MindServer, but not the %s copy.", p.id, other );
    else note( "Deleted %s here, in the %s pictures and on MindServer.", p.id, other );
  }
  gallery_rescan( );
}

static int sync_progress( unsigned long done, long total ) {
  char t[60];
  if ( total > 0 ) sprintf( t, "%lu of %ld bytes", done, total ); else sprintf( t, "%lu bytes", done );
  busy_text( t );
  return kbd_get( ) == K_ESC;
}

void gallery_sync( void ) {
  if ( !app_need_net( "Sync" ) ) return;
  // Read the list line by line (any length), note the pictures missing here, then fetch them
  // after the list is closed (one connection at a time).  Stops when the gallery would be full.
  int room = MAX_PICS - s_total;
  if ( room <= 0 ) {
    msg_box( "Sync", "The gallery is full (500 pictures).  Delete some pictures first.", "OK" );
    return;
  }
  typedef char Id9[9];
  Id9 *want = (Id9 *)malloc( sizeof( Id9 ) * room );
  if ( !want ) { msg_box( "Sync", "Not enough memory.", "OK" ); return; }
  busy_begin( "Sync from MindServer", "Asking for the picture list..." );
  int nwant = 0, have = 0, skipped = 0, st, cancelled = 0;
  if ( http_open( cfg.server, cfg.port, "GET", "/list", 0, 0, 0 ) ) st = NET_ERROR;
  else while ( ( st = http_headers( ) ) == NET_AGAIN ) if ( kbd_get( ) == K_ESC ) { cancelled = 1; break; }
  if ( st == 200 ) {
    static char line[ 120 ];
    for ( ;; ) {
      int rc = http_line( line, sizeof( line ) );
      if ( rc == NET_AGAIN ) { if ( kbd_get( ) == K_ESC ) { cancelled = 1; break; } continue; }
      if ( rc != 1 ) { if ( rc != NET_DONE ) st = rc; break; }
      if ( strlen( line ) < 8 ) continue;
      char id[9];
      memcpy( id, line, 8 ); id[8] = 0;
      if ( app_have_pic( id ) ) have++;
      else if ( nwant < room ) str_copy( want[ nwant++ ], id, 9 );
      else skipped++;
    }
  }
  http_close( );
  if ( cancelled || st != 200 ) {
    busy_end( ); free( want );
    if ( cancelled ) app_status( "Sync cancelled." );
    else msg_box( "Sync", st > 0 ? "MindServer refused the list." : net_error( ), "OK" );
    return;
  }
  int got = 0, failed = 0;
  for ( int i = 0; i < nwant; i++ ) {
    char msg[60];
    sprintf( msg, "Picture %d of %d: %s", i + 1, nwant, want[i] );
    busy_text( msg );
    char file[80], path[40];
    app_pic_path( file, want[i] );
    sprintf( path, "/img/%s%s", want[i], cfg_cga ? "?mode=cga" : "" );
    int s2;
    snd_log( SL_DISK_BEGIN, SD_DOWNLOAD );
    long rc = http_get_file( cfg.server, cfg.port, path, file, sync_progress, &s2 );
    snd_log( SL_DISK_END, SD_DOWNLOAD );
    if ( rc < 0 ) {
      failed++;
      if ( !strcmp( net_error( ), "Cancelled" ) ) break;     // Esc stops the whole sync
    }
    else got++;
  }
  free( want );
  busy_end( );
  gallery_rescan( );
  gallery_draw( );
  // After the folder scan, not before: the real TL/3 once held the chime's last note through it
  if ( got ) snd_play( SND_IMAGE );
  if ( skipped )
    app_status( "Sync: %d new.  Gallery full (500): %d not fetched, delete some and sync again.", got, skipped );
  else
    app_status( "Sync: %d new, %d already here%s", got, have, failed ? ", some failed" : "" );
}

// ---------------------------------------------------------------- slideshow

static int slide_path( void *, int i, char *out ) {
  if ( i < 0 || i >= s_n ) return 0;
  app_pic_path( out, s_p[i].id );
  return 1;
}

static void slide_poll( void ) { jb_poll( ); }      // random songs go on during the show

void gallery_slideshow( void ) {
  if ( s_n == 0 ) { msg_box( "Slideshow", "There are no pictures yet.", "OK" ); return; }
  SlideOpts o;
  o.delay = cfg.slide_delay > 0 ? cfg.slide_delay : 8;
  o.effect = cfg.slide_effect;
  o.titles = 1; o.shuffle = cfg.slide_shuffle; o.loop = 1;
  o.poll = slide_poll;
  int music = jb_slides_begin( );            // Settings: slideshow music (unless a song already plays)
  gui_mouse_hide( );
  // Oldest first; a picture picked further down the list (or any, in random order) plays first
  int shown = slide_run( s_n, ( o.shuffle || s_sel > 0 ) ? s_sel : -1, slide_path, 0, &o );
  jb_slides_end( music );
  app_gui_mode( );
  gui_mouse_show( );
  if ( !shown ) {
    app_redraw( );
    msg_box( "Slideshow", "Not enough memory for the slideshow (it needs 63K, or EMS). Close the chat or restart DeskMind.", "OK" );
  }
  else app_redraw( );
  app_status( "Slideshow: %d picture%s shown.", shown, shown == 1 ? "" : "s" );
}

// ---------------------------------------------------------------- screen API

void gallery_init( void ) {
  s_p = (GPic *)malloc( sizeof( GPic ) * MAX_PICS );
  s_thumb = (unsigned char far *)_fmalloc( TPI_THUMB_MAX );
  memset( s_ws, 0, sizeof( s_ws ) );
  int by = 150;
  s_ws[0].type = W_LIST;   s_ws[0].id = ID_LIST;   s_ws[0].x = 4; s_ws[0].y = 4; s_ws[0].w = 440; s_ws[0].h = 4 + 13 * ROW_H;
  s_ws[0].item = g_item;
  s_ws[1].type = W_BUTTON; s_ws[1].id = ID_VIEW;   s_ws[1].x = 4;   s_ws[1].y = by; s_ws[1].w = 64; s_ws[1].h = 14; s_ws[1].text = "View"; s_ws[1].flags = WF_DEFAULT;
  s_ws[2].type = W_BUTTON; s_ws[2].id = ID_RENAME; s_ws[2].x = 74;  s_ws[2].y = by; s_ws[2].w = 72; s_ws[2].h = 14; s_ws[2].text = "Rename";
  s_ws[3].type = W_BUTTON; s_ws[3].id = ID_DELETE; s_ws[3].x = 152; s_ws[3].y = by; s_ws[3].w = 72; s_ws[3].h = 14; s_ws[3].text = "Delete";
  s_ws[4].type = W_BUTTON; s_ws[4].id = ID_ASK;    s_ws[4].x = 230; s_ws[4].y = by; s_ws[4].w = 88; s_ws[4].h = 14; s_ws[4].text = "Ask Qwen";
  s_ws[5].type = W_BUTTON; s_ws[5].id = ID_SYNC;   s_ws[5].x = 324; s_ws[5].y = by; s_ws[5].w = 64; s_ws[5].h = 14; s_ws[5].text = "Sync";
  s_ws[6].type = W_BUTTON; s_ws[6].id = ID_MODE;   s_ws[6].x = 548; s_ws[6].y = by; s_ws[6].w = 72; s_ws[6].h = 14; s_ws[6].text = "List";
  s_ws[7].type = W_BUTTON; s_ws[7].id = ID_SLIDES; s_ws[7].x = 394; s_ws[7].y = by; s_ws[7].w = 96; s_ws[7].h = 14; s_ws[7].text = "Slideshow";
  form_init( &s_form, "Gallery", 0, WIN_Y, 640, WIN_H, s_ws, 8 );
  s_form.focus = 1;
  gallery_rescan( );
}

int gallery_event( Event *e ) {
  if ( s_grid && e->type == EV_KEY ) {
    int k = e->key;
    if ( k == K_LEFT )  { select( s_sel - 1 ); return CMD_NONE; }
    if ( k == K_RIGHT ) { select( s_sel + 1 ); return CMD_NONE; }
    if ( k == K_UP )    { select( s_sel - GCOLS ); return CMD_NONE; }
    if ( k == K_DOWN )  { select( s_sel + GCOLS ); return CMD_NONE; }
    if ( k == K_PGUP )  { select( s_sel - PER_PAGE ); return CMD_NONE; }
    if ( k == K_PGDN )  { select( s_sel + PER_PAGE ); return CMD_NONE; }
    if ( k == K_HOME )  { select( 0 ); return CMD_NONE; }
    if ( k == K_END )   { select( s_n - 1 ); return CMD_NONE; }
  }
  if ( s_grid && e->type == EV_DOWN && ui_hit( e->x, e->y, SBX, SBY, 10, SBH ) ) {
    sb_track( SBX, SBY, SBH, grid_pages( ), 1, s_sel / PER_PAGE, 1, e->x, e->y, grid_scrolled, 0 );
    return CMD_NONE;
  }
  if ( s_grid && e->type == EV_DOWN && e->y >= GY0 - 3 && e->y < GY0 + GROWS * CH ) {
    int col = ( e->x - GX0 + 4 ) / CW, row = ( e->y - GY0 + 3 ) / CH;
    if ( col >= 0 && col < GCOLS && row >= 0 && row < GROWS ) {
      int i = ( s_sel / PER_PAGE ) * PER_PAGE + row * GCOLS + col;
      if ( i < s_n ) {
        int same = ( i == s_sel );
        select( i );
        if ( e->key == 2 && same ) { app_view_pic( s_p[i].id ); }
      }
      return CMD_NONE;
    }
  }
  int oldSel = s_ws[0].sel;
  int id = form_event( &s_form, e );
  if ( !s_grid && s_ws[0].sel != oldSel ) { s_sel = s_ws[0].sel; draw_list_preview( ); }
  switch ( id ) {
    case ID_LIST:
    case ID_VIEW:   if ( s_sel < s_n ) app_view_pic( s_p[ s_sel ].id ); break;
    case ID_RENAME: do_rename( ); gallery_draw( ); break;
    case ID_DELETE: do_delete( ); gallery_draw( ); break;
    case ID_SYNC:   gallery_sync( ); break;
    case ID_SLIDES: gallery_slideshow( ); break;
    case ID_MODE:
      s_grid = !s_grid;
      s_form.focus = s_grid ? 1 : 0;          // list view: arrows move in the list straight away
      gallery_draw( );
      break;
    case ID_ASK:
      if ( s_sel < s_n ) {
        str_copy( gallery_ask_id, s_p[ s_sel ].id, 9 );
        s_form.focus = 1;                       // coming back, Enter views again
        return CMD_GOTO_CHAT;
      }
      break;
  }
  // After any action the focus returns to View, so Enter always views (it used to stay
  // on the last button clicked, e.g. Ask Qwen)
  if ( id && id != ID_MODE && s_form.focus != 1 ) form_set_focus( &s_form, 1 );
  return CMD_NONE;
}
