// DeskMind - Music screen: the song library, play / loop / random / stop, rename, delete, sync.

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <dos.h>
#include <io.h>
#include <direct.h>
#include <conio.h>

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

#define NWS 13                                // list + 8 buttons + 4 revision buttons
static Widget s_ws[16];                      // room to spare: an array shorter than its controls once
static Form s_form;                          // corrupted the Settings dialog
enum { ID_LIST = 100, ID_PLAY, ID_LOOP, ID_RANDOM, ID_STOP, ID_RENAME, ID_DELETE, ID_SYNC, ID_COMPOSE,
       ID_REVISE, ID_UNDO, ID_TRYTWO, ID_KEEP };
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
  if ( music_output( ) == MUSIC_SPEAKER ) vid_text( PX, PY + 58, "On the PC speaker", DGRAY, WHITE );
  if ( music_streaming( ) && music_underruns( ) ) {
    char u[40];
    sprintf( u, "Waited for the disk %u times", music_underruns( ) );
    ui_text_fit( PX, PY + 58, 164, u, RED, WHITE );
  }
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
  music_release( file );                         // a song streaming from this file stops before it goes
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

// ---------------------------------------------------------------- revisions (MindServer jobs)

// After a job: download the new songs, delete the ones they replace, select and play the first new one
static void apply_job( char ids[][9], char titles[][40], int n, char repl[][9], int nr ) {
  int got = 0;
  for ( int k = 0; k < n; k++ ) {
    if ( music_have( ids[k] ) ) { got++; continue; }
    char msg[70];
    sprintf( msg, "Downloading %.40s", titles[k] );
    busy_text( msg );
    if ( music_download( ids[k], titles[k], 0 ) == 0 ) got++;
  }
  for ( int r = 0; r < nr; r++ ) {
    int keep = 0;
    for ( int k = 0; k < n; k++ ) if ( str_ieq( ids[k], repl[r] ) ) keep = 1;
    int i = jb_find( repl[r] );
    if ( !keep && i >= 0 ) jb_delete( i );
  }
  music_rescan( );
  int i = n ? jb_find( ids[0] ) : -1;
  if ( i >= 0 ) {
    s_ws[0].sel = i;
    jb_play( i, JB_ONCE );
  }
  if ( got < n ) note( "Some songs could not be downloaded: %s.  Sync tries again.", net_error( ) );
  else if ( n > 1 ) note( "Two sketches: \"%s\" and \"%s\".  Play both, then Keep the one you like.", titles[0], titles[1] );
  else if ( n ) note( "Now: \"%s\".  Undo brings back the version before.", titles[0] );
}

// POST path (JSON body or none) -> "J <n>", then GET /music/job/<n>?wait=1 for the result:
// M <id> <title> per new song and R <id> per song it replaces.  0 = it worked.
// The WiFi drops packets now and then (real TL/3, round 10): a lost answer used to end the job with
// "Connection reset" or "The server stopped answering" although MindServer finished it, and the
// user's retry made a second revision.  Now each request carries a key (?req=): a POST that got
// no answer is sent again with the same key and MindServer hands back the job the first one made.
// The wait for the result reconnects too (it only asks), and MindServer sends a line every 5 s.
#define JOB_TRIES 3

static int s_cancel;

static void retry_pause( const char *text ) {
  busy_text( text );
  unsigned long t0 = ticks( );
  while ( ticks( ) - t0 < 18 ) { net_poll( ); if ( kbd_get( ) == K_ESC ) s_cancel = 1; }
}

// One POST: the job number, 0 if MindServer answered without one (err set, *st = HTTP status),
// or 0 with *st < 0 when the network lost it.
static int post_job( const char *path, const char *body, char *err, int *st ) {
  static char line[ 120 ];
  int job = 0;
  *st = NET_ERROR;
  if ( http_open( cfg.server, cfg.port, "POST", path, body, body ? (unsigned)strlen( body ) : 0,
                  body ? "application/json" : 0 ) == 0 ) {
    while ( ( *st = http_headers( ) ) == NET_AGAIN ) if ( kbd_get( ) == K_ESC ) { s_cancel = 1; break; }
    while ( *st > 0 && !s_cancel ) {
      int rc = http_line( line, sizeof( line ) );
      if ( rc == NET_AGAIN ) continue;
      if ( rc == 1 && line[0] == 'J' ) job = atoi( line + 2 );
      else if ( rc == 1 && line[0] == 'E' ) str_copy( err, line + 2, sizeof( err ) );
      else if ( rc != NET_DONE && rc != 1 ) *st = rc;      // lost on the way
      break;
    }
  }
  http_close( );
  return job;
}

static int run_job( const char *path, const char *body, const char *what ) {
  if ( !app_need_net( what ) ) return 1;
  static char line[ 120 ], url[ 64 ];
  static char ids[3][9], titles[3][40], repl[4][9];
  int n = 0, nr = 0, job = 0, st = NET_ERROR, done = 0;
  char err[100] = "";
  unsigned long save = net_timeout_ms;
  s_cancel = 0;
  outp( 0x43, 0 );                               // request key: BIOS ticks and the PIT counter
  unsigned pit = inp( 0x40 ); pit |= inp( 0x40 ) << 8;
  sprintf( url, "%s?req=%08lX%04X", path, ticks( ), pit );
  net_timeout_ms = 20000;
  busy_begin( what, "Asking MindServer..." );
  for ( int t = 0; t < JOB_TRIES && !s_cancel; t++ ) {
    if ( t ) retry_pause( "No answer, asking again..." );
    if ( s_cancel ) break;
    err[0] = 0;
    job = post_job( url, body, err, &st );
    if ( job > 0 || st > 0 ) break;              // a job, or MindServer said no: don't repeat
  }
  if ( job > 0 ) {
    char p[40];
    sprintf( p, "/music/job/%d?wait=1", job );
    busy_text( "Composing on the PC..." );
    for ( int t = 0; t < JOB_TRIES && !done && !s_cancel; t++ ) {
      if ( t ) retry_pause( "Connection lost, asking again..." );
      if ( s_cancel ) break;
      n = nr = 0; err[0] = 0;                    // the results come again, all of them
      if ( http_open( cfg.server, cfg.port, "GET", p, 0, 0, 0 ) == 0 ) {
        while ( ( st = http_headers( ) ) == NET_AGAIN ) if ( kbd_get( ) == K_ESC ) { s_cancel = 1; break; }
        while ( st == 200 && !s_cancel ) {
          int rc = http_line( line, sizeof( line ) );
          if ( rc == NET_AGAIN ) { if ( kbd_get( ) == K_ESC ) s_cancel = 1; continue; }
          if ( rc != 1 ) break;                  // lost (or closed without D): ask again
          if ( line[0] == 'D' ) { done = 1; break; }
          if ( line[0] == 'M' && n < 3 && strlen( line ) >= 10 ) {
            str_copy( ids[n], line + 2, 9 );
            str_copy( titles[n], strlen( line ) > 11 ? line + 11 : ids[n], 40 );
            n++;
          }
          else if ( line[0] == 'R' && nr < 4 ) str_copy( repl[ nr++ ], line + 2, 9 );
          else if ( line[0] == 'E' ) str_copy( err, line + 2, sizeof( err ) );
        }
        if ( st > 0 && st != 200 ) done = 1;     // MindServer answered (no such job): final
      }
      http_close( );                             // one socket: downloads only after this
    }
  }
  int cancelled = s_cancel;
  net_timeout_ms = save;
  if ( !cancelled && n ) apply_job( ids, titles, n, repl, nr );
  busy_end( );
  if ( cancelled ) { note( "Cancelled (MindServer may still finish it: Sync fetches it)." ); return 1; }
  if ( !n ) {
    static char m[160];
    if ( err[0] ) str_copy( m, err, sizeof( m ) );
    else if ( job > 0 ) sprintf( m, "%s.  MindServer may still finish it: Sync fetches it.", net_error( ) );
    else str_copy( m, st > 0 ? "MindServer refused it (update MindServer)." : net_error( ), sizeof( m ) );
    msg_box( what, m, "OK" );
    return 1;
  }
  return 0;
}

static const Song *selected( void ) {
  const Song *s = jb_song( s_ws[0].sel );
  if ( !s ) note( "Select a song first." );
  return s;
}

static void do_revise( void ) {
  const Song *s = selected( );
  if ( !s ) return;
  static const char *const drumT[] = { "Drums: as they are", "Drums: none", "Drums: light", "Drums: full",
                                       "Drums: another pattern" };
  static const char *const tempoT[] = { "Tempo: as it is", "Tempo: faster", "Tempo: slower" };
  static const char *const keyT[] = { "Key: as it is", "Key: higher", "Key: lower" };
  static const char *const lenT[] = { "Length: as it is", "Length: short", "Length: medium", "Length: long",
                                      "Length: extended", "Length: epic" };
  static const char *const lenN[] = { "", "short", "medium", "long", "extended", "epic" };
#define MAXW 14
  static Widget ws[ MAXW ];
  memset( ws, 0, sizeof( ws ) );
  int w = 380, h = 140, x = ( ( 640 - w ) / 2 ) & ~1, y = 24, n = 0;
  int sel[4] = { 0, 0, 0, 0 }, cyc[4];
  static const int counts[4] = { 5, 3, 3, 6 };
  const char *const *texts[4] = { drumT, tempoT, keyT, lenT };
  ws[n].type = W_LABEL; ws[n].x = 8; ws[n].y = 4; ws[n].w = 360; ws[n].h = 12;
  ws[n].text = "Keep these parts note for note:"; n++;
  int ck = n;
  static const char *const parts[] = { "The tune (lead)", "The accompaniment", "The bass" };
  for ( int k = 0; k < 3; k++ ) {
    ws[n].type = W_CHECK; ws[n].x = 16; ws[n].y = 18 + 13 * k; ws[n].w = 340; ws[n].h = 10;
    ws[n].text = parts[k]; ws[n].checked = 1; n++;
  }
  for ( int k = 0; k < 4; k++ ) {
    cyc[k] = n;
    ws[n].type = W_BUTTON; ws[n].id = 3 + k; ws[n].x = k % 2 ? 196 : 8; ws[n].y = 62 + 18 * ( k / 2 ); ws[n].w = 176;
    ws[n].h = 14; ws[n].text = texts[k][0]; n++;
  }
  ws[n].type = W_LABEL; ws[n].x = 8; ws[n].y = 102; ws[n].w = 360; ws[n].h = 12;
  ws[n].text = "Unticked parts get new notes."; n++;
  ws[n].type = W_BUTTON; ws[n].id = 1; ws[n].x = w - 188; ws[n].y = 118; ws[n].w = 80; ws[n].h = 14;
  ws[n].text = "Revise"; ws[n].flags = WF_DEFAULT; n++;
  ws[n].type = W_BUTTON; ws[n].id = 2; ws[n].x = w - 100; ws[n].y = 118; ws[n].w = 80; ws[n].h = 14;
  ws[n].text = "Cancel"; ws[n].flags = WF_CANCEL; n++;
  if ( n > MAXW ) { msg_box( "Revise", "Internal error: too many controls.", "OK" ); return; }
#undef MAXW
  Form f;
  unsigned char far *save = ui_save( x, y, w + 6, h + 3 );
  form_init( &f, "Revise song", x, y, w, h, ws, n );
  form_draw( &f );
  int r;
  while ( ( r = form_run( &f ) ) >= 3 && r <= 6 ) {      // the cycle buttons
    int k = r - 3;
    sel[k] = ( sel[k] + 1 ) % counts[k];
    ws[ cyc[k] ].text = texts[k][ sel[k] ];
    form_draw_widget( &f, cyc[k] );
  }
  ui_restore( x, y, w + 6, h + 3, save );
  if ( r != 1 ) return;
  static char body[200];
  static const char *const partN[] = { "lead", "accompaniment", "bass" };
  strcpy( body, "{\"regenerate\":[" );
  int any = 0;
  for ( int k = 0; k < 3; k++ ) if ( !ws[ ck + k ].checked ) {
    sprintf( body + strlen( body ), "%s\"%s\"", any ? "," : "", partN[k] );
    any = 1;
  }
  strcat( body, "]" );
  static const char *const drumN[] = { "", "none", "light", "full" };
  if ( sel[0] >= 1 && sel[0] <= 3 ) sprintf( body + strlen( body ), ",\"drums\":\"%s\"", drumN[ sel[0] ] );
  if ( sel[0] == 4 ) strcat( body, ",\"drum_pattern\":\"next\"" );
  if ( sel[1] ) sprintf( body + strlen( body ), ",\"tempo\":\"%s\"", sel[1] == 1 ? "faster" : "slower" );
  if ( sel[2] ) sprintf( body + strlen( body ), ",\"key\":\"%s\"", sel[2] == 1 ? "up" : "down" );
  if ( sel[3] ) sprintf( body + strlen( body ), ",\"length\":\"%s\"", lenN[ sel[3] ] );
  strcat( body, "}" );
  if ( !any && !sel[0] && !sel[1] && !sel[2] && !sel[3] ) { note( "Nothing to change: untick a part or pick a change." ); return; }
  char path[40];
  sprintf( path, "/music/%s/revise", s->file );
  run_job( path, body, "Revise song" );
}

static void do_undo( void ) {
  const Song *s = selected( );
  if ( !s ) return;
  char path[40];
  sprintf( path, "/music/%s/undo", s->file );
  run_job( path, 0, "Undo" );
}

static void do_trytwo( void ) {
  const Song *s = selected( );
  if ( !s ) return;
  static char body[40];
  sprintf( body, "{\"like\":\"%s\"}", s->file );
  run_job( "/music/sketch", body, "Try two" );
}

static void do_keep( void ) {
  const Song *s = selected( );
  if ( !s ) return;
  if ( strncmp( s->title, "Sketch ", 7 ) ) {
    msg_box( "Keep", "Keep is for sketches: Try two makes two short sketches of a song, and Keep turns the "
                     "one you like into a full song.", "OK" );
    return;
  }
  char path[40];
  sprintf( path, "/music/%s/build", s->file );
  run_job( path, 0, "Keep sketch" );
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
  // Revisions, under the "now playing" panel
  static const struct { int id, x, y; const char *t; } r[] = {
    { ID_REVISE, 452, 104, "Revise..." }, { ID_UNDO, 540, 104, "Undo" },
    { ID_TRYTWO, 452, 122, "Try two" }, { ID_KEEP, 540, 122, "Keep" } };
  for ( int k = 0; k < 4; k++ ) {
    Widget &w = s_ws[ k + 9 ];
    w.type = W_BUTTON; w.id = r[k].id; w.x = r[k].x; w.y = r[k].y; w.w = 84; w.h = 14; w.text = r[k].t;
  }
  s_ws[1].flags = WF_DEFAULT;
  form_init( &s_form, "Music", 0, WIN_Y, 640, WIN_H, s_ws, NWS );
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
    case ID_REVISE:  do_revise( ); break;
    case ID_UNDO:    do_undo( ); break;
    case ID_TRYTWO:  do_trytwo( ); break;
    case ID_KEEP:    do_keep( ); break;
  }
  if ( id ) {
    music_draw( );
    app_menu_status( );
    if ( s_form.focus != 0 ) form_set_focus( &s_form, 0 );   // arrows and Enter work on the list again
  }
  return CMD_NONE;
}
