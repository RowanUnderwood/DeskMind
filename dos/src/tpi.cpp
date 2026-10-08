// DeskMind - TPI picture files.  See tpi.h.

#include <stdio.h>
#include <string.h>
#include <dos.h>
#include <fcntl.h>
#include <io.h>

#include "tpi.h"
#include "video.h"

int tpi_cga = 0;

static int family_ok( const TpiHeader *h ) {
  return tpi_cga ? ( h->mode == 3 || h->mode == 4 ) : ( h->mode == 1 || h->mode == 2 );
}

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
  return family_ok( h ) ? 0 : 4;
}

int tpi_vmode( const TpiHeader *h ) {
  switch ( h->mode ) {
    case 1: return VM_640;
    case 2: return VM_320;
    case 3: return VM_CGA4;
    case 4: return VM_CGA2;
  }
  return VM_NONE;
}

void tpi_set_mode( const TpiHeader *h ) {
  if ( !vid_is_cga( ) ) return;
  if ( h->mode == 3 ) vid_cga_palette( h->cga_pal, h->cga_color );
  vid_switch( tpi_vmode( h ) );
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
  if ( !family_ok( &h ) ) { close( fd ); return 2; }
  tpi_set_mode( &h );
  if ( tpi_vmode( &h ) != vid_mode || h.width != (unsigned)vid_w || h.height != (unsigned)vid_h ) { close( fd ); return 2; }
  lseek( fd, h.image_off, SEEK_SET );
  unsigned pitch = vid_pitch;
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
  if ( h->thumb_len > TPI_THUMB_MAX || !family_ok( h ) ) { close( fd ); return 2; }
  lseek( fd, h->thumb_off, SEEK_SET );
  unsigned got;
  int bad = _dos_read( fd, buf, h->thumb_len, &got ) || got != h->thumb_len;
  close( fd );
  h->title[ sizeof( h->title ) - 1 ] = 0;
  return bad ? 3 : 0;
}

unsigned tpi_thumb_pitch( int tw ) { return tpi_cga ? (unsigned)tw / 8 : (unsigned)tw / 2; }

void tpi_draw_thumb( int x, int y, const unsigned char far *buf, int tw, int th ) {
  if ( tpi_cga ) {                 // 1 bit per pixel; the 640x200x2 blit takes any x
    vid_blit( x, y, tw, th, buf, (unsigned)tw / 8 );
    return;
  }
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
  unsigned want = h->mode == 1 ? 64000u : 16000u;
  if ( !family_ok( h ) || h->mode == 2 || h->height != 200 || h->image_len != want ) { close( fd ); return 2; }
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
  if ( tpi_vmode( &h ) != vid_mode || ( h.mode != 1 && !vid_is_cga( ) ) ) { close( fd ); return 2; }
  lseek( fd, h.image_off + (unsigned long)y0 * vid_pitch, SEEK_SET );
  unsigned got;
  int bad = 0;
  if ( h.mode == 1 )               // linear: one read
    bad = _dos_read( fd, vid_line_ptr( y0 ), (unsigned)n * 320u, &got ) || got != (unsigned)n * 320u;
  else
    for ( int y = y0; y < y0 + n && !bad; y++ )
      bad = _dos_read( fd, vid_line_ptr( y ), vid_pitch, &got ) || got != vid_pitch;
  close( fd );
  return bad ? 3 : 0;
}
