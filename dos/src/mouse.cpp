// DeskMind - mouse and software cursor.  See mouse.h.

#include <dos.h>
#include <i86.h>

#include "video.h"
#include "mouse.h"

static int s_present = 0;

int mouse_init( void ) {
  union REGS r;
  // Is anything hooked on INT 33h at all?
  unsigned long far *ivt = (unsigned long far *)MK_FP( 0, 0 );
  if ( ivt[ 0x33 ] == 0 ) return 0;

  // CuteMouse picks its coordinate step from the BIOS video mode: in a text mode it
  // rounds to 8-pixel character cells (the pointer jumped in big steps).  Our 640x200
  // mode is not a BIOS mode, so during the reset we let it see mode 6 (CGA 640x200),
  // which gives pixel-exact positions.  Its own cursor stays hidden; we draw ours.
  unsigned char far *biosMode = (unsigned char far *)MK_FP( 0x40, 0x49 );
  unsigned char oldMode = *biosMode;
  *biosMode = 6;
  r.w.ax = 0x0000;
  int86( 0x33, &r, &r );
  *biosMode = oldMode;
  if ( r.w.ax != 0xFFFF ) return 0;
  int buttons = ( r.w.bx == 0xFFFF ) ? 2 : r.w.bx;

  // Virtual screen = 640x200 in every mode we use
  r.w.ax = 0x0007; r.w.cx = 0; r.w.dx = 639; int86( 0x33, &r, &r );
  r.w.ax = 0x0008; r.w.cx = 0; r.w.dx = 199; int86( 0x33, &r, &r );
  // Pixels are ~2.4x taller than wide: halve the vertical speed
  r.w.ax = 0x000F; r.w.cx = 8; r.w.dx = 16; int86( 0x33, &r, &r );

  s_present = 1;
  mouse_warp( 320, 100 );
  return buttons;
}

void mouse_read( int *x, int *y, int *buttons ) {
  if ( !s_present ) { *x = 0; *y = 0; *buttons = 0; return; }
  union REGS r;
  r.w.ax = 0x0003;
  int86( 0x33, &r, &r );
  *x = r.w.cx; *y = r.w.dx; *buttons = r.w.bx & 3;
  if ( vid_mode == VM_320 ) *x >>= 1;
}

void mouse_warp( int x, int y ) {
  if ( !s_present ) return;
  union REGS r;
  r.w.ax = 0x0004; r.w.cx = ( vid_mode == VM_320 ) ? x * 2 : x; r.w.dx = y;
  int86( 0x33, &r, &r );
}


// ---------------------------------------------------------------- cursor
//
// 12 x 9 arrow; wider than tall to look right on 640x200.
//   '#' = black outline, 'o' = white, '.' = transparent

#define CUR_W 12
#define CUR_H 9

static const char *s_arrow[ CUR_H ] = {
  "##..........",
  "#oo##.......",
  "#oooo##.....",
  "#oooooo##...",
  "#oooooooo##.",
  "#oooo######.",
  "#oo#oo#.....",
  "##...#oo#...",
  "......###..."
};

static unsigned char s_under[ CUR_H * ( CUR_W / 2 + 1 ) ];
static int s_cx = 0, s_cy = 0, s_shown = 0;

static void draw_arrow( int x, int y ) {
  for ( int r = 0; r < CUR_H; r++ ) {
    const char *row = s_arrow[r];
    for ( int c = 0; c < CUR_W; c++ ) {
      if ( row[c] == '#' ) vid_pset( x + c, y + r, 0 );
      else if ( row[c] == 'o' ) vid_pset( x + c, y + r, 15 );
    }
  }
}

void cursor_show( int x, int y ) {
  if ( s_shown ) cursor_hide( );
  s_cx = x; s_cy = y;
  vid_save( x, y, CUR_W, CUR_H, s_under );
  draw_arrow( x, y );
  s_shown = 1;
}

void cursor_hide( void ) {
  if ( !s_shown ) return;
  vid_restore( s_cx, s_cy, CUR_W, CUR_H, s_under );
  s_shown = 0;
}

void cursor_move( int x, int y ) {
  if ( !s_shown || ( x == s_cx && y == s_cy ) ) return;
  cursor_show( x, y );
}

int cursor_visible( void ) { return s_shown; }
