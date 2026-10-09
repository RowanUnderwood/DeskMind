// DeskMind - screens.

#ifndef DM_SCR_H
#define DM_SCR_H

#include "gui.h"

// Chat
void chat_init( void );
int  chat_ready( void );
void chat_draw( void );
int  chat_event( Event *e );
void chat_attach( const char *id );
void chat_new( void );
void chat_open( void );
void chat_pick_attach( void );
void chat_prefill( const char *text );   // start the input line with text (cursor at its end)

// Create
void create_init( void );
void create_draw( void );
int  create_event( Event *e );
void create_edit_rules( void );

// Gallery
void gallery_init( void );
void gallery_draw( void );
int  gallery_event( Event *e );
void gallery_sync( void );
void gallery_rescan( void );
void gallery_slideshow( void );
extern char gallery_ask_id[9];     // set when the user picks "Ask Qwen" (main switches to Chat)

// Music
void music_init( void );
void music_draw( void );
int  music_event( Event *e );
void music_tick( int changed );    // main loop, when idle: clock and list follow the song
void music_rescan( void );
void music_sync( void );
int  music_download( const char *id, const char *title, unsigned long when );   // 0 = ok
int  music_have( const char *id );
extern char music_compose_ask;     // Compose was pressed (main switches to Chat)

#endif
