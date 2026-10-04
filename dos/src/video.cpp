// DeskMind - Tandy Video II graphics.  See video.h.
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

int vid_open( int mode ) {
  union REGS r;
  r.h.ah = 0x0F;
  int86( 0x10, &r, &r );
  s_oldMode = r.h.al;

  if ( mode != VM_640 && mode != VM_320 ) return 1;
  tail_protect( );              // both modes' video RAM covers DOS's top block
  if ( mode == VM_640 ) {
    set_640( );
    vid_w = 640; vid_h = 200; vid_pitch = 320;
  }
  else if ( mode == VM_320 ) {
    bios_mode( 9 );
    vid_w = 320; vid_h = 200; vid_pitch = 160;
  }
  vid_mode = mode;
  build_line_table( );
  if ( !s_fontLo ) vid_font_rom( );
  return 0;
}

void vid_close( void ) {
  if ( vid_mode == VM_640 ) unset_640( );
  vid_mode = VM_NONE;
  bios_mode( ( s_oldMode == 7 ) ? 3 : s_oldMode );
  tail_restore( );              // after the mode change: text mode no longer uses that RAM
}


// ---------------------------------------------------------------- drawing

static unsigned s_lineSeg = 0xA000;
static unsigned s_lineOff[ 200 ];

static void build_line_table( void ) {
  for ( unsigned y = 0; y < 200; y++ ) {
    if ( vid_mode == VM_640 ) s_lineOff[y] = y * 320u;
    else                      s_lineOff[y] = ( ( y & 3 ) << 13 ) + ( y >> 2 ) * 160u;
  }
  s_lineSeg = ( vid_mode == VM_640 ) ? 0xA000 : 0xB800;
}

unsigned char far *vid_line_ptr( int y ) {
  return (unsigned char far *)MK_FP( s_lineSeg, s_lineOff[ y ] );
}

void vid_scroll_down( int x, int y, int w, int h, int n, unsigned char fill ) {
  // Byte-aligned region (x and w even); moves lines y.. down by n (bottom first), fills the top n lines
  if ( n <= 0 || h <= 0 ) return;
  if ( n >= h ) { vid_fill( x, y, w, h, fill ); return; }
  unsigned bx = (unsigned)x >> 1, bytes = (unsigned)w >> 1;
  for ( int r = h - 1; r >= n; r-- ) {
    _fmemcpy( vid_line_ptr( y + r ) + bx, vid_line_ptr( y + r - n ) + bx, bytes );
  }
  vid_fill( x, y, w, n, fill );
}

void vid_scroll_up( int x, int y, int w, int h, int n, unsigned char fill ) {
  // Byte-aligned region (x and w even); moves lines y+n.. up to y.., fills the bottom n lines
  if ( n <= 0 || h <= 0 ) return;
  if ( n >= h ) { vid_fill( x, y, w, h, fill ); return; }
  unsigned bx = (unsigned)x >> 1, bytes = (unsigned)w >> 1;
  for ( int r = 0; r < h - n; r++ ) {
    _fmemcpy( vid_line_ptr( y + r ) + bx, vid_line_ptr( y + r + n ) + bx, bytes );
  }
  vid_fill( x, y + h - n, w, n, fill );
}

void vid_clear( unsigned char c ) {
  vid_fill( 0, 0, vid_w, vid_h, c );
}

void vid_pset( int x, int y, unsigned char c ) {
  if ( (unsigned)x >= (unsigned)vid_w || (unsigned)y >= (unsigned)vid_h ) return;
  unsigned char far *p = vid_line_ptr( y ) + ( x >> 1 );
  if ( x & 1 ) *p = ( *p & 0xF0 ) | ( c & 0x0F );
  else         *p = ( *p & 0x0F ) | ( c << 4 );
}

unsigned char vid_pget( int x, int y ) {
  if ( (unsigned)x >= (unsigned)vid_w || (unsigned)y >= (unsigned)vid_h ) return 0;
  unsigned char b = vid_line_ptr( y )[ x >> 1 ];
  return ( x & 1 ) ? ( b & 0x0F ) : ( b >> 4 );
}

void vid_hline( int x0, int x1, int y, unsigned char c ) {
  if ( x0 > x1 ) { int t = x0; x0 = x1; x1 = t; }
  if ( (unsigned)y >= (unsigned)vid_h || x1 < 0 || x0 >= vid_w ) return;
  if ( x0 < 0 ) x0 = 0;
  if ( x1 >= vid_w ) x1 = vid_w - 1;
  unsigned char far *line = vid_line_ptr( y );
  if ( x0 & 1 ) { vid_pset( x0, y, c ); x0++; }
  if ( !( x1 & 1 ) ) { vid_pset( x1, y, c ); x1--; }
  if ( x1 > x0 ) _fmemset( line + ( x0 >> 1 ), c * 0x11, ( x1 - x0 + 1 ) >> 1 );
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

void vid_font_rom( void ) {
  s_fontLo = (const unsigned char far *)MK_FP( 0xF000, 0xFA6E );
  unsigned long far *ivt = (unsigned long far *)MK_FP( 0, 0 );
  unsigned long v = ivt[ 0x1F ];
  s_fontHi = v ? (const unsigned char far *)MK_FP( (unsigned)( v >> 16 ), (unsigned)v ) : 0;
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

int vid_char( int x, int y, unsigned char ch, unsigned char fg, int bg ) {
  const unsigned char far *glyph;
  if ( ch < 128 ) glyph = s_fontLo + ch * 8;
  else if ( s_fontHi ) glyph = s_fontHi + ( ch - 128 ) * 8;
  else glyph = s_fontLo + '?' * 8;

  if ( ( x & 1 ) || x < 0 || x + 8 > vid_w || y < 0 || y + 8 > vid_h ) {
    // Slow path: odd x or partly off screen
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
  if ( x < 0 || y < 0 || x + w > vid_w || y + h > vid_h ) return;
  unsigned bytes = (unsigned)w >> 1;
  for ( int r = 0; r < h; r++ ) {
    _fmemcpy( vid_line_ptr( y + r ) + ( x >> 1 ), src, bytes );
    src += srcPitch;
  }
}

unsigned vid_save_size( int w, int h ) {
  return (unsigned)h * ( ( (unsigned)w >> 1 ) + 1 );
}

static int save_bounds( int &x, int &y, int &w, int &h, unsigned &bx, unsigned &bytes ) {
  if ( x < 0 ) { w += x; x = 0; }
  if ( y < 0 ) { h += y; y = 0; }
  if ( x + w > vid_w ) w = vid_w - x;
  if ( y + h > vid_h ) h = vid_h - y;
  if ( w <= 0 || h <= 0 ) return 0;
  bx = (unsigned)x >> 1;
  bytes = ( ( (unsigned)( x + w - 1 ) ) >> 1 ) - bx + 1;
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
