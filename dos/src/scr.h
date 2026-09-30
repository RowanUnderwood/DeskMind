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

#endif
