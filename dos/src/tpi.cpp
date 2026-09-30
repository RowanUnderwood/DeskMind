// DeskMind - TPI picture files.  See tpi.h.

#include <stdio.h>
#include <string.h>
#include <dos.h>
#include <fcntl.h>
#include <io.h>

#include "tpi.h"
#include "video.h"

static int open_check( const char *path, TpiHeader *h ) {
  int fd = open( path, O_RDONLY | O_BINARY );
  if ( fd < 0 ) return -1;
  if ( read( fd, h, sizeof( *h ) ) != sizeof( *h ) || memcmp( h->magic, "TPI1", 4 ) != 0 ) {
    close( fd );
    return -1;
  }
  return fd;
}

int tpi_header( const char *path, TpiHeader *h ) {
  int fd = open_check( path, h );
  if ( fd < 0 ) return 1;
  close( fd );
  h->title[ sizeof( h->title ) - 1 ] = 0;
  return 0;
}

void tpi_id( const TpiHeader *h, char *out9 ) {
  memcpy( out9, h->id, 8 );
  out9[8] = 0;
}

// Reads straight from the file into video memory, line by line: no 64K buffer needed.
int tpi_show( const char *path ) {
  TpiHeader h;
  int fd = open_check( path, &h );
  if ( fd < 0 ) return 1;
  int want = ( h.mode == 1 ) ? VM_640 : VM_320;
  if ( want != vid_mode || h.width != (unsigned)vid_w || h.height != (unsigned)vid_h ) { close( fd ); return 2; }
  lseek( fd, h.image_off, SEEK_SET );
  unsigned pitch = h.width / 2;
  if ( vid_mode == VM_640 ) {
    // Linear: read big chunks directly into A000
    unsigned char far *v = vid_line_ptr( 0 );
    unsigned left = h.image_len;
    while ( left ) {
      unsigned n = left > 16000u ? 16000u : left;
      unsigned got;
      if ( _dos_read( fd, v, n, &got ) || got != n ) { close( fd ); return 3; }
      v += n; left -= n;
    }
  }
  else {
    for ( unsigned y = 0; y < h.height; y++ ) {
      unsigned got;
      if ( _dos_read( fd, vid_line_ptr( y ), pitch, &got ) || got != pitch ) { close( fd ); return 3; }
    }
  }
  close( fd );
  return 0;
}

int tpi_thumb( const char *path, unsigned char far *buf, TpiHeader *h ) {
  int fd = open_check( path, h );
  if ( fd < 0 ) return 1;
  if ( h->thumb_len > TPI_THUMB_MAX ) { close( fd ); return 2; }
  lseek( fd, h->thumb_off, SEEK_SET );
  unsigned got;
  int bad = _dos_read( fd, buf, h->thumb_len, &got ) || got != h->thumb_len;
  close( fd );
  h->title[ sizeof( h->title ) - 1 ] = 0;
  return bad ? 3 : 0;
}

void tpi_draw_thumb( int x, int y, const unsigned char far *buf, int tw, int th ) {
  if ( x & 1 ) x++;
  vid_blit( x, y, tw, th, buf, (unsigned)tw / 2 );
}

int tpi_prompt( const char *path, char *buf, unsigned size ) {
  TpiHeader h;
  int fd = open_check( path, &h );
  if ( fd < 0 ) return 1;
  unsigned n = h.prompt_len < size - 1 ? h.prompt_len : size - 1;
  lseek( fd, h.prompt_off, SEEK_SET );
  int got = read( fd, buf, n );
  close( fd );
  buf[ got > 0 ? got : 0 ] = 0;
  return 0;
}

int tpi_set_title( const char *path, const char *title ) {
  int fd = open( path, O_RDWR | O_BINARY );
  if ( fd < 0 ) return 1;
  char t[40];
  memset( t, 0, sizeof( t ) );
  strncpy( t, title, sizeof( t ) - 1 );
  lseek( fd, 32, SEEK_SET );
  int ok = write( fd, t, sizeof( t ) ) == sizeof( t );
  close( fd );
  return ok ? 0 : 2;
}

int tpi_image( const char *path, unsigned char far *buf, TpiHeader *h ) {
  int fd = open_check( path, h );
  if ( fd < 0 ) return 1;
  h->title[ sizeof( h->title ) - 1 ] = 0;
  if ( h->mode != 1 || h->width != 640 || h->height != 200 || h->image_len != 64000u ) { close( fd ); return 2; }
  lseek( fd, h->image_off, SEEK_SET );
  unsigned left = h->image_len;
  while ( left ) {
    unsigned n = left > 16000u ? 16000u : left, got;
    if ( _dos_read( fd, buf, n, &got ) || got != n ) { close( fd ); return 3; }
    buf += n; left -= n;
  }
  close( fd );
  return 0;
}

int tpi_lines_to_screen( const char *path, int y0, int n ) {
  TpiHeader h;
  int fd = open_check( path, &h );
  if ( fd < 0 ) return 1;
  if ( h.mode != 1 || vid_mode != VM_640 ) { close( fd ); return 2; }
  lseek( fd, h.image_off + (unsigned long)y0 * 320ul, SEEK_SET );
  unsigned got;
  int bad = _dos_read( fd, vid_line_ptr( y0 ), (unsigned)n * 320u, &got ) || got != (unsigned)n * 320u;
  close( fd );
  return bad ? 3 : 0;
}
