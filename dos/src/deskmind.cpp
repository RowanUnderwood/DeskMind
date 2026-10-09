// DeskMind - generative AI for the Tandy 1000 TL/3 (and CGA PCs).
//
//   DESKMIND [/NONET] [/NOMOUSE] [/CGA] [/NOEMS]   (/NOEMS: slideshow picture buffer not in EMS)
//
// /CGA forces CGA mode (640x200 black and white, CGA pictures in PICSCGA); any PC
// without Tandy video uses it anyway.
//
// Chat with Qwen, create pictures with Krea 2, browse them in a gallery.
// All the heavy work is done by MindServer on the PC (see helper\).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <direct.h>
#include <dos.h>

#include "app.h"
#include "scr.h"
#include "video.h"
#include "mouse.h"
#include "sound.h"
#include "sys.h"
#include "cfg.h"
#include "net.h"
#include "tpi.h"
#include "slide.h"
#include "music.h"
#include "jukebox.h"

enum { M_ABOUT = 300, M_SETTINGS, M_EXIT,
       M_CHAT, M_CHAT_NEW, M_CHAT_OPEN, M_CHAT_ATTACH,
       M_CREATE, M_RULES, M_GALLERY, M_SYNC, M_SLIDES,
       M_MUSIC, M_MUSIC_RANDOM, M_MUSIC_STOP, M_MUSIC_SYNC, M_MUSIC_COMPOSE };

static const MenuItem dmItems[]   = { { "About DeskMind...", M_ABOUT }, { "Settings...    F5", M_SETTINGS },
                                      { "-", 0 }, { "Exit", M_EXIT } };
static const MenuItem chatItems[] = { { "Chat           F2", M_CHAT }, { "New chat", M_CHAT_NEW },
                                      { "Open chat...", M_CHAT_OPEN }, { "Attach picture...", M_CHAT_ATTACH } };
static const MenuItem picItems[]  = { { "Create         F3", M_CREATE }, { "Enhance rules...", M_RULES },
                                      { "-", 0 }, { "Gallery        F4", M_GALLERY }, { "Sync from server", M_SYNC },
                                      { "Slideshow      F6", M_SLIDES } };
static const MenuItem musItems[]  = { { "Music          F7", M_MUSIC }, { "Play random songs", M_MUSIC_RANDOM },
                                      { "Stop music     F8", M_MUSIC_STOP }, { "-", 0 },
                                      { "Compose a song...", M_MUSIC_COMPOSE }, { "Sync from server", M_MUSIC_SYNC } };
static const Menu s_menus[] = { { "DeskMind", dmItems, 4 }, { "Chat", chatItems, 4 }, { "Pictures", picItems, 6 },
                                { "Music", musItems, 6 } };

const Menu *main_menus( int *n ) { *n = 4; return s_menus; }

static int s_screen = -1;

static void draw_screen( void ) {
  switch ( s_screen ) {
    case SCR_CHAT:    chat_draw( ); break;
    case SCR_CREATE:  create_draw( ); break;
    case SCR_GALLERY: gallery_draw( ); break;
    case SCR_MUSIC:   music_draw( ); break;
  }
}

static void redraw_all( void ) {
  gui_mouse_hide( );
  vid_fill( 0, 0, 640, 200, BLUE );
  app_menu_status( );
  draw_screen( );
  gui_mouse_show( );
}

static void go( int scr ) {
  if ( scr == s_screen ) return;
  s_screen = scr;
  draw_screen( );
}

static unsigned long s_lastPing = 0;
static int s_musicChanged = 0;           // the jukebox moved on: the main loop redraws (not in dialogs)

static void idle( void ) {
  if ( app_net ) net_poll( );
  if ( jb_poll( ) ) s_musicChanged = 1;    // the next random song starts even while a dialog is open
}

// Compose (Music screen or menu): Chat, with the start of a request typed in
static void compose( void ) {
  go( SCR_CHAT );
  if ( !music_available( ) ) { msg_box( "Compose", "Songs play on the Tandy sound chip, which this PC does not have.", "OK" ); return; }
  chat_prefill( "Compose a song: " );
  chat_draw( );
  app_status( "Describe the song (mood, speed, style, for which pictures...) and press Enter." );
}

static void play_random( void ) {
  int rc = jb_random( );
  if ( rc ) app_status( "Cannot play: %s.", rc == MUS_NO_FILE ? "no songs yet (Music, F7)" : music_error( rc ) );
  else app_status( "Random songs: %s", jb_now( ) );
  app_menu_status( );
}

// ---------------------------------------------------------------- F9: sound check
// For the stuck-note hunt.  Each press within 10 s goes one step further, and every press
// appends the sound state and the recent sound events to SOUND.LOG next to DESKMIND.EXE:
//   1 nothing changed (a snapshot)   2 sound chip silenced   3 PC speaker gate off
//   4 speaker timer + sound switch reset
// The user notes which press stopped the note.  No step plays a sound.
static int s_f9Step = 0;
static unsigned long s_f9Last = 0;

static void net_event( int open ) { snd_log( SL_NET, open ); }

static void sound_check( void ) {
  if ( ticks( ) - s_f9Last > 182 ) s_f9Step = 0;
  s_f9Last = ticks( );
  if ( ++s_f9Step > 4 ) s_f9Step = 1;
  if ( s_f9Step == 1 ) snd_log( SL_USER, 1 ); else snd_force( s_f9Step );
  char path[80];
  sprintf( path, "%sSOUND.LOG", app_dir );
  FILE *f = fopen( path, "a" );
  if ( f ) {
    struct dosdate_t d;
    struct dostime_t t;
    _dos_getdate( &d );
    _dos_gettime( &t );
    fprintf( f, "\n=== F9 step %d   %04u-%02u-%02u %02u:%02u:%02u   DeskMind 0.9.1 ===\n",
             s_f9Step, d.year, d.month, d.day, t.hour, t.minute, t.second );
    snd_diag( f );
    fclose( f );
  }
  static const char *const msg[] = { "",                     // the status line shows 79 characters
    "F9 check 1: saved to SOUND.LOG.  Still hearing the note?  Press F9 again.",
    "F9 check 2: sound chip told to be quiet.  Still hearing it?  Press F9 again.",
    "F9 check 3: PC speaker switched off.  Still hearing it?  Press F9 again.",
    "F9 check 4: speaker timer and sound switch reset.  Which step stopped it?" };
  if ( f ) app_status( "%s", msg[ s_f9Step ] );
  else app_status( "F9 check %d done, but SOUND.LOG could not be written.", s_f9Step );
}

static int run_command( int id ) {
  switch ( id ) {
    case M_ABOUT:       app_about( ); break;
    case M_SETTINGS:    app_settings( ); break;
    case M_EXIT:        return msg_box( "Exit", "Leave DeskMind?", "Yes|No" ) == 1;
    case M_CHAT:        go( SCR_CHAT ); break;
    case M_CHAT_NEW:    go( SCR_CHAT ); chat_new( ); break;
    case M_CHAT_OPEN:   go( SCR_CHAT ); chat_open( ); break;
    case M_CHAT_ATTACH: go( SCR_CHAT ); chat_pick_attach( ); break;
    case M_CREATE:      go( SCR_CREATE ); break;
    case M_RULES:       go( SCR_CREATE ); create_edit_rules( ); create_draw( ); break;
    case M_GALLERY:     go( SCR_GALLERY ); break;
    case M_SYNC:        go( SCR_GALLERY ); gallery_sync( ); break;
    case M_SLIDES:      go( SCR_GALLERY ); gallery_slideshow( ); break;
    case M_MUSIC:       go( SCR_MUSIC ); break;
    case M_MUSIC_RANDOM: play_random( ); break;
    case M_MUSIC_STOP:  jb_stop( ); app_menu_status( ); if ( s_screen == SCR_MUSIC ) music_draw( ); else app_status( "Music stopped." ); break;
    case M_MUSIC_SYNC:  go( SCR_MUSIC ); music_sync( ); break;
    case M_MUSIC_COMPOSE: compose( ); break;
  }
  return 0;
}

int main( int argc, char *argv[] ) {
  int noNet = 0, noMouse = 0, forceCga = 0;
  for ( int i = 1; i < argc; i++ ) {
    if ( str_ieq( argv[i], "/NONET" ) ) noNet = 1;
    else if ( str_ieq( argv[i], "/NOMOUSE" ) ) noMouse = 1;
    else if ( str_ieq( argv[i], "/CGA" ) ) forceCga = 1;
    else if ( str_ieq( argv[i], "/NOEMS" ) ) slide_no_ems = 1;
    else if ( argv[i][0] == '/' || argv[i][0] == '?' ) {
      printf( "DESKMIND [/NONET] [/NOMOUSE] [/CGA] [/NOEMS]\n" );
      return 0;
    }
  }
  // 640x200x16 needs Tandy Video II; everything else gets CGA
  cfg_cga = forceCga || vid_detect( ) != VT_SLTL;
  tpi_cga = cfg_cga;

  str_copy( app_dir, argv[0], sizeof( app_dir ) );
  char *slash = strrchr( app_dir, '\\' );
  if ( slash ) slash[1] = 0; else app_dir[0] = 0;
  cfg_defaults( app_dir );
  char cfgFile[80];
  cfg_path( cfgFile, app_dir );
  if ( cfg_load( cfgFile ) ) cfg_save( cfgFile );
  mkdir( cfg_pics( ) );
  mkdir( cfg.chats );
  str_copy( net_token, cfg.token, sizeof( net_token ) );
  snd_enabled = cfg.sound;

  printf( cfg_cga ? "DeskMind 0.9.1 (CGA)\n" : "DeskMind 0.9.1\n" );
  if ( !noNet ) {
    printf( "Starting the network...\n" );
    app_net = ( net_init( ) == 0 );
    if ( !app_net ) printf( "No network: %s\nDeskMind starts offline (gallery only).\n", net_error( ) );
    else {
      printf( "Looking for MindServer at %s:%u...\n", cfg.server, cfg.port );
      if ( app_ping( ) ) printf( "MindServer is not answering (start it on the PC). Continuing.\n" );
    }
  }
  app_ems = ems_init( );

  chat_init( );
  create_init( );
  gallery_init( );
  music_init( );
  if ( !chat_ready( ) ) { printf( "Not enough memory.\n" ); if ( app_net ) net_done( ); return 1; }

  int gmode = cfg_cga ? VM_CGA2 : VM_640;
  int rc = vid_reserve( gmode );             // nothing to reserve for CGA
  if ( rc ) {
    printf( "Cannot use 640x200 graphics: %s\n", vid_reserve_error( rc ) );
    if ( app_net ) net_done( );
    return 1;
  }
  snd_init( );
  music_owner_hook = snd_music;           // songs take the chip: sound effects step aside
  vid_open( gmode );
  if ( !noMouse ) gui_init( );
  gui_idle = idle;
  gui_f9 = sound_check;
  net_event_hook = net_event;
  net_wait_hook = gui_pump;               // the pointer keeps moving during network waits
  app_redraw_fn = redraw_all;

  s_screen = SCR_CHAT;
  redraw_all( );
  // Startup sound: a 56k modem handshake when MindServer answered, else the old chime.
  // The modem plays in the background (DeskMind is usable at once); any key or click stops it.
  {
    char modem[80];
    sprintf( modem, "%sMODEM.T3", app_dir );
    if ( !( cfg.sound && app_net && app_server.ok && jb_jingle( modem ) == MUS_OK ) ) snd_play( SND_STARTUP );
  }
  if ( !app_net ) app_status( cfg_cga ? "Offline: the Gallery works; Chat and Create need the network (packet driver)."
                                      : "Offline: the Gallery works; Chat and Create need the network (boot with W)." );
  else if ( !app_server.ok ) app_status( "MindServer is not answering at %s:%u. Start it on the PC, or check Settings (F5).", cfg.server, cfg.port );

  s_lastPing = ticks( );
  Event e;
  for ( int quit = 0; !quit; ) {
    gui_poll( &e );
    if ( jb_mode( ) == JB_JINGLE && ( e.type == EV_KEY || e.type == EV_DOWN ) ) jb_stop( );   // the modem sound
    if ( e.type == EV_NONE ) {
      if ( s_musicChanged ) {
        s_musicChanged = 0;
        app_menu_status( );
        if ( s_screen == SCR_MUSIC ) music_tick( 1 );
      }
      else if ( s_screen == SCR_MUSIC ) music_tick( 0 );
      if ( app_net && ticks( ) - s_lastPing > 18ul * 120 ) {      // every two minutes, when idle
        int was = app_server.ok * 4 + app_server.qwen * 2 + app_server.comfy;
        app_ping( );
        if ( was != app_server.ok * 4 + app_server.qwen * 2 + app_server.comfy ) app_menu_status( );
        s_lastPing = ticks( );
      }
      continue;
    }
    int id = 0;
    if ( e.type == EV_KEY ) {
      switch ( e.key ) {
        case K_F2: go( SCR_CHAT ); continue;
        case K_F3: go( SCR_CREATE ); continue;
        case K_F4: go( SCR_GALLERY ); continue;
        case K_F5: app_settings( ); continue;
        case K_F6: run_command( M_SLIDES ); continue;
        case K_F7: go( SCR_MUSIC ); continue;
        case K_F8: run_command( M_MUSIC_STOP ); continue;
        case K_F10: id = menubar_run( 0, 0 ); break;
        default:
          if ( menubar_key( e.key ) >= 0 ) id = menubar_run( menubar_key( e.key ), 0 );
      }
    }
    else if ( e.type == EV_DOWN && menubar_hit( e.x, e.y ) >= 0 ) {
      id = menubar_run( -1, &e );
    }
    if ( id ) { quit = run_command( id ); continue; }
    if ( e.type == EV_NONE ) continue;
    if ( e.type == EV_KEY && e.key == K_ESC ) {
      if ( msg_box( "Exit", "Leave DeskMind?", "Yes|No" ) == 1 ) quit = 1;
      continue;
    }

    int cmd = CMD_NONE;
    switch ( s_screen ) {
      case SCR_CHAT:    cmd = chat_event( &e ); break;
      case SCR_CREATE:  cmd = create_event( &e ); break;
      case SCR_GALLERY: cmd = gallery_event( &e ); break;
      case SCR_MUSIC:   cmd = music_event( &e ); break;
    }
    if ( cmd == CMD_GOTO_CHAT && music_compose_ask ) { music_compose_ask = 0; compose( ); }
    else if ( cmd == CMD_GOTO_CHAT ) {
      go( SCR_CHAT );
      if ( gallery_ask_id[0] ) {
        chat_attach( gallery_ask_id );
        gallery_ask_id[0] = 0;
        chat_draw( );
      }
    }
    else if ( cmd == CMD_GOTO_CREATE ) go( SCR_CREATE );
    else if ( cmd == CMD_GOTO_MUSIC ) go( SCR_MUSIC );
    else if ( cmd == CMD_GOTO_GALLERY ) go( SCR_GALLERY );
  }

  jb_stop( );
  gui_done( );
  snd_done( );
  vid_close( );
  vid_unreserve( );
  if ( app_net ) net_done( );
  cfg_save( cfgFile );
  printf( "Thanks for using DeskMind.\n" );
  return 0;
}
