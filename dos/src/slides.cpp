// SLIDES - stand-alone DeskMind slideshow (no network needed, any boot mode).
//
//   SLIDES [folder] [/D seconds] [/E effect] [/R] [/NOTITLE] [/ONCE]
//
//   folder     pictures (*.TPI); default: DeskMind's PICS folder from DESKMIND.CFG,
//              else the current folder
//   /D n       seconds per picture (default from DESKMIND.CFG, else 8)
//   /E n       transition: 0 random, 1 cut, 2 wipe right, 3 wipe down, 4 blinds,
//              5 interlace, 6 dissolve, 7 box out, 8 box in, 9 slide in
//   /R         random order
//   /NOTITLE   no title strip
//   /ONCE      stop after the last picture (default: loop)
//
// Keys: Space/Enter/Right next, Left back, P pause, T titles, E try effects, Esc quit.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dos.h>
#include <malloc.h>

#include "video.h"
#include "mouse.h"
#include "sys.h"
#include "cfg.h"
#include "tpi.h"
#include "slide.h"

struct Entry { char name[13]; unsigned long when; };
static Entry *s_e = 0;
static int s_n = 0;
static char s_dir[80];

static int newest_first( const void *a, const void *b ) {
  unsigned long x = ( (const Entry *)a )->when, y = ( (const Entry *)b )->when;
  return x < y ? 1 : x > y ? -1 : 0;
}

static int path_of( void *, int i, char *out ) {
  if ( i < 0 || i >= s_n ) return 0;
  sprintf( out, "%s\\%s", s_dir, s_e[i].name );
  return 1;
}

static void usage( void ) {
  printf( "SLIDES - DeskMind slideshow for the Tandy 1000 SL/TL/RL (640x200x16)\n\n"
          "  SLIDES [folder] [/D seconds] [/E effect] [/R] [/NOTITLE] [/ONCE] [/LIST]\n\n"
          "  /R: random order (or tick \"Slideshow: random order\" in DeskMind's Settings)\n"
          "  /LIST: print the play order and exit\n"
          "  /E: 0 random, 1 cut, 2 wipe right, 3 wipe down, 4 blinds, 5 interlace,\n"
          "      6 dissolve, 7 box out, 8 box in, 9 slide in\n\n"
          "  Keys: Space/Right next, Left back, P pause, T titles, E effects, Esc quit\n" );
}

int main( int argc, char *argv[] ) {
  // Settings: DESKMIND.CFG next to this program, if there is one
  char exeDir[80], cfgFile[80];
  str_copy( exeDir, argv[0], sizeof( exeDir ) );
  char *slash = strrchr( exeDir, '\\' );
  if ( slash ) slash[1] = 0; else exeDir[0] = 0;
  cfg_defaults( exeDir );
  cfg_path( cfgFile, exeDir );
  int haveCfg = cfg_load( cfgFile ) == 0;

  SlideOpts o;
  o.delay = cfg.slide_delay > 0 ? cfg.slide_delay : 8;
  o.effect = cfg.slide_effect;
  o.titles = 1;
  o.shuffle = cfg.slide_shuffle;       // Settings: "Slideshow: random order"
  o.loop = 1;
  int bench = 0, noMouse = 0, listOnly = 0;
  str_copy( s_dir, haveCfg ? cfg.pics : ".", sizeof( s_dir ) );

  for ( int i = 1; i < argc; i++ ) {
    char *a = argv[i];
    if ( str_ieq( a, "/?" ) || str_ieq( a, "/H" ) ) { usage( ); return 0; }
    else if ( str_ieq( a, "/D" ) && i + 1 < argc ) o.delay = atoi( argv[++i] );
    else if ( str_ieq( a, "/E" ) && i + 1 < argc ) o.effect = atoi( argv[++i] );
    else if ( str_ieq( a, "/R" ) ) o.shuffle = 1;
    else if ( str_ieq( a, "/NOTITLE" ) ) o.titles = 0;
    else if ( str_ieq( a, "/ONCE" ) ) o.loop = 0;
    else if ( str_ieq( a, "/BENCH" ) ) bench = 1;
    else if ( str_ieq( a, "/LIST" ) ) listOnly = 1;
    else if ( str_ieq( a, "/NOMOUSE" ) ) noMouse = 1;
    else if ( a[0] != '/' ) str_copy( s_dir, a, sizeof( s_dir ) );
    else { printf( "Unknown option %s\n\n", a ); usage( ); return 1; }
  }
  if ( o.delay < 1 ) o.delay = 1;
  if ( o.effect < 0 || o.effect >= FX_COUNT ) o.effect = FX_RANDOM;
  int len = (int)strlen( s_dir );
  if ( len > 1 && s_dir[ len - 1 ] == '\\' ) s_dir[ len - 1 ] = 0;

  // Find the pictures (640x200 ones), newest first
  s_e = (Entry *)malloc( sizeof( Entry ) * 500 );
  if ( !s_e ) { printf( "Out of memory\n" ); return 1; }
  struct find_t ff;
  char pattern[90];
  sprintf( pattern, "%s\\*.TPI", s_dir );
  unsigned rc = _dos_findfirst( pattern, _A_NORMAL, &ff );
  while ( rc == 0 && s_n < 500 ) {
    char p[100];
    TpiHeader h;
    sprintf( p, "%s\\%s", s_dir, ff.name );
    if ( tpi_header( p, &h ) == 0 && h.mode == 1 ) {
      str_copy( s_e[s_n].name, ff.name, sizeof( s_e[0].name ) );
      s_e[s_n].when = ( (unsigned long)h.year << 20 ) | ( (unsigned long)h.month << 16 ) |
                      ( (unsigned long)h.day << 11 ) | ( h.hour << 6 ) | h.minute;
      s_n++;
    }
    rc = _dos_findnext( &ff );
  }
  if ( !s_n ) { printf( "No DeskMind pictures (*.TPI) in %s\n", s_dir ); return 1; }
  qsort( s_e, s_n, sizeof( Entry ), newest_first );

  if ( listOnly ) {                   // the play order, without graphics (also checks /R)
    int *order = (int *)malloc( sizeof( int ) * s_n );
    if ( !order ) return 1;
    slide_make_order( order, s_n, -1, o.shuffle );
    printf( "%d pictures in %s, %s order:\n", s_n, s_dir, o.shuffle ? "random" : "newest first" );
    for ( int i = 0; i < s_n; i++ ) printf( "%3d  %s\n", i + 1, s_e[ order[i] ].name );
    free( order );
    return 0;
  }

  if ( vid_detect( ) != VT_SLTL ) { printf( "SLIDES needs a Tandy 1000 SL, TL or RL (640x200x16 video).\n" ); return 1; }
  int r = vid_reserve( VM_640 );
  if ( r ) { printf( "Cannot use 640x200 graphics: %s\n", vid_reserve_error( r ) ); return 1; }
  if ( !noMouse ) mouse_init( );      // only for "click to quit"; its pointer stays hidden
  vid_open( VM_640 );
  if ( bench ) {
    // Every transition once, alternating two pictures; times go to SLIDES.LOG
    static unsigned long ms[ FX_COUNT ];
    unsigned char far *buf = (unsigned char far *)_fmalloc( 64000u );
    char p[100];
    TpiHeader h;
    for ( int fx = FX_CUT; fx < FX_COUNT && buf; fx++ ) {
      path_of( 0, fx % s_n, p );
      if ( tpi_image( p, buf, &h ) ) continue;
      unsigned long t0 = ticks( );
      slide_transition( fx, buf );
      ms[fx] = ( ticks( ) - t0 ) * 55ul;
      wait_ticks( 18 );
    }
    vid_close( );
    vid_unreserve( );
    FILE *f = fopen( "SLIDES.LOG", "a" );
    for ( int fx = FX_CUT; fx < FX_COUNT; fx++ ) {
      printf( "%-12s %5lu ms\n", slide_effect_name( fx ), ms[fx] );
      if ( f ) fprintf( f, "%-12s %5lu ms\n", slide_effect_name( fx ), ms[fx] );
    }
    if ( f ) fclose( f );
    return 0;
  }
  int shown = slide_run( s_n, 0, path_of, 0, &o );
  vid_close( );
  vid_unreserve( );
  printf( "%d picture%s shown from %s.\n", shown, shown == 1 ? "" : "s", s_dir );
  return 0;
}
