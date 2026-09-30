// DeskMind - mouse via INT 33h (CuteMouse).  The driver's own cursor is
// never shown: it cannot draw in Tandy 16-colour modes, so we draw ours.

#ifndef MOUSE_H
#define MOUSE_H

#define MB_LEFT  1
#define MB_RIGHT 2

int  mouse_init( void );                 // number of buttons, 0 = no driver
void mouse_read( int *x, int *y, int *buttons );
void mouse_warp( int x, int y );

// Software cursor (save-under).  Hide it before drawing under it.
void cursor_show( int x, int y );
void cursor_hide( void );
void cursor_move( int x, int y );        // no-op if hidden
int  cursor_visible( void );

#endif
