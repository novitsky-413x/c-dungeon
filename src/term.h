#ifndef TERM_H
#define TERM_H

void term_enable_ansi(void);
void term_hide_cursor(void);
void term_show_cursor(void);
void term_clear_screen(void);

#ifndef _WIN32
void term_enable_raw_mode(void);
#endif

#endif // TERM_H


