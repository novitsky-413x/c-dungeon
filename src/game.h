#ifndef GAME_H
#define GAME_H

#include "types.h"

void game_init(void);
void game_spawn_enemies(int count);
int game_move_enemies(void);
int game_is_blocked(int x, int y);
int game_is_enemy_at(int x, int y);
int game_attempt_move_player(int dx, int dy);
void game_check_win_lose(void);
void game_draw(void);

extern int game_running;
extern int game_player_won;
extern int game_tick_count;

#endif // GAME_H


