// DeskMind - Tandy 3-voice sound, or the PC speaker elsewhere.  See sound.h.

#include <stdio.h>
#include <dos.h>
#include <conio.h>
#include <i86.h>

#include "sound.h"
#include "video.h"

int snd_enabled = 1;
int snd_mode = 2;

static void (__interrupt __far *s_oldTick)( void ) = 0;
static const SndNote * volatile s_seq = 0;
static volatile unsigned char s_left = 0;
static volatile unsigned char s_vol = 15;
static unsigned char s_old61 = 0;
static unsigned s_start = 0, s_len = 0;    // snd_poll's watchdog: BIOS tick at start, ticks allowed
volatile unsigned long snd_isr_count = 0;

// ---------------------------------------------------------------- diagnostics ring
// The stuck-note hunt (0.8.x): what happened when, in BIOS ticks and tick-handler calls.
// Written from the tick handler and the main loop, so main-loop writes disable interrupts.
struct SndEv { unsigned tick, isr; unsigned char code; int arg; unsigned x; };
#define RING 64
static SndEv s_ring[ RING ];
static unsigned s_ringN = 0;               // events ever logged (the ring keeps the last RING)

// BIOS tick counter (INT 08h keeps it going even when the INT 1Ch chain stalls)
static unsigned bios_ticks( void ) { return *(volatile unsigned far *)MK_FP( 0x40, 0x6C ); }

static void ring_put( int code, int arg, unsigned x ) {
  SndEv &e = s_ring[ s_ringN % RING ];
  e.tick = bios_ticks( );
  e.isr = (unsigned)snd_isr_count;
  e.code = (unsigned char)code;
  e.arg = arg;
  e.x = x;
  s_ringN++;
}

// Interrupts off, returning the old flags; and back as they were (the caller may run with them off)
extern unsigned irq_off( void );
#pragma aux irq_off = "pushf" "pop ax" "cli" value [ax];
extern void irq_restore( unsigned flags );
#pragma aux irq_restore = "push ax" "popf" parm [ax];

void snd_log( int code, int arg ) {
  unsigned flags = irq_off( );
  ring_put( code, arg, 0 );
  irq_restore( flags );
}

extern const SndNote SND_STARTUP[], SND_REPLY[], SND_IMAGE[], SND_ERROR[], SND_CLICK[];

static int effect_index( const SndNote *seq ) {
  if ( seq == SND_STARTUP ) return 1;
  if ( seq == SND_REPLY ) return 2;
  if ( seq == SND_IMAGE ) return 3;
  if ( seq == SND_ERROR ) return 4;
  if ( seq == SND_CLICK ) return 5;
  return 0;
}

// The SN76496 needs about 32 of its 3.58 MHz clocks (9 us) to take a byte.  Back-to-back
// writes left notes ringing at full volume on the real TL/3 now and then (a lost latch
// byte sends the next data byte to the wrong register), so each write is followed by a
// pause of about 12 us: ISA port reads, which take the same time on any CPU speed.
static void sn_write( unsigned char b ) {
  outp( 0xC0, b );
  if ( snd_mode >= 1 ) for ( int i = 0; i < 12; i++ ) inp( 0x61 );
}

static void sn_tone( int ch, unsigned hz ) {
  unsigned n = hz ? (unsigned)( 111861ul / hz ) : 0;
  if ( n > 1023 ) n = 1023;
  sn_write( 0x80 | ( ch << 5 ) | ( n & 0x0F ) );
  sn_write( ( n >> 4 ) & 0x3F );
}

static void sn_vol( int ch, unsigned char att ) {
  sn_write( 0x90 | ( ch << 5 ) | ( att & 0x0F ) );
}

static void sn_silence( void ) {
  for ( int ch = 0; ch < 4; ch++ ) sn_vol( ch, 15 );
}

// PC speaker (any PC that is not a Tandy): one voice, no volume, so a note stops
// once its decay passes SPK_CUT.  Port C0h must never be written there: on an AT
// it is the second DMA controller.
#define SPK_CUT 9
static int s_spk = 0;                       // 1 = PC speaker instead of the Tandy chip
static unsigned s_spkHz = 0, s_spkOn = 0;

static void spk( unsigned hz ) {
  if ( hz == s_spkOn ) return;              // reprogramming the same tone would click
  s_spkOn = hz;
  if ( !hz ) { outp( 0x61, inp( 0x61 ) & ~3 ); return; }
  unsigned d = (unsigned)( 1193182ul / hz );
  outp( 0x43, 0xB6 );
  outp( 0x42, d & 0xFF );
  outp( 0x42, d >> 8 );
  outp( 0x61, inp( 0x61 ) | 3 );
}

static void v_tone( int ch, unsigned hz ) {
  if ( !s_spk ) sn_tone( ch, hz );
  else if ( ch == 0 ) s_spkHz = hz;
}

static void v_vol( int ch, unsigned char att ) {
  if ( !s_spk ) sn_vol( ch, att );
  else if ( ch == 0 ) spk( att < SPK_CUT ? s_spkHz : 0 );
}

static void v_silence( void ) {
  if ( s_spk ) spk( 0 ); else sn_silence( );
}

static void start_note( const SndNote *n ) {
  s_left = n->ticks;
  s_vol = n->vol;
  if ( n->hz1 ) { v_tone( 0, n->hz1 ); v_vol( 0, s_vol ); } else v_vol( 0, 15 );
  if ( n->hz2 ) { v_tone( 1, n->hz2 ); v_vol( 1, s_vol + 2 > 15 ? 15 : s_vol + 2 ); } else v_vol( 1, 15 );
}

// Every tick, write the whole volume state again: all four voices, silent ones at 15.
// Rewriting an unchanged volume makes no sound, and a lost or garbled write then lasts
// one tick (55 ms) instead of until the next sound.
static void refresh( void ) {
  const SndNote *n = s_seq;
  unsigned char v0 = n && n->hz1 ? s_vol : 15;
  unsigned char v1 = n && n->hz2 ? ( s_vol + 2 > 15 ? 15 : s_vol + 2 ) : 15;
  if ( s_spk ) {
    if ( n ) spk( v0 < SPK_CUT ? s_spkHz : 0 );                     // the decay cut, as v_vol does
    else { s_spkOn = 0; outp( 0x61, inp( 0x61 ) & ~3 ); }           // speaker gate off when idle
    return;
  }
  sn_vol( 0, v0 );
  sn_vol( 1, v1 );
  sn_vol( 2, 15 );
  sn_vol( 3, 15 );
}

// THIS FILE IS COMPILED WITH -zu (see the makefile).  The tick handler interrupts whatever
// runs, often DOS, the disk BIOS or the packet driver, whose SS is not our DGROUP.  Without
// -zu the compiler reaches DGROUP variables through SS in the helpers below, so a tick inside
// foreign code read and wrote someone else's memory: v_silence() read a garbage s_spk and
// "silenced" the PC speaker instead of the chip (the stuck notes, since 0.8 added the
// speaker path), and the 86Box Tandy crashed ("Divide overflow") during disk writes.
static void __interrupt __far tick_handler( void ) {
  snd_isr_count++;
  if ( s_seq ) {
    if ( s_left ) s_left--;
    if ( s_left == 0 ) {
      s_seq++;
      if ( s_seq->ticks == 0 ) { s_seq = 0; v_silence( ); ring_put( SL_END, 0, 0 ); }
      else start_note( s_seq );
    }
    else if ( s_vol < 13 ) {
      s_vol += 2;                   // simple decay
      if ( snd_mode < 2 ) {
        if ( s_seq->hz1 ) v_vol( 0, s_vol );
        if ( s_seq->hz2 ) v_vol( 1, s_vol + 2 > 15 ? 15 : s_vol + 2 );
      }
    }
  }
  if ( snd_mode >= 2 ) refresh( );
  _chain_intr( s_oldTick );
}

void snd_init( void ) {
  s_spk = vid_detect( ) == VT_OTHER;
  s_old61 = inp( 0x61 );
  if ( !s_spk ) outp( 0x61, s_old61 | 0x60 );   // route the 3-voice chip to the speaker (Tandy multiplexer bits)
  v_silence( );
  if ( !s_oldTick ) {
    s_oldTick = _dos_getvect( 0x1C );
    _dos_setvect( 0x1C, tick_handler );
  }
}

void snd_done( void ) {
  s_seq = 0;
  v_silence( );
  if ( s_oldTick ) {
    _dos_setvect( 0x1C, s_oldTick );
    s_oldTick = 0;
  }
  outp( 0x61, s_old61 & ~3 );               // speaker gate off either way
}

void snd_play( const SndNote *seq ) {
  if ( !snd_enabled || !seq || !s_oldTick ) return;
  unsigned len = 3;
  for ( const SndNote *n = seq; n->ticks; n++ ) len += n->ticks;
  _disable( );
  s_seq = seq;
  start_note( seq );
  s_start = bios_ticks( );
  s_len = len;
  ring_put( SL_PLAY, effect_index( seq ), 0 );
  _enable( );
}

// Main-loop safety nets, both logged:
// - a sequence still running past its length means the tick handler missed its end (the
//   real TL/3 once held the chime's last note after a Sync): silence it;
// - BIOS ticks moving while our tick handler isn't called means the INT 1Ch chain is
//   bypassed or stalled: log the vector, the PIC mask and port 61h, and silence the chip.
static unsigned s_pollTick = 0, s_pollIsr = 0;
static int s_stalled = 0;

void snd_poll( void ) {
  unsigned t = bios_ticks( ), i = (unsigned)snd_isr_count;
  if ( i != s_pollIsr ) { s_pollIsr = i; s_pollTick = t; s_stalled = 0; }
  else if ( !s_stalled && (unsigned)( t - s_pollTick ) >= 3 && s_oldTick ) {
    s_stalled = 1;
    void (__interrupt __far *v)( void ) = _dos_getvect( 0x1C );
    unsigned flags = irq_off( );
    ring_put( SL_STALL, ( inp( 0x61 ) << 8 ) | inp( 0x21 ), FP_SEG( v ) );
    s_seq = 0;
    v_silence( );
    irq_restore( flags );
  }
  if ( s_seq && (unsigned)( t - s_start ) > s_len ) { snd_log( SL_WATCHDOG, 0 ); snd_stop( ); }
}

// Waits until the current sound has ended (at most its length, by BIOS ticks).  Called before
// disk work: the real TL/3 sometimes held a note through a folder scan.
void snd_settle( void ) {
  if ( !s_seq ) return;
  snd_log( SL_SETTLE, 0 );
  while ( s_seq && (unsigned)( bios_ticks( ) - s_start ) <= s_len ) ;
  if ( s_seq ) { snd_log( SL_WATCHDOG, 1 ); snd_stop( ); }
}

// F9 steps (DeskMind's sound check): each one silences a different possible source
void snd_force( int step ) {
  unsigned flags = irq_off( );
  ring_put( SL_USER, step, 0 );
  if ( step == 2 ) { s_seq = 0; if ( s_spk ) { s_spkOn = 0; outp( 0x61, inp( 0x61 ) & ~3 ); } else sn_silence( ); }
  if ( step == 3 ) outp( 0x61, inp( 0x61 ) & ~3 );                 // speaker gate (PIT channel 2) off
  if ( step == 4 ) {
    outp( 0x43, 0xB0 ); outp( 0x42, 1 ); outp( 0x42, 0 );          // PIT channel 2: mode 0, ends low
    outp( 0x61, ( inp( 0x61 ) & ~3 ) | ( s_spk ? 0 : 0x60 ) );     // multiplexer back to the 3-voice chip
    if ( !s_spk ) sn_silence( );
  }
  irq_restore( flags );
}

static const char *const s_names[] = { "?", "startup", "reply", "image", "error", "click" };
static const char *const s_codes[] = { "", "PLAY", "END", "STOP", "WATCHDOG", "NET", "STALL", "F9",
                                       "DISK+", "DISK-", "SETTLE" };
static const char *const s_disks[] = { "rescan", "delete", "chat load", "chat save", "download", "chat list" };

void snd_diag( FILE *f ) {
  void (__interrupt __far *v1c)( void ) = _dos_getvect( 0x1C );
  void (__interrupt __far *v08)( void ) = _dos_getvect( 0x08 );
  static SndEv copy[ RING ];
  unsigned flags = irq_off( );
  unsigned long isr = snd_isr_count;
  unsigned t = bios_ticks( ), n = s_ringN;
  int busy = s_seq != 0, left = s_left, vol = s_vol;
  unsigned char p21 = inp( 0x21 ), p61 = inp( 0x61 );
  for ( int k = 0; k < RING; k++ ) copy[k] = s_ring[k];
  irq_restore( flags );
  fprintf( f, "tick handler calls %lu   BIOS tick %u   playing %d (left %d, vol %d)   mode %d   %s\n",
           isr, t, busy, left, vol, snd_mode, s_spk ? "PC speaker" : "Tandy chip" );
  fprintf( f, "INT 1Ch %04X:%04X (%s)   INT 08h %04X:%04X   PIC mask %02X   port 61h %02X\n",
           FP_SEG( v1c ), FP_OFF( v1c ), v1c == tick_handler ? "ours" : "NOT ours",
           FP_SEG( v08 ), FP_OFF( v08 ), p21, p61 );
  fprintf( f, "Last events (BIOS tick, ticks since the previous event, tick handler calls):\n" );
  unsigned first = n > RING ? n - RING : 0, prev = 0;
  for ( unsigned k = first; k < n; k++ ) {
    const SndEv &e = copy[ k % RING ];
    unsigned d = k == first ? 0 : (unsigned)( e.tick - prev );
    prev = e.tick;
    fprintf( f, "  %5u +%-4u isr %5u  %-8s ", e.tick, d, e.isr, e.code < 11 ? s_codes[ e.code ] : "?" );
    switch ( e.code ) {
      case SL_PLAY:  fprintf( f, "%s", s_names[ e.arg >= 0 && e.arg < 6 ? e.arg : 0 ] ); break;
      case SL_NET:   fprintf( f, "%s", e.arg ? "open" : "close" ); break;
      case SL_STALL: fprintf( f, "port 61h %02X, PIC mask %02X, INT 1Ch segment %04X", e.arg >> 8, e.arg & 0xFF, e.x ); break;
      case SL_DISK_BEGIN: case SL_DISK_END: fprintf( f, "%s", e.arg >= 0 && e.arg < 6 ? s_disks[ e.arg ] : "?" ); break;
      case SL_USER:  fprintf( f, "step %d", e.arg ); break;
      case SL_WATCHDOG: fprintf( f, "%s", e.arg ? "(settle)" : "" ); break;
    }
    fprintf( f, "\n" );
  }
}

void snd_stop( void ) {
  _disable( );
  if ( s_seq ) ring_put( SL_STOP, 0, 0 );
  s_seq = 0;
  v_silence( );
  _enable( );
}

int snd_busy( void ) { return s_seq != 0; }


// ---------------------------------------------------------------- effects
// Kept short and quiet on purpose ("subtle").

const SndNote SND_STARTUP[] = {
  {  523,    0, 2, 4 }, {  659,    0, 2, 4 }, {  784,    0, 2, 4 },
  { 1047,  523, 5, 3 }, { 0, 0, 0, 0 }
};
const SndNote SND_REPLY[] = {
  { 1319,    0, 1, 6 }, { 1760,    0, 2, 7 }, { 0, 0, 0, 0 }
};
const SndNote SND_IMAGE[] = {
  {  784,    0, 1, 5 }, {  988,    0, 1, 5 }, { 1175,    0, 1, 5 },
  { 1568,  784, 4, 4 }, { 0, 0, 0, 0 }
};
const SndNote SND_ERROR[] = {
  {  330,  311, 3, 4 }, {  220,  208, 5, 4 }, { 0, 0, 0, 0 }
};
const SndNote SND_CLICK[] = {
  { 2093,    0, 1, 9 }, { 0, 0, 0, 0 }
};
