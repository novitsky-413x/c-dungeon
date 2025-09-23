#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "types.h"
#include "term.h"
#include "timeutil.h"
#include "input.h"
#include "game.h"

static int needsRedraw = 1;

static void handleInput(void) {
    int c = input_read_nonblocking();
    if (!c) return;
    switch (c) {
        case 'w': case 'W': if (game_attempt_move_player(0, -1)) needsRedraw = 1; break;
        case 's': case 'S': if (game_attempt_move_player(0, 1)) needsRedraw = 1; break;
        case 'a': case 'A': if (game_attempt_move_player(-1, 0)) needsRedraw = 1; break;
        case 'd': case 'D': if (game_attempt_move_player(1, 0)) needsRedraw = 1; break;
        case 'q': case 'Q': game_running = 0; break;
        default: break;
    }
}

int main(void) {
    srand((unsigned int)time(NULL));
    term_enable_ansi();
    term_hide_cursor();
#ifndef _WIN32
    term_enable_raw_mode();
#endif

    game_init();
    game_spawn_enemies(4);

    while (game_running) {
        double frameStart = now_ms();
        handleInput();
        if ((game_tick_count % 6) == 0) { if (game_move_enemies()) needsRedraw = 1; }
        game_check_win_lose();
        if (needsRedraw) { game_draw(); needsRedraw = 0; }
        const double targetFrameMs = 16.6667;
        double elapsed = now_ms() - frameStart;
        double remaining = targetFrameMs - elapsed;
        if (remaining > 0) {
#ifdef _WIN32
            Sleep((DWORD)(remaining + 0.5));
#else
            struct timespec req; long ms = (long)remaining; long ns = (long)((remaining - (double)ms) * 1e6) * 1000L;
            if (ns < 0) ns = 0; req.tv_sec = ms / 1000; req.tv_nsec = (ms % 1000) * 1000000L + ns; if (req.tv_nsec >= 1000000000L) { req.tv_sec += 1; req.tv_nsec -= 1000000000L; }
            nanosleep(&req, NULL);
#endif
        }
        game_tick_count++;
    }

    term_clear_screen();
    term_show_cursor();
    if (game_player_won) printf("You escaped the dungeon!\n"); else printf("You were caught by an enemy. Game Over.\n");
    printf("Thanks for playing.\n");
    return 0;
}


