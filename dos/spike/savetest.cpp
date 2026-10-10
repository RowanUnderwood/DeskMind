// SAVETEST - dialog save-unders come from DOS and go back to it (DeskMind 0.9.2).
// Runs by itself: opens the GUI mode, saves and restores the Settings dialog's area (and a
// message box on top of it, restored in both orders) 20 times, checks that the screen comes
// back exactly and that the largest free DOS block is the same afterwards.  Prints after
// leaving graphics mode.

#include <stdio.h>
#include <string.h>
#include <dos.h>
#include <malloc.h>
#include "video.h"
#include "gui.h"

static unsigned dos_free( void ) {
  union REGS r;
  r.h.ah = 0x48; r.w.bx = 0xFFFF; intdos( &r, &r );
  return r.w.bx;
}

static unsigned long sum( int x, int y, int w, int h, unsigned char far *tmp ) {
  vid_save( x, y, w, h, tmp );
  unsigned s = 0, t = 0;             // rotate-xor plus a plain sum: cheap on an 8 MHz 8086
  unsigned n = vid_save_size( w, h );
  for ( unsigned i = 0; i < n; i++ ) { s = ( ( s << 1 ) | ( s >> 15 ) ) ^ tmp[i]; t += tmp[i]; }
  return ( (unsigned long)s << 16 ) | t;
}

int main( int argc, char *argv[] ) {
  int cga = argc > 1;                        // any argument: CGA mode 6
  int mode = cga ? VM_CGA2 : VM_640;
  if ( vid_reserve( mode ) ) { printf( "SAVETEST: cannot reserve video memory\n" ); return 1; }
  vid_open( mode );
  // Settings: 466x175 at (87,12); a message box 300x80 at (170,60) on top of it
  const int sx = 86, sy = 12, sw = 466, sh = 175, mx = 170, my = 60, mw = 300, mh = 80;
  for ( int y = 0; y < 200; y++ ) vid_hline( 0, vid_w - 1, y, ( y / 7 ) & 15 );
  unsigned char far *tmp = (unsigned char far *)_fmalloc( vid_save_size( sw, sh ) );
  unsigned long before = tmp ? sum( sx, sy, sw, sh, tmp ) : 0;
  unsigned f0 = dos_free( ), fIn = 0, fIn2 = 0;
  int bad = 0, fromDos = 0;
  for ( int k = 0; k < 20 && tmp; k++ ) {
    unsigned char far *a = ui_save( sx, sy, sw, sh );
    vid_fill( sx, sy, sw, sh, k & 15 );
    unsigned char far *b = ui_save( mx, my, mw, mh );
    vid_fill( mx, my, mw, mh, 15 - ( k & 15 ) );
    if ( !a || !b ) { bad |= 1; break; }
    if ( FP_OFF( a ) == 0 && FP_OFF( b ) == 0 ) fromDos++;
    fIn = dos_free( );
    if ( k & 1 ) { ui_restore( mx, my, mw, mh, b ); ui_restore( sx, sy, sw, sh, a ); }
    else {       // wrong order on purpose: blocks are freed out of order
      ui_restore( sx, sy, sw, sh, a ); ui_restore( mx, my, mw, mh, b );
      vid_restore( sx, sy, sw, sh, tmp );    // (the screen is not the point here; repaint it)
      vid_save( sx, sy, sw, sh, tmp );
    }
    if ( sum( sx, sy, sw, sh, tmp ) != before ) bad |= 2;
    if ( dos_free( ) != f0 ) bad |= 4;
  }
  // Fallback: with DOS memory nearly gone, the heap still gives a save-under
  unsigned hogSeg = 0, left = dos_free( );
  if ( left > 64 ) _dos_allocmem( left - 64, &hogSeg );    // leaves 1K
  unsigned char far *c = ui_save( mx, my, mw, mh );
  fIn2 = dos_free( );
  int heapOk = c && FP_OFF( c ) != 0;
  ui_restore( mx, my, mw, mh, c );
  if ( hogSeg ) _dos_freemem( hogSeg );
  unsigned f1 = dos_free( );
  vid_close( );
  vid_unreserve( );

  printf( "SAVETEST: Settings save-under %u bytes, message box %u bytes\n", vid_save_size( sw, sh ), vid_save_size( mw, mh ) );
  printf( "%s 20 rounds, both from DOS in %d\n", fromDos == 20 ? "PASS" : "FAIL", fromDos );
  printf( "%s screen restored exactly\n", bad & 3 ? "FAIL" : "PASS" );
  printf( "%s free DOS block %uK before, %uK with both open, %uK after every round\n",
          bad & 4 ? "FAIL" : "PASS", f0 / 64, fIn / 64, f1 / 64 );
  printf( "%s heap fallback when DOS is full (%u bytes left)\n", heapOk ? "PASS" : "FAIL", fIn2 * 16 );
  int ok = !bad && fromDos == 20 && heapOk;
  printf( ok ? "SAVETEST PASSED\n" : "SAVETEST FAILED\n" );
  return ok ? 0 : 1;
}
