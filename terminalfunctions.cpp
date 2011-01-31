#include <unistd.h>

#include "terminaldispatcher.hpp"
#include "terminalframebuffer.hpp"

using namespace Terminal;

static void clearline( Framebuffer *fb, int row, int start, int end )
{
  for ( int col = start; col <= end; col++ ) {
    fb->get_cell( row, col )->reset();
  }
}

void CSI_EL( Framebuffer *fb, Dispatcher *dispatch )
{
  switch ( dispatch->getparam( 0, 0 ) ) {
  case 0: /* default: active position to end of line, inclusive */
    clearline( fb, -1, fb->ds.get_cursor_col(), fb->ds.get_width() - 1 );    
    break;
  case 1: /* start of screen to active position, inclusive */
    clearline( fb, -1, 0, fb->ds.get_cursor_col() );
    break;
  case 2: /* all of line */
    clearline( fb, -1, 0, fb->ds.get_width() - 1 );
    break;
  }
}

static Function func_CSI_EL( CSI, "K", CSI_EL );

void CSI_ED( Framebuffer *fb, Dispatcher *dispatch ) {
  switch ( dispatch->getparam( 0, 0 ) ) {
  case 0: /* active position to end of screen, inclusive */
    clearline( fb, -1, fb->ds.get_cursor_col(), fb->ds.get_width() - 1 );
    for ( int y = fb->ds.get_cursor_row() + 1; y < fb->ds.get_height(); y++ ) {
      clearline( fb, y, 0, fb->ds.get_width() - 1 );
    }
    break;
  case 1: /* start of screen to active position, inclusive */
    for ( int y = 0; y < fb->ds.get_cursor_row(); y++ ) {
      clearline( fb, y, 0, fb->ds.get_width() - 1 );
    }
    clearline( fb, -1, 0, fb->ds.get_cursor_col() );
    break;
  case 2: /* entire screen */
    for ( int y = 0; y < fb->ds.get_height(); y++ ) {
      clearline( fb, y, 0, fb->ds.get_width() - 1 );
    }
    break;
  }
}

static Function func_CSI_ED( CSI, "J", CSI_ED );

void CSI_cursormove( Framebuffer *fb, Dispatcher *dispatch )
{
  int num = dispatch->getparam( 0, 1 );

  switch ( dispatch->get_dispatch_chars()[ 0 ] ) {
  case 'A':
    fb->ds.move_row( -num, true );
    break;
  case 'B':
    fb->ds.move_row( num, true );
    break;
  case 'C':
    fb->ds.move_col( num, true );
    break;
  case 'D':
    fb->ds.move_col( -num, true );
    break;
  case 'H':
  case 'f':
    int x = dispatch->getparam( 0, 1 );
    int y = dispatch->getparam( 1, 1 );
    fb->ds.move_row( x - 1 );
    fb->ds.move_col( y - 1 );
  }
}

static Function func_CSI_cursormove_A( CSI, "A", CSI_cursormove );
static Function func_CSI_cursormove_B( CSI, "B", CSI_cursormove );
static Function func_CSI_cursormove_C( CSI, "C", CSI_cursormove );
static Function func_CSI_cursormove_D( CSI, "D", CSI_cursormove );
static Function func_CSI_cursormove_H( CSI, "H", CSI_cursormove );
static Function func_CSI_cursormove_f( CSI, "f", CSI_cursormove );

void CSI_DA( Framebuffer *fb __attribute((unused)), Dispatcher *dispatch )
{
  dispatch->terminal_to_host.append( "\033[?1;0c" );
}

static Function func_CSI_DA( CSI, "c", CSI_DA );

void Esc_DECALN( Framebuffer *fb, Dispatcher *dispatch __attribute((unused)) )
{
  for ( int y = 0; y < fb->ds.get_height(); y++ ) {
    for ( int x = 0; x < fb->ds.get_width(); x++ ) {
      fb->get_cell( y, x )->reset();
      fb->get_cell( y, x )->contents.push_back( L'E' );
    }
  }
}

static Function func_Esc_DECALN( ESCAPE, "#8", Esc_DECALN );

void Ctrl_LF( Framebuffer *fb, Dispatcher *dispatch __attribute((unused)) )
{
  fb->move_rows_autoscroll( 1 );
}

static Function func_Ctrl_LF( CONTROL, "\x0a", Ctrl_LF );
static Function func_Ctrl_IND( CONTROL, "\x84", Ctrl_LF );

void Ctrl_CR( Framebuffer *fb, Dispatcher *dispatch __attribute((unused)) )
{
  fb->ds.move_col( 0 );
}

static Function func_Ctrl_CR( CONTROL, "\x0d", Ctrl_CR );

void Ctrl_BS( Framebuffer *fb, Dispatcher *dispatch __attribute((unused)) )
{
  fb->ds.move_col( -1, true );
}

static Function func_Ctrl_BS( CONTROL, "\x08", Ctrl_BS );

void Ctrl_RI( Framebuffer *fb, Dispatcher *dispatch __attribute((unused)) )
{
  fb->move_rows_autoscroll( -1 );
}

static Function func_Ctrl_RI( CONTROL, "\x8D", Ctrl_RI );

void Ctrl_NEL( Framebuffer *fb, Dispatcher *dispatch __attribute((unused)) )
{
  fb->ds.move_col( 0 );
  fb->move_rows_autoscroll( 1 );
}

static Function func_Ctrl_NEL( CONTROL, "\x85", Ctrl_NEL );

void Ctrl_HT( Framebuffer *fb, Dispatcher *dispatch __attribute((unused)) )
{
  int col = fb->ds.get_next_tab();
  if ( col == -1 ) { /* no tabs, go to end of line */
    fb->ds.move_col( fb->ds.get_width() - 1 );
  } else {
    fb->ds.move_col( col );
  }
}

static Function func_Ctrl_HT( CONTROL, "\x09", Ctrl_HT );

void Ctrl_HTS( Framebuffer *fb, Dispatcher *dispatch __attribute((unused)) )
{
  fb->ds.set_tab();
}

static Function func_Ctrl_HTS( CONTROL, "\x88", Ctrl_HTS );

void CSI_TBC( Framebuffer *fb, Dispatcher *dispatch )
{
  if ( dispatch->getparam( 0, 0 ) == 3 ) { /* clear all tab stops */
    for ( int x = 0; x < fb->ds.get_width(); x++ ) {
      fb->ds.clear_tab( x );
    }
  } else {
    fb->ds.clear_tab( fb->ds.get_cursor_col() );
  }
}

static Function func_CSI_TBC( CSI, "g", CSI_TBC );

static bool *get_DEC_mode( int param, Framebuffer *fb ) {
  switch ( param ) {
  case 6: /* origin */
    return &(fb->ds.origin_mode);
  case 7: /* auto wrap */
    return &(fb->ds.auto_wrap_mode);
  }
  return NULL;
}

void CSI_DECSM( Framebuffer *fb, Dispatcher *dispatch )
{
  for ( int i = 0; i < dispatch->param_count(); i++ ) {
    bool *mode = get_DEC_mode( dispatch->getparam( i, 0 ), fb );
    if ( mode ) {
      *mode = true;
    }
  }
}

void CSI_DECRM( Framebuffer *fb, Dispatcher *dispatch )
{
  for ( int i = 0; i < dispatch->param_count(); i++ ) {
    bool *mode = get_DEC_mode( dispatch->getparam( i, 0 ), fb );
    if ( mode ) {
      *mode = false;
    }
  }
}

static Function func_CSI_DECSM( CSI, "?h", CSI_DECSM );
static Function func_CSI_DECRM( CSI, "?l", CSI_DECRM );