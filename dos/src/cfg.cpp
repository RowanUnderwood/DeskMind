// DeskMind - settings file.  See cfg.h.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "cfg.h"
#include "sys.h"

DmConfig cfg;

void cfg_defaults( const char *exeDir ) {
  memset( &cfg, 0, sizeof( cfg ) );
  str_copy( cfg.server, "192.168.2.192", sizeof( cfg.server ) );
  cfg.port = 8286;
  cfg.sound = 1;
  cfg.mode = 640;
  cfg.enhance = 1;
  cfg.review = 1;
  cfg.draw_enhance = -1;
  cfg.slide_delay = 8;
  cfg.slide_effect = 0;
  sprintf( cfg.pics, "%sPICS", exeDir );
  sprintf( cfg.pics_cga, "%sPICSCGA", exeDir );
  sprintf( cfg.chats, "%sCHATS", exeDir );
  sprintf( cfg.music, "%sMUSIC", exeDir );
}

int cfg_cga = 0;

const char *cfg_pics( void ) { return cfg_cga ? cfg.pics_cga : cfg.pics; }
const char *cfg_pics_other( void ) { return cfg_cga ? cfg.pics : cfg.pics_cga; }

void cfg_path( char *out, const char *exeDir ) {
  sprintf( out, "%sDESKMIND.CFG", exeDir );
}

static void trim( char *s ) {
  char *e = s + strlen( s );
  while ( e > s && isspace( (unsigned char)e[-1] ) ) *--e = 0;
  char *b = s;
  while ( *b && isspace( (unsigned char)*b ) ) b++;
  if ( b != s ) memmove( s, b, strlen( b ) + 1 );
}

int cfg_load( const char *path ) {
  FILE *f = fopen( path, "r" );
  if ( !f ) return 1;
  char line[160];
  while ( fgets( line, sizeof( line ), f ) ) {
    char *eq = strchr( line, '=' );
    if ( !eq || line[0] == ';' || line[0] == '#' ) continue;
    *eq = 0;
    char *key = line, *val = eq + 1;
    trim( key ); trim( val );
    int n = atoi( val );
    if      ( str_ieq( key, "server" ) )       str_copy( cfg.server, val, sizeof( cfg.server ) );
    else if ( str_ieq( key, "port" ) )         cfg.port = (unsigned)n;
    else if ( str_ieq( key, "token" ) )        str_copy( cfg.token, val, sizeof( cfg.token ) );
    else if ( str_ieq( key, "sound" ) )        cfg.sound = n;
    else if ( str_ieq( key, "mode" ) )         cfg.mode = ( n == 320 ) ? 320 : 640;
    else if ( str_ieq( key, "enhance" ) )      cfg.enhance = n;
    else if ( str_ieq( key, "review" ) )       cfg.review = n;
    else if ( str_ieq( key, "draw_enhance" ) ) cfg.draw_enhance = n;
    else if ( str_ieq( key, "slide_delay" ) )  cfg.slide_delay = n > 0 ? n : 8;
    else if ( str_ieq( key, "slide_effect" ) ) cfg.slide_effect = n;
    else if ( str_ieq( key, "slide_shuffle" ) ) cfg.slide_shuffle = n;
    else if ( str_ieq( key, "pics" ) )         str_copy( cfg.pics, val, sizeof( cfg.pics ) );
    else if ( str_ieq( key, "pics_cga" ) )     str_copy( cfg.pics_cga, val, sizeof( cfg.pics_cga ) );
    else if ( str_ieq( key, "chats" ) )        str_copy( cfg.chats, val, sizeof( cfg.chats ) );
    else if ( str_ieq( key, "music" ) )        str_copy( cfg.music, val, sizeof( cfg.music ) );
    else if ( str_ieq( key, "slide_music" ) )  cfg.slide_music = n >= 0 && n <= 2 ? n : 0;
    else if ( str_ieq( key, "slide_song" ) )   str_copy( cfg.slide_song, val, sizeof( cfg.slide_song ) );
  }
  fclose( f );
  return 0;
}

int cfg_save( const char *path ) {
  FILE *f = fopen( path, "w" );            // text mode: DOS gets CRLF line ends
  if ( !f ) return 1;
  fprintf( f, "; DeskMind settings\n" );
  fprintf( f, "server=%s\nport=%u\ntoken=%s\n", cfg.server, cfg.port, cfg.token );
  fprintf( f, "sound=%d\nmode=%d\nenhance=%d\nreview=%d\ndraw_enhance=%d\n",
           cfg.sound, cfg.mode, cfg.enhance, cfg.review, cfg.draw_enhance );
  fprintf( f, "slide_delay=%d\nslide_effect=%d\nslide_shuffle=%d\npics=%s\npics_cga=%s\nchats=%s\n",
           cfg.slide_delay, cfg.slide_effect, cfg.slide_shuffle, cfg.pics, cfg.pics_cga, cfg.chats );
  fprintf( f, "music=%s\nslide_music=%d\nslide_song=%s\n", cfg.music, cfg.slide_music, cfg.slide_song );
  int bad = ferror( f );
  fclose( f );
  return bad;
}
