// DeskMind - keyboard, clock, EMS and small helpers.

#ifndef SYS_H
#define SYS_H

// ---------------------------------------------------------------- keyboard
// kbd_get() returns 0 (no key), an ASCII code, or one of these:
#define K_ENTER   13
#define K_ESC     27
#define K_BS       8
#define K_TAB      9
#define K_UP      0x148
#define K_DOWN    0x150
#define K_LEFT    0x14B
#define K_RIGHT   0x14D
#define K_HOME    0x147
#define K_END     0x14F
#define K_PGUP    0x149
#define K_PGDN    0x151
#define K_INS     0x152
#define K_DEL     0x153
#define K_SHTAB   0x10F
#define K_F1      0x13B
#define K_F2      0x13C
#define K_F3      0x13D
#define K_F4      0x13E
#define K_F5      0x13F
#define K_F6      0x140
#define K_F7      0x141
#define K_F8      0x142
#define K_F9      0x143
#define K_F10     0x144
#define K_ALT(c)  ( 0x200 | (c) )          // Alt+letter, c = 'A'..'Z'

int  kbd_get( void );
int  kbd_shift( void );                    // 1 if a Shift key is down

// ---------------------------------------------------------------- clock
unsigned long ticks( void );               // BIOS ticks, 18.2 per second
void wait_ticks( unsigned n );

// ---------------------------------------------------------------- EMS
// Expanded memory in 16K pages through the page frame (PMEMM on the Tandy).
int  ems_init( void );                     // number of free pages, 0 = no EMS
int  ems_alloc( unsigned pages );          // handle, or -1
void ems_free( int handle );
// Copy between a far buffer and an EMS handle at a byte offset (any size)
int  ems_write( int handle, unsigned long offset, const void far *src, unsigned len );
int  ems_read( int handle, unsigned long offset, void far *dst, unsigned len );
// Maps logical pages 0..pages-1 (at most 4) to the whole page frame; returns the frame, or 0
unsigned char far *ems_frame_map( int handle, unsigned pages );

// ---------------------------------------------------------------- strings
void str_copy( char *dst, const char *src, unsigned size );   // always terminates
int  str_ieq( const char *a, const char *b );

#endif
