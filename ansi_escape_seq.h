#ifndef __ANSI_ESCAPE_SEQ_H__
#define __ANSI_ESCAPE_SEQ_H__

#define ESC '\e'

#define MOVE_CURSOR_HOME "\e[H"
#define MOVE_CURSOR_RIGHT_COLUMNS(x) "\e[" x "C"
#define CLEAR_FROM_CURSOR_TO_BEGINNING "\e[1J"
#define CLEAR_CURRENT_LINE "\e[K"

#define KEY_DELETE      "\e[3~"
#define KEY_UP          "\e[A"
#define KEY_DOWN        "\e[B"
#define KEY_RIGHT       "\e[C"
#define KEY_LEFT        "\e[D"
#define KEY_CTRL_UP     "\e[1;5A"
#define KEY_CTRL_DOWN   "\e[1;5B"
#define KEY_CTRL_RIGHT  "\e[1;5C"
#define KEY_CTRL_LEFT   "\e[1;5D"
#define KEY_ALT_UP      "\e[1;3A"
#define KEY_ALT_DOWN    "\e[1;3B"
#define KEY_ALT_RIGHT   "\e[1;3C"
#define KEY_ALT_LEFT    "\e[1;3D"
#define KEY_SHIFT_UP    "\e[1;2A"
#define KEY_SHIFT_DOWN  "\e[1;2B"
#define KEY_SHIFT_RIGHT "\e[1;2C"
#define KEY_SHIFT_LEFT  "\e[1;2D"

#define KEY_CTRL_C 3
#define KEY_CTRL_D 4
#define KEY_CTRL_G 7
#define KEY_CTRL_S 19
#define KEY_ENTER '\r'
#define KEY_BACKSPACE_1 0x7f
#define KEY_BACKSPACE_2 0x8

//Regular text
#define BLK "\e[0;30m"
#define RED "\e[0;31m"
#define GRN "\e[0;32m"
#define YEL "\e[0;33m"
#define BLU "\e[0;34m"
#define MAG "\e[0;35m"
#define CYN "\e[0;36m"
#define WHT "\e[0;37m"

//Regular bold text
#define BBLK "\e[1;30m"
#define BRED "\e[1;31m"
#define BGRN "\e[1;32m"
#define BYEL "\e[1;33m"
#define BBLU "\e[1;34m"
#define BMAG "\e[1;35m"
#define BCYN "\e[1;36m"
#define BWHT "\e[1;37m"
#define RESET_STYLE "\e[0m"

//Regular underline text
#define UBLK "\e[4;30m"
#define URED "\e[4;31m"
#define UGRN "\e[4;32m"
#define UYEL "\e[4;33m"
#define UBLU "\e[4;34m"
#define UMAG "\e[4;35m"
#define UCYN "\e[4;36m"
#define UWHT "\e[4;37m"

#endif