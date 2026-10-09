// DeskMind - the song library and what plays next (DeskMind and SLIDES.EXE).
//
// Songs are <cfg.music>\<FILE>.T3 (FILE = MindServer's 8-hex ID, or any 8.3 name copied there).
// SONGS.LST in the same folder holds their titles and dates, one per line:
//   FILE YYYYMMDDHHMM SECONDS TITLE
// A .T3 file without a line shows its file name and the file's date.

#ifndef JUKEBOX_H
#define JUKEBOX_H

#define JB_MAX 150

struct Song {
  char file[9];                 // without .T3
  char title[40];
  unsigned long when;           // packed date (JB_WHEN): newest first
  unsigned secs;
};

enum { JB_IDLE, JB_ONCE, JB_LOOP, JB_RANDOM, JB_JINGLE };

// Dates: SONGS.LST and MindServer write YYYYMMDDHHMM (12 digits, too big for 32 bits);
// in memory they are packed like the Gallery's picture dates
#define JB_WHEN( y, mo, d, h, mi ) ( ( (unsigned long)( y ) << 20 ) | ( (unsigned long)( mo ) << 16 ) | \
                                    ( (unsigned long)( d ) << 11 ) | ( (unsigned)( h ) << 6 ) | ( mi ) )
unsigned long jb_when_parse( const char *yyyymmddhhmm );     // 0 if not 12 digits
void jb_when_text( unsigned long when, char *out13 );        // back to YYYYMMDDHHMM
void jb_when_date( unsigned long when, char *out11 );        // YYYY-MM-DD

int  jb_scan( void );                       // reads the folder; returns the number of songs
int  jb_count( void );
int  jb_total( void );                      // songs on disk (more than jb_count when full)
const Song *jb_song( int i );
int  jb_find( const char *file );           // index or -1
void jb_path( char *out, const char *file );

int  jb_play( int i, int mode );            // JB_ONCE, JB_LOOP or JB_RANDOM (starts with i); MUS_*
int  jb_random( void );                     // random songs, one after another; MUS_*
int  jb_jingle( const char *path );         // a short sound (the modem): any key stops it; MUS_*
void jb_stop( void );
int  jb_poll( void );                       // call often; 1 = what plays changed (redraw)
int  jb_mode( void );                       // JB_* (JB_IDLE when nothing plays)
int  jb_current( void );                    // index playing, or -1
const char *jb_now( void );                 // "title  1:23 / 2:05", or "" (for a status line)

int  jb_add( const char *file, const char *title, unsigned long when );   // after a download (when packed, or 0)
int  jb_rename( int i, const char *title );
int  jb_delete( int i );                    // 0 = ok

// Slideshow music (cfg.slide_music): starts it unless something already plays.
// Returns 1 if it started music, which jb_slides_end() then stops.
int  jb_slides_begin( void );
void jb_slides_end( int started );

#endif
