// DeskMind - Tandy Video II and CGA graphics
//
// 640x200x16 ("mode E"): linear at A000:0000, 320 bytes per line,
// two pixels per byte, left pixel in the high nibble.  Not a BIOS mode;
// vid_open() programs the registers directly.
//
// 320x200x16 (BIOS mode 9) is the fallback: B800:0000, 160 bytes per line,
// four interleaved 8K banks (line & 3).
//
// CGA (for a plain 286): 640x200x2 (BIOS mode 6, the GUI) and 320x200x4
// (mode 4/5, pictures).  B800:0000, 80 bytes per line, two interleaved 8K
// banks (line & 1), leftmost pixel in the high bits.  The GUI's 16 logical
// colours are mapped: dark ones to black, light ones to white (mode 6), or to
// the nearest of the four palette colours (mode 4).  Pixel data passed to the
// blit functions is in the mode's own format and is not mapped.

#ifndef VIDEO_H
#define VIDEO_H

#define VM_NONE 0
#define VM_640  1
#define VM_320  2
#define VM_CGA2 3      // 640x200, 2 colours
#define VM_CGA4 4      // 320x200, 4 colours

// Values returned by vid_detect()
#define VT_OTHER 0
#define VT_1000  1   // Tandy 1000 / PCjr (no 640x200x16)
#define VT_SLTL  2   // Tandy 1000 SL/TL/RL (Video II)

extern int  vid_mode;          // VM_*
extern int  vid_w, vid_h;      // pixels
extern unsigned vid_pitch;     // bytes per line
extern int  vid_bpp;           // bits per pixel: 4, 2 or 1
extern int  vid_shift;         // x >> vid_shift = byte in the line

int  vid_detect( void );
int  vid_is_cga( void );       // VM_CGA2 or VM_CGA4
// Claim the conventional memory that the video RAM overlaps (Tandy modes only;
// CGA RAM is separate).  Returns 0 on success, else an error code (see vid_reserve_error()).
int  vid_reserve( int mode );
const char *vid_reserve_error( int rc );
void vid_unreserve( void );
unsigned vid_reserve_seg( void );   // segment claimed (0 if none needed)
const char *vid_reserve_info( void ); // "block xxxx-yyyy, DOS top zzzz" for logs
unsigned vid_tail_paras( void );    // size of DOS's UMB link block kept safe (paragraphs)

int  vid_open( int mode );     // from text mode; 0 = ok
int  vid_switch( int mode );   // to another graphics mode while open (vid_close still returns to text)
void vid_close( void );        // back to 80x25 colour text

// CGA 320x200x4: palette 0-5 (helper dither.CGA_SETS: bit 0 = intensity, 4-5 = the
// mode-5 cyan/red set) and background colour 0-15.  Also re-maps the logical colours.
void vid_cga_palette( int pal, int bg );

// Drawing (clipped to the screen); c is a logical colour 0-15
void vid_clear( unsigned char c );
void vid_pset( int x, int y, unsigned char c );
unsigned char vid_pget( int x, int y );        // raw pixel value
void vid_hline( int x0, int x1, int y, unsigned char c );
void vid_vline( int x, int y0, int y1, unsigned char c );
void vid_fill( int x, int y, int w, int h, unsigned char c );
void vid_rect( int x, int y, int w, int h, unsigned char c );

// 8x8 text.  bg < 0 = transparent.  Returns the x after the text.
void vid_font_rom( void );     // use the BIOS 8x8 font (F000:FA6E + INT 1Fh)
int  vid_char( int x, int y, unsigned char ch, unsigned char fg, int bg );
int  vid_text( int x, int y, const char *s, unsigned char fg, int bg );
int  vid_text_n( int x, int y, const char far *s, int n, unsigned char fg, int bg );   // at most n chars

// Move a region up/down by n lines and fill the gap (whole bytes move: keep x and w
// byte-aligned, or only vertical edges in the partial bytes)
void vid_scroll_up( int x, int y, int w, int h, int n, unsigned char fill );
void vid_scroll_down( int x, int y, int w, int h, int n, unsigned char fill );

// Whole-screen copy from a buffer in the native layout, lines in order (vid_pitch * vid_h bytes)
void vid_blit_full( const unsigned char far *src );
// Copy a w x h rectangle of packed pixels to the screen.  x must be byte-aligned,
// except in 640x200x2 (any x).
void vid_blit( int x, int y, int w, int h, const unsigned char far *src, unsigned srcPitch );
// Save / restore a rectangle, whole bytes (for the mouse cursor, menus)
void vid_save( int x, int y, int w, int h, unsigned char far *dst );
void vid_restore( int x, int y, int w, int h, const unsigned char far *src );
unsigned vid_save_size( int w, int h );

unsigned char far *vid_line_ptr( int y );

#endif
