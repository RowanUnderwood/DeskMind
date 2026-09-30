// DeskMind - generative AI for the Tandy 1000 TL/3.
//
//   DESKMIND [/NONET] [/NOMOUSE]
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

enum { M_ABOUT = 300, M_SETTINGS, M_EXIT,
       M_CHAT, M_CHAT_NEW, M_CHAT_OPEN, M_CHAT_ATTACH,
       M_CREATE, M_RULES, M_GALLERY, M_SYNC, M_SLIDES };

static const MenuItem dmItems[]   = { { "About DeskMind...", M_ABOUT }, { "Settings...    F5", M_SETTINGS },
                                      { "-", 0 }, { "Exit", M_EXIT } };
static const MenuItem chatItems[] = { { "Chat           F2", M_CHAT }, { "New chat", M_CHAT_NEW },
                                      { "Open chat...", M_CHAT_OPEN }, { "Attach picture...", M_CHAT_ATTACH } };
static const MenuItem picItems[]  = { { "Create         F3", M_CREATE }, { "Enhance rules...", M_RULES },
                                      { "-", 0 }, { "Gallery        F4", M_GALLERY }, { "Sync from server", M_SYNC },
                                      { "Slideshow      F6", M_SLIDES } };
static const Menu s_menus[] = { { "DeskMind", dmItems, 4 }, { "Chat", chatItems, 4 }, { "Pictures", picItems, 6 } };

const Menu *main_menus( int *n ) { *n = 3; return s_menus; }

static int s_screen = -1;

static void draw_screen( void ) {
  switch ( s_screen ) {
    case SCR_CHAT:    chat_draw( ); break;
    case SCR_CREATE:  create_draw( ); break;
    case SCR_GALLERY: gallery_draw( ); break;
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

static void idle( void ) {
  if ( app_net ) net_poll( );
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
  }
  return 0;
}

int main( int argc, char *argv[] ) {
  int noNet = 0, noMouse = 0;
  for ( int i = 1; i < argc; i++ ) {
    if ( str_ieq( argv[i], "/NONET" ) ) noNet = 1;
    else if ( str_ieq( argv[i], "/NOMOUSE" ) ) noMouse = 1;
    else if ( argv[i][0] == '/' || argv[i][0] == '?' ) {
      printf( "DESKMIND [/NONET] [/NOMOUSE]\n" );
      return 0;
    }
  }

  str_copy( app_dir, argv[0], sizeof( app_dir ) );
  char *slash = strrchr( app_dir, '\\' );
  if ( slash ) slash[1] = 0; else app_dir[0] = 0;
  cfg_defaults( app_dir );
  char cfgFile[80];
  cfg_path( cfgFile, app_dir );
  if ( cfg_load( cfgFile ) ) cfg_save( cfgFile );
  mkdir( cfg.pics );
  mkdir( cfg.chats );
  str_copy( net_token, cfg.token, sizeof( net_token ) );
  snd_enabled = cfg.sound;

  printf( "DeskMind 0.7\n" );
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
  if ( !chat_ready( ) ) { printf( "Not enough memory.\n" ); if ( app_net ) net_done( ); return 1; }

  int rc = vid_reserve( VM_640 );
  if ( rc ) {
    printf( "Cannot use 640x200 graphics: %s\n", vid_reserve_error( rc ) );
    if ( app_net ) net_done( );
    return 1;
  }
  snd_init( );
  vid_open( VM_640 );
  if ( !noMouse ) gui_init( );
  gui_idle = idle;
  net_wait_hook = gui_pump;               // the pointer keeps moving during network waits
  app_redraw_fn = redraw_all;

  s_screen = SCR_CHAT;
  redraw_all( );
  snd_play( SND_STARTUP );
  if ( !app_net ) app_status( "Offline: the Gallery works; Chat and Create need the network (boot with W)." );
  else if ( !app_server.ok ) app_status( "MindServer is not answering at %s:%u. Start it on the PC, or check Settings (F5).", cfg.server, cfg.port );

  s_lastPing = ticks( );
  Event e;
  for ( int quit = 0; !quit; ) {
    gui_poll( &e );
    if ( e.type == EV_NONE ) {
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
    }
    if ( cmd == CMD_GOTO_CHAT ) {
      go( SCR_CHAT );
      if ( gallery_ask_id[0] ) {
        chat_attach( gallery_ask_id );
        gallery_ask_id[0] = 0;
        chat_draw( );
      }
    }
    else if ( cmd == CMD_GOTO_CREATE ) go( SCR_CREATE );
    else if ( cmd == CMD_GOTO_GALLERY ) go( SCR_GALLERY );
  }

  gui_done( );
  snd_done( );
  vid_close( );
  vid_unreserve( );
  if ( app_net ) net_done( );
  cfg_save( cfgFile );
  printf( "Thanks for using DeskMind.\n" );
  return 0;
}
