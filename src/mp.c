#include "mp.h"

int g_mp_active = 0;
int g_my_player_id = -1;
RemotePlayer g_remote_players[MAX_REMOTE_PLAYERS];
RemoteBullet g_remote_bullets[MAX_REMOTE_BULLETS];
RemoteEnemy g_remote_enemies[MAX_REMOTE_ENEMIES];
int g_mp_joined = 0;


