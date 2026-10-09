// DeskMind - shared application state and helpers.  See app.h.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <malloc.h>
#include <dos.h>
#include <io.h>

#include "app.h"
#include "video.h"
#include "sound.h"
#include "sys.h"
#include "cfg.h"
#include "tpi.h"
#include "net.h"
#include "slide.h"
#include "scr.h"

char app_dir[80] = "";
int  app_net = 0;
int  app_ems = 0;
ServerState app_server = { 0, 0, 0 };
void ( *app_redraw_fn )( void ) = 0;

static char s_status[120];

void app_status( const char *fmt, ... ) {
  va_list ap;
  va_start( ap, fmt );
  vsprintf( s_status, fmt, ap );
  va_end( ap );
  ui_status( s_status );
}

void app_spin( const char *text ) {
  static const char marks[] = "|/-\\";
  static unsigned long last = 0;
  static int k = 0;
  if ( ticks( ) - last < 3 ) return;
  last = ticks( );
  char t[100];
  sprintf( t, "%c %s", marks[ k++ & 3 ], text );
  ui_status( t );
}

void app_redraw( void ) {
  if ( app_redraw_fn ) app_redraw_fn( );
}

// ---------------------------------------------------------------- pictures

void app_pic_path( char *out, const char *id ) {
  sprintf( out, "%s\\%.8s.TPI", cfg_pics( ), id );
}

int app_have_pic( const char *id ) {
  char p[80];
  app_pic_path( p, id );
  return access( p, 0 ) == 0;
}

// The same picture synced in the other mode (PICS vs PICSCGA on one Tandy)
int app_twin_path( char *out, const char *id ) {
  sprintf( out, "%s\\%.8s.TPI", cfg_pics_other( ), id );
  return access( out, 0 ) == 0;
}

const char *app_pc( void ) { return cfg_cga ? "PC" : "Tandy"; }

int app_need_net( const char *what ) {
  if ( app_net ) return 1;
  char t[160];
  if ( cfg_cga ) sprintf( t, "%s needs the network. Start the packet driver (and WiFi), then start DeskMind again.", what );
  else sprintf( t, "%s needs the network. Restart the Tandy with W (WiFi), then start DeskMind again.", what );
  msg_box( "No network", t, "OK" );
  return 0;
}

static int dl_progress( unsigned long done, long total ) {
  char t[60];
  if ( total > 0 ) sprintf( t, "Downloading picture... %lu%%", done * 100ul / (unsigned long)total );
  else sprintf( t, "Downloading picture..." );
  app_spin( t );
  return kbd_get( ) == K_ESC;
}

int app_download_pic( const char *id ) {
  if ( !app_net ) return 1;
  char file[80], path[40];
  app_pic_path( file, id );
  sprintf( path, "/img/%.8s%s", id, cfg_cga ? "?mode=cga" : "" );
  int st = 0;
  snd_settle( );
  snd_log( SL_DISK_BEGIN, SD_DOWNLOAD );
  long n = http_get_file( cfg.server, cfg.port, path, file, dl_progress, &st );
  snd_log( SL_DISK_END, SD_DOWNLOAD );
  return n < 0 ? 1 : 0;
}

int app_upload_pic( const char *id ) {
  if ( !app_net ) return 1;
  char file[80], path[40];
  app_pic_path( file, id );
  sprintf( path, "/img/%.8s/upload", id );
  app_status( "Sending the picture to MindServer..." );
  int st = http_post_file( cfg.server, cfg.port, path, file, "application/octet-stream" );
  return st == 200 ? 0 : 1;
}

int app_view_pic( const char *id ) {
  char p[80];
  app_pic_path( p, id );
  // A picture announced in a chat whose download failed: fetch it now
  if ( !app_have_pic( id ) ) {
    char t[160];
    if ( !app_net ) {
      sprintf( t, "This picture is not on this %s yet. Start the network to download it.", app_pc( ) );
      msg_box( "Picture", t, "OK" );
      return 1;
    }
    app_status( "Downloading the picture..." );
    if ( app_download_pic( id ) ) {
      sprintf( t, "Could not download this picture: %s", net_error( ) );
      msg_box( "Picture", t, "OK" );
      return 1;
    }
    gallery_rescan( );
  }
  gui_mouse_hide( );
  int rc = tpi_show( p );
  if ( rc ) {
    if ( app_gui_mode( ) ) app_redraw( );
    gui_mouse_show( );
    msg_box( "Picture", "Could not show this picture.", "OK" );
    return rc;
  }
  Event e;
  do { gui_poll( &e ); } while ( e.type != EV_KEY && e.type != EV_DOWN );
  app_gui_mode( );
  gui_mouse_show( );
  app_redraw( );
  return 0;
}

int app_gui_mode( void ) {
  // CGA: pictures may have switched to 320x200x4; the GUI lives in 640x200x2
  if ( !vid_is_cga( ) || vid_mode == VM_CGA2 ) return 0;
  vid_switch( VM_CGA2 );
  return 1;
}

// ---------------------------------------------------------------- server

int app_ping( void ) {
  app_server.ok = app_server.qwen = app_server.comfy = 0;
  if ( !app_net ) return 1;
  static char buf[ 300 ];
  int st = 0;
  // Short timeouts: this runs in the background and must not freeze the screen for long
  unsigned long save = net_timeout_ms, saveConn = net_connect_ms;
  net_timeout_ms = 3000;
  net_connect_ms = 3000;
  long n = http_get_all( cfg.server, cfg.port, "/ping", buf, sizeof( buf ) - 1, &st );
  net_timeout_ms = save;
  net_connect_ms = saveConn;
  if ( n < 0 || st != 200 ) return 1;
  buf[n] = 0;
  app_server.ok = strncmp( buf, "OK", 2 ) == 0;
  app_server.qwen = strstr( buf, "qwen up" ) != 0;
  app_server.comfy = strstr( buf, "comfy up" ) != 0;
  return app_server.ok ? 0 : 1;
}

int app_post_short( const char *path, const char *body ) {
  // Short connect timeout: a stopped MindServer must answer "no" in seconds, not freeze the screen
  unsigned long save = net_timeout_ms, saveConn = net_connect_ms;
  net_timeout_ms = 8000;
  net_connect_ms = 3000;
  int st = NET_ERROR;
  if ( http_open( cfg.server, cfg.port, "POST", path, body, body ? (unsigned)strlen( body ) : 0,
                  body ? "text/plain" : 0 ) == 0 ) {
    while ( ( st = http_headers( ) ) == NET_AGAIN ) ;
    http_close( );
  }
  net_timeout_ms = save;
  net_connect_ms = saveConn;
  return st;
}

void app_menu_status( void ) {
  static char right[60];
  if ( !app_net ) strcpy( right, "offline" );
  else if ( !app_server.ok ) strcpy( right, "MindServer ?" );
  else sprintf( right, "Qwen %s  Draw %s", app_server.qwen ? "ok" : "--", app_server.comfy ? "ok" : "--" );
  extern const Menu *main_menus( int *n );
  int n;
  const Menu *m = main_menus( &n );
  menubar_set( m, n, right );
  menubar_draw( );
}

// ---------------------------------------------------------------- settings

void app_settings( void ) {
  // 16 controls today.  The array once had fewer slots than controls: the extra one was
  // written over the port/seconds/effect buffers (smileys on screen, Cancel dead).
#define MAXW 20
  static Widget ws[ MAXW ];
  static char port[8], delay[6];
  memset( ws, 0, sizeof( ws ) );
  sprintf( port, "%u", cfg.port );
  sprintf( delay, "%d", cfg.slide_delay );
  int w = 460, h = 172;
  int x = ( ( 640 - w ) / 2 ) & ~1, y = 12;
  int n = 0;
#define LABEL( _y, _t ) ws[n].type = W_LABEL; ws[n].x = 8; ws[n].y = _y; ws[n].w = 150; ws[n].h = 12; ws[n].text = _t; n++;
  LABEL( 4, "MindServer address" );
  ws[n].type = W_EDIT; ws[n].x = 160; ws[n].y = 3; ws[n].w = 200; ws[n].h = 13;
  ws[n].buf = cfg.server; ws[n].size = sizeof( cfg.server ); ws[n].cur = (unsigned)strlen( cfg.server ); n++;
  LABEL( 20, "Port" );
  ws[n].type = W_EDIT; ws[n].x = 160; ws[n].y = 19; ws[n].w = 72; ws[n].h = 13;
  ws[n].buf = port; ws[n].size = sizeof( port ); ws[n].cur = (unsigned)strlen( port ); n++;
  LABEL( 36, "Token (optional)" );
  ws[n].type = W_EDIT; ws[n].x = 160; ws[n].y = 35; ws[n].w = 200; ws[n].h = 13;
  ws[n].buf = cfg.token; ws[n].size = sizeof( cfg.token ); ws[n].cur = (unsigned)strlen( cfg.token ); n++;
  int ck = n;
  ws[n].type = W_CHECK; ws[n].x = 8; ws[n].y = 54; ws[n].w = 420; ws[n].h = 10;
  ws[n].text = "Sound effects"; ws[n].checked = cfg.sound; n++;
  ws[n].type = W_CHECK; ws[n].x = 8; ws[n].y = 67; ws[n].w = 420; ws[n].h = 10;
  ws[n].text = "Create: improve my prompt with Qwen"; ws[n].checked = cfg.enhance; n++;
  ws[n].type = W_CHECK; ws[n].x = 8; ws[n].y = 80; ws[n].w = 420; ws[n].h = 10;
  ws[n].text = "Create: let me edit the improved prompt first"; ws[n].checked = cfg.review; n++;
  ws[n].type = W_CHECK; ws[n].x = 8; ws[n].y = 93; ws[n].w = 420; ws[n].h = 10;
  ws[n].text = "Chat: also improve prompts of pictures Qwen draws"; ws[n].checked = cfg.draw_enhance == 1; n++;
  LABEL( 108, "Slideshow seconds" );
  ws[n].type = W_EDIT; ws[n].x = 160; ws[n].y = 107; ws[n].w = 56; ws[n].h = 13;
  ws[n].buf = delay; ws[n].size = sizeof( delay ); ws[n].cur = (unsigned)strlen( delay ); n++;
  static char fxLabel[32];
  int effect = cfg.slide_effect;
  sprintf( fxLabel, "Effect: %s", slide_effect_name( effect ) );
  int fxw = n;
  ws[n].type = W_BUTTON; ws[n].id = 3; ws[n].x = 228; ws[n].y = 106; ws[n].w = 200; ws[n].h = 14;
  ws[n].text = fxLabel; n++;
  int shw = n;
  ws[n].type = W_CHECK; ws[n].x = 8; ws[n].y = 124; ws[n].w = 420; ws[n].h = 10;
  ws[n].text = "Slideshow: random order"; ws[n].checked = cfg.slide_shuffle; n++;
  ws[n].type = W_BUTTON; ws[n].id = 1; ws[n].x = w - 200; ws[n].y = 140; ws[n].w = 80; ws[n].h = 14;
  ws[n].text = "Save"; ws[n].flags = WF_DEFAULT; n++;
  ws[n].type = W_BUTTON; ws[n].id = 2; ws[n].x = w - 108; ws[n].y = 140; ws[n].w = 80; ws[n].h = 14;
  ws[n].text = "Cancel"; ws[n].flags = WF_CANCEL; n++;
#undef LABEL
  if ( n > MAXW ) { msg_box( "Settings", "Internal error: too many controls.", "OK" ); return; }
#undef MAXW

  // Keep a copy in case of Cancel
  DmConfig old = cfg;
  Form f;
  unsigned char far *save = ui_save( x, y, w + 6, h + 3 );
  form_init( &f, "Settings", x, y, w, h, ws, n );
  form_draw( &f );
  int r;
  while ( ( r = form_run( &f ) ) == 3 ) {           // the effect button cycles through the transitions
    effect = ( effect + 1 ) % FX_COUNT;
    sprintf( fxLabel, "Effect: %s", slide_effect_name( effect ) );
    form_draw_widget( &f, fxw );
  }
  ui_restore( x, y, w + 6, h + 3, save );
  if ( r != 1 ) { cfg = old; return; }
  cfg.slide_effect = effect;
  cfg.slide_shuffle = ws[shw].checked;
  cfg.port = (unsigned)atoi( port ) ? (unsigned)atoi( port ) : 8286;
  cfg.slide_delay = atoi( delay ) > 0 ? atoi( delay ) : 8;
  cfg.sound = ws[ck].checked;
  cfg.enhance = ws[ck + 1].checked;
  cfg.review = ws[ck + 2].checked;
  cfg.draw_enhance = ws[ck + 3].checked ? 1 : -1;
  snd_enabled = cfg.sound;
  str_copy( net_token, cfg.token, sizeof( net_token ) );
  char path[80];
  cfg_path( path, app_dir );
  if ( cfg_save( path ) ) msg_box( "Settings", "Could not save DESKMIND.CFG.", "OK" );
  else app_status( "Settings saved." );
  app_ping( );
  app_menu_status( );
}

void app_about( void ) {
  char t[640];                             // msg_box shows at most 10 lines of 64
  union REGS r;
  r.h.ah = 0x48; r.w.bx = 0xFFFF; intdos( &r, &r );
  char ip[20] = "-";
  if ( app_net ) net_my_ip( ip );
  sprintf( t, "DeskMind 0.8.4 for the Tandy 1000 TL/3 and CGA PCs%s. Chat with Qwen and draw with Krea 2 through "
              "MindServer at %s:%u. This %s: %s. Free memory %uK, EMS %dK, slideshow %s. "
              "Built with Open Watcom and mTCP (GPLv3). "
              "Limits: the Gallery shows the newest 500 pictures, Chats lists the newest 100. "
              "A chat holds about 40,000 characters (300 messages); then Continue hides the "
              "older part, which stays saved.",
           cfg_cga ? " (CGA mode now)" : "", cfg.server, cfg.port, app_pc( ), ip, r.w.bx / 64, app_ems * 16, slide_mem_note( ) );
  msg_box( "About DeskMind", t, "OK" );
}

// ---------------------------------------------------------------- picture picker

struct PickEntry { char id[9]; char title[40]; unsigned long when; };
static PickEntry *s_pick = 0;
static int s_npick = 0, s_npicTotal = 0;       // shown / on disk
#define MAX_PICK 200

static int cmp_pick( const void *a, const void *b ) {
  unsigned long x = ( (const PickEntry *)a )->when, y = ( (const PickEntry *)b )->when;
  return x < y ? 1 : x > y ? -1 : 0;
}

static const char far *pick_item( void *, int i ) { return s_pick[i].title; }

static void scan_pictures( void ) {
  if ( !s_pick ) s_pick = (PickEntry *)malloc( sizeof( PickEntry ) * MAX_PICK );
  s_npick = s_npicTotal = 0;
  if ( !s_pick ) return;
  struct find_t ff;
  char pattern[80];
  sprintf( pattern, "%s\\*.TPI", cfg_pics( ) );
  unsigned rc = _dos_findfirst( pattern, _A_NORMAL, &ff );
  while ( rc == 0 ) {
    char path[80];
    sprintf( path, "%s\\%s", cfg_pics( ), ff.name );
    TpiHeader h;
    if ( tpi_header( path, &h ) == 0 ) {
      unsigned long when = ( (unsigned long)h.year << 20 ) | ( (unsigned long)h.month << 16 ) |
                           ( (unsigned long)h.day << 11 ) | ( h.hour << 6 ) | h.minute;
      s_npicTotal++;
      // DOS lists files in folder order: past MAX_PICK, a newer picture replaces the oldest one
      int slot = s_npick;
      if ( s_npick == MAX_PICK ) {
        slot = 0;
        for ( int i = 1; i < s_npick; i++ ) if ( s_pick[i].when < s_pick[slot].when ) slot = i;
        if ( s_pick[slot].when >= when ) slot = -1;
      }
      if ( slot >= 0 ) {
        s_pick[slot].when = when;
        tpi_id( &h, s_pick[slot].id );
        str_copy( s_pick[slot].title, h.title[0] ? h.title : ff.name, sizeof( s_pick[0].title ) );
        if ( slot == s_npick ) s_npick++;
      }
    }
    rc = _dos_findnext( &ff );
  }
  qsort( s_pick, s_npick, sizeof( PickEntry ), cmp_pick );
}

int app_pick_picture( const char *title, char *id9 ) {
  scan_pictures( );
  if ( s_npick == 0 ) { msg_box( title, "There are no pictures yet. Create one, or sync from MindServer.", "OK" ); return 0; }
  static Widget ws[3];
  memset( ws, 0, sizeof( ws ) );
  int w = 460, h = 150;
  int x = ( ( 640 - w ) / 2 ) & ~1, y = 24;
  ws[0].type = W_LIST; ws[0].id = 1; ws[0].x = 4; ws[0].y = 4; ws[0].w = 260; ws[0].h = 4 + 10 * ROW_H;
  list_set( &ws[0], s_npick, pick_item, 0 );
  ws[1].type = W_BUTTON; ws[1].id = 1; ws[1].x = 4; ws[1].y = 112; ws[1].w = 90; ws[1].h = 14; ws[1].text = "Choose"; ws[1].flags = WF_DEFAULT;
  ws[2].type = W_BUTTON; ws[2].id = 2; ws[2].x = 100; ws[2].y = 112; ws[2].w = 90; ws[2].h = 14; ws[2].text = "Cancel"; ws[2].flags = WF_CANCEL;
  Form f;
  unsigned char far *save = ui_save( x, y, w + 6, h + 3 );
  static char ft[80];
  if ( s_npicTotal > s_npick ) sprintf( ft, "%.50s (newest %d)", title, s_npick );
  else str_copy( ft, title, sizeof( ft ) );
  form_init( &f, ft, x, y, w, h, ws, 3 );
  form_draw( &f );
  unsigned char far *thumb = (unsigned char far *)_fmalloc( TPI_THUMB_MAX );
  int last = -1, r = 0;
  Event e;
  for ( ;; ) {
    if ( ws[0].sel != last && thumb ) {
      last = ws[0].sel;
      char p[80];
      TpiHeader th;
      app_pic_path( p, s_pick[ last ].id );
      int px = form_client_x( &f ) + 276, py = form_client_y( &f ) + 6;
      gui_mouse_hide( );
      ui_sunken( px - 4, py - 2, 168, 54, BLACK );
      if ( tpi_thumb( p, thumb, &th ) == 0 ) tpi_draw_thumb( px, py, thumb, th.thumb_w, th.thumb_h );
      gui_mouse_show( );
    }
    gui_poll( &e );
    if ( e.type == EV_NONE ) continue;
    r = form_event( &f, &e );
    if ( r ) break;
  }
  if ( thumb ) _ffree( thumb );
  ui_restore( x, y, w + 6, h + 3, save );
  if ( r != 1 || ws[0].sel < 0 ) return 0;
  str_copy( id9, s_pick[ ws[0].sel ].id, 9 );
  return 1;
}
