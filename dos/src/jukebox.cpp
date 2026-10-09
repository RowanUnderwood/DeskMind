// DeskMind - the song library and what plays next.  See jukebox.h.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dos.h>
#include <conio.h>

#include "jukebox.h"
#include "music.h"
#include "cfg.h"
#include "sys.h"

static Song *s_s = 0;
static int s_n = 0, s_total = 0;
static int s_mode = JB_IDLE;
static char s_cur[9] = "";                  // file playing (indexes move when the list changes)
static int s_seeded = 0;

void jb_path( char *out, const char *file ) { sprintf( out, "%s\\%s.T3", cfg.music, file ); }

static void list_path( char *out ) { sprintf( out, "%s\\SONGS.LST", cfg.music ); }

static int cmp_newest( const void *a, const void *b ) {
  unsigned long x = ( (const Song *)a )->when, y = ( (const Song *)b )->when;
  return x < y ? 1 : x > y ? -1 : 0;
}

static unsigned long dos_when( unsigned d, unsigned t ) {
  return JB_WHEN( ( d >> 9 ) + 1980, ( d >> 5 ) & 15, d & 31, t >> 11, ( t >> 5 ) & 63 );
}

static unsigned digits( const char *s, int n ) {
  unsigned v = 0;
  while ( n-- ) v = v * 10 + ( *s++ - '0' );
  return v;
}

unsigned long jb_when_parse( const char *s ) {
  for ( int i = 0; i < 12; i++ ) if ( s[i] < '0' || s[i] > '9' ) return 0;
  return JB_WHEN( digits( s, 4 ), digits( s + 4, 2 ), digits( s + 6, 2 ), digits( s + 8, 2 ), digits( s + 10, 2 ) );
}

void jb_when_text( unsigned long w, char *out ) {
  sprintf( out, "%04u%02u%02u%02u%02u", (unsigned)( w >> 20 ), (unsigned)( w >> 16 ) & 15, (unsigned)( w >> 11 ) & 31,
           (unsigned)( w >> 6 ) & 31, (unsigned)w & 63 );
}

void jb_when_date( unsigned long w, char *out ) {
  sprintf( out, "%04u-%02u-%02u", (unsigned)( w >> 20 ), (unsigned)( w >> 16 ) & 15, (unsigned)( w >> 11 ) & 31 );
}

int jb_scan( void ) {
  if ( !s_s ) s_s = (Song *)malloc( sizeof( Song ) * JB_MAX );
  s_n = s_total = 0;
  if ( !s_s ) return 0;
  struct find_t ff;
  char pattern[80];
  sprintf( pattern, "%s\\*.T3", cfg.music );
  for ( unsigned rc = _dos_findfirst( pattern, _A_NORMAL, &ff ); rc == 0; rc = _dos_findnext( &ff ) ) {
    s_total++;
    if ( s_n >= JB_MAX ) continue;
    char path[80];
    sprintf( path, "%s\\%s", cfg.music, ff.name );
    Song &s = s_s[ s_n ];
    if ( music_probe( path, &s.secs ) != MUS_OK ) { s_total--; continue; }
    char *dot = strchr( ff.name, '.' );
    if ( dot ) *dot = 0;
    str_copy( s.file, ff.name, sizeof( s.file ) );
    str_copy( s.title, ff.name, sizeof( s.title ) );
    s.when = dos_when( ff.wr_date, ff.wr_time );
    s_n++;
  }
  // Titles and dates from SONGS.LST
  char lp[80];
  list_path( lp );
  FILE *f = fopen( lp, "r" );
  if ( f ) {
    char line[100];
    while ( fgets( line, sizeof( line ), f ) ) {
      char file[13], stamp[16], title[60] = "";
      unsigned secs;
      int k = 0;
      if ( sscanf( line, "%12s %15s %u %n", file, stamp, &secs, &k ) < 3 ) continue;
      unsigned long when = jb_when_parse( stamp );
      str_copy( title, line + k, sizeof( title ) );
      int e = (int)strlen( title );
      while ( e && ( title[e-1] == '\n' || title[e-1] == '\r' || title[e-1] == ' ' ) ) title[--e] = 0;
      int i = jb_find( file );
      if ( i < 0 ) continue;
      if ( title[0] ) str_copy( s_s[i].title, title, sizeof( s_s[i].title ) );
      if ( when ) s_s[i].when = when;
    }
    fclose( f );
  }
  qsort( s_s, s_n, sizeof( Song ), cmp_newest );
  return s_n;
}

static int save_list( void ) {
  char lp[80];
  list_path( lp );
  FILE *f = fopen( lp, "w" );
  if ( !f ) return 1;
  for ( int i = 0; i < s_n; i++ ) {
    char stamp[16];
    jb_when_text( s_s[i].when, stamp );
    fprintf( f, "%s %s %u %s\n", s_s[i].file, stamp, s_s[i].secs, s_s[i].title );
  }
  int bad = ferror( f );
  fclose( f );
  return bad;
}

int jb_count( void ) { return s_n; }
int jb_total( void ) { return s_total; }
const Song *jb_song( int i ) { return i >= 0 && i < s_n ? &s_s[i] : 0; }

int jb_find( const char *file ) {
  for ( int i = 0; i < s_n; i++ ) if ( str_ieq( s_s[i].file, file ) ) return i;
  return -1;
}

int jb_current( void ) { return s_mode == JB_IDLE || s_mode == JB_JINGLE ? -1 : jb_find( s_cur ); }
int jb_mode( void ) { return s_mode; }

static void seed( void ) {
  if ( s_seeded ) return;
  outp( 0x43, 0 );
  unsigned pit = inp( 0x40 ); pit |= inp( 0x40 ) << 8;
  srand( (unsigned)ticks( ) ^ pit );
  s_seeded = 1;
}

static int start( int i, int mode ) {
  const Song *s = jb_song( i );
  if ( !s ) return MUS_NO_FILE;
  char path[80];
  jb_path( path, s->file );
  int rc = music_play( path, mode == JB_LOOP );
  if ( rc ) { s_mode = JB_IDLE; s_cur[0] = 0; return rc; }
  s_mode = mode;
  str_copy( s_cur, s->file, sizeof( s_cur ) );
  str_copy( cfg.slide_song, s->file, sizeof( cfg.slide_song ) );   // "the song last played" for slideshows
  return MUS_OK;
}

int jb_play( int i, int mode ) { return start( i, mode ); }

static int pick_random( void ) {
  if ( s_n <= 0 ) return -1;
  seed( );
  int cur = jb_find( s_cur ), i;
  do i = rand( ) % s_n; while ( s_n > 1 && i == cur );
  return i;
}

int jb_random( void ) {
  int i = pick_random( );
  return i < 0 ? MUS_NO_FILE : start( i, JB_RANDOM );
}

int jb_jingle( const char *path ) {
  int rc = music_play( path, 0 );
  s_mode = rc ? JB_IDLE : JB_JINGLE;
  s_cur[0] = 0;
  return rc;
}

void jb_stop( void ) {
  music_stop( );
  s_mode = JB_IDLE;
  s_cur[0] = 0;
}

int jb_poll( void ) {
  if ( s_mode == JB_IDLE ) return 0;
  if ( !music_playing( ) ) { s_mode = JB_IDLE; s_cur[0] = 0; return 1; }   // stopped elsewhere
  if ( !music_poll( ) ) return 0;
  // The song ended (and is stopped): random mode goes on, the rest ends
  if ( s_mode == JB_RANDOM ) {
    int i = pick_random( );
    if ( i >= 0 && start( i, JB_RANDOM ) == MUS_OK ) return 1;
  }
  s_mode = JB_IDLE;
  s_cur[0] = 0;
  return 1;
}

const char *jb_now( void ) {
  static char t[64];
  t[0] = 0;
  if ( s_mode == JB_IDLE || s_mode == JB_JINGLE || !music_playing( ) ) return t;
  int i = jb_find( s_cur );
  unsigned p = music_position( ), l = music_length( );
  sprintf( t, "%.36s  %u:%02u / %u:%02u", i >= 0 ? s_s[i].title : s_cur, p / 60, p % 60, l / 60, l % 60 );
  return t;
}

int jb_add( const char *file, const char *title, unsigned long when ) {
  jb_scan( );
  int i = jb_find( file );
  if ( i < 0 ) return 1;
  str_copy( s_s[i].title, title, sizeof( s_s[i].title ) );
  if ( when ) s_s[i].when = when;
  qsort( s_s, s_n, sizeof( Song ), cmp_newest );
  return save_list( );
}

int jb_rename( int i, const char *title ) {
  if ( i < 0 || i >= s_n ) return 1;
  str_copy( s_s[i].title, title, sizeof( s_s[i].title ) );
  return save_list( );
}

int jb_delete( int i ) {
  if ( i < 0 || i >= s_n ) return 1;
  if ( str_ieq( s_s[i].file, s_cur ) ) jb_stop( );
  char path[80];
  jb_path( path, s_s[i].file );
  if ( remove( path ) ) return 1;
  memmove( &s_s[i], &s_s[i+1], sizeof( Song ) * ( s_n - i - 1 ) );
  s_n--;
  s_total--;
  save_list( );
  return 0;
}

int jb_slides_begin( void ) {
  if ( !cfg.slide_music || music_playing( ) || !music_available( ) ) return 0;
  if ( !s_s ) jb_scan( );
  if ( cfg.slide_music == 2 && cfg.slide_song[0] ) {
    int i = jb_find( cfg.slide_song );
    if ( i >= 0 ) return start( i, JB_LOOP ) == MUS_OK;
  }
  return jb_random( ) == MUS_OK;               // 1, or 2 with the song missing
}

void jb_slides_end( int started ) {
  if ( started ) jb_stop( );
}
