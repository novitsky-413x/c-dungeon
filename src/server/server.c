#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>
typedef int sock_t;
#endif

#include "../types.h"

#define WORLD_W 3
#define WORLD_H 3
#define MAX_CLIENTS MAX_REMOTE_PLAYERS

typedef struct {
    char tiles[MAP_HEIGHT][MAP_WIDTH + 1];
} Map;

typedef struct {
    int active;
    int worldX;
    int worldY;
    Vec2 pos;
    Direction dir;
} SrvBullet;

typedef struct {
    int active;
    int worldX;
    int worldY;
    Vec2 pos;
    int hp;
} SrvEnemy;

typedef struct {
    int connected;
    sock_t sock;
    int worldX, worldY;
    Vec2 pos;
    int color;
    time_t lastActive;
    char addr[64];
    char port[16];
    unsigned long long connId;
} Client;

static Map world[WORLD_H][WORLD_W];
static Client clients[MAX_CLIENTS];
static unsigned long long g_nextConnId = 1ULL;
static SrvBullet bullets[MAX_REMOTE_BULLETS];
static SrvEnemy enemies[WORLD_H][WORLD_W][MAX_ENEMIES];

static FILE *try_open_map(const char *prefix, int mx, int my) {
    char path[256]; snprintf(path, sizeof(path), "%smaps/x%d-y%d.txt", prefix, mx, my);
    return fopen(path, "rb");
}

static void load_map_file(int mx, int my) {
    FILE *f = NULL;
    f = try_open_map("", mx, my);
    if (!f) f = try_open_map("../", mx, my);
    if (!f) f = try_open_map("../../", mx, my);
    Map *m = &world[my][mx];
    if (!f) {
        for (int y = 0; y < MAP_HEIGHT; ++y) {
            for (int x = 0; x < MAP_WIDTH; ++x) m->tiles[y][x] = (y == 0 || y == MAP_HEIGHT-1 || x == 0 || x == MAP_WIDTH-1) ? '#' : '.';
            m->tiles[y][MAP_WIDTH] = '\0';
        }
        return;
    }
    char line[512];
    for (int y = 0; y < MAP_HEIGHT; ++y) {
        if (!fgets(line, sizeof(line), f)) { for (; y < MAP_HEIGHT; ++y) { for (int x = 0; x < MAP_WIDTH; ++x) m->tiles[y][x] = '#'; m->tiles[y][MAP_WIDTH] = '\0'; } break; }
        int len = (int)strcspn(line, "\r\n");
        for (int x = 0; x < MAP_WIDTH; ++x) { char c = (x < len) ? line[x] : '#'; if (c!='#'&&c!='.'&&c!='X'&&c!='W'&&c!='@') c='.'; m->tiles[y][x] = c; }
        m->tiles[y][MAP_WIDTH] = '\0';
    }
    fclose(f);
}

static int is_open(Map *m, int x, int y) { if (x<0||x>=MAP_WIDTH||y<0||y>=MAP_HEIGHT) return 0; return m->tiles[y][x] != '#'; }

static void spawn_enemies_for_map(int mx, int my, int count) {
    if (count > MAX_ENEMIES) count = MAX_ENEMIES;
    for (int i = 0; i < MAX_ENEMIES; ++i) enemies[my][mx][i].active = 0;

    // Collect all open tiles on this map
    Vec2 candidates[MAP_HEIGHT * MAP_WIDTH];
    int numCandidates = 0;
    for (int y = 0; y < MAP_HEIGHT; ++y) {
        for (int x = 0; x < MAP_WIDTH; ++x) {
            if (is_open(&world[my][mx], x, y)) {
                candidates[numCandidates].x = x;
                candidates[numCandidates].y = y;
                numCandidates++;
            }
        }
    }

    if (numCandidates == 0) return;

    // Cap the number of enemies by available open tiles
    if (count > numCandidates) count = numCandidates;

    // Shuffle candidates (Fisher–Yates)
    for (int i = numCandidates - 1; i > 0; --i) {
        int j = rand() % (i + 1);
        Vec2 tmp = candidates[i];
        candidates[i] = candidates[j];
        candidates[j] = tmp;
    }

    for (int i = 0; i < count; ++i) {
        enemies[my][mx][i].active = 1;
        enemies[my][mx][i].worldX = mx;
        enemies[my][mx][i].worldY = my;
        enemies[my][mx][i].pos.x = candidates[i].x;
        enemies[my][mx][i].pos.y = candidates[i].y;
        enemies[my][mx][i].hp = 2;
    }
}

static void place_random(Client *c) {
    // Spawn everyone on the same map (0,0) so players can see each other immediately
    for (int tries = 0; tries < 5000; ++tries) {
        int wx = 0, wy = 0; int x = rand()%MAP_WIDTH, y = rand()%MAP_HEIGHT;
        if (!is_open(&world[wy][wx], x, y)) continue;
        // avoid spawning on an already occupied tile by a connected client on same map
        int occupied = 0;
        for (int i = 0; i < MAX_CLIENTS; ++i) {
            if (!clients[i].connected) continue;
            if (clients[i].worldX == wx && clients[i].worldY == wy && clients[i].pos.x == x && clients[i].pos.y == y) { occupied = 1; break; }
        }
        if (occupied) continue;
        c->worldX = wx; c->worldY = wy; c->pos.x = x; c->pos.y = y; return;
    }
    c->worldX = 0; c->worldY = 0; c->pos.x = 1; c->pos.y = 1;
}

static void broadcast_state(void) {
    char line[128]; char buf[8192]; int off = 0;
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        int active = clients[i].connected ? 1 : 0;
        int n = snprintf(line, sizeof(line), "PLAYER %d %d %d %d %d %d %d\n", i, clients[i].worldX, clients[i].worldY, clients[i].pos.x, clients[i].pos.y, clients[i].color, active);
        if (off + n < (int)sizeof(buf)) { memcpy(buf + off, line, n); off += n; }
    }
    // broadcast bullets
    for (int b = 0; b < MAX_REMOTE_BULLETS; ++b) {
        if (!bullets[b].active) continue;
        int n = snprintf(line, sizeof(line), "BULLET %d %d %d %d %d\n", bullets[b].worldX, bullets[b].worldY, bullets[b].pos.x, bullets[b].pos.y, 1);
        if (off + n < (int)sizeof(buf)) { memcpy(buf + off, line, n); off += n; }
    }
    // broadcast enemies
    for (int wy = 0; wy < WORLD_H; ++wy) {
        for (int wx = 0; wx < WORLD_W; ++wx) {
            for (int i = 0; i < MAX_ENEMIES; ++i) {
                SrvEnemy *e = &enemies[wy][wx][i];
                if (!e->active) continue;
                int n = snprintf(line, sizeof(line), "ENEMY %d %d %d %d %d\n", wx, wy, e->pos.x, e->pos.y, e->hp);
                if (off + n < (int)sizeof(buf)) { memcpy(buf + off, line, n); off += n; }
            }
        }
    }
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (!clients[i].connected) continue;
#ifdef _WIN32
        send(clients[i].sock, buf, off, 0);
#else
        send(clients[i].sock, buf, off, 0);
#endif
    }
}

static void broadcast_tile(int wx, int wy, int x, int y, char ch) {
    char line[64];
    int n = snprintf(line, sizeof(line), "TILE %d %d %d %d %c\n", wx, wy, x, y, ch);
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (!clients[i].connected) continue;
        send(clients[i].sock, line, n, 0);
    }
}

static void step_bullets(void) {
    for (int i = 0; i < MAX_REMOTE_BULLETS; ++i) {
        if (!bullets[i].active) continue;
        int dx = 0, dy = 0;
        switch (bullets[i].dir) { case DIR_UP: dy = -1; break; case DIR_DOWN: dy = 1; break; case DIR_LEFT: dx = -1; break; case DIR_RIGHT: dx = 1; break; }
        int nx = bullets[i].pos.x + dx;
        int ny = bullets[i].pos.y + dy;
        if (nx < 0 || nx >= MAP_WIDTH || ny < 0 || ny >= MAP_HEIGHT) { bullets[i].active = 0; continue; }
        Map *m = &world[bullets[i].worldY][bullets[i].worldX];
        // Check enemy hit
        for (int ei = 0; ei < MAX_ENEMIES; ++ei) {
            SrvEnemy *e = &enemies[bullets[i].worldY][bullets[i].worldX][ei];
            if (!e->active) continue;
            if (e->pos.x == nx && e->pos.y == ny) {
                if (e->hp > 0) e->hp--;
                if (e->hp <= 0) e->active = 0;
                bullets[i].active = 0;
                goto bullet_continue;
            }
        }
        if (m->tiles[ny][nx] == '#') {
            // simple destructible after 5 hits not tracked server-side; just break wall immediately for now
            m->tiles[ny][nx] = '.';
            broadcast_tile(bullets[i].worldX, bullets[i].worldY, nx, ny, '.');
            bullets[i].active = 0;
            continue;
        }
        bullets[i].pos.x = nx; bullets[i].pos.y = ny;
bullet_continue:
        ;
    }
}

static void step_enemies(void) {
    for (int wy = 0; wy < WORLD_H; ++wy) {
        for (int wx = 0; wx < WORLD_W; ++wx) {
            for (int i = 0; i < MAX_ENEMIES; ++i) {
                SrvEnemy *e = &enemies[wy][wx][i];
                if (!e->active) continue;
                int dir = rand() % 4;
                int dx = 0, dy = 0;
                switch (dir) { case 0: dy = -1; break; case 1: dy = 1; break; case 2: dx = -1; break; case 3: dx = 1; break; }
                int nx = e->pos.x + dx;
                int ny = e->pos.y + dy;
                if (nx < 0 || nx >= MAP_WIDTH || ny < 0 || ny >= MAP_HEIGHT) continue;
                if (!is_open(&world[wy][wx], nx, ny)) continue;
                int occ = 0;
                for (int j = 0; j < MAX_ENEMIES; ++j) {
                    if (j == i) continue;
                    SrvEnemy *o = &enemies[wy][wx][j];
                    if (!o->active) continue;
                    if (o->pos.x == nx && o->pos.y == ny) { occ = 1; break; }
                }
                if (occ) continue;
                e->pos.x = nx; e->pos.y = ny;
            }
        }
    }
}

int main(int argc, char **argv) {
    srand((unsigned int)time(NULL));
#ifdef _WIN32
    WSADATA wsa; WSAStartup(MAKEWORD(2,2), &wsa);
#endif
    const char *port = (argc > 1) ? argv[1] : "5555";

    for (int y = 0; y < WORLD_H; ++y) for (int x = 0; x < WORLD_W; ++x) load_map_file(x, y);
    memset(clients, 0, sizeof(clients));
    memset(bullets, 0, sizeof(bullets));
    for (int y = 0; y < WORLD_H; ++y) for (int x = 0; x < WORLD_W; ++x) spawn_enemies_for_map(x, y, 4);

    struct addrinfo hints; memset(&hints, 0, sizeof(hints)); hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM; hints.ai_flags = AI_PASSIVE;
    struct addrinfo *res = NULL; if (getaddrinfo(NULL, port, &hints, &res) != 0) { fprintf(stderr, "getaddrinfo failed\n"); return 1; }
    sock_t lsock = (sock_t)socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    int yes = 1; setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
    if (bind(lsock, res->ai_addr, (int)res->ai_addrlen) != 0) { fprintf(stderr, "bind failed\n"); return 1; }
    if (listen(lsock, 16) != 0) { fprintf(stderr, "listen failed\n"); return 1; }
    freeaddrinfo(res);

    printf("[srv] Listening on port %s\n", port);
    fflush(stdout);

    fd_set readfds;
    while (1) {
        FD_ZERO(&readfds); FD_SET(lsock, &readfds); sock_t maxfd = lsock;
        for (int i = 0; i < MAX_CLIENTS; ++i) { if (clients[i].connected) { FD_SET(clients[i].sock, &readfds); if (clients[i].sock > maxfd) maxfd = clients[i].sock; } }
        struct timeval tv; tv.tv_sec = 0; tv.tv_usec = 50000; // 50ms tick
        select((int)(maxfd+1), &readfds, NULL, NULL, &tv);

        if (FD_ISSET(lsock, &readfds)) {
            struct sockaddr_storage ss; socklen_t slen = sizeof(ss);
            sock_t cs = accept(lsock, (struct sockaddr*)&ss, &slen);
            if (cs >= 0) {
                int idx = -1; for (int i = 0; i < MAX_CLIENTS; ++i) if (!clients[i].connected) { idx = i; break; }
                if (idx >= 0) {
                    clients[idx].connected = 1; clients[idx].sock = cs; clients[idx].color = idx;
                    place_random(&clients[idx]);
                    clients[idx].lastActive = time(NULL);
                    char host[64] = {0}, serv[16] = {0};
                    if (getnameinfo((struct sockaddr*)&ss, slen, host, sizeof(host), serv, sizeof(serv), NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
                        strncpy(host, "?", sizeof(host)-1); strncpy(serv, "?", sizeof(serv)-1);
                    }
                    strncpy(clients[idx].addr, host, sizeof(clients[idx].addr)-1);
                    strncpy(clients[idx].port, serv, sizeof(clients[idx].port)-1);
                    clients[idx].connId = g_nextConnId++;
                    printf("[srv] Client %d (cid=%llu) connected from %s:%s, color=%d, spawn=(%d,%d)@(%d,%d)\n",
                           idx, clients[idx].connId, clients[idx].addr, clients[idx].port, clients[idx].color,
                           clients[idx].worldX, clients[idx].worldY, clients[idx].pos.x, clients[idx].pos.y);
                    fflush(stdout);
                    char you[32]; int n = snprintf(you, sizeof(you), "YOU %d\n", idx);
                    send(clients[idx].sock, you, n, 0);
                } else {
                    const char *full = "FULL\n"; send(cs, full, (int)strlen(full), 0);
#ifdef _WIN32
                    closesocket(cs);
#else
                    close(cs);
#endif
                    char host[64] = {0}, serv[16] = {0};
                    if (getnameinfo((struct sockaddr*)&ss, slen, host, sizeof(host), serv, sizeof(serv), NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
                        strncpy(host, "?", sizeof(host)-1); strncpy(serv, "?", sizeof(serv)-1);
                    }
                    printf("[srv] Connection refused (server full) from %s:%s\n", host, serv);
                    fflush(stdout);
                }
            }
        }

        // Read inputs
        char buf[256];
        for (int i = 0; i < MAX_CLIENTS; ++i) {
            if (!clients[i].connected) continue;
            if (!FD_ISSET(clients[i].sock, &readfds)) continue; // only read if socket is ready
            int n = (int)recv(clients[i].sock, buf, sizeof(buf)-1, 0);
            if (n == 0) {
                // orderly disconnect
                printf("[srv] Client %d (cid=%llu) disconnected (socket closed) %s:%s\n", i, clients[i].connId, clients[i].addr, clients[i].port);
                fflush(stdout);
                clients[i].connected = 0;
#ifdef _WIN32
                closesocket(clients[i].sock);
#else
                close(clients[i].sock);
#endif
                clients[i].sock = 0;
                continue;
            }
            if (n < 0) {
                // no data
                continue;
            }
            buf[n] = '\0';
            // parse simple commands: INPUT dx dy shoot
            char *p = buf;
            while (*p) {
                char *eol = strchr(p, '\n'); if (eol) *eol = '\0';
                int dx, dy, shoot;
                if (strcmp(p, "BYE") == 0) {
                    printf("[srv] Client %d (cid=%llu) disconnected (BYE) %s:%s\n", i, clients[i].connId, clients[i].addr, clients[i].port);
                    fflush(stdout);
                    clients[i].connected = 0;
#ifdef _WIN32
                    closesocket(clients[i].sock);
#else
                    close(clients[i].sock);
#endif
                    clients[i].sock = 0;
                } else if (sscanf(p, "INPUT %d %d %d", &dx, &dy, &shoot) == 3) {
                    clients[i].lastActive = time(NULL);
                    int nx = clients[i].pos.x + dx;
                    int ny = clients[i].pos.y + dy;
                    // clamp
                    if (nx < 0) {
                        if (clients[i].worldX > 0 && is_open(&world[clients[i].worldY][clients[i].worldX-1], MAP_WIDTH-1, ny)) { clients[i].worldX--; nx = MAP_WIDTH-1; }
                    } else if (nx >= MAP_WIDTH) {
                        if (clients[i].worldX < WORLD_W-1 && is_open(&world[clients[i].worldY][clients[i].worldX+1], 0, ny)) { clients[i].worldX++; nx = 0; }
                    }
                    if (ny < 0) {
                        if (clients[i].worldY > 0 && is_open(&world[clients[i].worldY-1][clients[i].worldX], nx, MAP_HEIGHT-1)) { clients[i].worldY--; ny = MAP_HEIGHT-1; }
                    } else if (ny >= MAP_HEIGHT) {
                        if (clients[i].worldY < WORLD_H-1 && is_open(&world[clients[i].worldY+1][clients[i].worldX], nx, 0)) { clients[i].worldY++; ny = 0; }
                    }
                    if (nx >= 0 && nx < MAP_WIDTH && ny >= 0 && ny < MAP_HEIGHT && is_open(&world[clients[i].worldY][clients[i].worldX], nx, ny)) {
                        clients[i].pos.x = nx; clients[i].pos.y = ny;
                    }
                    if (shoot) {
                        // spawn a server bullet in player's direction inferred from dx/dy; default right
                        Direction dir = DIR_RIGHT;
                        if (dx < 0) dir = DIR_LEFT; else if (dx > 0) dir = DIR_RIGHT; else if (dy < 0) dir = DIR_UP; else if (dy > 0) dir = DIR_DOWN;
                        int slot = -1; for (int bi = 0; bi < MAX_REMOTE_BULLETS; ++bi) if (!bullets[bi].active) { slot = bi; break; }
                        if (slot >= 0) { bullets[slot].active = 1; bullets[slot].worldX = clients[i].worldX; bullets[slot].worldY = clients[i].worldY; bullets[slot].pos = clients[i].pos; bullets[slot].dir = dir; }
                    }
                }
                if (!eol) break; p = eol + 1;
            }
        }

        // Inactivity timeout (3 minutes)
        time_t now = time(NULL);
        for (int i = 0; i < MAX_CLIENTS; ++i) {
            if (!clients[i].connected) continue;
            if (now - clients[i].lastActive > 180) {
                printf("[srv] Client %d (cid=%llu) disconnected (timeout) %s:%s\n", i, clients[i].connId, clients[i].addr, clients[i].port);
                fflush(stdout);
                clients[i].connected = 0;
#ifdef _WIN32
                closesocket(clients[i].sock);
#else
                close(clients[i].sock);
#endif
                clients[i].sock = 0;
            }
        }

        step_bullets();
        step_enemies();
        broadcast_state();
    }

    return 0;
}


