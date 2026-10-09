// DeskMind - slideshow and transitions.  See slide.h.
//
// Every transition is a pattern of plain memory copies from the next picture
// (in a 64000-byte buffer) into video memory, sized so it takes well under a
// second on the TL/3 (a full-screen copy is ~77 ms there).  Only one picture
// buffer is used: the title strip is repaired by re-reading those lines from
// the picture file, so the next picture can be preloaded while one shows.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <dos.h>
#include <conio.h>

#include "slide.h"
#include "video.h"
#include "mouse.h"
#include "sys.h"
#include "tpi.h"

#define PITCH vid_pitch          // 320 bytes (Tandy 640x200x16) or 80 (CGA)
#define LINES 200
#define U     ( vid_pitch / 80 )   // effect step unit: 4 bytes on the Tandy, 1 on CGA

static const char *s_names[ FX_COUNT ] = {
  "Random", "Cut", "Wipe right", "Wipe down", "Blinds", "Interlace",
  "Dissolve", "Box out", "Box in", "Slide in"
};

const char *slide_effect_name( int e ) {
  return ( e >= 0 && e < FX_COUNT ) ? s_names[e] : "?";
}

static void copy_rect( const unsigned char far *img, unsigned bx0, unsigned bx1, int y0, int y1 ) {
  // bytes [bx0, bx1) on lines [y0, y1)
  if ( bx1 <= bx0 || y1 <= y0 ) return;
  for ( int y = y0; y < y1; y++ )
    _fmemcpy( vid_line_ptr( y ) + bx0, img + (unsigned)y * PITCH + bx0, bx1 - bx0 );
}

// Wait until the next BIOS tick (55 ms): keeps stepped effects at a steady pace
static void tick_wait( void ) {
  unsigned long t = ticks( );
  while ( ticks( ) == t ) ;
}

void slide_transition( int fx, const unsigned char far *img ) {
  switch ( fx ) {
    default:
    case FX_CUT:
      copy_rect( img, 0, PITCH, 0, LINES );
      break;

    case FX_WIPE_RIGHT:                        // 40 columns of 16 pixels
      for ( unsigned bx = 0; bx < PITCH; bx += 2 * U ) copy_rect( img, bx, bx + 2 * U, 0, LINES );
      break;

    case FX_WIPE_DOWN:                         // 4 lines at a time
      for ( int y = 0; y < LINES; y += 4 ) copy_rect( img, 0, PITCH, y, y + 4 );
      break;

    case FX_BLINDS:                            // 10 slats of 20 lines, one line per slat per step
      for ( int k = 0; k < 20; k++ ) {
        for ( int y = k; y < LINES; y += 20 ) copy_rect( img, 0, PITCH, y, y + 1 );
        if ( k & 1 ) tick_wait( );
      }
      break;

    case FX_INTERLACE:                         // every 4th line, then the ones between
      for ( int k = 0; k < 4; k++ ) {
        static const int order[4] = { 0, 2, 1, 3 };
        for ( int y = order[k]; y < LINES; y += 4 ) copy_rect( img, 0, PITCH, y, y + 1 );
        tick_wait( );
      }
      break;

    case FX_DISSOLVE: {                        // 2000 blocks of 8 lines (80 per line) in LFSR order
      unsigned lfsr = 1, u = U;
      do {
        unsigned n = lfsr - 1;
        if ( n < 2000 ) {
          unsigned bx = ( n % 80 ) * u, by = ( n / 80 ) * 8;
          const unsigned char far *s = img + by * PITCH + bx;
          for ( unsigned r = 0; r < 8; r++ ) {
            unsigned char far *d = vid_line_ptr( by + r ) + bx;
            if ( u == 4 ) *(unsigned long far *)d = *(const unsigned long far *)s;
            else *d = *s;
            s += PITCH;
          }
        }
        lfsr = ( lfsr >> 1 ) ^ ( ( lfsr & 1 ) ? 0x500 : 0 );
      } while ( lfsr != 1 );
      break;
    }

    case FX_BOX_OUT:                           // a growing rectangle from the centre
      for ( int s = 1; s <= 20; s++ ) {
        unsigned hw = (unsigned)s * 2 * U;     // half width in bytes (16 px per step)
        int hh = s * 5;                        // half height in lines
        copy_rect( img, PITCH / 2 - hw, PITCH / 2 + hw, 100 - hh, 100 + hh );
      }
      break;

    case FX_BOX_IN:                            // shrinking frame from the edges
      for ( int s = 0; s < 20; s++ ) {
        unsigned o = (unsigned)s * 2 * U, n = o + 2 * U;
        int y0 = s * 5, y1 = y0 + 5;
        copy_rect( img, 0, PITCH, y0, y1 );                    // top band
        copy_rect( img, 0, PITCH, LINES - y1, LINES - y0 );    // bottom band
        copy_rect( img, o, n, y1, LINES - y1 );                // left band
        copy_rect( img, PITCH - n, PITCH - o, y1, LINES - y1 );// right band
      }
      break;

    case FX_SLIDE_IN:                          // the new picture slides in over the old one
      for ( unsigned o = 4 * U; o <= PITCH; o += 4 * U ) {
        for ( int y = 0; y < LINES; y++ )
          _fmemcpy( vid_line_ptr( y ) + ( PITCH - o ), img + (unsigned)y * PITCH, o );
      }
      break;
  }
}

// ---------------------------------------------------------------- the show

static void title_strip( const char *title, int n, int total, const char *extra ) {
  char t[100];
  sprintf( t, " %s", title );
  vid_fill( 0, LINES - 11, vid_w, 11, 0 );
  vid_hline( 0, vid_w - 1, LINES - 11, 8 );
  char r[40];
  sprintf( r, "%s%d/%d ", extra, n, total );
  int rx = ( vid_w - (int)strlen( r ) * 8 ) & ~1;
  t[ ( rx - 12 ) / 8 > 0 ? ( rx - 12 ) / 8 : 0 ] = 0;     // 320 wide: keep the title clear of the counter
  vid_text( 4, LINES - 9, t, 15, -1 );
  vid_text( rx, LINES - 9, r, 7, -1 );
}

// Returns a key code, 1 for a mouse click, or 0
static int input( void ) {
  int k = kbd_get( );
  if ( k ) return k;
  int x, y, b;
  static int lastB = 0;
  mouse_read( &x, &y, &b );
  int click = ( b & MB_LEFT ) && !( lastB & MB_LEFT );
  lastB = b;
  return click ? 1 : 0;
}

static int s_pal = -1, s_bg = -1;     // CGA palette on screen

void slide_make_order( int *order, int count, int start, int shuffle ) {
  static int seeded = 0;
  if ( !seeded ) {
    // BIOS ticks plus the PIT counter: different on every run
    outp( 0x43, 0 );
    unsigned pit = inp( 0x40 ); pit |= inp( 0x40 ) << 8;
    srand( (unsigned)ticks( ) ^ pit );
    seeded = 1;
  }
  // The lists are newest first; play them oldest first, newest last
  for ( int i = 0; i < count; i++ ) order[i] = count - 1 - i;
  if ( !shuffle ) return;
  int lo = 0;
  if ( start >= 0 && start < count ) {           // the chosen picture first, the rest shuffled
    order[count - 1 - start] = order[0]; order[0] = start;
    lo = 1;
  }
  for ( int i = count - 1; i > lo; i-- ) {
    int j = lo + rand( ) % ( i - lo + 1 );
    int t = order[i]; order[i] = order[j]; order[j] = t;
  }
}

// The picture buffer (64000 bytes, CGA 16000) lives in the EMS page frame when there is EMS,
// so it costs no conventional memory.  Otherwise it is an exact DOS block, freed afterwards:
// from the C heap it cost about 100K the first time (the heap first grows its last segment to
// 64K, then adds a new one) and the heap never gave it back.
int slide_no_ems = 0;
static int s_bufEms = -1;
static unsigned s_bufSeg = 0;

static unsigned buf_size( void ) { return vid_is_cga( ) ? 16000u : 64000u; }

static unsigned char far *buf_alloc( void ) {
  unsigned size = buf_size( );
  unsigned pages = ( size + 16383u ) / 16384u;
  if ( !slide_no_ems && ( s_bufEms = ems_alloc( pages ) ) >= 0 ) {
    unsigned char far *p = ems_frame_map( s_bufEms, pages );
    if ( p ) return p;
    ems_free( s_bufEms );
    s_bufEms = -1;
  }
  if ( _dos_allocmem( ( size + 15u ) >> 4, &s_bufSeg ) == 0 ) return (unsigned char far *)MK_FP( s_bufSeg, 0 );
  s_bufSeg = 0;
  return 0;
}

static void buf_free( void ) {
  if ( s_bufEms >= 0 ) { ems_free( s_bufEms ); s_bufEms = -1; }
  if ( s_bufSeg ) { _dos_freemem( s_bufSeg ); s_bufSeg = 0; }
}

const char *slide_mem_note( void ) {
  static char t[24];
  unsigned pages = ( buf_size( ) + 16383u ) / 16384u;
  if ( !slide_no_ems && ems_init( ) >= (int)pages ) return "in EMS";
  union REGS r;
  r.h.ah = 0x48; r.w.bx = 0xFFFF; intdos( &r, &r );
  if ( r.w.bx >= ( ( buf_size( ) + 15u ) >> 4 ) ) return "fits";
  sprintf( t, "needs %uK", ( buf_size( ) + 1023u ) / 1024u );
  return t;
}

int slide_run( int count, int start, slide_path_fn pathOf, void *ctx, SlideOpts *o ) {
  if ( count <= 0 ) return 0;
  s_pal = s_bg = -1;
  int *order = (int *)malloc( sizeof( int ) * count );
  if ( !order ) return 0;
  unsigned char far *buf = buf_alloc( );
  if ( !buf ) { free( order ); return 0; }
  slide_make_order( order, count, start, o->shuffle );
  int pos = 0;
  if ( !o->shuffle ) pos = ( start >= 0 && start < count ) ? count - 1 - start : 0;

  char path[80], curPath[80] = "";
  TpiHeader h;
  int shown = 0, paused = 0, fxCycle = FX_WIPE_RIGHT;
  int loaded = -1;                  // index (into order) of the picture in buf
  int quit = 0;
  int fails = 0;

  while ( !quit ) {
    // Load the picture at pos (if it isn't preloaded already)
    if ( loaded != pos ) {
      if ( !pathOf( ctx, order[pos], path ) || tpi_image( path, buf, &h ) ) {
        if ( ++fails >= count ) break;                 // nothing showable
        pos = ( pos + 1 ) % count;
        if ( pos == 0 && !o->loop && shown ) break;
        continue;
      }
      loaded = pos;
    }
    fails = 0;
    if ( vid_is_cga( ) ) {
      // CGA: each picture has its own mode and palette.  A new mode starts blank (the BIOS
      // clears it); a new palette in the same mode would recolour the old picture, so blank first.
      if ( tpi_vmode( &h ) != vid_mode ) tpi_set_mode( &h );
      else if ( h.mode == 3 && ( h.cga_pal != s_pal || h.cga_color != s_bg ) ) {
        for ( int y = 0; y < LINES; y++ ) _fmemset( vid_line_ptr( y ), 0, PITCH );
        vid_cga_palette( h.cga_pal, h.cga_color );
      }
      s_pal = h.cga_pal; s_bg = h.cga_color;
    }
    int fx = o->effect;
    if ( fx == FX_RANDOM ) fx = FX_WIPE_RIGHT + rand( ) % ( FX_COUNT - FX_WIPE_RIGHT );
    slide_transition( fx, buf );
    strcpy( curPath, path );
    shown++;
    char title[40];
    str_copy( title, h.title[0] ? h.title : "(untitled)", sizeof( title ) );

    int titleOn = 0;
    if ( o->titles ) { title_strip( title, count - order[pos], count, "" ); titleOn = 1; }
    unsigned long t0 = ticks( );

    // Preload the next one while this one is on screen
    int next = pos + 1;
    int atEnd = ( next >= count );
    if ( atEnd ) next = 0;
    if ( !( atEnd && !o->loop ) && count > 1 ) {
      if ( pathOf( ctx, order[next], path ) && tpi_image( path, buf, &h ) == 0 ) loaded = next;
      else loaded = -1;
    }

    // Wait, handling keys
    int advance = 0;
    while ( !advance && !quit ) {
      unsigned long el = ticks( ) - t0;
      if ( titleOn && el > 55 ) {                    // ~3 s, then repair the strip from the file
        tpi_lines_to_screen( curPath, LINES - 11, 11 );
        titleOn = 0;
      }
      if ( !paused && el >= (unsigned long)o->delay * 182ul / 10 ) advance = 1;
      int k = input( );
      if ( !k ) continue;
      if ( k == K_ESC || k == 1 || k == 'q' || k == 'Q' ) { quit = 1; break; }
      if ( k == ' ' || k == K_ENTER || k == K_RIGHT || k == K_PGDN ) { advance = 1; paused = 0; }
      else if ( k == K_LEFT || k == K_PGUP ) {
        next = ( pos + count - 1 ) % count;
        atEnd = 0;
        advance = 1; paused = 0;
      }
      else if ( k == 'p' || k == 'P' ) {
        paused = !paused;
        title_strip( title, count - order[pos], count, paused ? "PAUSED  " : "" );
        titleOn = 1; t0 = ticks( );
        if ( paused ) t0 = ticks( ) - 1;
      }
      else if ( k == 't' || k == 'T' ) {
        o->titles = !o->titles;
        if ( o->titles ) { title_strip( title, count - order[pos], count, "" ); titleOn = 1; t0 = ticks( ); }
        else if ( titleOn ) { tpi_lines_to_screen( curPath, LINES - 11, 11 ); titleOn = 0; }
      }
      else if ( k == 'e' || k == 'E' ) {             // try the effects one after another
        o->effect = fxCycle;
        char ex[40];
        sprintf( ex, "Effect: %s   ", slide_effect_name( fxCycle ) );
        title_strip( title, count - order[pos], count, ex );
        titleOn = 1; t0 = ticks( );
        fxCycle = fxCycle + 1 >= FX_COUNT ? FX_CUT : fxCycle + 1;
      }
    }
    if ( quit ) break;
    if ( atEnd && !o->loop ) break;
    pos = next;
  }
  buf_free( );
  free( order );
  return shown;
}
