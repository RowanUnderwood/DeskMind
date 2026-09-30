// DeskMind - Tandy 3-voice sound.  See sound.h.

#include <dos.h>
#include <conio.h>
#include <i86.h>

#include "sound.h"

int snd_enabled = 1;

static void (__interrupt __far *s_oldTick)( void ) = 0;
static const SndNote * volatile s_seq = 0;
static volatile unsigned char s_left = 0;
static volatile unsigned char s_vol = 15;
static unsigned char s_old61 = 0;

static void sn_write( unsigned char b ) { outp( 0xC0, b ); }

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

static void start_note( const SndNote *n ) {
  s_left = n->ticks;
  s_vol = n->vol;
  if ( n->hz1 ) { sn_tone( 0, n->hz1 ); sn_vol( 0, s_vol ); } else sn_vol( 0, 15 );
  if ( n->hz2 ) { sn_tone( 1, n->hz2 ); sn_vol( 1, s_vol + 2 > 15 ? 15 : s_vol + 2 ); } else sn_vol( 1, 15 );
}

static void __interrupt __far tick_handler( void ) {
  if ( s_seq ) {
    if ( s_left ) s_left--;
    if ( s_left == 0 ) {
      s_seq++;
      if ( s_seq->ticks == 0 ) { s_seq = 0; sn_silence( ); }
      else start_note( s_seq );
    }
    else if ( s_vol < 13 ) {
      s_vol += 2;                   // simple decay
      if ( s_seq->hz1 ) sn_vol( 0, s_vol );
      if ( s_seq->hz2 ) sn_vol( 1, s_vol + 2 > 15 ? 15 : s_vol + 2 );
    }
  }
  _chain_intr( s_oldTick );
}

void snd_init( void ) {
  // Route the 3-voice chip to the speaker (Tandy sound multiplexer bits)
  s_old61 = inp( 0x61 );
  outp( 0x61, s_old61 | 0x60 );
  sn_silence( );
  if ( !s_oldTick ) {
    s_oldTick = _dos_getvect( 0x1C );
    _dos_setvect( 0x1C, tick_handler );
  }
}

void snd_done( void ) {
  s_seq = 0;
  sn_silence( );
  if ( s_oldTick ) {
    _dos_setvect( 0x1C, s_oldTick );
    s_oldTick = 0;
  }
  outp( 0x61, s_old61 );
}

void snd_play( const SndNote *seq ) {
  if ( !snd_enabled || !seq || !s_oldTick ) return;
  _disable( );
  s_seq = seq;
  start_note( seq );
  _enable( );
}

void snd_stop( void ) {
  _disable( );
  s_seq = 0;
  sn_silence( );
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
