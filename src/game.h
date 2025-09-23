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

// Shooting API
void game_player_shoot(void);
int game_update_projectiles(void);
int game_tick_status(void);

// MP helpers (client-side): apply authoritative world changes from server
void game_mp_set_tile(int wx, int wy, int x, int y, char tile);
int game_mp_get_cur_world_x(void);
int game_mp_get_cur_world_y(void);
// In MP, server is authoritative for our own position/world
void game_mp_set_self(int wx, int wy, int x, int y);

extern int game_running;
extern int game_player_won;
extern int game_tick_count;
extern int game_player_lives;
extern int game_score;

#endif // GAME_H


