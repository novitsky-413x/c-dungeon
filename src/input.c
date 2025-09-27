#ifdef _WIN32
#include <conio.h>
#else
#include <unistd.h>
#endif

#ifndef _WIN32
#include "timeutil.h"
#endif

int input_read_nonblocking(void) {
#ifdef _WIN32
    if (_kbhit()) {
        int c = _getch();
        if (c == 0 || c == 224) {
            int c2 = _getch();
            switch (c2) { case 72: return 'w'; case 80: return 's'; case 75: return 'a'; case 77: return 'd'; default: return 0; }
        }
        return c;
    }
    return 0;
#else
    static int esc_state = 0;              // 0: idle, 1: collecting after ESC
    static unsigned char esc_buf[8];
    static int esc_len = 0;
    static double esc_deadline_ms = 0.0;   // when to give up waiting for rest

    unsigned char c;
    ssize_t n = read(STDIN_FILENO, &c, 1);
    if (n == 1) {
        if (!esc_state) {
            if (c == '\x1b') {
                esc_state = 1;
                esc_buf[0] = c;
                esc_len = 1;
                esc_deadline_ms = now_ms() + 50.0; // short window to assemble sequence
                // fall through to try draining any immediately available bytes
            } else {
                if (c == '\r') return '\n';
                return c;
            }
        } else {
            // Already collecting; append this byte first
            if (esc_len < (int)sizeof(esc_buf)) esc_buf[esc_len++] = c;
        }
        // Drain any additional available bytes this call
        while (esc_len < (int)sizeof(esc_buf)) {
            unsigned char b;
            ssize_t m = read(STDIN_FILENO, &b, 1);
            if (m == 1) { esc_buf[esc_len++] = b; continue; }
            break;
        }
        // Try to decode common arrow sequences
        if (esc_len >= 3 && esc_buf[0] == '\x1b' && esc_buf[1] == '[') {
            unsigned char k = esc_buf[2];
            esc_state = 0; esc_len = 0;
            switch (k) { case 'A': return 'w'; case 'B': return 's'; case 'D': return 'a'; case 'C': return 'd'; default: break; }
            return 0;
        }
        // xterm/VT alternate arrows: ESC O A/B/C/D
        if (esc_len >= 3 && esc_buf[0] == '\x1b' && esc_buf[1] == 'O') {
            unsigned char k = esc_buf[2];
            esc_state = 0; esc_len = 0;
            switch (k) { case 'A': return 'w'; case 'B': return 's'; case 'D': return 'a'; case 'C': return 'd'; default: break; }
            return 0;
        }
        // Not enough to decide yet; keep waiting across calls
        return 0;
    }
    // No new byte this call. If waiting on ESC, check timeout.
    if (esc_state) {
        if (now_ms() >= esc_deadline_ms) {
            // Timeout: treat a bare ESC as no-op; if only ESC was read, emit nothing
            // Reset state either way.
            int onlyEsc = (esc_len == 1 && esc_buf[0] == '\x1b');
            esc_state = 0; esc_len = 0;
            if (!onlyEsc) return 0;
        }
    }
    return 0;
#endif
}


