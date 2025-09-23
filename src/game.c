#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "types.h"
#include "term.h"
#include "mp.h"

static Vec2 playerPos;
static Direction playerFacing = DIR_RIGHT;
static Projectile projectiles[MAX_PROJECTILES];

// World configuration (3x3 grid: x0-y0 to x2-y2)
#define WORLD_W 3
#define WORLD_H 3

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
                if (c != '#' && c != '.' && c != 'X' && c != 'W' && c != '@') c = '.';
                m->tiles[y][x] = c;
            }
            m->tiles[y][MAP_WIDTH] = '\0';
        }
        fclose(f);
    }
    memset(m->wallDmg, 0, sizeof(m->wallDmg));
    m->numEnemies = 0;
    m->initialized = 0;
}

static void world_init(void) {
    for (int y = 0; y < WORLD_H; ++y) for (int x = 0; x < WORLD_W; ++x) load_map_file(x, y);
    curWorldX = 0; curWorldY = 0; curMap = &world[curWorldY][curWorldX];
    // Find spawn '@'
    int found = 0;
    for (int y = 0; y < MAP_HEIGHT && !found; ++y) for (int x = 0; x < MAP_WIDTH && !found; ++x) if (curMap->tiles[y][x] == '@') { playerPos.x = x; playerPos.y = y; curMap->tiles[y][x] = '.'; found = 1; }
    if (!found) { playerPos.x = 1; playerPos.y = 1; }
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
    if (nx >= 0 && nx < MAP_WIDTH && ny >= 0 && ny < MAP_HEIGHT) {
    if (!game_is_blocked(nx, ny)) {
        if (playerPos.x != nx || playerPos.y != ny) { playerPos.x = nx; playerPos.y = ny; return 1; }
    }
        return 0;
    }
    if (nx < 0) return try_enter_map(curWorldX - 1, curWorldY, MAP_WIDTH - 1, ny);
    if (nx >= MAP_WIDTH) return try_enter_map(curWorldX + 1, curWorldY, 0, ny);
    if (ny < 0) return try_enter_map(curWorldX, curWorldY - 1, nx, MAP_HEIGHT - 1);
    if (ny >= MAP_HEIGHT) return try_enter_map(curWorldX, curWorldY + 1, nx, 0);
    return 0;
}

void game_check_win_lose(void) {
    if (game_is_enemy_at(playerPos.x, playerPos.y) && invincible_frames <= 0) {
        // lose a life and respawn at current map's spawn '@' if available; otherwise top-left open
        game_player_lives--;
        if (game_player_lives <= 0) {
            // Reset lives and score, and teleport to a random different map
            game_player_lives = 3;
            game_score = 0;
            int fromX = curWorldX, fromY = curWorldY;
            int placedGlobal = 0;
            int tries = 100;
            while (tries-- > 0 && !placedGlobal) {
                int rx = rand() % WORLD_W;
                int ry = rand() % WORLD_H;
                if (rx == fromX && ry == fromY) continue;
                MapState *m = &world[ry][rx];
                for (int y2 = 0; y2 < MAP_HEIGHT && !placedGlobal; ++y2) {
                    for (int x2 = 0; x2 < MAP_WIDTH && !placedGlobal; ++x2) {
                        char c = m->tiles[y2][x2];
                        if (c == '.' || c == '@') { curWorldX = rx; curWorldY = ry; curMap = m; playerPos.x = x2; playerPos.y = y2; placedGlobal = 1; }
                    }
                }
            }
            if (!placedGlobal) {
                // Fallback deterministic search
                for (int ry = 0; ry < WORLD_H && !placedGlobal; ++ry) {
                    for (int rx = 0; rx < WORLD_W && !placedGlobal; ++rx) {
                        if (rx == fromX && ry == fromY) continue;
                        MapState *m = &world[ry][rx];
                        for (int y2 = 0; y2 < MAP_HEIGHT && !placedGlobal; ++y2) {
                            for (int x2 = 0; x2 < MAP_WIDTH && !placedGlobal; ++x2) {
                                char c = m->tiles[y2][x2];
                                if (c == '.' || c == '@') { curWorldX = rx; curWorldY = ry; curMap = m; playerPos.x = x2; playerPos.y = y2; placedGlobal = 1; }
                            }
                        }
                    }
                }
            }
            for (int i = 0; i < MAX_PROJECTILES; ++i) projectiles[i].active = 0;
            // continue running
            return;
        }
        // Remain in place, grant 3 seconds invincibility (~180 frames at 60 FPS)
        invincible_frames = 180;
        return;
    }
    // Restore lives when stepping on 'X'
    if (curMap && curMap->tiles[playerPos.y][playerPos.x] == 'X') {
        if (game_player_lives < 3) game_player_lives = 3;
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
    for (int i = 0; i < MAX_PROJECTILES; ++i) {
        if (!projectiles[i].active) {
            projectiles[i].active = 1;
            projectiles[i].pos = playerPos;
            projectiles[i].dir = playerFacing;
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
    return changed;
}

void game_draw(void) {
    // Increase buffer to account for ANSI color sequences per cell
    char frame[16384];
    int pos = 0;
    int cap = (int)sizeof(frame);
    int n = snprintf(frame + pos, cap - pos, "\x1b[2J\x1b[H");
    if (n > 0) { pos += n; if (pos > cap) pos = cap; }
    for (int y = 0; y < MAP_HEIGHT; ++y) {
        for (int x = 0; x < MAP_WIDTH; ++x) {
            const char *color = TERM_FG_WHITE;
            char out;
            if (x == playerPos.x && y == playerPos.y) {
                // Flicker player during invincibility: toggle visible every ~8 frames
                int visible = (invincible_frames <= 0) || ((invincible_frames / 8) % 2 == 0);
                out = visible ? '@' : ' ';
                color = TERM_FG_BRIGHT_CYAN; // player
            } else if (game_is_enemy_at(x, y)) {
                out = 'E';
                color = TERM_FG_BRIGHT_RED; // enemies
            } else {
                int drew = 0;
                // Projectiles
                for (int pi = 0; pi < MAX_PROJECTILES; ++pi) {
                    if (projectiles[pi].active && projectiles[pi].pos.x == x && projectiles[pi].pos.y == y) {
                        out = '*';
                        color = TERM_FG_BRIGHT_GREEN;
                        drew = 1;
                        break;
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
            n = snprintf(frame + pos, cap - pos, "%s%c%s", color, out, TERM_SGR_RESET);
            if (n > 0) { pos += n; if (pos > cap) pos = cap; }
        }
        if (pos < cap) frame[pos++] = '\n';
    }
    // Overlay remote players for this map
    if (g_mp_active) {
        for (int i = 0; i < MAX_REMOTE_PLAYERS; ++i) {
            if (!g_remote_players[i].active) continue;
            if (g_remote_players[i].worldX == curWorldX && g_remote_players[i].worldY == curWorldY) {
                int rx = g_remote_players[i].pos.x;
                int ry = g_remote_players[i].pos.y;
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
                    n = snprintf(frame + pos, cap - pos, "\x1b[%d;%dH%s@%s", ry + 1, rx + 1, pcolor, TERM_SGR_RESET);
                    if (n > 0) { pos += n; if (pos > cap) pos = cap; }
                }
            }
        }
    }
    n = snprintf(frame + pos, cap - pos, "\nLives: %d    Score: %d    Location: (%d,%d)\nUse WASD/Arrows to move, Space to shoot.\nFind purple W to win. Press Q to quit.\n", game_player_lives, game_score, curWorldX * MAP_WIDTH + playerPos.x, curWorldY * MAP_HEIGHT + playerPos.y);
    if (n > 0) { pos += n; if (pos > cap) pos = cap; }
    fwrite(frame, 1, (size_t)(pos < cap ? pos : cap), stdout);
    fflush(stdout);
}


