#ifndef MP_H
#define MP_H

#include "types.h"

extern int g_mp_active;
extern int g_my_player_id;
extern RemotePlayer g_remote_players[MAX_REMOTE_PLAYERS];
extern RemoteBullet g_remote_bullets[MAX_REMOTE_BULLETS];
extern RemoteEnemy g_remote_enemies[MAX_REMOTE_ENEMIES];
extern int g_mp_joined; // set when we receive our own PLAYER snapshot

#endif // MP_H


