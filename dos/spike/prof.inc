static void profile( const char *id ) {
  char path[80];
  chat_file( path, id );
  FILE *f = fopen( path, "rb" );
  setvbuf( f, s_fbuf, _IOFBF, sizeof( s_fbuf ) );
  clear_chat( );
  static const Cost half = { TX_SIZE / 2, MAX_DL / 2, MAX_MSGS / 2 };
  fseek( f, 0, SEEK_END );
  long size = ftell( f );
  unsigned long t0 = ticks( );
  static char buf[ 4096 ];
  fseek( f, size - 20000, SEEK_SET );
  long got = 0; unsigned n;
  while ( ( n = fread( buf, 1, sizeof( buf ), f ) ) > 0 ) got += n;
  unsigned long t1 = ticks( );
  long head = tch_fit( f, size - 20000, &half );
  unsigned long t2 = ticks( );
  parse_from( f, head );
  unsigned long t3 = ticks( );
  layout_from( 0 );
  unsigned long t4 = ticks( );
  printf( "fread %ld: %lu ms  fit %lu ms  parse %lu ms  layout %lu ms\n", got, ( t1 - t0 ) * 55, ( t2 - t1 ) * 55, ( t3 - t2 ) * 55, ( t4 - t3 ) * 55 );
  fclose( f );
}
