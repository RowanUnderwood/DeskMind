// DeskMind - TPI picture files (format: helper\mindserver\tpi.py).

#ifndef TPI_H
#define TPI_H

#pragma pack( push, 1 )
struct TpiHeader {
  char           magic[4];         // "TPI1"
  unsigned char  mode;             // 1 = 640x200, 2 = 320x200
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
  unsigned char  reserved[38];
};
#pragma pack( pop )

#define TPI_THUMB_MAX 4000         // 160x50 at 2 pixels per byte

int  tpi_header( const char *path, TpiHeader *h );          // 0 = ok
void tpi_id( const TpiHeader *h, char *out9 );               // "1A2B3C4D"
int  tpi_show( const char *path );                           // full image to the screen, 0 = ok
int  tpi_thumb( const char *path, unsigned char far *buf, TpiHeader *h );   // loads thumbnail pixels
void tpi_draw_thumb( int x, int y, const unsigned char far *buf, int tw, int th );
int  tpi_prompt( const char *path, char *buf, unsigned size );
int  tpi_set_title( const char *path, const char *title );
// 640x200 pictures only: the whole image into buf (64000 bytes, line order), 0 = ok
int  tpi_image( const char *path, unsigned char far *buf, TpiHeader *h );
// Lines y0..y0+n-1 of a 640x200 picture straight into video memory (to repair an overlay)
int  tpi_lines_to_screen( const char *path, int y0, int n );

#endif
