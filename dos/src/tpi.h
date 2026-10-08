// DeskMind - TPI picture files (format: helper\mindserver\tpi.py).

#ifndef TPI_H
#define TPI_H

#pragma pack( push, 1 )
struct TpiHeader {
  char           magic[4];         // "TPI1"
  unsigned char  mode;             // 1 = 640x200x16, 2 = 320x200x16, 3 = CGA 320x200x4, 4 = CGA 640x200x2
  unsigned char  flags;
  unsigned short width, height;
  unsigned short thumb_w, thumb_h;
  char           id[8];            // 8 hex digits, no NUL
  unsigned short year;
  unsigned char  month, day, hour, minute;
  unsigned long  seed;
  char           title[40];        // NUL padded
  unsigned long  prompt_off;
  unsigned short prompt_len;
  unsigned long  thumb_off;
  unsigned short thumb_len;
  unsigned long  image_off;
  unsigned short image_len;
  unsigned char  cga_pal;          // mode 3: palette 0-5 (see vid_cga_palette)
  unsigned char  cga_color;        // mode 3: background colour; mode 4: foreground (15)
  unsigned char  reserved[36];
};
#pragma pack( pop )

#define TPI_THUMB_MAX 4000         // 160x50 at 2 pixels per byte (CGA: 1000, 8 per byte)

// 1 = this run uses CGA: only CGA pictures count (modes 3-4), else only Tandy ones (1-2).
// The other family's files are treated as not pictures (tpi_header returns 4).
extern int tpi_cga;

int  tpi_header( const char *path, TpiHeader *h );          // 0 = ok
void tpi_id( const TpiHeader *h, char *out9 );               // "1A2B3C4D"
// Full image to the screen, 0 = ok.  CGA: switches to the picture's mode and palette first
// (the caller switches back to VM_CGA2 and redraws).
int  tpi_show( const char *path );
int  tpi_thumb( const char *path, unsigned char far *buf, TpiHeader *h );   // loads thumbnail pixels
void tpi_draw_thumb( int x, int y, const unsigned char far *buf, int tw, int th );
unsigned tpi_thumb_pitch( int tw );                          // bytes per thumbnail line
int  tpi_prompt( const char *path, char *buf, unsigned size );
int  tpi_set_title( const char *path, const char *title );
int  tpi_vmode( const TpiHeader *h );                       // VM_* the picture is shown in
void tpi_set_mode( const TpiHeader *h );                     // switch to it (CGA family; palette too)
// 640x200x16 or CGA pictures: the whole image into buf (64000 or 16000 bytes, line order), 0 = ok
int  tpi_image( const char *path, unsigned char far *buf, TpiHeader *h );
// Lines y0..y0+n-1 of the picture straight into video memory (to repair an overlay)
int  tpi_lines_to_screen( const char *path, int y0, int n );

#endif
