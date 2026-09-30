// DeskMind - a small DeskMate-style GUI for 640x200x16.
//
// Screen layout: menu bar y 0-10, work area y 11-189, status bar y 190-199.
// Text is the 8x8 ROM font; one text row is ROW_H pixels (8 + 2 spacing).
// Pixels are ~2.4x taller than wide, so shapes are drawn wider than tall.

#ifndef GUI_H
#define GUI_H

// Tandy colour numbers
enum { BLACK, BLUE, GREEN, CYAN, RED, MAGENTA, BROWN, LGRAY, DGRAY,
       LBLUE, LGREEN, LCYAN, LRED, LMAGENTA, YELLOW, WHITE };

#define ROW_H      10
#define MENU_H     11
#define STATUS_Y   190
#define WORK_Y     MENU_H
#define WORK_H     ( STATUS_Y - MENU_H )

// ---------------------------------------------------------------- events

#define EV_NONE   0
#define EV_KEY    1
#define EV_DOWN   2          // mouse button pressed (key = 1 single, 2 double click)
#define EV_UP     3
#define EV_MOVE   4          // mouse moved with the left button held

struct Event {
  int type;
  int key;                   // EV_KEY: key code (sys.h); EV_DOWN: click count
  int x, y;
  int buttons;
};

extern void ( *gui_idle )( void );     // called on every poll (network, animations)

int  gui_init( void );                 // mouse, sound; returns 0 = ok (video must be open)
void gui_done( void );
int  gui_poll( Event *e );             // fills e, returns e->type (EV_NONE when idle)
void gui_pump( void );                 // move the pointer during long work (no events)
void gui_mouse_hide( void );           // nested; hide before drawing
void gui_mouse_show( void );
int  gui_has_mouse( void );

// ---------------------------------------------------------------- drawing

void ui_desktop( void );
void ui_bevel( int x, int y, int w, int h, int raised );          // 3-D frame
void ui_sunken( int x, int y, int w, int h, unsigned char fill );
void ui_window( int x, int y, int w, int h, const char *title, int active );
void ui_button( int x, int y, int w, int h, const char *label, int flags );
void ui_status( const char *text );                               // bottom line
int  ui_text_fit( int x, int y, int w, const char far *s, unsigned char fg, int bg );   // clipped to w pixels
int  ui_hit( int x, int y, int rx, int ry, int rw, int rh );

#define BF_PRESSED  1
#define BF_DEFAULT  2
#define BF_FOCUS    4
#define BF_DISABLED 8

// Save / restore a rectangle (for dialogs and drop-downs).  NULL when out of memory.
unsigned char far *ui_save( int x, int y, int w, int h );
void ui_restore( int x, int y, int w, int h, unsigned char far *buf );   // also frees it

// ---------------------------------------------------------------- widgets

enum { W_LABEL, W_BUTTON, W_EDIT, W_MEMO, W_LIST, W_CHECK };

#define WF_DEFAULT  1        // button: Enter activates it
#define WF_CANCEL   2        // button: Esc activates it
#define WF_HIDDEN   4
#define WF_DISABLED 8
#define WF_READONLY 16       // edit/memo

struct Widget;
typedef const char far *( *list_item_fn )( void *ctx, int index );

struct Widget {
  int type;
  int id;                    // returned by form_run for buttons (and list double-click)
  int x, y, w, h;            // relative to the form's client area
  const char *text;          // label / button caption / check box caption
  int flags;
  // edit and memo
  char far *buf;
  unsigned size;             // buffer size including the NUL
  unsigned cur;              // cursor position
  unsigned top;              // edit: first visible char; memo: first visible line
  // list
  int count;
  list_item_fn item;
  void *ctx;
  int sel;
  int ltop;
  // check box
  int checked;
  // colours for labels (0 = default)
  unsigned char fg;
  // what the scroll bar shows now (redrawn only when it changes)
  int sbn, sbtop;
};

struct Form {
  const char *title;
  int x, y, w, h;            // outer rectangle (screen coordinates)
  Widget *w_;
  int n;
  int focus;
  int dirty;                 // set by widgets when their contents change
};

void form_init( Form *f, const char *title, int x, int y, int w, int h, Widget *ws, int n );
void form_draw( Form *f );
void form_draw_widget( Form *f, int i );
// Handles one event.  Returns a button id when a button is activated, -1 for Esc
// (if no cancel button), or 0.  Events the form does not use are left in *e with
// e->type unchanged; used ones become EV_NONE.
int  form_event( Form *f, Event *e );
// Modal loop: polls, handles events, returns the id of the activated button (-1 = Esc).
int  form_run( Form *f );
void form_set_focus( Form *f, int i );   // move the focus and redraw both widgets
int  form_client_x( Form *f );
int  form_client_y( Form *f );

void list_set( Widget *w, int count, list_item_fn item, void *ctx );
void memo_to_end( Widget *w );

// ---------------------------------------------------------------- dialogs

// buttons: "OK" or "Yes|No" or "OK|Cancel"; returns the button number (1-based), 0 = Esc
int  msg_box( const char *title, const char *text, const char *buttons );
// Returns 1 if OK, 0 if cancelled; buf keeps the (edited) text
int  input_box( const char *title, const char *prompt, char *buf, unsigned size );
// A multi-line editor dialog (e.g. an enhanced prompt); returns 1 if OK
int  memo_box( const char *title, const char *prompt, char far *buf, unsigned size );
// Small "please wait" box; draw once, then remove it with busy_end()
void busy_begin( const char *title, const char *text );
void busy_text( const char *text );
void busy_end( void );

// ---------------------------------------------------------------- menu bar

struct MenuItem { const char *label; int id; };      // label "-" = separator
struct Menu { const char *title; const MenuItem *items; int n; };

void menubar_set( const Menu *menus, int n, const char *rightText );
void menubar_draw( void );
// Opens menu `start` (-1 = from a click at e) and runs it; returns the chosen item id or 0.
int  menubar_run( int start, const Event *e );
int  menubar_hit( int x, int y );                    // menu index under (x,y), or -1
int  menubar_key( int key );                         // menu index for Alt+letter, or -1

#endif
