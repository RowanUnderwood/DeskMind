// DeskMind - shared application state and helpers.

#ifndef APP_H
#define APP_H

#include "gui.h"

extern char app_dir[80];           // folder of DESKMIND.EXE, with trailing backslash
extern int  app_net;               // 1 = network running
extern int  app_ems;               // free EMS pages at start

struct ServerState {
  int ok;                          // MindServer answered /ping
  int qwen;                        // 1 = up
  int comfy;                       // 1 = up
};
extern ServerState app_server;

// Screens
enum { SCR_CHAT, SCR_CREATE, SCR_GALLERY, SCR_MUSIC };
// Commands a screen can return to the main loop
enum { CMD_NONE = 0, CMD_GOTO_CHAT = 1, CMD_GOTO_CREATE, CMD_GOTO_GALLERY, CMD_QUIT, CMD_GOTO_MUSIC };

// Work area used by the screens (inside menu bar and status bar)
#define SCR_X  0
#define SCR_Y  WORK_Y
#define SCR_W  640
#define SCR_H  WORK_H

void app_status( const char *fmt, ... );
void app_spin( const char *text );          // animated status line, call repeatedly
void app_pic_path( char *out, const char *id );
int  app_have_pic( const char *id );
int  app_twin_path( char *out, const char *id );   // the other mode's copy: 1 if it exists
int  app_download_pic( const char *id );    // GET /img/<id> into PICS; 0 = ok
int  app_upload_pic( const char *id );      // POST /img/<id>/upload; 0 = ok
int  app_view_pic( const char *id );        // full screen until a key or click; 0 = ok
int  app_ping( void );                      // refresh app_server; 0 = MindServer answered
const char *app_pc( void );                 // "Tandy", or "PC" in CGA mode (for messages)
int  app_gui_mode( void );                  // CGA: back to the GUI mode after a picture; 1 = switched (redraw)
int  app_post_short( const char *path, const char *body );  // small POST, 3 s connect; HTTP status or NET_* (< 0)
void app_menu_status( void );               // server lights in the menu bar
int  app_need_net( const char *what );      // shows a message and returns 0 if offline
void app_settings( void );                  // the settings dialog
void app_about( void );
void app_redraw( void );                    // full redraw of the current screen (set by main)
extern void ( *app_redraw_fn )( void );

// Pick a picture (list with thumbnail); returns 1 and fills id9, or 0
int  app_pick_picture( const char *title, char *id9 );

#endif
