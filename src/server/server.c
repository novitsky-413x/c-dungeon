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
    int connected;
    sock_t sock;
    int worldX, worldY;
    Vec2 pos;
    int color;
} Client;

static Map world[WORLD_H][WORLD_W];
static Client clients[MAX_CLIENTS];

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

static void place_random(Client *c) {
    for (int tries = 0; tries < 1000; ++tries) {
        int wx = rand() % WORLD_W, wy = rand() % WORLD_H; int x = rand()%MAP_WIDTH, y = rand()%MAP_HEIGHT;
        if (!is_open(&world[wy][wx], x, y)) continue;
        c->worldX = wx; c->worldY = wy; c->pos.x = x; c->pos.y = y; return;
    }
    c->worldX = 0; c->worldY = 0; c->pos.x = 1; c->pos.y = 1;
}

static void broadcast_state(void) {
    char line[128]; char buf[4096]; int off = 0;
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (!clients[i].connected) continue;
        int n = snprintf(line, sizeof(line), "PLAYER %d %d %d %d %d %d %d\n", i, clients[i].worldX, clients[i].worldY, clients[i].pos.x, clients[i].pos.y, clients[i].color, 1);
        if (off + n < (int)sizeof(buf)) { memcpy(buf + off, line, n); off += n; }
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

int main(int argc, char **argv) {
    srand((unsigned int)time(NULL));
#ifdef _WIN32
    WSADATA wsa; WSAStartup(MAKEWORD(2,2), &wsa);
#endif
    const char *port = (argc > 1) ? argv[1] : "5555";

    for (int y = 0; y < WORLD_H; ++y) for (int x = 0; x < WORLD_W; ++x) load_map_file(x, y);
    memset(clients, 0, sizeof(clients));

    struct addrinfo hints; memset(&hints, 0, sizeof(hints)); hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM; hints.ai_flags = AI_PASSIVE;
    struct addrinfo *res = NULL; if (getaddrinfo(NULL, port, &hints, &res) != 0) { fprintf(stderr, "getaddrinfo failed\n"); return 1; }
    sock_t lsock = (sock_t)socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    int yes = 1; setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
    if (bind(lsock, res->ai_addr, (int)res->ai_addrlen) != 0) { fprintf(stderr, "bind failed\n"); return 1; }
    if (listen(lsock, 16) != 0) { fprintf(stderr, "listen failed\n"); return 1; }
    freeaddrinfo(res);

    printf("Server listening on port %s\n", port);

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
                    char you[32]; int n = snprintf(you, sizeof(you), "YOU %d\n", idx);
                    send(clients[idx].sock, you, n, 0);
                } else {
                    const char *full = "FULL\n"; send(cs, full, (int)strlen(full), 0);
#ifdef _WIN32
                    closesocket(cs);
#else
                    close(cs);
#endif
                }
            }
        }

        // Read inputs
        char buf[256];
        for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (!clients[i].connected) continue;
        int n = (int)recv(clients[i].sock, buf, sizeof(buf)-1, 0);
            if (n <= 0) continue; buf[n] = '\0';
            // parse simple commands: INPUT dx dy shoot
            char *p = buf;
            while (*p) {
                char *eol = strchr(p, '\n'); if (eol) *eol = '\0';
                int dx, dy, shoot;
                if (sscanf(p, "INPUT %d %d %d", &dx, &dy, &shoot) == 3) {
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
                    (void)shoot; // shooting not yet handled server-side in this minimal version
                }
                if (!eol) break; p = eol + 1;
            }
        }

        broadcast_state();
    }

    return 0;
}


