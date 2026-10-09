// DeskMind - keyboard, clock, EMS and small helpers.  See sys.h.

#include <dos.h>
#include <i86.h>
#include <string.h>
#include <bios.h>

#include "sys.h"

// ---------------------------------------------------------------- keyboard

// Scan codes of Alt+A..Z (0x1E = A ...), in alphabet order
static const unsigned char s_altScan[26] = {
  0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
  0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C
};

int kbd_get( void ) {
  if ( !_bios_keybrd( _KEYBRD_READY ) ) return 0;
  unsigned k = _bios_keybrd( _KEYBRD_READ );
  unsigned char ascii = k & 0xFF, scan = k >> 8;
  if ( ascii == 0 || ascii == 0xE0 ) {
    for ( int i = 0; i < 26; i++ ) if ( s_altScan[i] == scan ) return K_ALT( 'A' + i );
    return 0x100 | scan;
  }
  return ascii;
}

int kbd_shift( void ) {
  return ( _bios_keybrd( _KEYBRD_SHIFTSTATUS ) & 3 ) != 0;
}

// ---------------------------------------------------------------- clock

unsigned long ticks( void ) {
  return *(volatile unsigned long far *)MK_FP( 0x40, 0x6C );
}

void wait_ticks( unsigned n ) {
  unsigned long t = ticks( );
  while ( ticks( ) - t < n ) ;
}

// ---------------------------------------------------------------- EMS

static unsigned s_frame = 0;

int ems_init( void ) {
  // An EMM driver's device header is named "EMMXXXX0"
  unsigned long far *ivt = (unsigned long far *)MK_FP( 0, 0 );
  unsigned seg = (unsigned)( ivt[ 0x67 ] >> 16 );
  if ( !seg ) return 0;
  if ( _fmemcmp( MK_FP( seg, 10 ), "EMMXXXX0", 8 ) != 0 ) return 0;
  union REGS r;
  r.h.ah = 0x40; int86( 0x67, &r, &r );            // status
  if ( r.h.ah ) return 0;
  r.h.ah = 0x41; int86( 0x67, &r, &r );            // page frame
  if ( r.h.ah ) return 0;
  s_frame = r.w.bx;
  r.h.ah = 0x42; int86( 0x67, &r, &r );            // free pages
  if ( r.h.ah ) return 0;
  return r.w.bx;
}

int ems_alloc( unsigned pages ) {
  if ( !s_frame ) return -1;
  union REGS r;
  r.h.ah = 0x43; r.w.bx = pages;
  int86( 0x67, &r, &r );
  return r.h.ah ? -1 : (int)r.w.dx;
}

void ems_free( int handle ) {
  if ( handle < 0 ) return;
  union REGS r;
  r.h.ah = 0x45; r.w.dx = (unsigned)handle;
  int86( 0x67, &r, &r );
}

static int ems_map( int handle, unsigned logical ) {
  union REGS r;
  r.h.ah = 0x44; r.h.al = 0; r.w.bx = logical; r.w.dx = (unsigned)handle;
  int86( 0x67, &r, &r );
  return r.h.ah ? -1 : 0;
}

static int ems_copy( int handle, unsigned long offset, void far *buf, unsigned len, int toEms ) {
  unsigned char far *b = (unsigned char far *)buf;
  while ( len ) {
    unsigned page = (unsigned)( offset >> 14 ), in = (unsigned)( offset & 0x3FFF );
    unsigned n = 0x4000 - in;
    if ( n > len ) n = len;
    if ( ems_map( handle, page ) ) return -1;
    unsigned char far *win = (unsigned char far *)MK_FP( s_frame, in );
    if ( toEms ) _fmemcpy( win, b, n ); else _fmemcpy( b, win, n );
    b += n; offset += n; len -= n;
  }
  return 0;
}

int ems_write( int handle, unsigned long offset, const void far *src, unsigned len ) {
  return ems_copy( handle, offset, (void far *)src, len, 1 );
}

int ems_read( int handle, unsigned long offset, void far *dst, unsigned len ) {
  return ems_copy( handle, offset, dst, len, 0 );
}

unsigned char far *ems_frame_map( int handle, unsigned pages ) {
  if ( !s_frame || pages > 4 ) return 0;
  for ( unsigned i = 0; i < pages; i++ ) {
    union REGS r;
    r.h.ah = 0x44; r.h.al = (unsigned char)i; r.w.bx = i; r.w.dx = (unsigned)handle;
    int86( 0x67, &r, &r );
    if ( r.h.ah ) return 0;
  }
  return (unsigned char far *)MK_FP( s_frame, 0 );
}

// ---------------------------------------------------------------- strings

void str_copy( char *dst, const char *src, unsigned size ) {
  if ( !size ) return;
  strncpy( dst, src, size - 1 );
  dst[ size - 1 ] = 0;
}

int str_ieq( const char *a, const char *b ) {
  return stricmp( a, b ) == 0;
}
