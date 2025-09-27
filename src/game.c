#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h> // write, STDOUT_FILENO
#include <sys/types.h> // ssize_t
#include <errno.h>
#include <time.h>
#include <termios.h>
#endif
#include "types.h"
#include "term.h"
#include "mp.h"

static Vec2 playerPos;
static Direction playerFacing = DIR_RIGHT;
static Projectile projectiles[MAX_PROJECTILES];

// World configuration (3x3 grid: x0-y0 to x2-y2)
#define WORLD_W 9
#define WORLD_H 9

typedef struct {
    char tiles[MAP_HEIGHT][MAP_WIDTH + 1];
    unsigned char wallDmg[MAP_HEIGHT][MAP_WIDTH];
    Enemy enemies[MAX_ENEMIES];
    int numEnemies;
    int initialized;
} MapState;

static MapState world[WORLD_H][WORLD_W];
static int curWorldX = 0;
static int curWorldY = 0;
static MapState *curMap = NULL;

int game_running = 1;
int game_player_won = 0;
int game_tick_count = 0;
int game_player_lives = 3;
int game_score = 0;
static int invincible_frames = 0; // frames remaining of invincibility
static int super_frames = 0; // frames remaining of super attack (spammable)
static int shoot_cooldown_frames = 0; // frames until next allowed shot

// External maps will be loaded from files instead of default hardcoded map

static int clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void load_map_file(int mx, int my) {
    char path[256];
    snprintf(path, sizeof(path), "maps/x%d-y%d.txt", mx, my);
    MapState *m = &world[my][mx];
    FILE *f = fopen(path, "rb");
    if (!f) {
        for (int y = 0; y < MAP_HEIGHT; ++y) {
            for (int x = 0; x < MAP_WIDTH; ++x) m->tiles[y][x] = (y == 0 || y == MAP_HEIGHT - 1 || x == 0 || x == MAP_WIDTH - 1) ? '#' : '.';
            m->tiles[y][MAP_WIDTH] = '\0';
        }
    } else {
        char line[512];
    for (int y = 0; y < MAP_HEIGHT; ++y) {
            if (fgets(line, (int)sizeof(line), f) == NULL) {
                for (; y < MAP_HEIGHT; ++y) {
                    for (int x = 0; x < MAP_WIDTH; ++x) m->tiles[y][x] = '#';
                    m->tiles[y][MAP_WIDTH] = '\0';
                }
                break;
            }
            int len = (int)strcspn(line, "\r\n");
        for (int x = 0; x < MAP_WIDTH; ++x) {
                char c = (x < len) ? line[x] : '#';
                if (c != '#' && c != '.' && c != 'X' && c != 'W' && c != '@' && c != 'S') c = '.';
                m->tiles[y][x] = c;
            }
            m->tiles[y][MAP_WIDTH] = '\0';
        }
        fclose(f);
    }
    memset(m->wallDmg, 0, sizeof(m->wallDmg));
    // Ensure inter-map connectivity: center doors on interior edges
    int midX = MAP_WIDTH / 2;
    int midY = MAP_HEIGHT / 2;
    if (mx > 0) curMap = curMap; // no-op to silence unused warnings in some compilers
    if (mx > 0) m->tiles[midY][0] = '.';
    if (mx < WORLD_W - 1) m->tiles[midY][MAP_WIDTH - 1] = '.';
    if (my > 0) m->tiles[0][midX] = '.';
    if (my < WORLD_H - 1) m->tiles[MAP_HEIGHT - 1][midX] = '.';
    // Ensure a central spawn exists at world center if none provided by files
    if (mx == WORLD_W / 2 && my == WORLD_H / 2) {
        int hasS = 0;
        for (int y = 0; y < MAP_HEIGHT && !hasS; ++y) {
            for (int x = 0; x < MAP_WIDTH && !hasS; ++x) {
                if (m->tiles[y][x] == 'S') hasS = 1;
            }
        }
        if (!hasS) m->tiles[midY][midX] = 'S';
    }
    m->numEnemies = 0;
    m->initialized = 0;
}

static void world_init(void) {
    for (int y = 0; y < WORLD_H; ++y) for (int x = 0; x < WORLD_W; ++x) load_map_file(x, y);
    curWorldX = 0; curWorldY = 0; curMap = &world[curWorldY][curWorldX];
    // In singleplayer, prefer global 'S' across all maps; fallback to '@' in current map.
    // In multiplayer, server is authoritative for our spawn.
    if (!g_mp_active) {
        int found = 0; int smx = 0, smy = 0, sx = 1, sy = 1;
        for (int wy = 0; wy < WORLD_H && !found; ++wy) {
            for (int wx = 0; wx < WORLD_W && !found; ++wx) {
                MapState *m = &world[wy][wx];
                for (int y = 0; y < MAP_HEIGHT && !found; ++y) {
                    for (int x = 0; x < MAP_WIDTH && !found; ++x) {
                        if (m->tiles[y][x] == 'S') { smx = wx; smy = wy; sx = x; sy = y; found = 1; }
                    }
                }
            }
        }
        if (found) { curWorldX = smx; curWorldY = smy; curMap = &world[curWorldY][curWorldX]; playerPos.x = sx; playerPos.y = sy; }
        else {
            int foundAt = 0;
            for (int y = 0; y < MAP_HEIGHT && !foundAt; ++y) for (int x = 0; x < MAP_WIDTH && !foundAt; ++x) if (curMap->tiles[y][x] == '@') { playerPos.x = x; playerPos.y = y; curMap->tiles[y][x] = '.'; foundAt = 1; }
            if (!foundAt) { playerPos.x = 1; playerPos.y = 1; }
        }
    } else {
        // Placeholder until server snapshot arrives
        playerPos.x = 0; playerPos.y = 0;
    }
}

void game_init(void) {
    game_running = 1;
    game_player_won = 0;
    game_tick_count = 0;
    world_init();
    playerFacing = DIR_RIGHT;
    for (int i = 0; i < MAX_PROJECTILES; ++i) projectiles[i].active = 0;
    game_player_lives = 3;
    game_score = 0;
}

int game_is_blocked(int x, int y) {
    if (x < 0 || x >= MAP_WIDTH || y < 0 || y >= MAP_HEIGHT) return 1;
    char c = curMap->tiles[y][x];
    return c == '#';
}

void game_spawn_enemies(int count) {
    if (count > MAX_ENEMIES) count = MAX_ENEMIES;
    if (curMap->initialized) return;
    // Do not spawn enemies on the starting map (map containing 'S')
    int hasSpawn = 0;
    for (int y = 0; y < MAP_HEIGHT && !hasSpawn; ++y) {
        for (int x = 0; x < MAP_WIDTH && !hasSpawn; ++x) {
            if (curMap->tiles[y][x] == 'S') hasSpawn = 1;
        }
    }
    if (hasSpawn) { curMap->numEnemies = 0; curMap->initialized = 1; return; }
    curMap->numEnemies = count;
    for (int i = 0; i < curMap->numEnemies; ++i) {
        curMap->enemies[i].isAlive = 1;
        curMap->enemies[i].hp = 2;
        for (int attempt = 0; attempt < 1000; ++attempt) {
            int x = rand() % MAP_WIDTH;
            int y = rand() % MAP_HEIGHT;
            if (game_is_blocked(x, y)) continue;
            if ((x == playerPos.x && y == playerPos.y)) continue;
            if (abs(x - playerPos.x) + abs(y - playerPos.y) < 6) continue;
            curMap->enemies[i].pos.x = x;
            curMap->enemies[i].pos.y = y;
            break;
        }
    }
    curMap->initialized = 1;
}

int game_is_enemy_at(int x, int y) {
    for (int i = 0; i < curMap->numEnemies; ++i) {
        if (curMap->enemies[i].isAlive && curMap->enemies[i].pos.x == x && curMap->enemies[i].pos.y == y) return 1;
    }
    return 0;
}

int game_move_enemies(void) {
    if (g_mp_active) return 0; // disabled in multiplayer; enemies come from server
    int moved = 0;
    for (int i = 0; i < curMap->numEnemies; ++i) {
        if (!curMap->enemies[i].isAlive) continue;
        int dir = rand() % 4;
        int dx = 0, dy = 0;
        switch (dir) { case 0: dy = -1; break; case 1: dy = 1; break; case 2: dx = -1; break; case 3: dx = 1; break; }
        int nx = clamp(curMap->enemies[i].pos.x + dx, 0, MAP_WIDTH - 1);
        int ny = clamp(curMap->enemies[i].pos.y + dy, 0, MAP_HEIGHT - 1);
        if (!game_is_blocked(nx, ny)) {
            int occupied = 0;
            for (int j = 0; j < curMap->numEnemies; ++j) {
                if (j == i || !curMap->enemies[j].isAlive) continue;
                if (curMap->enemies[j].pos.x == nx && curMap->enemies[j].pos.y == ny) { occupied = 1; break; }
            }
            if (!occupied) {
                if (curMap->enemies[i].pos.x != nx || curMap->enemies[i].pos.y != ny) moved = 1;
                curMap->enemies[i].pos.x = nx;
                curMap->enemies[i].pos.y = ny;
            }
        }
    }
    return moved;
}

static int try_enter_map(int newWorldX, int newWorldY, int targetX, int targetY) {
    if (newWorldX < 0 || newWorldX >= WORLD_W || newWorldY < 0 || newWorldY >= WORLD_H) return 0;
    MapState *next = &world[newWorldY][newWorldX];
    int tx = clamp(targetX, 0, MAP_WIDTH - 1);
    int ty = clamp(targetY, 0, MAP_HEIGHT - 1);
    // Only allow transition if the exact entry cell in the next map is open
    if (next->tiles[ty][tx] == '#') return 0;
    curWorldX = newWorldX; curWorldY = newWorldY; curMap = next;
    for (int i = 0; i < MAX_PROJECTILES; ++i) projectiles[i].active = 0;
    if (!curMap->initialized) game_spawn_enemies(4);
    playerPos.x = tx; playerPos.y = ty;
    return 1;
}

int game_attempt_move_player(int dx, int dy) {
    int nx = playerPos.x + dx;
    int ny = playerPos.y + dy;
    if (dx < 0) playerFacing = DIR_LEFT; else if (dx > 0) playerFacing = DIR_RIGHT; else if (dy < 0) playerFacing = DIR_UP; else if (dy > 0) playerFacing = DIR_DOWN;
    if (g_mp_active) return 0; // server authoritative; do not move locally
    if (nx >= 0 && nx < MAP_WIDTH && ny >= 0 && ny < MAP_HEIGHT) {
    if (!game_is_blocked(nx, ny)) {
        if (playerPos.x != nx || playerPos.y != ny) { playerPos.x = nx; playerPos.y = ny; return 1; }
    }
        return 0;
    }
    // When crossing world boundaries, preserve the player's current coordinate on the non-crossing axis
    int entryX = playerPos.x;
    int entryY = playerPos.y;
    if (nx < 0) return try_enter_map(curWorldX - 1, curWorldY, MAP_WIDTH - 1, entryY);
    if (nx >= MAP_WIDTH) return try_enter_map(curWorldX + 1, curWorldY, 0, entryY);
    if (ny < 0) return try_enter_map(curWorldX, curWorldY - 1, entryX, MAP_HEIGHT - 1);
    if (ny >= MAP_HEIGHT) return try_enter_map(curWorldX, curWorldY + 1, entryX, 0);
    return 0;
}

void game_check_win_lose(void) {
    if (g_mp_active) {
        // No local win/lose logic in MP mode
        return;
    }
    // Immune to enemy contact on spawn map
    int onSpawnMap = 0; for (int sy = 0; sy < MAP_HEIGHT && !onSpawnMap; ++sy) for (int sx = 0; sx < MAP_WIDTH && !onSpawnMap; ++sx) if (curMap->tiles[sy][sx] == 'S') onSpawnMap = 1;
    if (game_is_enemy_at(playerPos.x, playerPos.y) && !onSpawnMap && invincible_frames <= 0) {
        // decrement HP and grant temporary invincibility
        if (game_player_lives > 0) game_player_lives--;
        if (game_player_lives <= 0) {
            // Respawn at nearest/global spawn 'S', reset HP and score
            int found = 0; int sx = 1, sy = 1; int smx = curWorldX, smy = curWorldY;
            for (int my = 0; my < WORLD_H && !found; ++my) {
                for (int mx = 0; mx < WORLD_W && !found; ++mx) {
                    for (int yy = 0; yy < MAP_HEIGHT && !found; ++yy) {
                        for (int xx = 0; xx < MAP_WIDTH && !found; ++xx) {
                            if (world[my][mx].tiles[yy][xx] == 'S') { smx = mx; smy = my; sx = xx; sy = yy; found = 1; }
                        }
                    }
                }
            }
            curWorldX = smx; curWorldY = smy; curMap = &world[curWorldY][curWorldX];
            playerPos.x = sx; playerPos.y = sy;
            game_player_lives = 3;
            game_score = 0;
            for (int i = 0; i < MAX_PROJECTILES; ++i) projectiles[i].active = 0;
            invincible_frames = 180;
            return;
        }
        invincible_frames = 180;
        return;
    }
    // Restore lives when stepping on 'X'
    if (curMap && curMap->tiles[playerPos.y][playerPos.x] == 'X') {
        if (game_player_lives < 3) game_player_lives = 3;
        super_frames = 300; // 5s at 60 FPS
        invincible_frames = 180; // 3s at 60 FPS
        curMap->tiles[playerPos.y][playerPos.x] = '.'; // consume the X
    }
    if (curMap && curMap->tiles[playerPos.y][playerPos.x] == 'W') { game_running = 0; game_player_won = 1; }
}

static int is_wall(int x, int y) {
    if (x < 0 || x >= MAP_WIDTH || y < 0 || y >= MAP_HEIGHT) return 0;
    return curMap->tiles[y][x] == '#';
}

static void damage_wall(int x, int y) {
    if (!is_wall(x, y)) return;
    if (curMap->wallDmg[y][x] < 4) curMap->wallDmg[y][x]++;
    else { curMap->tiles[y][x] = '.'; curMap->wallDmg[y][x] = 0; }
}

void game_player_shoot(void) {
    if (g_mp_active) return; // client doesn't fire locally in MP
    if (super_frames <= 0 && shoot_cooldown_frames > 0) return;
    for (int i = 0; i < MAX_PROJECTILES; ++i) {
        if (!projectiles[i].active) {
            projectiles[i].active = 1;
            projectiles[i].pos = playerPos;
            projectiles[i].dir = playerFacing;
            if (super_frames <= 0) shoot_cooldown_frames = 12; // ~200ms at 60 FPS
            return;
        }
    }
}

static void step_projectile(Projectile *p) {
    int dx = 0, dy = 0;
    switch (p->dir) { case DIR_UP: dy = -1; break; case DIR_DOWN: dy = 1; break; case DIR_LEFT: dx = -1; break; case DIR_RIGHT: dx = 1; break; }
    int nx = clamp(p->pos.x + dx, 0, MAP_WIDTH - 1);
    int ny = clamp(p->pos.y + dy, 0, MAP_HEIGHT - 1);
    if (nx == p->pos.x && ny == p->pos.y) { p->active = 0; return; }
    for (int i = 0; i < curMap->numEnemies; ++i) {
        if (curMap->enemies[i].isAlive && curMap->enemies[i].pos.x == nx && curMap->enemies[i].pos.y == ny) {
            if (curMap->enemies[i].hp > 0) curMap->enemies[i].hp--;
            if (curMap->enemies[i].hp <= 0) { curMap->enemies[i].isAlive = 0; game_score += 1; }
            p->active = 0;
            return;
        }
    }
    if (is_wall(nx, ny)) { damage_wall(nx, ny); p->active = 0; return; }
    p->pos.x = nx; p->pos.y = ny;
}

int game_update_projectiles(void) {
    if (g_mp_active) return 0; // disabled in multiplayer; server bullets are rendered via overlay
    int changed = 0;
    for (int i = 0; i < MAX_PROJECTILES; ++i) {
        if (projectiles[i].active) {
            Vec2 before = projectiles[i].pos;
            step_projectile(&projectiles[i]);
            if (!projectiles[i].active || projectiles[i].pos.x != before.x || projectiles[i].pos.y != before.y) changed = 1;
        }
    }
    return changed;
}

int game_tick_status(void) {
    int changed = 0;
    if (invincible_frames > 0) { invincible_frames--; changed = 1; }
    if (super_frames > 0) { super_frames--; changed = 1; }
    if (shoot_cooldown_frames > 0) { shoot_cooldown_frames--; }
    return changed;
}

void game_draw(void) {
    // Larger buffer and safe appends to avoid truncation issues on some terminals
    char frame[65536];
    int pos = 0;
    int cap = (int)sizeof(frame);
    // Safe append macro: advances by actual written bytes; clamps on truncation
    #define APPEND_FMT(...) do { int __rem = cap - pos; if (__rem > 0) { int __w = snprintf(frame + pos, __rem, __VA_ARGS__); if (__w < 0) { /* ignore */ } else if (__w >= __rem) { pos = cap; } else { pos += __w; } } } while (0)
    APPEND_FMT("\x1b[r\x1b[2J\x1b[H");
    for (int y = 0; y < MAP_HEIGHT; ++y) {
        APPEND_FMT("\x1b[%d;1H\x1b[K", y + 1);
        for (int x = 0; x < MAP_WIDTH; ++x) {
            const char *color = TERM_FG_WHITE;
            char out;
            if (!g_mp_active && x == playerPos.x && y == playerPos.y) {
                // Flicker player during invincibility: toggle visible every ~8 frames
                int visible = (invincible_frames <= 0) || ((invincible_frames / 8) % 2 == 0);
                out = visible ? '@' : ' ';
                color = TERM_FG_BRIGHT_CYAN; // player
            } else if (!g_mp_active && game_is_enemy_at(x, y)) {
                out = 'E';
                color = TERM_FG_BRIGHT_RED; // enemies
            } else {
                int drew = 0;
                // Projectiles (local)
                if (!g_mp_active) {
                    for (int pi = 0; pi < MAX_PROJECTILES; ++pi) {
                        if (projectiles[pi].active && projectiles[pi].pos.x == x && projectiles[pi].pos.y == y) {
                            out = '*';
                            color = TERM_FG_BRIGHT_GREEN;
                            drew = 1;
                            break;
                        }
                    }
                }
                // Remote bullets overlay (same world only)
                if (!drew && g_mp_active) {
                    for (int bi = 0; bi < MAX_REMOTE_BULLETS; ++bi) {
                        if (!g_remote_bullets[bi].active) continue;
                        if (g_remote_bullets[bi].worldX != curWorldX || g_remote_bullets[bi].worldY != curWorldY) continue;
                        int bx = g_remote_bullets[bi].pos.x;
                        int by = g_remote_bullets[bi].pos.y;
                        extern int game_tick_count;
                        int bticks = game_tick_count - g_remote_bullets[bi].lastUpdateTick;
                        int sameWorldHistoryB = (g_remote_bullets[bi].lastWorldX == g_remote_bullets[bi].worldX && g_remote_bullets[bi].lastWorldY == g_remote_bullets[bi].worldY);
                        if (sameWorldHistoryB) {
                            int lx = g_remote_bullets[bi].lastPos.x;
                            int ly = g_remote_bullets[bi].lastPos.y;
                            int mdx = bx - lx;
                            int mdy = by - ly;
                            if (bticks > 0 && bticks < REMOTE_INTERP_TICKS) {
                                if (lx != bx || ly != by) {
                                    int sx = lx, sy = ly;
                                    if (bx > lx) sx = lx + 1; else if (bx < lx) sx = lx - 1;
                                    if (by > ly) sy = ly + 1; else if (by < ly) sy = ly - 1;
                                    bx = sx; by = sy;
                                }
                            } else if (bticks >= REMOTE_INTERP_TICKS && bticks < REMOTE_EXTRAP_TICKS) {
                                int sdx = (mdx > 0) ? 1 : (mdx < 0 ? -1 : 0);
                                int sdy = (mdy > 0) ? 1 : (mdy < 0 ? -1 : 0);
                                int ex = clamp(bx + sdx, 0, MAP_WIDTH - 1);
                                int ey = clamp(by + sdy, 0, MAP_HEIGHT - 1);
                                if (!is_wall(ex, ey)) { bx = ex; by = ey; }
                            }
                        }
                        if (bx == x && by == y) {
                            out = '*';
                            color = TERM_FG_BRIGHT_GREEN;
                            drew = 1;
                            break;
                        }
                    }
                }
                // Remote enemies overlay on base tiles when in MP
                if (!drew && g_mp_active) {
                    for (int ei = 0; ei < MAX_REMOTE_ENEMIES; ++ei) {
                        if (g_remote_enemies[ei].active && g_remote_enemies[ei].worldX == curWorldX && g_remote_enemies[ei].worldY == curWorldY && g_remote_enemies[ei].pos.x == x && g_remote_enemies[ei].pos.y == y) {
                            out = 'E';
                            color = TERM_FG_BRIGHT_RED;
                            drew = 1;
                            break;
                        }
                    }
                }
                if (!drew) {
                    char c = curMap->tiles[y][x];
                    if (c == '#') { out = '#'; color = TERM_FG_BRIGHT_WHITE; }
                    else if (c == '.') { out = '.'; color = TERM_FG_BRIGHT_BLACK; }
                    else if (c == 'X') { out = 'X'; color = TERM_FG_BRIGHT_YELLOW; }
                    else if (c == 'W') { out = 'W'; color = TERM_FG_BRIGHT_MAGENTA; }
                    else if (c == '@') { out = '.'; color = TERM_FG_BRIGHT_BLACK; }
                    else { out = ' '; color = TERM_FG_WHITE; }
                }
            }
            APPEND_FMT("%s%c%s", color, out, TERM_SGR_RESET);
        }
        APPEND_FMT("");
    }
    // Overlay remote players for this map (includes self in MP)
    if (g_mp_active) {
        for (int i = 0; i < MAX_REMOTE_PLAYERS; ++i) {
            if (!g_remote_players[i].active) continue;
            if (g_remote_players[i].worldX == curWorldX && g_remote_players[i].worldY == curWorldY) {
                // Simple smoothing & extrapolation
                int rx = g_remote_players[i].pos.x;
                int ry = g_remote_players[i].pos.y;
                extern int game_tick_count;
                int ticksSince = game_tick_count - g_remote_players[i].lastUpdateTick;
                int sameWorldHistory = (g_remote_players[i].lastWorldX == g_remote_players[i].worldX && g_remote_players[i].lastWorldY == g_remote_players[i].worldY);
                if (sameWorldHistory) {
                    int lx = g_remote_players[i].lastPos.x;
                    int ly = g_remote_players[i].lastPos.y;
                    int mdx = rx - lx;
                    int mdy = ry - ly;
                    if (ticksSince > 0 && ticksSince < REMOTE_INTERP_TICKS) {
                        // interpolate one step from last -> current
                        if (lx != rx || ly != ry) {
                            int sx = lx, sy = ly;
                            if (rx > lx) sx = lx + 1; else if (rx < lx) sx = lx - 1;
                            if (ry > ly) sy = ly + 1; else if (ry < ly) sy = ly - 1;
                            rx = sx; ry = sy;
                        }
                    } else if (ticksSince >= REMOTE_INTERP_TICKS && ticksSince < REMOTE_EXTRAP_TICKS) {
                        // extrapolate one step beyond current in last movement direction
                        int sdx = (mdx > 0) ? 1 : (mdx < 0 ? -1 : 0);
                        int sdy = (mdy > 0) ? 1 : (mdy < 0 ? -1 : 0);
                        int ex = clamp(rx + sdx, 0, MAP_WIDTH - 1);
                        int ey = clamp(ry + sdy, 0, MAP_HEIGHT - 1);
                        if (!game_is_blocked(ex, ey)) { rx = ex; ry = ey; }
                    }
                }
                if (rx >= 0 && rx < MAP_WIDTH && ry >= 0 && ry < MAP_HEIGHT) {
                    const char *pcolor = TERM_FG_BRIGHT_BLUE;
                    switch (g_remote_players[i].colorIndex % 6) {
                        case 0: pcolor = TERM_FG_BRIGHT_BLUE; break;
                        case 1: pcolor = TERM_FG_BRIGHT_MAGENTA; break;
                        case 2: pcolor = TERM_FG_BRIGHT_CYAN; break;
                        case 3: pcolor = TERM_FG_BRIGHT_GREEN; break;
                        case 4: pcolor = TERM_FG_BRIGHT_YELLOW; break;
                        case 5: pcolor = TERM_FG_BRIGHT_RED; break;
                    }
                    APPEND_FMT("\x1b[%d;%dH%s@%s", ry + 1, rx + 1, pcolor, TERM_SGR_RESET);
                }
            }
        }
    }
    // Ensure cursor is moved below the map before printing HUD
    APPEND_FMT("\x1b[%d;%dH", MAP_HEIGHT + 1, 1);
    int termRows = 24, termCols = 80;
    term_get_size(&termRows, &termCols);
    if (g_mp_active) {
        int myhp = 0;
        if (g_my_player_id >= 0 && g_my_player_id < MAX_REMOTE_PLAYERS && g_remote_players[g_my_player_id].active) myhp = g_remote_players[g_my_player_id].hp;
        // HP and Ping directly under map
        extern int g_net_ping_ms;
        if (g_net_ping_ms >= 0) APPEND_FMT("\x1b[KHP: %d   Ping: %d ms\r\n", myhp, g_net_ping_ms);
        else APPEND_FMT("\x1b[KHP: %d   Ping: -- ms\r\n", myhp);
    } else {
        // HP directly under map (score will be shown in the scoreboard)
        APPEND_FMT("\x1b[KHP: %d\r\n", game_player_lives);
    }

    // Scoreboard (left) and Minimap (right) on the same row
    int baseRow = MAP_HEIGHT + 2; // start right after HP line
    int leftCol = 1;
    int rows = 4, cols = 4;
    int cellW = 7; // '@' + 4 digits + 2 spaces = 7 columns
    int gap = 4;
    int rightCol = leftCol + cols * cellW + gap; // place minimap to the right of the scoreboard

    int needCols = rightCol + WORLD_W; // rough width including minimap
    int needRows = baseRow + 1 + (rows > WORLD_H ? rows : WORLD_H) + 2; // up to hints
    int showMinimap = (termCols >= needCols);
    int showScoreboard = 1;
    if (!showMinimap) {
        // If width tight, keep scoreboard and drop minimap
        rightCol = termCols + 1; // place offscreen
    }

    // Titles on the same line
    APPEND_FMT("\x1b[%d;%dH\x1b[KScoreboard", baseRow, leftCol);
    if (showMinimap) APPEND_FMT("\x1b[%d;%dH\x1b[KMinimap\r\n", baseRow, rightCol);
    else APPEND_FMT("\r\n");

    // Render a 4x4 table of player slots, showing colored '@' and a simple score
    for (int r = 0; r < rows; ++r) {
        APPEND_FMT("\x1b[%d;%dH\x1b[K", baseRow + 1 + r, leftCol);
        for (int c = 0; c < cols; ++c) {
            int idx = r * cols + c;
            const char *pcolor = TERM_FG_BRIGHT_BLACK;
            int showAt = 0;
            int scoreVal = 0;
            if (g_mp_active) {
                if (idx >= 0 && idx < MAX_REMOTE_PLAYERS && g_remote_players[idx].active) {
                    showAt = 1;
                    // Show server-reported score for each remote player
                    scoreVal = g_remote_players[idx].score;
                    switch (g_remote_players[idx].colorIndex % 6) {
                        case 0: pcolor = TERM_FG_BRIGHT_BLUE; break;
                        case 1: pcolor = TERM_FG_BRIGHT_MAGENTA; break;
                        case 2: pcolor = TERM_FG_BRIGHT_CYAN; break;
                        case 3: pcolor = TERM_FG_BRIGHT_GREEN; break;
                        case 4: pcolor = TERM_FG_BRIGHT_YELLOW; break;
                        case 5: pcolor = TERM_FG_BRIGHT_RED; break;
                        default: pcolor = TERM_FG_BRIGHT_WHITE; break;
                    }
                }
            } else {
                if (idx == 0) { showAt = 1; pcolor = TERM_FG_BRIGHT_CYAN; scoreVal = game_score; }
            }
            if (showAt) {
                APPEND_FMT("%s@%s%4d  ", pcolor, TERM_SGR_RESET, scoreVal);
            } else {
                APPEND_FMT("       ");
            }
        }
    }

    // Render a 9x9 grid of '.' with current map marked 'X' and players '@' (side-by-side on the same rows)
    int miniRow = baseRow + 1; // align top rows of both sections
    if (showMinimap) {
    for (int my = 0; my < WORLD_H; ++my) {
        // Position cursor at the start of this minimap row
        APPEND_FMT("\x1b[%d;%dH\x1b[K", miniRow + my, rightCol);
        for (int mx = 0; mx < WORLD_W; ++mx) {
            char ch = '.';
            const char *pcolor = TERM_FG_BRIGHT_BLACK;
            int isCurrentMap = (mx == curWorldX && my == curWorldY);
            if (isCurrentMap) { ch = 'X'; pcolor = TERM_FG_BRIGHT_WHITE; }
            if (g_mp_active) {
                for (int i = 0; i < MAX_REMOTE_PLAYERS; ++i) {
                    if (!g_remote_players[i].active) continue;
                    if (g_remote_players[i].worldX == mx && g_remote_players[i].worldY == my) {
                        // Keep 'X' on the current map; otherwise show '@' for maps with players
                        if (!isCurrentMap) ch = '@';
                        switch (g_remote_players[i].colorIndex % 6) {
                            case 0: pcolor = TERM_FG_BRIGHT_BLUE; break;
                            case 1: pcolor = TERM_FG_BRIGHT_MAGENTA; break;
                            case 2: pcolor = TERM_FG_BRIGHT_CYAN; break;
                            case 3: pcolor = TERM_FG_BRIGHT_GREEN; break;
                            case 4: pcolor = TERM_FG_BRIGHT_YELLOW; break;
                            case 5: pcolor = TERM_FG_BRIGHT_RED; break;
                        }
                        break;
                    }
                }
            } else {
                // Singleplayer: keep 'X' for current map; no overlay
            }
            APPEND_FMT("%s%c%s", pcolor, ch, TERM_SGR_RESET);
        }
    }
    }

    // Hints below both sections
    int hintsRow = showMinimap ? (miniRow + WORLD_H + 1) : (baseRow + rows + 1);
    APPEND_FMT("\x1b[%d;%dH\x1b[KUse WASD/Arrows to move, Space to shoot.\r\n", hintsRow, 1);
    if (!g_mp_active) {
        APPEND_FMT("\x1b[KFind purple W to win. Press Q to quit.\r\n");
    } else {
        APPEND_FMT("\x1b[KPress Q to quit.\r\n");
    }

    fwrite(frame, 1, (size_t)(pos < cap ? pos : cap), stdout);
    fflush(stdout);
#ifndef _WIN32
    tcdrain(STDOUT_FILENO);
#endif
    #undef APPEND_FMT
}

void game_draw_loading(int tick) {
    // Simple animated loading screen the size of the map area
    char frame[65536];
    int pos = 0; int cap = (int)sizeof(frame);
    #define APPEND_FMT(...) do { int __rem = cap - pos; if (__rem > 0) { int __w = snprintf(frame + pos, __rem, __VA_ARGS__); if (__w < 0) { /* ignore */ } else if (__w >= __rem) { pos = cap; } else { pos += __w; } } } while (0)
    APPEND_FMT("\x1b[r\x1b[2J\x1b[H");
    // Draw background with dim dots
    for (int y = 0; y < MAP_HEIGHT; ++y) {
        APPEND_FMT("\x1b[%d;1H\x1b[K", y + 1);
        for (int x = 0; x < MAP_WIDTH; ++x) {
            const char *color = TERM_FG_BRIGHT_BLACK;
            char out = '.';
            APPEND_FMT("%s%c%s", color, out, TERM_SGR_RESET);
        }
    }
    // Sparkles: pseudo-random deterministic per tick to avoid rand() here
    int sparkCount = 30; // number of sparkles per frame
    for (int i = 0; i < sparkCount; ++i) {
        unsigned int seed = (unsigned int)(tick * 1103515245u + 12345u + i * 2654435761u);
        int sx = (int)((seed >> 16) % MAP_WIDTH);
        int sy = (int)((seed >> 8) % MAP_HEIGHT);
        const char *scolor = TERM_FG_BRIGHT_YELLOW;
        char schar = '*';
        // position cursor and draw sparkle
        APPEND_FMT("\x1b[%d;%dH%s%c%s", sy + 1, sx + 1, scolor, schar, TERM_SGR_RESET);
    }
    // Centered LOADING text
    const char *text = "LOADING";
    int tlen = (int)strlen(text);
    int cx = (MAP_WIDTH - tlen) / 2;
    int cy = MAP_HEIGHT / 2;
    APPEND_FMT("\x1b[%d;%dH%s%s%s", cy + 1, cx + 1, TERM_FG_BRIGHT_WHITE, text, TERM_SGR_RESET);

    // Draw a subtle border around the map area using bright white
    // Top and bottom borders
    for (int x = 0; x < MAP_WIDTH; ++x) {
        APPEND_FMT("\x1b[%d;%dH%s-%s", 1, x + 1, TERM_FG_BRIGHT_WHITE, TERM_SGR_RESET);
        APPEND_FMT("\x1b[%d;%dH%s-%s", MAP_HEIGHT, x + 1, TERM_FG_BRIGHT_WHITE, TERM_SGR_RESET);
    }
    for (int y = 0; y < MAP_HEIGHT; ++y) {
        APPEND_FMT("\x1b[%d;%dH%s|%s", y + 1, 1, TERM_FG_BRIGHT_WHITE, TERM_SGR_RESET);
        APPEND_FMT("\x1b[%d;%dH%s|%s", y + 1, MAP_WIDTH, TERM_FG_BRIGHT_WHITE, TERM_SGR_RESET);
    }

    fwrite(frame, 1, (size_t)(pos < cap ? pos : cap), stdout);
    fflush(stdout);
#ifndef _WIN32
    tcdrain(STDOUT_FILENO);
#endif
    #undef APPEND_FMT
}

// --- MP helpers ---
void game_mp_set_tile(int wx, int wy, int x, int y, char tile) {
    if (wx < 0 || wx >= WORLD_W || wy < 0 || wy >= WORLD_H) return;
    if (x < 0 || x >= MAP_WIDTH || y < 0 || y >= MAP_HEIGHT) return;
    world[wy][wx].tiles[y][x] = tile;
    if (wx == curWorldX && wy == curWorldY) {
        // ensure curMap points to updated map
        curMap = &world[curWorldY][curWorldX];
    }
}

int game_mp_get_cur_world_x(void) { return curWorldX; }
int game_mp_get_cur_world_y(void) { return curWorldY; }

void game_mp_set_self(int wx, int wy, int x, int y) {
    if (wx < 0 || wx >= WORLD_W || wy < 0 || wy >= WORLD_H) return;
    curWorldX = wx;
    curWorldY = wy;
    curMap = &world[curWorldY][curWorldX];
    playerPos.x = x;
    playerPos.y = y;
}


