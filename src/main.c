// Enable POSIX prototypes for nanosleep/usleep on Unix
#ifndef _WIN32
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif
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
static int loadingStartTick = -1;
static const int MIN_LOADING_TICKS = 30; // ~0.5s at ~60 FPS

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
    printf("Dungeon\n\n1) Singleplayer\n2) Multiplayer\nSelect [1/2]: ");
    fflush(stdout);
}

// removed unused read_line

int main(void) {
    srand((unsigned int)time(NULL));
    term_enable_ansi();
    term_enter_alt_screen();
    term_hide_cursor();
#ifndef _WIN32
    term_enable_raw_mode();
#endif

    // Simple blocking menu (poll loop)
    int mode = 0;
    print_menu();
    while (!mode) {
        int c = input_read_nonblocking();
        if (c == '1') mode = 1;
        else if (c == '2') mode = 2;
        if (!mode) {
            // small sleep
#ifdef _WIN32
            Sleep(30);
#else
            struct timespec ts; ts.tv_sec = 0; ts.tv_nsec = 30 * 1000000L; nanosleep(&ts, NULL);
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
                struct timespec ts; ts.tv_sec = 0; ts.tv_nsec = 30 * 1000000L; nanosleep(&ts, NULL);
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
    // Clear menu before starting the game to avoid leftover lines in some shells (e.g., zsh on macOS)
    term_clear_screen();

    game_init();
    game_spawn_enemies(4);

    while (game_running) {
        double frameStart = now_ms();
        handleInput();
        if (g_mp_active) {
            extern int g_mp_joined;
            if (client_poll_messages()) needsRedraw = 1;
            // Track a minimum loading duration so animation is visible even on fast servers
            if (!g_mp_joined) {
                if (loadingStartTick < 0) loadingStartTick = game_tick_count;
                // Show loading screen (animate based on game_tick_count)
                game_draw_loading(game_tick_count);
            } else if (loadingStartTick >= 0 && (game_tick_count - loadingStartTick) < MIN_LOADING_TICKS) {
                game_draw_loading(game_tick_count);
            } else {
                loadingStartTick = -1;
                if ((game_tick_count % 6) == 0) { if (game_move_enemies()) needsRedraw = 1; }
                if (game_update_projectiles()) needsRedraw = 1;
                if (game_tick_status()) needsRedraw = 1;
                game_check_win_lose();
                if (needsRedraw) { game_draw(); needsRedraw = 0; }
            }
        } else {
            if ((game_tick_count % 6) == 0) { if (game_move_enemies()) needsRedraw = 1; }
            if (game_update_projectiles()) needsRedraw = 1;
            if (game_tick_status()) needsRedraw = 1;
            game_check_win_lose();
            if (needsRedraw) { game_draw(); needsRedraw = 0; }
        }
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

    // Restore terminal state
#ifndef _WIN32
    term_disable_raw_mode();
#endif
    term_exit_alt_screen();
    term_show_cursor();
    if (game_player_won) printf("You escaped the dungeon!\n"); else printf("You were caught by an enemy. Game Over.\n");
    printf("Thanks for playing.\n");
    return 0;
}


