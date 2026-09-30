// V640 - DeskMind Phase 0 feasibility test for the Tandy 1000 TL/3.
//
//   V640          640x200x16 (Tandy Video II, register-programmed)
//   V640 /320     320x200x16 (BIOS mode 9) fallback
//   V640 file.raw show a raw image (line order, 2 pixels/byte) on screen 2
//
// Screens: 1 colours + layout  2 image  3 speed  4 mouse + sound
//          5 transitions        Esc quits.  Results go to V640.LOG.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <malloc.h>
#include <dos.h>
#include <i86.h>
#include <conio.h>

#include "video.h"
#include "mouse.h"
#include "sound.h"

static FILE *g_log = 0;
static char  g_rawName[80] = "TEST640.RAW";
static unsigned char far *g_img = 0;     // image buffer (vid_pitch * 200)
static unsigned char far *g_pat = 0;     // generated pattern buffer
static int  g_haveImg = 0;
static int  g_mouseButtons = 0;
static int  g_noSound = 0, g_noMouse = 0, g_noWait = 0;   // /NOSND /NOMOUSE /GO (diagnostics)

static void logf( const char *fmt, ... ) {
  va_list ap;
  // Never print while a graphics mode is on: the BIOS still thinks it is in
  // text mode and writes the characters into our video memory (garbage rows).
  if ( vid_mode == VM_NONE ) {
    va_start( ap, fmt );
    vprintf( fmt, ap );
    va_end( ap );
  }
  if ( g_log ) {
    va_start( ap, fmt );
    vfprintf( g_log, fmt, ap );
    va_end( ap );
    fflush( g_log );
    _dos_commit( fileno( g_log ) );   // keep the log if the machine is switched off
  }
}

static unsigned long ticks( void ) {
  return *(volatile unsigned long far *)MK_FP( 0x40, 0x6C );
}

static int getkey( void ) {                // 0 if none waiting
  if ( !kbhit( ) ) return 0;
  int c = getch( );
  if ( c == 0 ) c = 0x100 | getch( );
  return c;
}

static unsigned largest_free_kb( void ) {
  union REGS r;
  r.h.ah = 0x48; r.w.bx = 0xFFFF;
  intdos( &r, &r );
  return r.w.bx / 64;
}

static unsigned psp_block_end( void ) {    // segment just past our memory block
  union REGS r;
  r.h.ah = 0x62; intdos( &r, &r );
  unsigned psp = r.w.bx;
  unsigned size = *(unsigned far *)MK_FP( psp - 1, 3 );
  return psp + size;
}


static void mcb_dump( void ) {
  union REGS r; struct SREGS s;
  segread( &s );
  r.h.ah = 0x52; intdosx( &r, &r, &s );
  unsigned seg = *(unsigned far *)MK_FP( s.es, r.w.bx - 2 );
  logf( "Memory chain (segment type owner size name):\n" );
  for ( int i = 0; i < 40; i++ ) {
    unsigned char far *m = (unsigned char far *)MK_FP( seg, 0 );
    unsigned owner = *(unsigned far *)( m + 1 ), size = *(unsigned far *)( m + 3 );
    char name[9];
    for ( int k = 0; k < 8; k++ ) { char c = m[ 8 + k ]; name[k] = ( c >= 32 && c < 127 ) ? c : ' '; }
    name[8] = 0;
    logf( "  %04X %c %04X %5u  %s\n", seg, m[0] == 'M' || m[0] == 'Z' ? m[0] : '?', owner, size, name );
    if ( m[0] != 'M' ) break;
    seg += size + 1;
  }
}


// ---------------------------------------------------------------- screen 1

static void title( const char *s ) {
  vid_fill( 0, 0, vid_w, 10, 1 );
  vid_text( 4, 1, s, 15, -1 );
  vid_text( vid_w - 8 * 28 - 4, 1, "1-5 = screens  Esc/Q = quit", 14, -1 );
}

static void screen_colours( void ) {
  char buf[40];
  vid_clear( 0 );
  title( vid_mode == VM_640 ? "DeskMind V640 - 640x200x16" : "DeskMind V640 - 320x200x16" );

  // 16 colour bars
  int bw = vid_w / 16;
  for ( int c = 0; c < 16; c++ ) {
    vid_fill( c * bw, 14, bw, 40, (unsigned char)c );
    sprintf( buf, "%d", c );
    vid_text( c * bw + 2, 56, buf, 15, 0 );
  }

  // Layout check: every 4th line a different colour.  With linear memory
  // these are thin evenly spaced lines; a wrong layout shows as bands.
  for ( int y = 68; y < 100; y++ ) {
    unsigned char c = (unsigned char)( 9 + ( y & 3 ) );
    vid_hline( 0, vid_w / 2 - 1, y, ( y & 1 ) ? 0 : c );
  }
  vid_text( 4, 102, "Lines: even rows coloured", 7, 0 );

  // 1-pixel vertical stripes (monitor sharpness)
  for ( int x = vid_w / 2; x < vid_w; x++ ) {
    vid_vline( x, 68, 99, ( x & 1 ) ? 15 : 0 );
  }

  // 50% checkerboard dithers of useful colour pairs
  static const unsigned char pairs[][2] = {
    { 0, 15 }, { 1, 14 }, { 4, 14 }, { 2, 10 }, { 6, 14 }, { 8, 7 },
    { 1, 9 }, { 5, 13 }, { 4, 6 }, { 3, 11 }, { 0, 8 }, { 12, 14 }
  };
  int n = sizeof( pairs ) / sizeof( pairs[0] );
  int pw = vid_w / n;
  for ( int i = 0; i < n; i++ ) {
    for ( int y = 112; y < 140; y++ ) {
      for ( int x = i * pw; x < ( i + 1 ) * pw - 2; x++ ) {
        vid_pset( x, y, pairs[i][ ( x + y ) & 1 ] );
      }
    }
  }
  vid_text( 4, 142, "50% dithers of colour pairs", 7, 0 );

  // ROM font, all 256 characters
  int perRow = vid_w / 8;
  for ( int ch = 0; ch < 256; ch++ ) {
    int x = ( ch % perRow ) * 8;
    int y = 154 + ( ch / perRow ) * 9;
    if ( y + 8 <= vid_h ) vid_char( x, y, (unsigned char)ch, (unsigned char)( ch < 128 ? 15 : 11 ), 0 );
  }
}


// ---------------------------------------------------------------- screen 2

static int load_raw( void ) {
  FILE *f = fopen( g_rawName, "rb" );
  if ( !f ) return 0;
  unsigned total = vid_pitch * 200u;
  unsigned got = 0;
  static char tmp[1024];
  while ( got < total ) {
    unsigned want = total - got; if ( want > sizeof( tmp ) ) want = sizeof( tmp );
    size_t n = fread( tmp, 1, want, f );
    if ( n == 0 ) break;
    _fmemcpy( g_img + got, tmp, n );
    got += n;
  }
  fclose( f );
  return got == total;
}

static void screen_image( void ) {
  if ( g_haveImg ) {
    unsigned long t0 = ticks( );
    vid_blit_full( g_img );
    unsigned long t1 = ticks( );
    (void)t0; (void)t1;
  }
  else {
    vid_clear( 0 );
    title( "Image" );
    vid_text( 8, 40, "No image file found:", 12, -1 );
    vid_text( 8, 52, g_rawName, 15, -1 );
    vid_text( 8, 72, "Put TEST640.RAW (64000 bytes) or TEST320.RAW", 7, -1 );
    vid_text( 8, 82, "(32000 bytes) next to V640.EXE.", 7, -1 );
  }
}


// ---------------------------------------------------------------- screen 3

static void make_pattern( void ) {
  // Diagonal colour bands, in the buffer layout (line order)
  for ( unsigned y = 0; y < 200; y++ ) {
    unsigned char far *p = g_pat + y * vid_pitch;
    for ( unsigned b = 0; b < vid_pitch; b++ ) {
      unsigned char c1 = (unsigned char)( ( ( b * 2 + y ) >> 4 ) & 15 );
      unsigned char c2 = (unsigned char)( ( ( b * 2 + 1 + y ) >> 4 ) & 15 );
      p[b] = ( c1 << 4 ) | c2;
    }
  }
}

static void screen_speed( void ) {
  char buf[80];
  const int N = 36;
  unsigned long t0, t1;

  t0 = ticks( );
  for ( int i = 0; i < N; i++ ) vid_blit_full( g_pat );
  t1 = ticks( );
  unsigned long blitMs = ( t1 - t0 ) * 55ul / N;

  t0 = ticks( );
  for ( int i = 0; i < N; i++ ) vid_clear( (unsigned char)( i & 15 ) );
  t1 = ticks( );
  unsigned long clearMs = ( t1 - t0 ) * 55ul / N;

  vid_clear( 0 );
  t0 = ticks( );
  int chars = 0;
  for ( int pass = 0; pass < 4; pass++ ) {
    for ( int y = 12; y < vid_h - 8; y += 8 ) {
      for ( int x = 0; x < vid_w; x += 8 ) {
        vid_char( x, y, (unsigned char)( 'A' + ( ( x / 8 + y / 8 + pass ) % 26 ) ), (unsigned char)( 1 + pass * 3 ), 0 );
        chars++;
      }
    }
  }
  t1 = ticks( );
  unsigned long charUs = ( t1 - t0 ) * 55000ul / chars;

  vid_clear( 0 );
  title( "Speed" );
  sprintf( buf, "Full-screen copy : %lu ms", blitMs );   vid_text( 8, 30, buf, 15, -1 ); logf( "%s\n", buf );
  sprintf( buf, "Full-screen clear: %lu ms", clearMs );  vid_text( 8, 42, buf, 15, -1 ); logf( "%s\n", buf );
  sprintf( buf, "8x8 character    : %lu us  (%d chars)", charUs, chars ); vid_text( 8, 54, buf, 15, -1 ); logf( "%s\n", buf );
  vid_text( 8, 80, "(timed with the 55 ms BIOS clock)", 7, -1 );
}


// ---------------------------------------------------------------- screen 4

struct Btn { int x, y, w, h; const char *label; const SndNote *snd; };
static Btn g_btns[5];

static void draw_btn( const Btn &b, int down ) {
  vid_fill( b.x, b.y, b.w, b.h, down ? 1 : 7 );
  vid_rect( b.x, b.y, b.w, b.h, 0 );
  vid_hline( b.x + 1, b.x + b.w - 2, b.y + 1, down ? 0 : 15 );
  vid_text( b.x + ( b.w - (int)strlen( b.label ) * 8 ) / 2, b.y + ( b.h - 8 ) / 2, b.label, down ? 15 : 0, -1 );
}

static void screen_mouse_init( void ) {
  vid_clear( 3 );
  title( "Mouse + Tandy sound  (click, or keys S R I E)" );
  const char *labels[5] = { "Startup", "Reply", "Image", "Error", "Sound" };
  const SndNote *snds[5] = { SND_STARTUP, SND_REPLY, SND_IMAGE, SND_ERROR, 0 };
  int bw = ( vid_mode == VM_640 ) ? 100 : 56;
  for ( int i = 0; i < 5; i++ ) {
    g_btns[i].x = 10 + i * ( bw + 12 ); g_btns[i].y = 40; g_btns[i].w = bw; g_btns[i].h = 20;
    g_btns[i].label = labels[i]; g_btns[i].snd = snds[i];
    draw_btn( g_btns[i], 0 );
  }
  vid_text( 10, 70, g_mouseButtons ? "Mouse driver found" : "NO MOUSE DRIVER (INT 33h)", g_mouseButtons ? 15 : 12, -1 );
  vid_text( 10, 90, snd_enabled ? "Sound: ON " : "Sound: OFF", 14, 3 );
}

static void mouse_status( int x, int y, int b ) {
  char buf[48];
  sprintf( buf, "x=%3d y=%3d buttons=%d   ", x, y, b );
  vid_text( 10, 80, buf, 15, 3 );
}


// ---------------------------------------------------------------- screen 5

static void wipe( const unsigned char far *src ) {
  unsigned bytesPerStep = ( vid_mode == VM_640 ) ? 8 : 4;
  for ( unsigned bx = 0; bx < vid_pitch; bx += bytesPerStep ) {
    for ( int y = 0; y < 200; y++ ) {
      _fmemcpy( vid_line_ptr( y ) + bx, src + y * vid_pitch + bx, bytesPerStep );
    }
  }
}

static void blinds( const unsigned char far *src ) {
  for ( int k = 0; k < 10; k++ ) {
    for ( int y = k; y < 200; y += 10 ) {
      _fmemcpy( vid_line_ptr( y ), src + y * vid_pitch, vid_pitch );
    }
    unsigned long t = ticks( ); while ( ticks( ) == t ) ;
  }
}

static void dissolve( const unsigned char far *src ) {
  // 8x8-pixel blocks in pseudo-random order: an 11-bit Galois LFSR visits
  // every value 1..2047 once, which covers the 80x25 (or 40x25) blocks.
  unsigned cols = vid_w / 8, blocks = cols * 25;
  unsigned lfsr = 1;
  do {
    unsigned n = lfsr - 1;
    if ( n < blocks ) {
      unsigned bx = ( n % cols ) * 4, by = ( n / cols ) * 8;
      const unsigned char far *s = src + by * vid_pitch + bx;
      for ( unsigned r = 0; r < 8; r++ ) {
        unsigned long far *d = (unsigned long far *)( vid_line_ptr( by + r ) + bx );
        *d = *(const unsigned long far *)s;
        s += vid_pitch;
      }
    }
    lfsr = ( lfsr >> 1 ) ^ ( ( lfsr & 1 ) ? 0x500 : 0 );
  } while ( lfsr != 1 );
}

static void screen_transitions( void ) {
  char buf[64];
  const unsigned char far *a = g_haveImg ? g_img : g_pat;
  const unsigned char far *b = g_pat;
  unsigned long t0, t1;

  vid_blit_full( b );
  t0 = ticks( ); wipe( a ); t1 = ticks( );
  unsigned long wipeMs = ( t1 - t0 ) * 55ul;
  delay( 600 );
  t0 = ticks( ); blinds( b ); t1 = ticks( );
  unsigned long blindMs = ( t1 - t0 ) * 55ul;
  delay( 600 );
  t0 = ticks( ); dissolve( a ); t1 = ticks( );
  unsigned long dissMs = ( t1 - t0 ) * 55ul;
  delay( 600 );

  sprintf( buf, "wipe %lu ms  blinds %lu ms  dissolve %lu ms", wipeMs, blindMs, dissMs );
  vid_fill( 0, 190, vid_w, 10, 0 );
  vid_text( 2, 191, buf, 15, 0 );
  logf( "Transitions: %s\n", buf );
}


// ---------------------------------------------------------------- main

int main( int argc, char *argv[] ) {
  int mode = VM_640;
  for ( int i = 1; i < argc; i++ ) {
    if ( !stricmp( argv[i], "/320" ) ) mode = VM_320;
    else if ( !stricmp( argv[i], "/NOSND" ) ) g_noSound = 1;
    else if ( !stricmp( argv[i], "/NOMOUSE" ) ) g_noMouse = 1;
    else if ( !stricmp( argv[i], "/GO" ) ) g_noWait = 1;
    else strncpy( g_rawName, argv[i], sizeof( g_rawName ) - 1 );
  }
  if ( mode == VM_320 && !stricmp( g_rawName, "TEST640.RAW" ) ) strcpy( g_rawName, "TEST320.RAW" );

  g_log = fopen( "V640.LOG", "a" );
  logf( "\n=== DeskMind V640 test ===\n" );

  int vt = vid_detect( );
  logf( "Video: %s\n", vt == VT_SLTL ? "Tandy 1000 SL/TL/RL (Video II)" :
                        vt == VT_1000 ? "Tandy 1000 / PCjr (no 640x200x16)" : "not a Tandy" );
  union REGS r;
  int86( 0x12, &r, &r );
  logf( "INT 12h memory: %uK\n", r.w.ax );
  logf( "Program block ends at %04X (video needs it below %04X)\n", psp_block_end( ), mode == VM_640 ? 0x9000 : 0x9800 );
  logf( "Largest free block before reserve: %uK\n", largest_free_kb( ) );

  if ( vt != VT_SLTL && mode == VM_640 ) {
    logf( "640x200x16 needs an SL/TL/RL. Try V640 /320.\n" );
    if ( vt == VT_OTHER ) { if ( g_log ) fclose( g_log ); return 1; }
    mode = VM_320;
  }

  mcb_dump( );
  int rc = vid_reserve( mode );
  logf( "Reserve video memory: %s (%s)\n", vid_reserve_error( rc ), vid_reserve_info( ) );
  if ( vid_tail_paras( ) ) logf( "DOS block kept safe during graphics: %u paragraph(s)\n", vid_tail_paras( ) );
  if ( rc ) { if ( g_log ) fclose( g_log ); return 1; }
  logf( "Largest free block after reserve: %uK\n", largest_free_kb( ) );

  unsigned bufSize = ( mode == VM_640 ) ? 64000u : 32000u;
  g_img = (unsigned char far *)_fmalloc( bufSize );
  g_pat = (unsigned char far *)_fmalloc( bufSize );
  if ( !g_img || !g_pat ) { logf( "Out of memory for buffers\n" ); vid_unreserve( ); return 1; }
  logf( "Largest free block after 2 buffers: %uK\n", largest_free_kb( ) );

  if ( !g_noWait ) {
    printf( "\nPress a key to start the graphics test (Esc = quit)... " );
    if ( getch( ) == 27 ) { vid_unreserve( ); if ( g_log ) fclose( g_log ); return 0; }
  }

  if ( !g_noMouse ) g_mouseButtons = mouse_init( );
  if ( !g_noSound ) snd_init( );
  vid_open( mode );
  vid_pitch = ( mode == VM_640 ) ? 320 : 160;
  g_haveImg = load_raw( );
  make_pattern( );
  snd_play( SND_STARTUP );

  int screen = 1, redraw = 1, lastB = 0;
  int mx = 0, my = 0, mb = 0;
  unsigned long shownAt = 0;
  for ( ;; ) {
    if ( redraw ) {
      cursor_hide( );
      switch ( screen ) {
        case 1: screen_colours( ); break;
        case 2: screen_image( ); break;
        case 3: screen_speed( ); break;
        case 4: screen_mouse_init( ); break;
        case 5: screen_transitions( ); break;
      }
      if ( screen == 4 ) { mouse_read( &mx, &my, &mb ); cursor_show( mx, my ); }
      redraw = 0;
      shownAt = ticks( );
    }

    int k = getkey( );
    if ( g_noWait && ticks( ) - shownAt > 36 ) {        // /GO: next screen every ~2 s
      if ( screen == 4 && !g_noSound ) snd_play( SND_IMAGE );
      if ( screen == 5 ) break;
      screen++; redraw = 1; continue;
    }
    if ( k == 27 || k == 'q' || k == 'Q' ) break;
    if ( k >= '1' && k <= '5' ) { screen = k - '0'; redraw = 1; continue; }

    if ( screen == 4 ) {
      mouse_read( &mx, &my, &mb );
      cursor_move( mx, my );
      mouse_status( mx, my, mb );
      int hit = -1;
      if ( ( mb & MB_LEFT ) && !( lastB & MB_LEFT ) ) {
        for ( int i = 0; i < 5; i++ ) {
          Btn &b = g_btns[i];
          if ( mx >= b.x && mx < b.x + b.w && my >= b.y && my < b.y + b.h ) hit = i;
        }
      }
      if ( k == 's' || k == 'S' ) hit = 0;
      if ( k == 'r' || k == 'R' ) hit = 1;
      if ( k == 'i' || k == 'I' ) hit = 2;
      if ( k == 'e' || k == 'E' ) hit = 3;
      if ( hit >= 0 ) {
        cursor_hide( );
        draw_btn( g_btns[hit], 1 );
        if ( hit == 4 ) {
          snd_enabled = !snd_enabled;
          vid_text( 10, 90, snd_enabled ? "Sound: ON " : "Sound: OFF", 14, 3 );
        }
        snd_play( g_btns[hit].snd ? g_btns[hit].snd : SND_CLICK );
        delay( 120 );
        draw_btn( g_btns[hit], 0 );
        cursor_show( mx, my );
        logf( "Button %s (mouse %d,%d)\n", g_btns[hit].label, mx, my );
      }
      lastB = mb;
    }
  }

  cursor_hide( );
  if ( !g_noSound ) snd_done( );
  vid_close( );
  vid_unreserve( );
  logf( "Largest free block at exit: %uK\n", largest_free_kb( ) );
  logf( "Image file %s: %s\n", g_rawName, g_haveImg ? "shown" : "not found" );
  if ( g_log ) fclose( g_log );
  printf( "Speed and transition results are in V640.LOG\n" );
  return 0;
}
