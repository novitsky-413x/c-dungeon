#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "types.h"
#include "term.h"

static char mapData[MAP_HEIGHT][MAP_WIDTH + 1];
static Vec2 playerPos;
static Vec2 exitPos;
static Enemy enemies[MAX_ENEMIES];
static int numEnemies = 0;
static Direction playerFacing = DIR_RIGHT;
static Projectile projectiles[MAX_PROJECTILES];
static unsigned char wallDamage[MAP_HEIGHT][MAP_WIDTH];

int game_running = 1;
int game_player_won = 0;
int game_tick_count = 0;

static const char *defaultMap[MAP_HEIGHT] = {
    "########################################",
    "#@....#...............#................X",
    "#.##..#..#####..####..#..#####..####..#",
    "#.#...#..#...#..#..#..#..#...#..#..#..#",
    "#.#...#..#...#..#..#..#..#...#..#..#..#",
    "#.#...#..#...#..#..#..#..#...#..#..#..#",
    "#.#...#..#####..####..#..#####..####..#",
    "#.#..................................#",
    "#.#..########..########..########..#.#",
    "#.#..#......#..#......#..#......#..#.#",
    "#.#..#......#..#......#..#......#..#.#",
    "#.#..########..########..########..#.#",
    "#.#..................................#",
    "#.#..#####..####..#####..####..#####.#",
    "#.#..#...#..#..#..#...#..#..#..#...#.#",
    "#.#..#...#..#..#..#...#..#..#..#...#.#",
    "#....#...#........#...#........#...#.#",
    "########################################"
};

static int clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void initMap(void) {
    int foundPlayer = 0;
    int foundExit = 0;
    for (int y = 0; y < MAP_HEIGHT; ++y) {
        for (int x = 0; x < MAP_WIDTH; ++x) mapData[y][x] = '#';
        mapData[y][MAP_WIDTH] = '\0';
        const char *src = defaultMap[y];
        for (int x = 0; x < MAP_WIDTH && src[x] != '\0'; ++x) mapData[y][x] = src[x];
        for (int x = 0; x < MAP_WIDTH; ++x) {
            if (mapData[y][x] == '@') { playerPos.x = x; playerPos.y = y; foundPlayer = 1; mapData[y][x] = '.'; }
            else if (mapData[y][x] == 'X') { exitPos.x = x; exitPos.y = y; foundExit = 1; mapData[y][x] = '.'; }
        }
    }
    if (!foundPlayer) {
        int placed = 0;
        for (int y = 1; y < MAP_HEIGHT - 1 && !placed; ++y)
            for (int x = 1; x < MAP_WIDTH - 1 && !placed; ++x)
                if (!mapData[y][x] || mapData[y][x] == '.') { playerPos.x = x; playerPos.y = y; placed = 1; }
        if (!placed) { playerPos.x = 1; playerPos.y = 1; }
    }
    if (!foundExit) {
        int placed = 0;
        for (int y = MAP_HEIGHT - 2; y >= 1 && !placed; --y)
            for (int x = MAP_WIDTH - 2; x >= 1 && !placed; --x)
                if (mapData[y][x] == '.' && (x != playerPos.x || y != playerPos.y)) { exitPos.x = x; exitPos.y = y; placed = 1; }
        if (!placed) { exitPos.x = MAP_WIDTH - 2; exitPos.y = MAP_HEIGHT - 2; }
    }
}

void game_init(void) {
    game_running = 1;
    game_player_won = 0;
    game_tick_count = 0;
    initMap();
    playerFacing = DIR_RIGHT;
    for (int i = 0; i < MAX_PROJECTILES; ++i) projectiles[i].active = 0;
    for (int y = 0; y < MAP_HEIGHT; ++y) for (int x = 0; x < MAP_WIDTH; ++x) wallDamage[y][x] = 0;
    for (int i = 0; i < MAX_ENEMIES; ++i) enemies[i].hp = 2;
}

int game_is_blocked(int x, int y) {
    if (x < 0 || x >= MAP_WIDTH || y < 0 || y >= MAP_HEIGHT) return 1;
    char c = mapData[y][x];
    return c == '#';
}

void game_spawn_enemies(int count) {
    if (count > MAX_ENEMIES) count = MAX_ENEMIES;
    numEnemies = count;
    for (int i = 0; i < numEnemies; ++i) {
        enemies[i].isAlive = 1;
        enemies[i].hp = 2;
        for (int attempt = 0; attempt < 1000; ++attempt) {
            int x = rand() % MAP_WIDTH;
            int y = rand() % MAP_HEIGHT;
            if (game_is_blocked(x, y)) continue;
            if ((x == playerPos.x && y == playerPos.y) || (x == exitPos.x && y == exitPos.y)) continue;
            if (abs(x - playerPos.x) + abs(y - playerPos.y) < 6) continue;
            enemies[i].pos.x = x;
            enemies[i].pos.y = y;
            break;
        }
    }
}

int game_is_enemy_at(int x, int y) {
    for (int i = 0; i < numEnemies; ++i) {
        if (enemies[i].isAlive && enemies[i].pos.x == x && enemies[i].pos.y == y) return 1;
    }
    return 0;
}

int game_move_enemies(void) {
    int moved = 0;
    for (int i = 0; i < numEnemies; ++i) {
        if (!enemies[i].isAlive) continue;
        int dir = rand() % 4;
        int dx = 0, dy = 0;
        switch (dir) { case 0: dy = -1; break; case 1: dy = 1; break; case 2: dx = -1; break; case 3: dx = 1; break; }
        int nx = clamp(enemies[i].pos.x + dx, 0, MAP_WIDTH - 1);
        int ny = clamp(enemies[i].pos.y + dy, 0, MAP_HEIGHT - 1);
        if (!game_is_blocked(nx, ny)) {
            int occupied = 0;
            for (int j = 0; j < numEnemies; ++j) {
                if (j == i || !enemies[j].isAlive) continue;
                if (enemies[j].pos.x == nx && enemies[j].pos.y == ny) { occupied = 1; break; }
            }
            if (!occupied) {
                if (enemies[i].pos.x != nx || enemies[i].pos.y != ny) moved = 1;
                enemies[i].pos.x = nx;
                enemies[i].pos.y = ny;
            }
        }
    }
    return moved;
}

int game_attempt_move_player(int dx, int dy) {
    int nx = clamp(playerPos.x + dx, 0, MAP_WIDTH - 1);
    int ny = clamp(playerPos.y + dy, 0, MAP_HEIGHT - 1);
    if (!game_is_blocked(nx, ny)) {
        if (dx < 0) playerFacing = DIR_LEFT; else if (dx > 0) playerFacing = DIR_RIGHT; else if (dy < 0) playerFacing = DIR_UP; else if (dy > 0) playerFacing = DIR_DOWN;
        if (playerPos.x != nx || playerPos.y != ny) { playerPos.x = nx; playerPos.y = ny; return 1; }
    }
    return 0;
}

void game_check_win_lose(void) {
    if (game_is_enemy_at(playerPos.x, playerPos.y)) { game_running = 0; game_player_won = 0; return; }
    if (playerPos.x == exitPos.x && playerPos.y == exitPos.y) { game_running = 0; game_player_won = 1; }
}

static int is_wall(int x, int y) {
    if (x < 0 || x >= MAP_WIDTH || y < 0 || y >= MAP_HEIGHT) return 0;
    return mapData[y][x] == '#';
}

static void damage_wall(int x, int y) {
    if (!is_wall(x, y)) return;
    if (wallDamage[y][x] < 4) wallDamage[y][x]++;
    else { mapData[y][x] = '.'; wallDamage[y][x] = 0; }
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
    for (int i = 0; i < numEnemies; ++i) {
        if (enemies[i].isAlive && enemies[i].pos.x == nx && enemies[i].pos.y == ny) {
            if (enemies[i].hp > 0) enemies[i].hp--;
            if (enemies[i].hp <= 0) enemies[i].isAlive = 0;
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
                out = '@';
                color = TERM_FG_BRIGHT_CYAN; // player
            } else if (x == exitPos.x && y == exitPos.y) {
                out = 'X';
                color = TERM_FG_BRIGHT_YELLOW; // exit
            } else if (game_is_enemy_at(x, y)) {
                out = 'E';
                color = TERM_FG_BRIGHT_RED; // enemies
            } else {
                int drew = 0;
                for (int pi = 0; pi < MAX_PROJECTILES; ++pi) {
                    if (projectiles[pi].active && projectiles[pi].pos.x == x && projectiles[pi].pos.y == y) {
                        out = '*';
                        color = TERM_FG_BRIGHT_GREEN;
                        drew = 1;
                        break;
                    }
                }
                if (!drew) {
                    char c = mapData[y][x];
                    if (c == '#') { out = '#'; color = TERM_FG_BRIGHT_WHITE; } // walls
                    else if (c == '.') { out = '.'; color = TERM_FG_BRIGHT_BLACK; } // floor
                    else { out = ' '; color = TERM_FG_WHITE; }
                }
            }
            n = snprintf(frame + pos, cap - pos, "%s%c%s", color, out, TERM_SGR_RESET);
            if (n > 0) { pos += n; if (pos > cap) pos = cap; }
        }
        if (pos < cap) frame[pos++] = '\n';
    }
    n = snprintf(frame + pos, cap - pos, "\nUse WASD or Arrow Keys to move. Space to shoot. Reach X, avoid E. Press Q to quit.\n");
    if (n > 0) { pos += n; if (pos > cap) pos = cap; }
    fwrite(frame, 1, (size_t)(pos < cap ? pos : cap), stdout);
    fflush(stdout);
}


