// DeskMind - Create screen: prompt -> (Qwen improves it, you may edit) -> Krea 2 -> Tandy picture.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#include "app.h"
#include "scr.h"
#include "video.h"
#include "sound.h"
#include "sys.h"
#include "cfg.h"
#include "tpi.h"
#include "net.h"

#define WIN_Y  SCR_Y
#define WIN_H  ( SCR_H - 1 )

static char s_prompt[ 600 ] = "";
static char s_final[ 1100 ] = "";          // what was actually sent to the generator
static char s_lastId[9] = "";
static char s_lastTitle[40] = "";

static Widget s_ws[10];
static Form s_form;
enum { ID_CREATE = 1, ID_AGAIN, ID_VIEW, ID_RULES };
enum { W_PROMPT = 1, W_ENH = 2, W_REV = 3, W_FINAL = 9 };

// ---------------------------------------------------------------- progress box

static int s_px, s_py, s_pw = 400, s_ph = 50;
static unsigned char far *s_psave = 0;

static void progress_begin( const char *title ) {
  s_px = ( ( 640 - s_pw ) / 2 ) & ~1; s_py = 70;
  s_psave = ui_save( s_px, s_py, s_pw + 6, s_ph + 3 );
  ui_window( s_px, s_py, s_pw, s_ph, title, 1 );
}

static void progress_set( int pct, const char *text ) {
  static int lastPct = -1;
  gui_mouse_hide( );
  vid_fill( s_px + 6, s_py + 15, s_pw - 12, 10, LGRAY );
  ui_text_fit( s_px + 10, s_py + 16, s_pw - 20, text, BLACK, -1 );
  if ( pct >= 0 ) {
    int bw = s_pw - 24;
    if ( pct != lastPct ) {
      ui_sunken( s_px + 10, s_py + 30, bw + 4, 12, WHITE );
      vid_fill( s_px + 12, s_py + 31, bw * pct / 100, 10, BLUE );
      lastPct = pct;
    }
  }
  gui_mouse_show( );
}

static void progress_end( void ) {
  if ( s_psave ) ui_restore( s_px, s_py, s_pw + 6, s_ph + 3, s_psave );
  s_psave = 0;
}

// ---------------------------------------------------------------- steps

static char s_enhErr[ 300 ];

// POST /enhance: returns 1 and fills out; 0 on error (reason in s_enhErr, empty = cancelled)
static int enhance( const char *idea, char *out, unsigned size ) {
  static char line[ 1200 ];
  s_enhErr[0] = 0;
  if ( http_open( cfg.server, cfg.port, "POST", cfg_cga ? "/enhance?mode=cga" : "/enhance", idea, (unsigned)strlen( idea ), "text/plain" ) ) {
    str_copy( s_enhErr, net_error( ), sizeof( s_enhErr ) );
    return 0;
  }
  unsigned long save = net_timeout_ms;
  net_timeout_ms = 120000ul;
  out[0] = 0;
  int st, ok = 0, err = 0;
  while ( ( st = http_headers( ) ) == NET_AGAIN ) progress_set( -1, "Qwen is improving your prompt..." );
  if ( st == 200 ) {
    for ( ;; ) {
      int r = http_line( line, sizeof( line ) );
      if ( r == NET_AGAIN ) {
        static const char *dots[] = { "Qwen is improving your prompt   ", "Qwen is improving your prompt.  ",
                                      "Qwen is improving your prompt.. ", "Qwen is improving your prompt..." };
        progress_set( -1, dots[ ( ticks( ) / 6 ) & 3 ] );
        if ( kbd_get( ) == K_ESC ) break;
        continue;
      }
      if ( r != 1 ) break;
      if ( line[0] == 'T' ) { str_copy( out, line + 2, size ); ok = 1; }
      else if ( line[0] == 'E' ) { err = 1; str_copy( out, line + 2, size ); }
      else if ( line[0] == 'D' ) break;
    }
  }
  http_close( );
  net_timeout_ms = save;
  if ( err ) { str_copy( s_enhErr, out, sizeof( s_enhErr ) ); return 0; }
  if ( st != 200 && !s_enhErr[0] ) str_copy( s_enhErr, st < 0 ? net_error( ) : "MindServer refused the request.", sizeof( s_enhErr ) );
  return ok && out[0];
}

// POST /gen then follow /job/<n>?wait=1.  Returns 1 and the picture id.
static int generate( const char *prompt, const char *title, char *id9, char *titleOut ) {
  static char path[ 200 ], enc[ 130 ], line[ 300 ];
  url_encode( enc, sizeof( enc ), title );
  sprintf( path, "/gen?mode=%s&title=%s", cfg_cga ? "cga" : "640", enc );
  static char buf[ 64 ];
  if ( http_open( cfg.server, cfg.port, "POST", path, prompt, (unsigned)strlen( prompt ), "text/plain" ) ) {
    progress_end( ); msg_box( "Create", net_error( ), "OK" ); return 0;
  }
  int st;
  while ( ( st = http_headers( ) ) == NET_AGAIN ) ;
  int n = 0, got = 0;
  while ( st == 200 ) {
    int r = http_read( buf + got, sizeof( buf ) - 1 - got );
    if ( r > 0 ) got += r; else if ( r != NET_AGAIN ) break;
  }
  http_close( );
  buf[got] = 0;
  if ( st != 200 || buf[0] != 'J' ) {
    progress_end( );
    msg_box( "Create", st == 200 ? buf + 2 : "MindServer did not accept the job.", "OK" );
    return 0;
  }
  n = atoi( buf + 2 );

  sprintf( path, "/job/%d?wait=1", n );
  if ( http_open( cfg.server, cfg.port, "GET", path, 0, 0, 0 ) ) { progress_end( ); msg_box( "Create", net_error( ), "OK" ); return 0; }
  unsigned long save = net_timeout_ms;
  net_timeout_ms = 180000ul;
  while ( ( st = http_headers( ) ) == NET_AGAIN ) ;
  int result = 0, cancelled = 0;
  while ( st == 200 && !cancelled ) {
    int r = http_line( line, sizeof( line ) );
    if ( r == NET_AGAIN ) { if ( kbd_get( ) == K_ESC ) cancelled = 1; continue; }
    if ( r != 1 ) break;
    if ( line[0] == 'S' ) {
      // "S running <pct> <text>"
      int pct = 0;
      char text[80] = "";
      sscanf( line + 2, "%*s %d %79[^\n]", &pct, text );
      char t[120];
      sprintf( t, "Drawing: %s  (%d%%)", text, pct );
      progress_set( pct, t );
    }
    else if ( line[0] == 'I' ) {
      str_copy( id9, line + 2, 9 );
      str_copy( titleOut, strlen( line ) > 11 ? line + 11 : "", 40 );
      result = 1;
    }
    else if ( line[0] == 'E' ) {
      progress_end( );
      http_close( );
      net_timeout_ms = save;
      msg_box( "Create", line + 2, "OK" );
      return 0;
    }
    else if ( line[0] == 'D' ) break;
  }
  http_close( );
  net_timeout_ms = save;
  if ( cancelled ) { progress_end( ); app_status( "Stopped following the job (MindServer finishes it; Sync fetches it)." ); }
  return result;
}

static void show_result( void );

static void create_picture( int again ) {
  if ( !app_need_net( "Creating pictures" ) ) return;
  if ( !again ) {
    if ( !s_prompt[0] ) { msg_box( "Create", "Describe your picture first.", "OK" ); return; }
    str_copy( s_final, s_prompt, sizeof( s_final ) );
    cfg.enhance = s_ws[ W_ENH ].checked;
    cfg.review = s_ws[ W_REV ].checked;
    if ( cfg.enhance ) {
      static char better[ 1100 ];
      int ok = 0;
      if ( !app_server.qwen ) app_ping( );             // maybe it has come up since
      if ( app_server.qwen ) {
        progress_begin( "Improving your prompt" );
        ok = enhance( s_prompt, better, sizeof( better ) );
        progress_end( );
      }
      else str_copy( s_enhErr, "Qwen isn't running on the MindServer PC right now.", sizeof( s_enhErr ) );
      if ( !ok ) {
        if ( !s_enhErr[0] ) { app_status( "Cancelled." ); return; }   // Esc while improving
        static char q[ 400 ];
        sprintf( q, "%s  Draw your prompt as you typed it?", s_enhErr );
        if ( msg_box( "Improve prompt", q, "Draw it|Cancel" ) != 1 ) return;
      }
      else str_copy( s_final, better, sizeof( s_final ) );
      if ( ok && cfg.review ) {
        if ( !memo_box( "Improved prompt", "Qwen's version of your idea. Edit it if you like, then OK to draw it:",
                        s_final, sizeof( s_final ) ) ) {
          app_status( "Cancelled." );
          return;
        }
      }
    }
  }
  else if ( !s_final[0] ) return;

  char title[40];
  str_copy( title, s_prompt[0] ? s_prompt : s_final, sizeof( title ) );
  progress_begin( "Drawing with Krea 2" );
  progress_set( 0, "Sending to MindServer..." );
  char id[9] = "", t[40] = "";
  if ( !generate( s_final, title, id, t ) ) return;
  progress_set( 100, "Downloading the picture..." );
  int rc = app_download_pic( id );
  progress_end( );
  if ( rc ) { msg_box( "Create", "The picture was made but could not be downloaded. Try Sync in the Gallery.", "OK" ); return; }
  str_copy( s_lastId, id, sizeof( s_lastId ) );
  str_copy( s_lastTitle, t, sizeof( s_lastTitle ) );
  gallery_rescan( );
  snd_play( SND_IMAGE );
  app_view_pic( id );             // full screen, then back here
}

void create_edit_rules( void ) {
  if ( !app_need_net( "Editing the enhancement rules" ) ) return;
  static char rules[ 2000 ];
  int st = 0;
  long n = http_get_all( cfg.server, cfg.port, "/prompt/enhance", rules, sizeof( rules ) - 1, &st );
  if ( n < 0 || st != 200 ) { msg_box( "Enhancement rules", "Could not read them from MindServer.", "OK" ); return; }
  rules[n] = 0;
  // CRLF -> LF for the editor
  char *w = rules;
  for ( char *r = rules; *r; r++ ) if ( *r != '\r' ) *w++ = *r;
  *w = 0;
  if ( !memo_box( "Enhancement rules", "How Qwen rewrites your picture ideas (saved on MindServer):", rules, sizeof( rules ) ) ) return;
  if ( http_open( cfg.server, cfg.port, "POST", "/prompt/enhance", rules, (unsigned)strlen( rules ), "text/plain" ) == 0 ) {
    int s;
    while ( ( s = http_headers( ) ) == NET_AGAIN ) ;
    http_close( );
    app_status( s == 200 ? "Enhancement rules saved on MindServer." : "MindServer did not accept the rules." );
  }
}

// ---------------------------------------------------------------- screen

static void show_result( void ) {
  int rx = 440, ry = WIN_Y + 88;          // below the prompt box
  gui_mouse_hide( );
  vid_fill( rx - 6, ry - 3, 190, 68, LGRAY );
  if ( s_lastId[0] ) {
    static unsigned char far *th = 0;
    if ( !th ) th = (unsigned char far *)_fmalloc( TPI_THUMB_MAX );
    char p[80];
    TpiHeader h;
    app_pic_path( p, s_lastId );
    ui_sunken( rx - 4, ry - 2, 168, 54, BLACK );
    if ( th && tpi_thumb( p, th, &h ) == 0 ) tpi_draw_thumb( rx, ry, th, h.thumb_w, h.thumb_h );
    ui_text_fit( rx - 4, ry + 54, 184, s_lastTitle, BLACK, -1 );
  }
  else {
    ui_text_fit( rx - 4, ry + 20, 184, "Your picture shows here", DGRAY, -1 );
  }
  gui_mouse_show( );
}

void create_init( void ) {
  memset( s_ws, 0, sizeof( s_ws ) );
  int n = 0;
  s_ws[n].type = W_LABEL; s_ws[n].x = 4; s_ws[n].y = 2; s_ws[n].w = 620; s_ws[n].h = 10;
  s_ws[n].text = "Describe the picture you want (any language, as long or short as you like):"; n++;
  s_ws[n].type = W_MEMO;  s_ws[n].x = 4; s_ws[n].y = 14; s_ws[n].w = 620; s_ws[n].h = 4 + 5 * ROW_H;
  s_ws[n].buf = s_prompt; s_ws[n].size = sizeof( s_prompt ); n++;
  s_ws[n].type = W_CHECK; s_ws[n].x = 4; s_ws[n].y = 72; s_ws[n].w = 400; s_ws[n].h = 10;
  s_ws[n].text = "Let Qwen improve my prompt"; s_ws[n].checked = cfg.enhance; n++;
  s_ws[n].type = W_CHECK; s_ws[n].x = 4; s_ws[n].y = 85; s_ws[n].w = 400; s_ws[n].h = 10;
  s_ws[n].text = "Let me edit the improved prompt"; s_ws[n].checked = cfg.review; n++;
  s_ws[n].type = W_BUTTON; s_ws[n].id = ID_CREATE; s_ws[n].x = 4;   s_ws[n].y = 102; s_ws[n].w = 96; s_ws[n].h = 16;
  s_ws[n].text = "Create"; s_ws[n].flags = WF_DEFAULT; n++;
  s_ws[n].type = W_BUTTON; s_ws[n].id = ID_AGAIN;  s_ws[n].x = 108; s_ws[n].y = 102; s_ws[n].w = 80; s_ws[n].h = 16; s_ws[n].text = "Again"; n++;
  s_ws[n].type = W_BUTTON; s_ws[n].id = ID_VIEW;   s_ws[n].x = 196; s_ws[n].y = 102; s_ws[n].w = 64; s_ws[n].h = 16; s_ws[n].text = "View"; n++;
  s_ws[n].type = W_BUTTON; s_ws[n].id = ID_RULES;  s_ws[n].x = 268; s_ws[n].y = 102; s_ws[n].w = 136; s_ws[n].h = 16; s_ws[n].text = "Enhance rules"; n++;
  s_ws[n].type = W_LABEL; s_ws[n].x = 4; s_ws[n].y = 124; s_ws[n].w = 400; s_ws[n].h = 10;
  s_ws[n].text = "Last prompt sent to the generator:"; s_ws[n].fg = DGRAY; n++;
  s_ws[n].type = W_MEMO;  s_ws[n].x = 4; s_ws[n].y = 136; s_ws[n].w = 404; s_ws[n].h = 4 + 2 * ROW_H;
  s_ws[n].buf = s_final; s_ws[n].size = sizeof( s_final ); s_ws[n].flags = WF_READONLY; n++;
  form_init( &s_form, "Create a picture", 0, WIN_Y, 640, WIN_H, s_ws, n );
  s_form.focus = W_PROMPT;
}

void create_draw( void ) {
  form_draw( &s_form );
  show_result( );
  app_status( "Type your idea, then Create (or Enter).  Esc in a dialog cancels.  F2 Chat  F4 Gallery" );
}

int create_event( Event *e ) {
  // Enter in the prompt editor creates (the memo does not insert line breaks)
  int id = form_event( &s_form, e );
  switch ( id ) {
    case ID_CREATE: create_picture( 0 ); create_draw( ); break;
    case ID_AGAIN:  create_picture( 1 ); create_draw( ); break;
    case ID_VIEW:   if ( s_lastId[0] ) app_view_pic( s_lastId ); break;
    case ID_RULES:  create_edit_rules( ); create_draw( ); break;
  }
  return CMD_NONE;
}
