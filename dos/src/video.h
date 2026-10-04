// DeskMind - Tandy Video II graphics
//
// 640x200x16 ("mode E"): linear at A000:0000, 320 bytes per line,
// two pixels per byte, left pixel in the high nibble.  Not a BIOS mode;
// vid_open() programs the registers directly.
//
// 320x200x16 (BIOS mode 9) is the fallback: B800:0000, 160 bytes per line,
// four interleaved 8K banks (line & 3).

#ifndef VIDEO_H
#define VIDEO_H

#define VM_NONE 0
#define VM_640  1
#define VM_320  2

// Values returned by vid_detect()
#define VT_OTHER 0
#define VT_1000  1   // Tandy 1000 / PCjr (no 640x200x16)
#define VT_SLTL  2   // Tandy 1000 SL/TL/RL (Video II)

extern int  vid_mode;          // VM_*
extern int  vid_w, vid_h;      // pixels
extern unsigned vid_pitch;     // bytes per line

int  vid_detect( void );
// Claim the conventional memory that the video RAM overlaps.
// Returns 0 on success, else an error code (see vid_reserve_error()).
int  vid_reserve( int mode );
const char *vid_reserve_error( int rc );
void vid_unreserve( void );
unsigned vid_reserve_seg( void );   // segment claimed (0 if none needed)
const char *vid_reserve_info( void ); // "block xxxx-yyyy, DOS top zzzz" for logs
unsigned vid_tail_paras( void );    // size of DOS's UMB link block kept safe (paragraphs)

int  vid_open( int mode );     // VM_640 or VM_320; 0 = ok
void vid_close( void );        // back to 80x25 colour text

// Drawing (clipped to the screen)
void vid_clear( unsigned char c );
void vid_pset( int x, int y, unsigned char c );
unsigned char vid_pget( int x, int y );
void vid_hline( int x0, int x1, int y, unsigned char c );
void vid_vline( int x, int y0, int y1, unsigned char c );
void vid_fill( int x, int y, int w, int h, unsigned char c );
void vid_rect( int x, int y, int w, int h, unsigned char c );

// 8x8 text.  bg < 0 = transparent.  Returns the x after the text.
void vid_font_rom( void );     // use the BIOS 8x8 font (F000:FA6E + INT 1Fh)
int  vid_char( int x, int y, unsigned char ch, unsigned char fg, int bg );
int  vid_text( int x, int y, const char *s, unsigned char fg, int bg );
int  vid_text_n( int x, int y, const char far *s, int n, unsigned char fg, int bg );   // at most n chars

// Move a byte-aligned region (x, w even) up by n lines and fill the gap
void vid_scroll_up( int x, int y, int w, int h, int n, unsigned char fill );
void vid_scroll_down( int x, int y, int w, int h, int n, unsigned char fill );

// Whole-screen copy from a buffer in the native layout (vid_pitch * vid_h bytes)
void vid_blit_full( const unsigned char far *src );
// Copy a w x h rectangle of packed pixels (w even, x even in 640 mode) to the screen
void vid_blit( int x, int y, int w, int h, const unsigned char far *src, unsigned srcPitch );
// Save / restore a byte-aligned rectangle (for the mouse cursor, menus)
void vid_save( int x, int y, int w, int h, unsigned char far *dst );
void vid_restore( int x, int y, int w, int h, const unsigned char far *src );
unsigned vid_save_size( int w, int h );

unsigned char far *vid_line_ptr( int y );

#endif
