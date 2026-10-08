// DeskMind - settings file DESKMIND.CFG (key=value lines, next to the EXE).

#ifndef DM_CFG_H
#define DM_CFG_H

struct DmConfig {
  char server[64];         // MindServer address, e.g. 192.168.2.192
  unsigned port;           // 8286
  char token[33];          // optional shared token
  int  sound;              // 1 = Tandy sound effects on
  int  mode;               // 640 or 320
  int  enhance;            // Create screen: enhance prompts by default
  int  review;             // show the enhanced prompt for editing before generating
  int  draw_enhance;       // chat: enhance pictures Qwen draws (-1 = server default)
  int  slide_delay;        // slideshow seconds per picture
  int  slide_effect;       // slideshow transition (0 = random)
  int  slide_shuffle;      // slideshow in random order
  char pics[64];           // picture folder, e.g. C:\DESKMIND\PICS
  char pics_cga[64];       // picture folder in CGA mode (CGA pictures only), e.g. C:\DESKMIND\PICSCGA
  char chats[64];          // chat folder
};

extern DmConfig cfg;
extern int cfg_cga;                        // 1 = this run uses CGA (set at startup, not saved)

const char *cfg_pics( void );              // the picture folder for this run
const char *cfg_pics_other( void );        // the other mode's folder (PICSCGA or PICS)

void cfg_defaults( const char *exeDir );
int  cfg_load( const char *path );         // 0 = ok, missing file keeps defaults
int  cfg_save( const char *path );
void cfg_path( char *out, const char *exeDir );   // <exeDir>\DESKMIND.CFG

#endif
