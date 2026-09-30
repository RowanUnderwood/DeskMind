// CHATTEST: load_chat / save_chat on long transcripts, text mode only (no network, no video).
//   CHATTEST id...   load each CHATS\<id>.TCH, print the counts, append 2 messages, save, reload
//   CHATTEST /id     time the steps of loading a long chat
// Built by chattest.bat; run in DOSBox or the VM next to a CHATS folder.
#include "scr_chat.cpp"
const Menu *main_menus( int *n ) { *n = 0; return 0; }

static void show( const char *tag ) {
  printf( "%s end=%ld saved=%d msgs=%d tx=%u dl=%d room=%d title=[%s]\n", tag, s_end, s_saved, s_nmsgs, s_txLen, s_ndl, chat_room( ), s_title );
  for ( int i = 0; i < 2 && i < s_nmsgs; i++ ) {
    char t[50]; unsigned n = s_msgs[i].len < 45 ? s_msgs[i].len : 45;
    _fmemcpy( t, s_tx + s_msgs[i].off, n ); t[n] = 0;
    printf( "  msg%d kind=%d [%s]\n", i, s_msgs[i].kind, t );
  }
}

#include "prof.inc"

int main( int argc, char *argv[] ) {
  cfg_defaults( "" );
  str_copy( cfg.chats, "CHATS", sizeof( cfg.chats ) );
  chat_init( );
  if ( !chat_ready( ) ) { printf( "no memory\n" ); return 1; }
  if ( argc > 1 && argv[1][0] == '/' ) { profile( argv[1] + 1 ); return 0; }
  for ( int a = 1; a < argc; a++ ) {
    unsigned long t0 = ticks( );
    if ( load_chat( argv[a] ) ) { printf( "%s: no file\n", argv[a] ); continue; }
    printf( "load %lu ms\n", ( ticks( ) - t0 ) * 55ul );
    show( argv[a] );
    add_msg( MK_USER, "NEW USER LINE", 0 );
    add_msg( MK_AI, "NEW AI LINE", 0 );
    t0 = ticks( );
    save_chat( );
    printf( "save %lu ms\n", ( ticks( ) - t0 ) * 55ul );
    load_chat( argv[a] );
    show( "  reloaded" );
  }
  clear_chat( );
  str_copy( s_chatId, "NEW00001", sizeof( s_chatId ) );
  add_msg( MK_USER, "first question", 0 ); add_msg( MK_AI, "first answer", 0 );
  add_msg( MK_ERR, "an error line (not saved)", 0 );
  save_chat( );
  add_msg( MK_USER, "second question", 0 ); add_msg( MK_AI, "second answer\nparagraph two", 0 );
  save_chat( );
  load_chat( "NEW00001" );
  show( "NEW00001" );
  return 0;
}
