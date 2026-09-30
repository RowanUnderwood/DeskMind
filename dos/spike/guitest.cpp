// GUITEST - DeskMind Phase 4 test of the base libraries (GUI, TPI, config, EMS, network).
//
//   GUITEST [/NONET] [/NOMOUSE] [/320]
//
// Menu: F10 or Alt+letter.  Tab moves between controls.  Sync pulls every picture
// from MindServer into the PICS folder; View shows one full screen.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <malloc.h>
#include <direct.h>
#include <dos.h>
#include <io.h>

#include "video.h"
#include "mouse.h"
#include "sound.h"
#include "sys.h"
#include "gui.h"
#include "cfg.h"
#include "tpi.h"
#include "net.h"

#define MAX_PICS 200

struct Pic { char file[13]; char title[40]; };
static Pic  *g_pics = 0;
static int   g_npics = 0;
static char  g_exeDir[80];
static int   g_net = 0;
static int   g_ems = 0;
static unsigned char far *g_thumb = 0;
static char  g_status[100];
static char  g_memo[1200] = "Edit me! This multi-line editor wraps words, and you can move "
                            "with the arrow keys, Home, End, PgUp and PgDn. It will be used "
                            "for editing prompts before they are sent to the image generator.";

enum { ID_LIST = 100, ID_VIEW, ID_RENAME, ID_PROMPT, ID_DELETE, ID_SYNC, ID_SOUND,
       M_SYNC = 200, M_EXIT, M_MEMO, M_INPUT, M_MSG, M_ABOUT, M_SOUND };

static const MenuItem fileItems[] = { { "Sync from server", M_SYNC }, { "-", 0 }, { "Exit", M_EXIT } };
static const MenuItem testItems[] = { { "Memo editor...", M_MEMO }, { "Input box...", M_INPUT },
                                      { "Message box...", M_MSG }, { "Toggle sound", M_SOUND } };
static const MenuItem helpItems[] = { { "About...", M_ABOUT } };
static const Menu menus[] = { { "File", fileItems, 3 }, { "Test", testItems, 4 }, { "Help", helpItems, 1 } };

static Widget ws[8];
static Form form;

// ---------------------------------------------------------------- pictures

static void pic_path( char *out, const char *file ) {
  sprintf( out, "%s\\%s", cfg.pics, file );
}

static void scan_pics( void ) {
  g_npics = 0;
  struct find_t ff;
  char pattern[80];
  sprintf( pattern, "%s\\*.TPI", cfg.pics );
  unsigned rc = _dos_findfirst( pattern, _A_NORMAL, &ff );
  while ( rc == 0 && g_npics < MAX_PICS ) {
    Pic &p = g_pics[ g_npics ];
    str_copy( p.file, ff.name, sizeof( p.file ) );
    char path[80];
    pic_path( path, p.file );
    TpiHeader h;
    if ( tpi_header( path, &h ) == 0 ) str_copy( p.title, h.title[0] ? h.title : p.file, sizeof( p.title ) );
    else str_copy( p.title, "(not a picture)", sizeof( p.title ) );
    g_npics++;
    rc = _dos_findnext( &ff );
  }
}

static const char far *pic_item( void *, int i ) {
  return g_pics[i].title;
}

static void status( const char *fmt, ... ) {
  va_list ap;
  va_start( ap, fmt );
  vsprintf( g_status, fmt, ap );
  va_end( ap );
  ui_status( g_status );
}

// Thumbnail of the selected picture, at the right of the list
static void draw_preview( void ) {
  int px = form_client_x( &form ) + 260, py = form_client_y( &form ) + 4;
  gui_mouse_hide( );
  ui_sunken( px - 4, py - 2, 168, 54, BLACK );
  if ( ws[0].sel >= 0 && ws[0].sel < g_npics ) {
    char path[80];
    TpiHeader h;
    pic_path( path, g_pics[ ws[0].sel ].file );
    if ( tpi_thumb( path, g_thumb, &h ) == 0 ) tpi_draw_thumb( px, py, g_thumb, h.thumb_w, h.thumb_h );
    vid_fill( px - 4, py + 54, 168, 10, LGRAY );
    char info[24];
    sprintf( info, "%04u-%02u-%02u %02u:%02u", h.year, h.month, h.day, h.hour, h.minute );
    vid_text( px, py + 55, info, DGRAY, -1 );
  }
  gui_mouse_show( );
}

static void refresh_list( void ) {
  scan_pics( );
  list_set( &ws[0], g_npics, pic_item, 0 );
  form_draw_widget( &form, 0 );
  draw_preview( );
}

// ---------------------------------------------------------------- actions

static int sync_progress( unsigned long done, long total ) {
  static unsigned long last = 0;
  if ( ticks( ) - last > 4 ) {
    char t[60];
    if ( total > 0 ) sprintf( t, "%lu of %ld bytes", done, total ); else sprintf( t, "%lu bytes", done );
    busy_text( t );
    last = ticks( );
  }
  return kbd_get( ) == K_ESC;
}

static void do_sync( void ) {
  if ( !g_net ) { msg_box( "Sync", "The network is not running. Start GUITEST after booting with W.", "OK" ); return; }
  busy_begin( "Sync from MindServer", "Asking for the picture list..." );
  static char list[ 4000 ];
  int st = 0;
  long n = http_get_all( cfg.server, cfg.port, "/list", list, sizeof( list ) - 1, &st );
  if ( n < 0 || st != 200 ) {
    busy_end( );
    msg_box( "Sync", net_error( ), "OK" );
    return;
  }
  list[n] = 0;
  int got = 0, have = 0, failed = 0;
  char *line = strtok( list, "\r\n" );
  static char ids[ 100 ][ 9 ];
  int nid = 0;
  while ( line && nid < 100 ) {
    if ( strlen( line ) >= 8 ) { memcpy( ids[nid], line, 8 ); ids[nid][8] = 0; nid++; }
    line = strtok( 0, "\r\n" );
  }
  for ( int i = 0; i < nid; i++ ) {
    char file[80], path[40], msg[60];
    sprintf( file, "%s\\%s.TPI", cfg.pics, ids[i] );
    if ( access( file, 0 ) == 0 ) { have++; continue; }
    sprintf( msg, "Picture %d of %d: %s", i + 1, nid, ids[i] );
    busy_text( msg );
    sprintf( path, "/img/%s?mode=%d", ids[i], cfg.mode );
    long r = http_get_file( cfg.server, cfg.port, path, file, sync_progress, &st );
    if ( r < 0 ) failed++; else got++;
  }
  busy_end( );
  if ( got ) snd_play( SND_IMAGE );
  status( "Sync: %d new, %d already here, %d failed", got, have, failed );
  refresh_list( );
}

static void do_view( void ) {
  if ( ws[0].sel < 0 || ws[0].sel >= g_npics ) return;
  char path[80];
  pic_path( path, g_pics[ ws[0].sel ].file );
  gui_mouse_hide( );
  unsigned long t0 = ticks( );
  int rc = tpi_show( path );
  unsigned long t1 = ticks( );
  if ( rc ) {
    gui_mouse_show( );
    msg_box( "View", rc == 2 ? "This picture is for the other video mode." : "Could not read the picture.", "OK" );
    return;
  }
  Event e;
  do { gui_poll( &e ); } while ( e.type != EV_KEY && e.type != EV_DOWN );
  ui_desktop( );
  menubar_draw( );
  form_draw( &form );
  draw_preview( );
  gui_mouse_show( );
  status( "Shown in %lu ms (loaded straight from disk into video memory)", ( t1 - t0 ) * 55ul );
}

static void do_rename( void ) {
  if ( ws[0].sel < 0 ) return;
  Pic &p = g_pics[ ws[0].sel ];
  char buf[40];
  str_copy( buf, p.title, sizeof( buf ) );
  if ( input_box( "Rename", "New title for this picture:", buf, sizeof( buf ) ) ) {
    char path[80];
    pic_path( path, p.file );
    if ( tpi_set_title( path, buf ) == 0 ) {
      str_copy( p.title, buf, sizeof( p.title ) );
      form_draw_widget( &form, 0 );
      status( "Renamed (the file stays %s)", p.file );
    }
  }
}

static void do_prompt( void ) {
  if ( ws[0].sel < 0 ) return;
  static char prompt[ 1000 ];
  char path[80];
  pic_path( path, g_pics[ ws[0].sel ].file );
  tpi_prompt( path, prompt, sizeof( prompt ) );
  memo_box( "Prompt of this picture", "The prompt it was made from (read only test of the memo):", prompt, sizeof( prompt ) );
}

static void do_delete( void ) {
  if ( ws[0].sel < 0 ) return;
  Pic &p = g_pics[ ws[0].sel ];
  char q[80];
  sprintf( q, "Delete \"%s\"?", p.title );
  if ( msg_box( "Delete", q, "Yes|No" ) == 1 ) {
    char path[80];
    pic_path( path, p.file );
    remove( path );
    refresh_list( );
    status( "Deleted %s", p.file );
  }
}

static void do_about( void ) {
  char t[300];
  unsigned long freeK = 0;
  union REGS r;
  r.h.ah = 0x48; r.w.bx = 0xFFFF; intdos( &r, &r ); freeK = r.w.bx / 64;
  sprintf( t, "DeskMind GUI test (Phase 4). Video %dx200x16, mouse %s, EMS %d KB free, "
              "largest free DOS block %luK, network %s. Server %s:%u.",
           vid_w, gui_has_mouse( ) ? "yes" : "no", g_ems * 16, freeK, g_net ? "on" : "off", cfg.server, cfg.port );
  msg_box( "About DeskMind", t, "OK" );
}

static void handle( int id ) {
  switch ( id ) {
    case ID_LIST: case ID_VIEW: do_view( ); break;
    case ID_RENAME: do_rename( ); break;
    case ID_PROMPT: do_prompt( ); break;
    case ID_DELETE: do_delete( ); break;
    case ID_SYNC: case M_SYNC: do_sync( ); break;
    case M_MEMO:
      if ( memo_box( "Memo editor", "Type, then OK:", g_memo, sizeof( g_memo ) ) ) status( "Memo: %u characters", (unsigned)strlen( g_memo ) );
      break;
    case M_INPUT: {
      char buf[40] = "MindServer";
      if ( input_box( "Input box", "Type something:", buf, sizeof( buf ) ) ) status( "You typed: %s", buf );
      break;
    }
    case M_MSG: status( "Answer: %d", msg_box( "Message box", "A longer message to test word wrapping in the "
                        "message box. Do you like the DeskMate look?", "Yes|No|Maybe" ) ); break;
    case M_SOUND: case ID_SOUND:
      snd_enabled = !snd_enabled;
      ws[6].checked = snd_enabled;
      form_draw_widget( &form, 6 );
      status( "Sound %s", snd_enabled ? "on" : "off" );
      break;
    case M_ABOUT: do_about( ); break;
  }
}

static void idle( void ) {
  if ( g_net ) net_poll( );
}

// ---------------------------------------------------------------- main

int main( int argc, char *argv[] ) {
  int noNet = 0, noMouse = 0, mode = VM_640;
  for ( int i = 1; i < argc; i++ ) {
    if ( str_ieq( argv[i], "/NONET" ) ) noNet = 1;
    else if ( str_ieq( argv[i], "/NOMOUSE" ) ) noMouse = 1;
    else if ( str_ieq( argv[i], "/320" ) ) mode = VM_320;
  }

  // EXE folder, e.g. "C:\DESKMIND\"
  str_copy( g_exeDir, argv[0], sizeof( g_exeDir ) );
  char *slash = strrchr( g_exeDir, '\\' );
  if ( slash ) slash[1] = 0; else g_exeDir[0] = 0;
  cfg_defaults( g_exeDir );
  char cfgFile[80];
  cfg_path( cfgFile, g_exeDir );
  if ( cfg_load( cfgFile ) ) cfg_save( cfgFile );
  mkdir( cfg.pics );
  str_copy( net_token, cfg.token, sizeof( net_token ) );

  if ( !noNet ) {
    printf( "Starting the network...\n" );
    g_net = ( net_init( ) == 0 );
    if ( !g_net ) printf( "No network (%s). Continuing without it.\n", net_error( ) );
  }
  g_ems = ems_init( );

  g_pics = (Pic *)malloc( sizeof( Pic ) * MAX_PICS );
  g_thumb = (unsigned char far *)_fmalloc( TPI_THUMB_MAX );
  if ( !g_pics || !g_thumb ) { printf( "Out of memory\n" ); return 1; }

  int rc = vid_reserve( mode );
  if ( rc ) { printf( "Video memory: %s\n", vid_reserve_error( rc ) ); if ( g_net ) net_done( ); return 1; }
  snd_enabled = cfg.sound;
  snd_init( );
  vid_open( mode );
  if ( !noMouse ) gui_init( );
  gui_idle = idle;

  ui_desktop( );
  menubar_set( menus, 3, "DeskMind" );
  menubar_draw( );

  memset( ws, 0, sizeof( ws ) );
  int W = ( mode == VM_640 ) ? 440 : 300;
  ws[0].type = W_LIST;   ws[0].id = ID_LIST;   ws[0].x = 4;   ws[0].y = 4;  ws[0].w = 248; ws[0].h = 4 + 10 * ROW_H;
  ws[1].type = W_BUTTON; ws[1].id = ID_VIEW;   ws[1].x = 4;   ws[1].y = 112; ws[1].w = 60; ws[1].h = 14; ws[1].text = "View"; ws[1].flags = WF_DEFAULT;
  ws[2].type = W_BUTTON; ws[2].id = ID_RENAME; ws[2].x = 68;  ws[2].y = 112; ws[2].w = 60; ws[2].h = 14; ws[2].text = "Rename";
  ws[3].type = W_BUTTON; ws[3].id = ID_PROMPT; ws[3].x = 132; ws[3].y = 112; ws[3].w = 60; ws[3].h = 14; ws[3].text = "Prompt";
  ws[4].type = W_BUTTON; ws[4].id = ID_DELETE; ws[4].x = 196; ws[4].y = 112; ws[4].w = 60; ws[4].h = 14; ws[4].text = "Delete";
  ws[5].type = W_BUTTON; ws[5].id = ID_SYNC;   ws[5].x = 260; ws[5].y = 112; ws[5].w = 80; ws[5].h = 14; ws[5].text = "Sync";
  ws[6].type = W_CHECK;  ws[6].id = ID_SOUND;  ws[6].x = 260; ws[6].y = 76;  ws[6].w = 150; ws[6].h = 10; ws[6].text = "Sound effects";
  ws[6].checked = snd_enabled;
  form_init( &form, "Pictures", ( vid_w - W ) / 2 - 20, 24, W, 150, ws, 7 );
  scan_pics( );
  list_set( &ws[0], g_npics, pic_item, 0 );
  form_draw( &form );
  draw_preview( );
  status( "%d pictures in %s.  F10 = menu, Tab = next control, Enter = view.", g_npics, cfg.pics );
  snd_play( SND_STARTUP );

  int lastSel = ws[0].sel;
  Event e;
  for ( int quit = 0; !quit; ) {
    gui_poll( &e );
    if ( e.type == EV_NONE ) continue;
    int id = 0;
    if ( e.type == EV_KEY && ( e.key == K_F10 || menubar_key( e.key ) >= 0 ) ) {
      id = menubar_run( e.key == K_F10 ? 0 : menubar_key( e.key ), 0 );
    }
    else if ( e.type == EV_DOWN && menubar_hit( e.x, e.y ) >= 0 ) {
      id = menubar_run( -1, &e );
    }
    else {
      id = form_event( &form, &e );
      if ( id == -1 ) { if ( msg_box( "Exit", "Leave the GUI test?", "Yes|No" ) == 1 ) quit = 1; id = 0; }
      if ( ws[6].checked != snd_enabled ) handle( ID_SOUND );
    }
    if ( id == M_EXIT ) quit = 1;
    else if ( id ) handle( id );
    if ( ws[0].sel != lastSel ) { lastSel = ws[0].sel; draw_preview( ); }
  }

  gui_done( );
  snd_done( );
  vid_close( );
  vid_unreserve( );
  if ( g_net ) net_done( );
  cfg.sound = snd_enabled;
  cfg_save( cfgFile );
  printf( "GUITEST done.\n" );
  return 0;
}
