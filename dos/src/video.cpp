// DeskMind - Tandy Video II and CGA graphics.  See video.h.
//
// 640x200x16 register values come from TANDOTS.ASM (tanvid.zip, 1995,
// oldskool.org Tandy archive), which was written for the SL/TL/RL.

#include <stdio.h>
#include <dos.h>
#include <conio.h>
#include <string.h>
#include <i86.h>

#include "video.h"

int      vid_mode  = VM_NONE;
int      vid_w     = 0;
int      vid_h     = 0;
unsigned vid_pitch = 0;

static unsigned char s_oldMode = 3;
static void build_line_table( void );
static unsigned      s_resSeg  = 0;

static const unsigned char far *s_fontLo = 0;   // chars 0..127
static const unsigned char far *s_fontHi = 0;   // chars 128..255 (INT 1Fh)


// ---------------------------------------------------------------- detect

int vid_detect( void ) {
  union REGS r;

  // A VGA/MCGA/EGA BIOS answers these; a Tandy BIOS does not.
  r.w.ax = 0x1A00; r.w.bx = 0xEEEE;
  int86( 0x10, &r, &r );
  if ( r.h.al == 0x1A ) return VT_OTHER;
  r.h.ah = 0x12; r.h.bl = 0x10;
  int86( 0x10, &r, &r );
  if ( r.h.bl != 0x10 ) return VT_OTHER;

  unsigned char far *model = (unsigned char far *)MK_FP( 0xFFFF, 0x000E );
  if ( *model == 0xFD ) return VT_1000;            // PCjr
  if ( *model != 0xFF ) return VT_OTHER;
  unsigned char far *tandy = (unsigned char far *)MK_FP( 0xFC00, 0x0000 );
  if ( *tandy != 0x21 ) return VT_OTHER;           // not a Tandy BIOS

  // SL/TL/RL BIOSes support INT 15h AH=C0h (return system configuration);
  // the older 1000s return carry.
  struct SREGS s;
  segread( &s );
  r.h.ah = 0xC0;
  int86x( 0x15, &r, &r, &s );
  if ( r.w.cflag ) return VT_1000;
  return VT_SLTL;
}


// ---------------------------------------------------------------- memory
//
// The video RAM is the top of the 640K.  DOS is normally told about only
// the top 16K (INT 12h = 624K), so for 640x200x16 we must own 576K-624K.
// A "last fit" DOS allocation carves that region from the top free block.
//
// With DOS=UMB, MS-DOS keeps a small system block (owner 8, name "SC")
// right at the top of conventional memory; it links the low chain to the
// upper memory blocks.  It lies inside the 640x200 video RAM, so while the
// mode is on we keep a copy of it (the "tail") and unlink the UMBs so DOS
// never walks through it, and put it back before returning to text mode.

#define TAIL_MAX_PARAS 64                  // 1K; the SC header is 1 paragraph

static unsigned      s_tailSeg = 0, s_tailParas = 0;
static unsigned char s_tail[ TAIL_MAX_PARAS * 16 ];
static int           s_tailSaved = 0;
static unsigned char s_savedLink = 0;
static char          s_resInfo[80] = "";

static unsigned videoSegFor( int mode ) {
  return ( mode == VM_640 ) ? 0x9000 : 0x9800;
}

static void dos_free( unsigned seg ) {
  union REGS r; struct SREGS s;
  segread( &s );
  s.es = seg; r.h.ah = 0x49;
  intdosx( &r, &r, &s );
}

// Is everything from `seg` up to `top` DOS system blocks (owner 8)?
static int only_system_blocks( unsigned seg, unsigned top ) {
  while ( seg < top ) {
    unsigned char far *mcb = (unsigned char far *)MK_FP( seg, 0 );
    if ( mcb[0] != 'M' && mcb[0] != 'Z' ) return 0;
    unsigned owner = *(unsigned far *)( mcb + 1 );
    unsigned size  = *(unsigned far *)( mcb + 3 );
    if ( owner != 8 ) return 0;
    if ( mcb[0] == 'Z' ) break;
    seg += size + 1;
  }
  return 1;
}

int vid_reserve( int mode ) {
  union REGS r;

  if ( s_resSeg ) return 0;
  if ( mode == VM_CGA2 || mode == VM_CGA4 ) return 0;     // CGA RAM is at B800, not in the 640K
  s_tailSeg = s_tailParas = 0;

  int86( 0x12, &r, &r );
  unsigned dosTop = r.w.ax * 64u;          // KB -> paragraphs
  unsigned vseg   = videoSegFor( mode );
  if ( dosTop <= vseg ) return 0;          // already protected
  unsigned need = dosTop - vseg;

  // Save allocation strategy and UMB link state
  r.w.ax = 0x5800; intdos( &r, &r ); unsigned oldStrat = r.w.ax;
  r.w.ax = 0x5802; intdos( &r, &r ); unsigned char oldLink = r.h.al;

  r.w.ax = 0x5803; r.w.bx = 0; intdos( &r, &r );   // unlink UMBs
  r.w.ax = 0x5801; r.w.bx = 2; intdos( &r, &r );   // last fit, low memory

  r.h.ah = 0x48; r.w.bx = need;
  intdos( &r, &r );
  int failed = r.w.cflag;
  unsigned seg = r.w.ax;

  r.w.ax = 0x5801; r.w.bx = oldStrat; intdos( &r, &r );
  r.w.ax = 0x5803; r.w.bx = oldLink;  intdos( &r, &r );

  if ( failed ) return 1;
  sprintf( s_resInfo, "block %04X-%04X, DOS top %04X", seg, seg + need, dosTop );
  if ( seg > vseg ) { dos_free( seg ); return 2; }

  // Last fit ends our block where the free block ended.  Anything between
  // there and the DOS top must be DOS's own UMB link block, nothing else.
  unsigned tail = seg + need;
  if ( tail < dosTop ) {
    if ( dosTop - tail > TAIL_MAX_PARAS || !only_system_blocks( tail, dosTop ) ) {
      dos_free( seg );
      return 3;
    }
    s_tailSeg = tail;
    s_tailParas = dosTop - tail;
  }
  s_resSeg = seg;
  return 0;
}

const char *vid_reserve_error( int rc ) {
  switch ( rc ) {
    case 0:  return "ok";
    case 1:  return "the memory under the video RAM is in use (576K-624K)";
    case 2:  return "the top of conventional memory is not free";
    case 3:  return "a program is loaded just below 624K";
  }
  return "unknown error";
}

const char *vid_reserve_info( void ) { return s_resInfo; }
unsigned vid_tail_paras( void ) { return s_tailParas; }

void vid_unreserve( void ) {
  if ( !s_resSeg ) return;
  dos_free( s_resSeg );
  s_resSeg = 0;
}

// Called around the 640x200 mode: keep DOS's top block safe from pixels.
static void tail_protect( void ) {
  union REGS r;
  r.w.ax = 0x5802; intdos( &r, &r ); s_savedLink = r.h.al;
  r.w.ax = 0x5803; r.w.bx = 0; intdos( &r, &r );     // DOS stops at our block
  if ( s_tailParas ) _fmemcpy( s_tail, MK_FP( s_tailSeg, 0 ), s_tailParas * 16 );
  s_tailSaved = 1;
}

static void tail_restore( void ) {
  if ( !s_tailSaved ) return;
  if ( s_tailParas ) _fmemcpy( MK_FP( s_tailSeg, 0 ), s_tail, s_tailParas * 16 );
  union REGS r;
  r.w.ax = 0x5803; r.w.bx = s_savedLink; intdos( &r, &r );
  s_tailSaved = 0;
}

unsigned vid_reserve_seg( void ) { return s_resSeg; }


// ---------------------------------------------------------------- colours
//
// Logical colours 0-15 -> pixel values of the current mode (s_map) and a byte
// full of that value (s_fill).  CGA 4-colour pictures map each logical colour to
// the nearest palette colour; 640x200x2 maps dark colours to black, light to white.

static const unsigned char s_rgb[16][3] = {
  { 0, 0, 0 }, { 0, 0, 2 }, { 0, 2, 0 }, { 0, 2, 2 }, { 2, 0, 0 }, { 2, 0, 2 }, { 2, 1, 0 }, { 2, 2, 2 },
  { 1, 1, 1 }, { 1, 1, 3 }, { 1, 3, 1 }, { 1, 3, 3 }, { 3, 1, 1 }, { 3, 1, 3 }, { 3, 3, 1 }, { 3, 3, 3 }
};
static const unsigned char s_cgaSets[6][3] = {
  { 2, 4, 6 }, { 10, 12, 14 }, { 3, 5, 7 }, { 11, 13, 15 }, { 3, 4, 7 }, { 11, 12, 15 }
};
static unsigned char s_map[16], s_fill[16];
static int s_cgaPal = 3, s_cgaBg = 0;
static int s_hwMode5 = 0;                      // currently in BIOS mode 5 (the cyan/red set)

int vid_bpp = 4;
int vid_shift = 1;
static unsigned char s_pmask = 1;              // pixels per byte - 1
static unsigned char s_vmask = 0x0F;           // one pixel's bits

static void set_maps( void ) {
  for ( int c = 0; c < 16; c++ ) {
    unsigned char v;
    if ( vid_bpp == 4 ) v = (unsigned char)c;
    else if ( vid_bpp == 1 ) v = ( c == 7 || c >= 9 ) ? 1 : 0;      // light grey and the bright colours
    else {
      int best = 0x7FFF;
      v = 0;
      for ( int i = 0; i < 4; i++ ) {
        int pc = i ? s_cgaSets[ s_cgaPal ][ i - 1 ] : s_cgaBg;
        int d = 0;
        for ( int k = 0; k < 3; k++ ) { int e = s_rgb[c][k] - s_rgb[pc][k]; d += e * e; }
        if ( d < best ) { best = d; v = (unsigned char)i; }
      }
    }
    s_map[c] = v;
    s_fill[c] = vid_bpp == 4 ? (unsigned char)( v * 0x11 ) : vid_bpp == 2 ? (unsigned char)( v * 0x55 ) : (unsigned char)( v ? 0xFF : 0 );
  }
}

static void set_depth( int bpp ) {
  vid_bpp = bpp;
  vid_shift = bpp == 4 ? 1 : bpp == 2 ? 2 : 3;
  s_pmask = (unsigned char)( ( 8 / bpp ) - 1 );
  s_vmask = (unsigned char)( ( 1 << bpp ) - 1 );
  set_maps( );
}


// ---------------------------------------------------------------- mode set

static void crtc( unsigned char reg, unsigned char val ) {
  outp( 0x3D4, reg ); outp( 0x3D5, val );
}

static void varray( unsigned char reg, unsigned char val ) {
  outp( 0x3DA, reg ); outp( 0x3DE, val );
}

static void bios_mode( unsigned char m ) {
  union REGS r;
  r.h.ah = 0; r.h.al = m;
  int86( 0x10, &r, &r );
}

static void set_640( void ) {
  bios_mode( 3 );             // so the BIOS data is valid when we go back

  outp( 0x3D8, 0x13 );        // video off while loading the registers
  // Scan lines per row first: with the text value (8) still set, the new
  // row counts below would briefly mean 1600+ lines, which crashes 86Box.
  crtc( 0x09, 0x00 );
  crtc( 0x10, 0x18 );
  crtc( 0x00, 0x71 );
  crtc( 0x01, 0x50 );
  crtc( 0x02, 0x5A );
  crtc( 0x03, 0x0E );
  crtc( 0x04, 0xFF );
  crtc( 0x05, 0x06 );
  crtc( 0x06, 0xC8 );
  crtc( 0x07, 0xE2 );
  crtc( 0x09, 0x00 );
  crtc( 0x0C, 0x00 );
  crtc( 0x0D, 0x00 );
  crtc( 0x11, 0x00 );
  crtc( 0x12, 0x46 );

  varray( 0x01, 0x0F );       // palette mask
  varray( 0x02, 0x00 );       // border colour
  varray( 0x03, 0x10 );       // mode control
  varray( 0x05, 0x01 );       // 640x200x16 enable
  varray( 0x08, 0x02 );

  outp( 0x3D9, 0x00 );
  outp( 0x3DD, 0x00 );        // extended RAM paging off
  outp( 0x3DF, 0x24 );        // addressing mode, page 2 (top 64K)

  _fmemset( MK_FP( 0xA000, 0 ), 0, 0x8000 );
  _fmemset( MK_FP( 0xA000, 0x8000 ), 0, 0x8000 );

  outp( 0x3D8, 0x1B );        // video on
}

static void unset_640( void ) {
  static const unsigned char regs[] = {
    0x00,0x71, 0x01,0x50, 0x02,0x5A, 0x03,0xFE, 0x04,0x1C, 0x05,0x01,
    0x06,0x19, 0x07,0x1A, 0x09,0x08, 0x0A,0x06, 0x0B,0x07, 0x0C,0x00,
    0x0D,0x00, 0x0E,0x00, 0x0F,0x00, 0x10,0x18, 0x11,0x20, 0x12,0x07,
    0x13,0x00
  };
  outp( 0x3D8, 0x01 );
  for ( unsigned i = 0; i < sizeof( regs ); i += 2 ) crtc( regs[i], regs[i+1] );
  outp( 0x3D9, 0x00 );
  outp( 0x3DD, 0x00 );
  varray( 0x01, 0x0F );
  varray( 0x02, 0x00 );
  varray( 0x03, 0x10 );
  varray( 0x05, 0x00 );
  varray( 0x08, 0x00 );
  outp( 0x3DF, 0x3F );
  outp( 0x3D8, 0x29 );
}

int vid_is_cga( void ) { return vid_mode == VM_CGA2 || vid_mode == VM_CGA4; }

// The CGA 4-colour palette into the hardware: the colour select register has the
// background in bits 0-3, intensity in bit 4 and the palette in bit 5; the
// cyan/red set needs BIOS mode 5.  A Tandy works the same way in its CGA modes:
// that colour then goes through its palette registers, which the BIOS leaves as
// they are (identity), so loading those with our colours would change nothing.
static void apply_cga_palette( void ) {
  if ( vid_mode != VM_CGA4 ) return;
  int mode5 = s_cgaPal >= 4;
  if ( mode5 != s_hwMode5 ) { bios_mode( mode5 ? 5 : 4 ); s_hwMode5 = mode5; }
  unsigned char v = (unsigned char)( ( s_cgaBg & 15 ) | ( ( s_cgaPal & 1 ) ? 0x10 : 0 ) | ( ( s_cgaPal == 2 || s_cgaPal == 3 ) ? 0x20 : 0 ) );
  outp( 0x3D9, v );
  *(unsigned char far *)MK_FP( 0x40, 0x66 ) = v;    // BIOS copy of the colour select register
}

void vid_cga_palette( int pal, int bg ) {
  s_cgaPal = ( pal >= 0 && pal < 6 ) ? pal : 3;
  s_cgaBg = bg & 15;
  apply_cga_palette( );
  if ( vid_mode == VM_CGA4 ) set_maps( );
}

static void set_cga( int mode ) {
  if ( mode == VM_CGA2 ) {
    bios_mode( 6 );
    vid_w = 640; vid_pitch = 80;
    set_depth( 1 );
  }
  else {
    s_hwMode5 = s_cgaPal >= 4;
    bios_mode( s_hwMode5 ? 5 : 4 );
    vid_w = 320; vid_pitch = 80;
    vid_mode = VM_CGA4;
    apply_cga_palette( );
    set_depth( 2 );
  }
  vid_h = 200;
}

static void build_line_table( void );

int vid_open( int mode ) {
  union REGS r;
  r.h.ah = 0x0F;
  int86( 0x10, &r, &r );
  s_oldMode = r.h.al;

  if ( mode < VM_640 || mode > VM_CGA4 ) return 1;
  if ( mode == VM_640 ) {
    tail_protect( );            // the video RAM covers DOS's top block
    set_640( );
    vid_w = 640; vid_h = 200; vid_pitch = 320;
    set_depth( 4 );
  }
  else if ( mode == VM_320 ) {
    tail_protect( );
    bios_mode( 9 );
    vid_w = 320; vid_h = 200; vid_pitch = 160;
    set_depth( 4 );
  }
  else {
    vid_mode = mode;
    set_cga( mode );
  }
  vid_mode = mode;
  build_line_table( );
  if ( !s_fontLo ) vid_font_rom( );
  return 0;
}

int vid_switch( int mode ) {
  if ( vid_mode == VM_NONE ) return vid_open( mode );
  if ( mode == vid_mode ) return 0;
  if ( vid_is_cga( ) && ( mode == VM_CGA2 || mode == VM_CGA4 ) ) {
    vid_mode = mode;
    set_cga( mode );
    build_line_table( );
    return 0;
  }
  unsigned char old = s_oldMode;
  vid_close( );
  int rc = vid_open( mode );
  s_oldMode = old;
  return rc;
}

void vid_close( void ) {
  if ( vid_mode == VM_640 ) unset_640( );
  vid_mode = VM_NONE;
  s_hwMode5 = 0;
  bios_mode( ( s_oldMode == 7 ) ? 3 : s_oldMode );
  tail_restore( );              // after the mode change: text mode no longer uses that RAM
}


// ---------------------------------------------------------------- drawing

static unsigned s_lineSeg = 0xA000;
static unsigned s_lineOff[ 200 ];

static void build_line_table( void ) {
  for ( unsigned y = 0; y < 200; y++ ) {
    if ( vid_mode == VM_640 )      s_lineOff[y] = y * 320u;
    else if ( vid_mode == VM_320 ) s_lineOff[y] = ( ( y & 3 ) << 13 ) + ( y >> 2 ) * 160u;
    else                           s_lineOff[y] = ( ( y & 1 ) << 13 ) + ( y >> 1 ) * 80u;
  }
  s_lineSeg = ( vid_mode == VM_640 ) ? 0xA000 : 0xB800;
}

unsigned char far *vid_line_ptr( int y ) {
  return (unsigned char far *)MK_FP( s_lineSeg, s_lineOff[ y ] );
}

// Bytes b0..b1 of a pixel span x0..x1, with the masks of the partial end bytes
struct Span { unsigned b0, b1; unsigned char m0, m1; };

static void span_of( int x0, int x1, Span *s ) {
  s->b0 = (unsigned)x0 >> vid_shift;
  s->b1 = (unsigned)x1 >> vid_shift;
  s->m0 = (unsigned char)( 0xFF >> ( ( x0 & s_pmask ) * vid_bpp ) );
  s->m1 = (unsigned char)( 0xFF << ( ( s_pmask - ( x1 & s_pmask ) ) * vid_bpp ) );
}

// Copy the span of one line to another, leaving the pixels outside it alone
static void copy_span( unsigned char far *dst, const unsigned char far *src, const Span *s ) {
  if ( s->b0 == s->b1 ) {
    unsigned char m = s->m0 & s->m1;
    dst[ s->b0 ] = (unsigned char)( ( dst[ s->b0 ] & ~m ) | ( src[ s->b0 ] & m ) );
    return;
  }
  unsigned from = s->b0, to = s->b1 + 1;
  if ( s->m0 != 0xFF ) { dst[ s->b0 ] = (unsigned char)( ( dst[ s->b0 ] & ~s->m0 ) | ( src[ s->b0 ] & s->m0 ) ); from++; }
  if ( s->m1 != 0xFF ) { dst[ s->b1 ] = (unsigned char)( ( dst[ s->b1 ] & ~s->m1 ) | ( src[ s->b1 ] & s->m1 ) ); to--; }
  if ( to > from ) _fmemcpy( dst + from, src + from, to - from );
}

void vid_scroll_down( int x, int y, int w, int h, int n, unsigned char fill ) {
  // Moves lines y.. down by n (bottom first), fills the top n lines
  if ( n <= 0 || h <= 0 || w <= 0 ) return;
  if ( n >= h ) { vid_fill( x, y, w, h, fill ); return; }
  Span s;
  span_of( x, x + w - 1, &s );
  for ( int r = h - 1; r >= n; r-- ) copy_span( vid_line_ptr( y + r ), vid_line_ptr( y + r - n ), &s );
  vid_fill( x, y, w, n, fill );
}

void vid_scroll_up( int x, int y, int w, int h, int n, unsigned char fill ) {
  // Moves lines y+n.. up to y.., fills the bottom n lines
  if ( n <= 0 || h <= 0 || w <= 0 ) return;
  if ( n >= h ) { vid_fill( x, y, w, h, fill ); return; }
  Span s;
  span_of( x, x + w - 1, &s );
  for ( int r = 0; r < h - n; r++ ) copy_span( vid_line_ptr( y + r ), vid_line_ptr( y + r + n ), &s );
  vid_fill( x, y + h - n, w, n, fill );
}

void vid_clear( unsigned char c ) {
  vid_fill( 0, 0, vid_w, vid_h, c );
}

void vid_pset( int x, int y, unsigned char c ) {
  if ( (unsigned)x >= (unsigned)vid_w || (unsigned)y >= (unsigned)vid_h ) return;
  unsigned char far *p = vid_line_ptr( y ) + ( (unsigned)x >> vid_shift );
  unsigned char sh = (unsigned char)( 8 - vid_bpp * ( ( x & s_pmask ) + 1 ) );
  unsigned char m = (unsigned char)( s_vmask << sh );
  *p = (unsigned char)( ( *p & ~m ) | ( ( s_map[ c & 15 ] << sh ) & m ) );
}

unsigned char vid_pget( int x, int y ) {
  if ( (unsigned)x >= (unsigned)vid_w || (unsigned)y >= (unsigned)vid_h ) return 0;
  unsigned char b = vid_line_ptr( y )[ (unsigned)x >> vid_shift ];
  unsigned char sh = (unsigned char)( 8 - vid_bpp * ( ( x & s_pmask ) + 1 ) );
  return (unsigned char)( ( b >> sh ) & s_vmask );
}

void vid_hline( int x0, int x1, int y, unsigned char c ) {
  if ( x0 > x1 ) { int t = x0; x0 = x1; x1 = t; }
  if ( (unsigned)y >= (unsigned)vid_h || x1 < 0 || x0 >= vid_w ) return;
  if ( x0 < 0 ) x0 = 0;
  if ( x1 >= vid_w ) x1 = vid_w - 1;
  unsigned char far *line = vid_line_ptr( y );
  unsigned char fb = s_fill[ c & 15 ];
  Span s;
  span_of( x0, x1, &s );
  if ( s.b0 == s.b1 ) {
    unsigned char m = s.m0 & s.m1;
    line[ s.b0 ] = (unsigned char)( ( line[ s.b0 ] & ~m ) | ( fb & m ) );
    return;
  }
  line[ s.b0 ] = (unsigned char)( ( line[ s.b0 ] & ~s.m0 ) | ( fb & s.m0 ) );
  line[ s.b1 ] = (unsigned char)( ( line[ s.b1 ] & ~s.m1 ) | ( fb & s.m1 ) );
  if ( s.b1 > s.b0 + 1 ) _fmemset( line + s.b0 + 1, fb, s.b1 - s.b0 - 1 );
}

void vid_vline( int x, int y0, int y1, unsigned char c ) {
  if ( y0 > y1 ) { int t = y0; y0 = y1; y1 = t; }
  for ( int y = y0; y <= y1; y++ ) vid_pset( x, y, c );
}

void vid_fill( int x, int y, int w, int h, unsigned char c ) {
  if ( w <= 0 || h <= 0 ) return;
  if ( y < 0 ) { h += y; y = 0; }
  if ( y + h > vid_h ) h = vid_h - y;
  for ( int i = 0; i < h; i++ ) vid_hline( x, x + w - 1, y + i, c );
}

void vid_rect( int x, int y, int w, int h, unsigned char c ) {
  vid_hline( x, x + w - 1, y, c );
  vid_hline( x, x + w - 1, y + h - 1, c );
  vid_vline( x, y, y + h - 1, c );
  vid_vline( x + w - 1, y, y + h - 1, c );
}


// ---------------------------------------------------------------- text

extern const unsigned char font_hi[ 128 * 8 ];     // fonthi.cpp

void vid_font_rom( void ) {
  s_fontLo = (const unsigned char far *)MK_FP( 0xF000, 0xFA6E );
  // Characters 128-255: INT 1Fh points at them on a Tandy (or with GRAFTABL); a plain
  // PC/AT leaves it empty, or pointing at something that is no font: use our own then
  unsigned long far *ivt = (unsigned long far *)MK_FP( 0, 0 );
  unsigned long v = ivt[ 0x1F ];
  s_fontHi = v ? (const unsigned char far *)MK_FP( (unsigned)( v >> 16 ), (unsigned)v ) : 0;
  if ( s_fontHi ) {
    unsigned i;
    for ( i = 1; i < 128 * 8 && s_fontHi[i] == s_fontHi[0]; i++ ) ;
    if ( i == 128 * 8 ) s_fontHi = 0;
  }
  if ( !s_fontHi ) s_fontHi = (const unsigned char far *)font_hi;
}

static unsigned long s_tab[ 256 ];
static unsigned char s_tabFg = 0xFF, s_tabBg = 0xFF;

static void build_char_table( unsigned char fg, unsigned char bg ) {
  unsigned char pair[4];
  pair[0] = ( bg << 4 ) | bg;  pair[1] = ( bg << 4 ) | fg;
  pair[2] = ( fg << 4 ) | bg;  pair[3] = ( fg << 4 ) | fg;
  for ( unsigned b = 0; b < 256; b++ ) {
    // byte order in memory = left to right on screen
    s_tab[b] = (unsigned long)pair[ ( b >> 6 ) & 3 ]
             | ( (unsigned long)pair[ ( b >> 4 ) & 3 ] << 8 )
             | ( (unsigned long)pair[ ( b >> 2 ) & 3 ] << 16 )
             | ( (unsigned long)pair[ b & 3 ] << 24 );
  }
  s_tabFg = fg; s_tabBg = bg;
}

// 640x200x2: a font row is already a screen byte (at any x: split over two bytes)
static void char_1bpp( int x, int y, const unsigned char far *glyph, unsigned char fg, int bg ) {
  unsigned char fv = s_map[ fg & 15 ], bv = bg >= 0 ? s_map[ bg & 15 ] : 0;
  unsigned sh = x & 7, bx = (unsigned)x >> 3;
  for ( int r = 0; r < 8; r++ ) {
    unsigned char g = glyph[r], row, mask;
    if ( bg >= 0 ) { row = fv ? ( bv ? 0xFF : g ) : ( bv ? (unsigned char)~g : 0 ); mask = 0xFF; }
    else           { row = fv ? g : 0; mask = g; }
    unsigned char far *p = (unsigned char far *)MK_FP( s_lineSeg, s_lineOff[ y + r ] + bx );
    if ( !sh ) { p[0] = (unsigned char)( ( p[0] & ~mask ) | ( row & mask ) ); continue; }
    unsigned char m0 = (unsigned char)( mask >> sh ), m1 = (unsigned char)( mask << ( 8 - sh ) );
    p[0] = (unsigned char)( ( p[0] & ~m0 ) | ( ( row >> sh ) & m0 ) );
    p[1] = (unsigned char)( ( p[1] & ~m1 ) | ( (unsigned char)( row << ( 8 - sh ) ) & m1 ) );
  }
}

int vid_char( int x, int y, unsigned char ch, unsigned char fg, int bg ) {
  const unsigned char far *glyph;
  if ( ch < 128 ) glyph = s_fontLo + ch * 8;
  else if ( s_fontHi ) glyph = s_fontHi + ( ch - 128 ) * 8;
  else glyph = s_fontLo + '?' * 8;

  int onScreen = x >= 0 && x + 8 <= vid_w && y >= 0 && y + 8 <= vid_h;
  if ( onScreen && vid_bpp == 1 ) { char_1bpp( x, y, glyph, fg, bg ); return x + 8; }
  if ( !onScreen || vid_bpp != 4 || ( x & 1 ) ) {
    // Slow path: odd x, partly off screen, or 320x200x4
    for ( int r = 0; r < 8; r++ ) {
      unsigned char bits = glyph[r];
      for ( int c = 0; c < 8; c++ ) {
        if ( bits & ( 0x80 >> c ) ) vid_pset( x + c, y + r, fg );
        else if ( bg >= 0 ) vid_pset( x + c, y + r, (unsigned char)bg );
      }
    }
    return x + 8;
  }

  fg &= 0x0F;
  if ( bg >= 0 ) {
    // Fastest path: a 256-entry table turns a font row straight into its 4 screen bytes
    if ( fg != s_tabFg || ( bg & 0x0F ) != s_tabBg ) build_char_table( fg, (unsigned char)( bg & 0x0F ) );
    unsigned x2 = (unsigned)x >> 1;
    for ( int r = 0; r < 8; r++ ) {
      unsigned long far *p = (unsigned long far *)MK_FP( s_lineSeg, s_lineOff[ y + r ] + x2 );
      *p = s_tab[ glyph[r] ];
    }
    return x + 8;
  }

  // Transparent: each pair of font bits merges into one byte
  unsigned char val[4], mask[4];
  {
    val[0] = 0;          mask[0] = 0xFF;
    val[1] = fg;         mask[1] = 0xF0;
    val[2] = fg << 4;    mask[2] = 0x0F;
    val[3] = fg * 0x11;  mask[3] = 0x00;
  }
  for ( int r = 0; r < 8; r++ ) {
    unsigned char far *p = vid_line_ptr( y + r ) + ( x >> 1 );
    unsigned char bits = glyph[r];
    for ( int k = 0; k < 4; k++ ) {
      unsigned char pr = ( bits >> 6 ) & 3;
      p[k] = ( p[k] & mask[pr] ) | val[pr];
      bits <<= 2;
    }
  }
  return x + 8;
}

int vid_text( int x, int y, const char *s, unsigned char fg, int bg ) {
  while ( *s ) x = vid_char( x, y, (unsigned char)*s++, fg, bg );
  return x;
}

int vid_text_n( int x, int y, const char far *s, int n, unsigned char fg, int bg ) {
  while ( n-- > 0 && *s ) x = vid_char( x, y, (unsigned char)*s++, fg, bg );
  return x;
}


// ---------------------------------------------------------------- blits

void vid_blit_full( const unsigned char far *src ) {
  if ( vid_mode == VM_640 ) {
    _fmemcpy( MK_FP( 0xA000, 0 ), src, 64000u );
    return;
  }
  for ( int y = 0; y < vid_h; y++ ) {
    _fmemcpy( vid_line_ptr( y ), src, vid_pitch );
    src += vid_pitch;
  }
}

void vid_blit( int x, int y, int w, int h, const unsigned char far *src, unsigned srcPitch ) {
  if ( x < 0 || y < 0 || x + w > vid_w || y + h > vid_h || w <= 0 ) return;
  unsigned bytes = ( (unsigned)w + s_pmask ) >> vid_shift;
  unsigned sh = vid_bpp == 1 ? ( x & 7 ) : 0;
  for ( int r = 0; r < h; r++ ) {
    unsigned char far *d = vid_line_ptr( y + r ) + ( (unsigned)x >> vid_shift );
    if ( !sh ) _fmemcpy( d, src, bytes );
    else {
      // 640x200x2 at any x: each source byte straddles two screen bytes
      unsigned char keep = (unsigned char)( 0xFF << ( 8 - sh ) );       // pixels left of x
      d[0] = (unsigned char)( ( d[0] & keep ) | ( src[0] >> sh ) );
      for ( unsigned i = 1; i < bytes; i++ )
        d[i] = (unsigned char)( ( src[ i - 1 ] << ( 8 - sh ) ) | ( src[i] >> sh ) );
      d[ bytes ] = (unsigned char)( ( d[ bytes ] & ~keep ) | (unsigned char)( src[ bytes - 1 ] << ( 8 - sh ) ) );
    }
    src += srcPitch;
  }
}

unsigned vid_save_size( int w, int h ) {
  return (unsigned)h * ( ( (unsigned)w >> vid_shift ) + 2 );
}

static int save_bounds( int &x, int &y, int &w, int &h, unsigned &bx, unsigned &bytes ) {
  if ( x < 0 ) { w += x; x = 0; }
  if ( y < 0 ) { h += y; y = 0; }
  if ( x + w > vid_w ) w = vid_w - x;
  if ( y + h > vid_h ) h = vid_h - y;
  if ( w <= 0 || h <= 0 ) return 0;
  bx = (unsigned)x >> vid_shift;
  bytes = ( ( (unsigned)( x + w - 1 ) ) >> vid_shift ) - bx + 1;
  return 1;
}

void vid_save( int x, int y, int w, int h, unsigned char far *dst ) {
  unsigned bx, bytes;
  if ( !save_bounds( x, y, w, h, bx, bytes ) ) return;
  for ( int r = 0; r < h; r++ ) {
    _fmemcpy( dst, vid_line_ptr( y + r ) + bx, bytes );
    dst += bytes;
  }
}

void vid_restore( int x, int y, int w, int h, const unsigned char far *src ) {
  unsigned bx, bytes;
  if ( !save_bounds( x, y, w, h, bx, bytes ) ) return;
  for ( int r = 0; r < h; r++ ) {
    _fmemcpy( vid_line_ptr( y + r ) + bx, src, bytes );
    src += bytes;
  }
}
