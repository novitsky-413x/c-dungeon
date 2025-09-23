#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
static struct termios originalTermios;
#endif

static int ansi_inited = 0;

void term_enable_ansi(void) {
#ifdef _WIN32
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == INVALID_HANDLE_VALUE) return;
    DWORD mode = 0;
    if (!GetConsoleMode(hOut, &mode)) return;
    mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    SetConsoleMode(hOut, mode);
#else
    // nothing
#endif
    ansi_inited = 1;
}

void term_hide_cursor(void) {
    if (!ansi_inited) term_enable_ansi();
    printf("\x1b[?25l");
}

void term_show_cursor(void) {
    if (!ansi_inited) term_enable_ansi();
    printf("\x1b[?25h");
    fflush(stdout);
}

void term_clear_screen(void) {
    if (!ansi_inited) term_enable_ansi();
    printf("\x1b[2J\x1b[H");
}

#ifndef _WIN32
void term_enable_raw_mode(void) {
    tcgetattr(STDIN_FILENO, &originalTermios);
    struct termios raw = originalTermios;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
}
#endif


