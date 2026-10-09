// DeskMind - Music screen: the song library, play / loop / random / stop, rename, delete, sync.

#include <stdio.h>
#include <stdarg.h>
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
#include "net.h"
#include "music.h"
#include "jukebox.h"

#define WIN_Y  SCR_Y
#define WIN_H  ( SCR_H - 1 )

static Widget s_ws[10];
static Form s_form;
enum { ID_LIST = 100, ID_PLAY, ID_LOOP, ID_RANDOM, ID_STOP, ID_RENAME, ID_DELETE, ID_SYNC, ID_COMPOSE };
static char s_note[120] = "";
static unsigned s_shownSec = 0xFFFF;         // what the "now playing" panel shows (redrawn when it changes)
static int s_shownMode = -1;
char music_compose_ask = 0;                  // set by Compose: main switches to Chat with a request

// ---------------------------------------------------------------- list

static const char far *m_item( void *, int i ) {
  static char t[70];
  const Song *s = jb_song( i );
  if ( !s ) return "";
  char date[12];
  jb_when_date( s->when, date );
  sprintf( t, "%c %-31.31s %2u:%02u  %s", i == jb_current( ) ? 14 : ' ', s->title, s->secs / 60, s->secs % 60, date );
  return t;
}

void music_rescan( void ) {
  int sel = s_ws[0].sel;
  jb_scan( );
  if ( sel >= jb_count( ) ) sel = jb_count( ) - 1;
  if ( sel < 0 ) sel = 0;
  list_set( &s_ws[0], jb_count( ), m_item, 0 );
  s_ws[0].sel = sel;
}

// ---------------------------------------------------------------- drawing

#define PX 456
#define PY ( WIN_Y + 20 )

static void draw_panel( void ) {
  int mode = jb_mode( );
  gui_mouse_hide( );
  ui_sunken( PX - 4, PY - 2, 172, 76, WHITE );
  vid_text( PX, PY + 2, mode == JB_IDLE ? "Nothing playing" : "Now playing", DGRAY, WHITE );
  if ( mode != JB_IDLE && mode != JB_JINGLE ) {
    const Song *s = jb_song( jb_current( ) );
    ui_text_fit( PX, PY + 16, 164, s ? s->title : "?", BLACK, WHITE );
    char t[40];
    unsigned p = music_position( ), l = music_length( );
    sprintf( t, "%u:%02u / %u:%02u", p / 60, p % 60, l / 60, l % 60 );
    ui_text_fit( PX, PY + 30, 164, t, BLACK, WHITE );
    const char *how = "Plays once";
    if ( mode == JB_LOOP ) how = "Repeats this song";
    else if ( mode == JB_RANDOM ) how = "Random songs";
    ui_text_fit( PX, PY + 44, 164, how, DGRAY, WHITE );
  }
  if ( !music_available( ) ) vid_text( PX, PY + 58, "Needs the Tandy sound chip", RED, WHITE );
  gui_mouse_show( );
  s_shownSec = music_position( );
  s_shownMode = mode;
}

void music_draw( void ) {
  s_ws[0].flags &= ~WF_FREEVIEW;
  form_draw( &s_form );
  draw_panel( );
  if ( s_note[0] ) { app_status( "%s", s_note ); s_note[0] = 0; }
  else if ( jb_total( ) > jb_count( ) )
    app_status( "Showing the newest %d of %d songs.  Delete some to see the rest.", jb_count( ), jb_total( ) );
  else if ( !jb_count( ) )
    app_status( "No songs yet.  Compose one (or ask Qwen in Chat), or Sync the songs made on the PC." );
  else
    app_status( "%d songs.  Enter or double-click plays.  F2 Chat  F4 Gallery", jb_count( ) );
}

// Called by the main loop when nothing happens: the clock and the list mark follow the song
void music_tick( int changed ) {
  if ( changed ) { form_draw_widget( &s_form, 0 ); draw_panel( ); return; }
  if ( jb_mode( ) != s_shownMode || ( jb_mode( ) != JB_IDLE && music_position( ) != s_shownSec ) ) draw_panel( );
}

// ---------------------------------------------------------------- actions

static void note( const char *fmt, ... ) {
  va_list a;
  va_start( a, fmt );
  vsprintf( s_note, fmt, a );
  va_end( a );
}

static void play( int mode ) {
  int i = s_ws[0].sel;
  int rc = mode == JB_RANDOM && jb_count( ) > 0 && !jb_song( i ) ? jb_random( ) : jb_play( i, mode );
  if ( rc ) note( "Cannot play: %s.", music_error( rc ) );
  else note( mode == JB_LOOP ? "Repeating \"%s\".  Stop ends it." : mode == JB_RANDOM ? "Random songs, starting with \"%s\"." : "Playing \"%s\".",
             jb_song( jb_current( ) ) ? jb_song( jb_current( ) )->title : "" );
}

static void do_rename( void ) {
  int i = s_ws[0].sel;
  const Song *s = jb_song( i );
  if ( !s ) return;
  char buf[40];
  str_copy( buf, s->title, sizeof( buf ) );
  if ( !input_box( "Rename", "New title for this song:", buf, sizeof( buf ) ) || !buf[0] ) return;
  if ( jb_rename( i, buf ) ) { msg_box( "Rename", "Could not save SONGS.LST.", "OK" ); return; }
  if ( app_net && app_server.ok ) {
    char pth[40];
    sprintf( pth, "/music/%s/title", s->file );
    app_post_short( pth, buf );                       // best effort: MindServer keeps its own title otherwise
  }
  note( "Renamed.  The file stays %s.T3.", s->file );
}

static void do_delete( void ) {
  int i = s_ws[0].sel;
  const Song *s = jb_song( i );
  if ( !s ) return;
  char q[160], file[9];
  str_copy( file, s->file, sizeof( file ) );
  int net = app_net && app_server.ok;
  sprintf( q, "Delete \"%s\"?%s", s->title, net ? "  Everywhere = on MindServer too." : "" );
  int r = msg_box( "Delete song", q, net ? "Delete|Everywhere|Cancel" : "Delete|Cancel" );
  if ( r == 0 || ( net ? r == 3 : r == 2 ) ) return;
  if ( net && r == 2 ) {
    char pth[40];
    sprintf( pth, "/music/%s/del", file );
    int st = app_post_short( pth, 0 );
    if ( st != 200 && st != 404 &&
         msg_box( "Delete song", "MindServer did not delete it (Sync brings it back). Delete it here anyway?", "Delete here|Cancel" ) != 1 ) return;
  }
  if ( jb_delete( i ) ) { msg_box( "Delete song", "Could not delete the file.", "OK" ); return; }
  music_rescan( );
  note( "Deleted %s.", file );
}

static int dl_progress( unsigned long done, long total ) {
  char t[60];
  if ( total > 0 ) sprintf( t, "Downloading song... %lu%%", done * 100ul / (unsigned long)total );
  else sprintf( t, "Downloading song..." );
  app_spin( t );
  return kbd_get( ) == K_ESC;
}

// GET /music/<id> into MUSIC\<id>.T3 (through a .TMP file, checked before it counts).  0 = ok.
int music_download( const char *id, const char *title, unsigned long when ) {
  if ( !app_net ) return 1;
  mkdir( cfg.music );
  char tmp[80], file[80], path[40];
  sprintf( tmp, "%s\\%.8s.TMP", cfg.music, id );
  jb_path( file, id );
  sprintf( path, "/music/%.8s", id );
  int st = 0;
  snd_settle( );
  long n = http_get_file( cfg.server, cfg.port, path, tmp, dl_progress, &st );
  if ( n < 0 || st != 200 || music_probe( tmp, 0 ) != MUS_OK ) { remove( tmp ); return 1; }
  remove( file );
  if ( rename( tmp, file ) ) { remove( tmp ); return 1; }
  jb_add( id, title, when );
  return 0;
}

int music_have( const char *id ) {
  char p[80];
  jb_path( p, id );
  return access( p, 0 ) == 0;
}

void music_sync( void ) {
  if ( !app_need_net( "Sync" ) ) return;
  struct Want { char id[9]; char title[40]; unsigned long when; };
  int room = JB_MAX - jb_total( );
  if ( room <= 0 ) { msg_box( "Sync", "The song list is full (150 songs).  Delete some first.", "OK" ); return; }
  if ( room > 40 ) room = 40;                          // per sync; the next one fetches more
  Want *want = (Want *)malloc( sizeof( Want ) * room );
  if ( !want ) { msg_box( "Sync", "Not enough memory.", "OK" ); return; }
  busy_begin( "Sync songs", "Asking for the song list..." );
  int nwant = 0, have = 0, more = 0, st, cancelled = 0;
  if ( http_open( cfg.server, cfg.port, "GET", "/music/list", 0, 0, 0 ) ) st = NET_ERROR;
  else while ( ( st = http_headers( ) ) == NET_AGAIN ) if ( kbd_get( ) == K_ESC ) { cancelled = 1; break; }
  if ( st == 200 ) {
    static char line[ 120 ];
    for ( ;; ) {
      int rc = http_line( line, sizeof( line ) );
      if ( rc == NET_AGAIN ) { if ( kbd_get( ) == K_ESC ) { cancelled = 1; break; } continue; }
      if ( rc != 1 ) { if ( rc != NET_DONE ) st = rc; break; }
      Want w;
      char stamp[16];
      unsigned secs;
      int k = 0;
      if ( sscanf( line, "%8s %15s %u %n", w.id, stamp, &secs, &k ) < 3 ) continue;
      w.when = jb_when_parse( stamp );
      str_copy( w.title, line + k, sizeof( w.title ) );
      if ( music_have( w.id ) ) have++;
      else if ( nwant < room ) want[ nwant++ ] = w;
      else more++;
    }
  }
  http_close( );
  if ( cancelled || st != 200 ) {
    busy_end( ); free( want );
    if ( cancelled ) app_status( "Sync cancelled." );
    else msg_box( "Sync", st == 404 ? "This MindServer has no music yet (update it)." : st > 0 ? "MindServer refused the list." : net_error( ), "OK" );
    return;
  }
  int got = 0, failed = 0;
  for ( int i = 0; i < nwant; i++ ) {
    char msg[70];
    sprintf( msg, "Song %d of %d: %.40s", i + 1, nwant, want[i].title );
    busy_text( msg );
    if ( music_download( want[i].id, want[i].title, want[i].when ) ) {
      failed++;
      if ( !strcmp( net_error( ), "Cancelled" ) ) break;
    }
    else got++;
  }
  free( want );
  busy_end( );
  music_rescan( );
  note( "Sync: %d new, %d already here%s%s", got, have, failed ? ", some failed" : "", more ? ".  Sync again for more" : "" );
  music_draw( );
}

// ---------------------------------------------------------------- screen API

void music_init( void ) {
  memset( s_ws, 0, sizeof( s_ws ) );
  int by = 150;
  s_ws[0].type = W_LIST; s_ws[0].id = ID_LIST; s_ws[0].x = 4; s_ws[0].y = 4; s_ws[0].w = 440; s_ws[0].h = 4 + 13 * ROW_H;
  s_ws[0].item = m_item;
  static const struct { int id, x, w; const char *t; } b[] = {
    { ID_PLAY, 4, 56, "Play" }, { ID_LOOP, 64, 56, "Loop" }, { ID_RANDOM, 124, 72, "Random" },
    { ID_STOP, 200, 56, "Stop" }, { ID_RENAME, 262, 72, "Rename" }, { ID_DELETE, 338, 72, "Delete" },
    { ID_SYNC, 414, 56, "Sync" }, { ID_COMPOSE, 532, 88, "Compose" } };
  for ( int k = 0; k < 8; k++ ) {
    Widget &w = s_ws[ k + 1 ];
    w.type = W_BUTTON; w.id = b[k].id; w.x = b[k].x; w.y = by; w.w = b[k].w; w.h = 14; w.text = b[k].t;
  }
  s_ws[1].flags = WF_DEFAULT;
  form_init( &s_form, "Music", 0, WIN_Y, 640, WIN_H, s_ws, 9 );
  s_form.focus = 0;
  music_rescan( );
}

int music_event( Event *e ) {
  int id = form_event( &s_form, e );
  switch ( id ) {
    case ID_LIST:
    case ID_PLAY:    play( JB_ONCE ); break;
    case ID_LOOP:    play( JB_LOOP ); break;
    case ID_RANDOM:  play( JB_RANDOM ); break;
    case ID_STOP:    jb_stop( ); note( "Stopped." ); break;
    case ID_RENAME:  do_rename( ); break;
    case ID_DELETE:  do_delete( ); break;
    case ID_SYNC:    music_sync( ); return CMD_NONE;
    case ID_COMPOSE: music_compose_ask = 1; return CMD_GOTO_CHAT;
  }
  if ( id ) {
    music_draw( );
    app_menu_status( );
    if ( s_form.focus != 0 ) form_set_focus( &s_form, 0 );   // arrows and Enter work on the list again
  }
  return CMD_NONE;
}
