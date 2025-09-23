#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "types.h"
#include "term.h"
#include "timeutil.h"
#include "input.h"
#include "game.h"
#include "mp.h"
#include "client_net.h"

static int needsRedraw = 1;

static void handleInput(void) {
    int c = input_read_nonblocking();
    if (!c) return;
    switch (c) {
        case 'w': case 'W': if (g_mp_active) { client_send_input(0, -1, 0); needsRedraw = 1; } else if (game_attempt_move_player(0, -1)) { needsRedraw = 1; } break;
        case 's': case 'S': if (g_mp_active) { client_send_input(0, 1, 0); needsRedraw = 1; } else if (game_attempt_move_player(0, 1))  { needsRedraw = 1; } break;
        case 'a': case 'A': if (g_mp_active) { client_send_input(-1, 0, 0); needsRedraw = 1; } else if (game_attempt_move_player(-1, 0)) { needsRedraw = 1; } break;
        case 'd': case 'D': if (g_mp_active) { client_send_input(1, 0, 0); needsRedraw = 1; } else if (game_attempt_move_player(1, 0))  { needsRedraw = 1; } break;
        case ' ': if (g_mp_active) { client_send_input(0, 0, 1); needsRedraw = 1; } else { game_player_shoot(); needsRedraw = 1; } break;
        case 'q': case 'Q': if (g_mp_active) client_send_bye(); game_running = 0; break;
        default: break;
    }
}

static void print_menu(void) {
    term_clear_screen();
    printf("Dungeon\n\n1) Singleplayer\n2) Multiplayer\nSelect: ");
    fflush(stdout);
}

// removed unused read_line

int main(void) {
    srand((unsigned int)time(NULL));
    term_enable_ansi();
    term_hide_cursor();
#ifndef _WIN32
    term_enable_raw_mode();
#endif

    // Simple blocking menu (poll loop)
    int mode = 0;
    print_menu();
    while (!mode) {
        int c = input_read_nonblocking();
        if (c == '1') mode = 1; else if (c == '2') mode = 2;
        if (!mode) {
            // small sleep
#ifdef _WIN32
            Sleep(30);
#else
            usleep(30000);
#endif
        }
    }
    if (mode == 2) {
        term_clear_screen();
        printf("Enter server host[:port] (default port 5555): "); fflush(stdout);
        char addr[256] = {0};
        // crude line read: wait until user hits Enter
        int entered = 0; int c;
        while (!entered) {
            c = input_read_nonblocking();
            if (c == 0) {
#ifdef _WIN32
                Sleep(30);
#else
                usleep(30000);
#endif
                continue;
            }
            if (c == '\r' || c == '\n') { entered = 1; }
            else if (c == 8 || c == 127) { int len = (int)strlen(addr); if (len > 0) { addr[len-1] = '\0'; printf("\b \b"); fflush(stdout); } }
            else if (c >= 32 && c <= 126) { size_t len = strlen(addr); if (len < sizeof(addr)-1) { addr[len] = (char)c; addr[len+1] = '\0'; putchar((char)c); fflush(stdout); } }
        }
        // Connect to server
        if (client_connect(addr) == 0) {
            g_mp_active = 1;
        } else {
            printf("\nFailed to connect. Starting singleplayer.\n");
        }
    }

    game_init();
    game_spawn_enemies(4);

    while (game_running) {
        double frameStart = now_ms();
        handleInput();
        if (g_mp_active) client_poll_messages();
        if ((game_tick_count % 6) == 0) { if (game_move_enemies()) needsRedraw = 1; }
        if (game_update_projectiles()) needsRedraw = 1;
        if (game_tick_status()) needsRedraw = 1;
        game_check_win_lose();
        if (needsRedraw) { game_draw(); needsRedraw = 0; }
        const double targetFrameMs = 16.6667;
        double elapsed = now_ms() - frameStart;
        double remaining = targetFrameMs - elapsed;
        if (remaining > 0) {
#ifdef _WIN32
            Sleep((DWORD)(remaining + 0.5));
#else
            struct timespec req;
            long ms = (long)remaining;
            long ns = (long)((remaining - (double)ms) * 1000000.0);
            if (ns < 0) ns = 0;
            req.tv_sec = ms / 1000;
            req.tv_nsec = (ms % 1000) * 1000000L + ns;
            if (req.tv_nsec >= 1000000000L) {
                req.tv_sec += 1;
                req.tv_nsec -= 1000000000L;
            }
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


