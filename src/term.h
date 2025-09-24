#ifndef TERM_H
#define TERM_H

void term_enable_ansi(void);
void term_hide_cursor(void);
void term_show_cursor(void);
void term_clear_screen(void);
void term_enter_alt_screen(void);
void term_exit_alt_screen(void);

// ANSI SGR color codes (foreground)
#define TERM_SGR_RESET "\x1b[0m"
#define TERM_FG_BLACK "\x1b[30m"
#define TERM_FG_RED "\x1b[31m"
#define TERM_FG_GREEN "\x1b[32m"
#define TERM_FG_YELLOW "\x1b[33m"
#define TERM_FG_BLUE "\x1b[34m"
#define TERM_FG_MAGENTA "\x1b[35m"
#define TERM_FG_CYAN "\x1b[36m"
#define TERM_FG_WHITE "\x1b[37m"
#define TERM_FG_BRIGHT_BLACK "\x1b[90m"
#define TERM_FG_BRIGHT_RED "\x1b[91m"
#define TERM_FG_BRIGHT_GREEN "\x1b[92m"
#define TERM_FG_BRIGHT_YELLOW "\x1b[93m"
#define TERM_FG_BRIGHT_BLUE "\x1b[94m"
#define TERM_FG_BRIGHT_MAGENTA "\x1b[95m"
#define TERM_FG_BRIGHT_CYAN "\x1b[96m"
#define TERM_FG_BRIGHT_WHITE "\x1b[97m"

#ifndef _WIN32
void term_enable_raw_mode(void);
#endif

#endif // TERM_H


