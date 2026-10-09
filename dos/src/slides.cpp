// SLIDES - stand-alone DeskMind slideshow (no network needed, any boot mode).
//
//   SLIDES [folder] [/D seconds] [/E effect] [/R] [/NOTITLE] [/ONCE] [/CGA]
//
//   folder     pictures (*.TPI); default: DeskMind's PICS folder from DESKMIND.CFG
//              (PICSCGA in CGA mode), else the current folder
//   /CGA       CGA pictures on any PC (automatic without Tandy Video II)
//   /D n       seconds per picture (default from DESKMIND.CFG, else 8)
//   /E n       transition: 0 random, 1 cut, 2 wipe right, 3 wipe down, 4 blinds,
//              5 interlace, 6 dissolve, 7 box out, 8 box in, 9 slide in
//   /R         random order
//   /NOTITLE   no title strip
//   /ONCE      stop after the last picture (default: loop)
//   /M x       music: R = random songs, OFF = none, or a song (a name in DeskMind's MUSIC
//              folder, or a path to a .T3 file) played in a loop.  Default: DeskMind's setting
//   /NOEMS     keep the picture buffer out of EMS (it uses 63K of DOS memory then)
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
#include "music.h"
#include "jukebox.h"

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

static void poll_music( void ) { jb_poll( ); }

static void usage( void ) {
  printf( "SLIDES - DeskMind slideshow for the Tandy 1000 SL/TL/RL (640x200x16) and CGA PCs\n\n"
          "  SLIDES [folder] [/D seconds] [/E effect] [/R] [/NOTITLE] [/ONCE] [/LIST] [/CGA] [/NOEMS]\n"
          "         [/M R|OFF|song]\n\n"
          "  /CGA: CGA pictures (automatic on a PC without Tandy Video II)\n"
          "  /R: random order (or tick \"Slideshow: random order\" in DeskMind's Settings)\n"
          "  /LIST: print the play order and exit\n"
          "  /M: music: R random songs, OFF none, or a song (name in the MUSIC folder or a .T3 path)\n"
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
  // CGA first: it decides the default folder and which pictures count
  int forceCga = 0;
  for ( int i = 1; i < argc; i++ ) if ( str_ieq( argv[i], "/CGA" ) ) forceCga = 1;
  cfg_cga = forceCga || vid_detect( ) != VT_SLTL;
  tpi_cga = cfg_cga;

  SlideOpts o;
  o.delay = cfg.slide_delay > 0 ? cfg.slide_delay : 8;
  o.effect = cfg.slide_effect;
  o.titles = 1;
  o.shuffle = cfg.slide_shuffle;       // Settings: "Slideshow: random order"
  o.loop = 1;
  o.poll = poll_music;
  int bench = 0, noMouse = 0, listOnly = 0;
  const char *song = 0;                // /M with a song name or path
  str_copy( s_dir, haveCfg ? cfg_pics( ) : ".", sizeof( s_dir ) );

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
    else if ( str_ieq( a, "/NOEMS" ) ) slide_no_ems = 1;
    else if ( str_ieq( a, "/M" ) && i + 1 < argc ) {
      a = argv[++i];
      if ( str_ieq( a, "OFF" ) ) cfg.slide_music = 0;
      else if ( str_ieq( a, "R" ) ) cfg.slide_music = 1;
      else song = a;
    }
    else if ( str_ieq( a, "/CGA" ) ) ;
    else if ( a[0] != '/' ) str_copy( s_dir, a, sizeof( s_dir ) );
    else { printf( "Unknown option %s\n\n", a ); usage( ); return 1; }
  }
  if ( o.delay < 1 ) o.delay = 1;
  if ( o.effect < 0 || o.effect >= FX_COUNT ) o.effect = FX_RANDOM;
  int len = (int)strlen( s_dir );
  if ( len > 1 && s_dir[ len - 1 ] == '\\' ) s_dir[ len - 1 ] = 0;

  // Find the pictures (640x200x16 ones, or CGA ones in CGA mode), newest first
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
    if ( tpi_header( p, &h ) == 0 && h.mode != 2 ) {
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
    printf( "%d pictures in %s, %s order:\n", s_n, s_dir, o.shuffle ? "random" : "oldest first" );
    for ( int i = 0; i < s_n; i++ ) printf( "%3d  %s\n", i + 1, s_e[ order[i] ].name );
    free( order );
    return 0;
  }

  if ( !slide_no_ems ) ems_init( );   // the picture buffer goes into EMS when there is some
  int gmode = cfg_cga ? VM_CGA2 : VM_640;
  int r = vid_reserve( gmode );       // nothing to reserve for CGA
  if ( r ) { printf( "Cannot use 640x200 graphics: %s\n", vid_reserve_error( r ) ); return 1; }
  if ( !noMouse ) mouse_init( );      // only for "click to quit"; its pointer stays hidden
  vid_open( gmode );
  if ( bench ) {
    // Every transition once, alternating two pictures; times go to SLIDES.LOG
    static unsigned long ms[ FX_COUNT ];
    unsigned char far *buf = (unsigned char far *)_fmalloc( 64000u );
    char p[100];
    TpiHeader h;
    for ( int fx = FX_CUT; fx < FX_COUNT && buf; fx++ ) {
      path_of( 0, fx % s_n, p );
      if ( tpi_image( p, buf, &h ) ) continue;
      tpi_set_mode( &h );             // CGA: the picture's mode and palette
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
  // Music: a song given with /M loops; otherwise DeskMind's setting (random songs, or the
  // song last played in its Music screen)
  int music = 0, mrc = MUS_OK;
  if ( song ) {
    char path[80];
    if ( strchr( song, '\\' ) || strchr( song, '.' ) ) str_copy( path, song, sizeof( path ) );
    else jb_path( path, song );
    music = ( mrc = music_play( path, 1 ) ) == MUS_OK;
  }
  else music = jb_slides_begin( );
  int shown = slide_run( s_n, -1, path_of, 0, &o );   // -1: from the oldest (or random)
  if ( music ) jb_stop( );
  vid_close( );
  vid_unreserve( );
  if ( mrc ) printf( "No music: %s (%s).\n", music_error( mrc ), song );
  if ( !shown ) printf( "Not enough memory for the picture buffer (slideshow %s).\n", slide_mem_note( ) );
  printf( "%d picture%s shown from %s.\n", shown, shown == 1 ? "" : "s", s_dir );
  return 0;
}
