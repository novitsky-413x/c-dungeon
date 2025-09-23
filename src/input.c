#ifdef _WIN32
#include <conio.h>
#else
#include <unistd.h>
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
    unsigned char c;
    if (read(STDIN_FILENO, &c, 1) == 1) {
        if (c == '\x1b') {
            unsigned char seq[2];
            if (read(STDIN_FILENO, &seq[0], 1) == 1 && read(STDIN_FILENO, &seq[1], 1) == 1) {
                if (seq[0] == '[') {
                    switch (seq[1]) { case 'A': return 'w'; case 'B': return 's'; case 'D': return 'a'; case 'C': return 'd'; }
                }
            }
            return 0;
        }
        return c;
    }
    return 0;
#endif
}


