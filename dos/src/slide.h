// DeskMind - slideshow with 286-friendly transitions (used by the Gallery and SLIDES.EXE).
// 640x200x16 only; the video mode must already be open.

#ifndef SLIDE_H
#define SLIDE_H

enum { FX_RANDOM, FX_CUT, FX_WIPE_RIGHT, FX_WIPE_DOWN, FX_BLINDS, FX_INTERLACE,
       FX_DISSOLVE, FX_BOX_OUT, FX_BOX_IN, FX_SLIDE_IN, FX_COUNT };

struct SlideOpts {
  int delay;          // seconds per picture
  int effect;         // FX_*; FX_RANDOM picks one each time
  int titles;         // 1 = show the title strip for a few seconds
  int shuffle;        // 1 = random order
  int loop;           // 1 = start again after the last picture
};

// Fills out (80 chars) with the path of picture i; returns 0 if there is none
typedef int ( *slide_path_fn )( void *ctx, int i, char *out );

// Runs until Esc, a mouse click, or the end (when !loop).  Returns the number of pictures shown.
int  slide_run( int count, int start, slide_path_fn path, void *ctx, SlideOpts *o );
void slide_transition( int effect, const unsigned char far *img );
// Play order: 0..count-1, or shuffled (with `start` first when it is a valid index)
void slide_make_order( int *order, int count, int start, int shuffle );
const char *slide_effect_name( int effect );

#endif
