#include "client_net.h"
#include "game.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static net_socket_t g_sock = -1;

static void parse_host_port(const char *in, char *host, size_t hostcap, char *port, size_t portcap) {
    const char *colon = strrchr(in, ':');
    if (colon) {
        size_t hlen = (size_t)(colon - in); if (hlen >= hostcap) hlen = hostcap - 1; memcpy(host, in, hlen); host[hlen] = '\0';
        strncpy(port, colon + 1, portcap - 1); port[portcap - 1] = '\0';
    } else {
        strncpy(host, in, hostcap - 1); host[hostcap - 1] = '\0';
        strncpy(port, "5555", portcap - 1); port[portcap - 1] = '\0';
    }
    // Normalize localhost to IPv4 to match server's IPv4 listen by default
    if (_stricmp(host, "localhost") == 0) {
        strncpy(host, "127.0.0.1", hostcap - 1); host[hostcap - 1] = '\0';
    }
}

int client_connect(const char *addr_input) {
    if (net_init() != 0) return -1;
    char host[256], port[32]; parse_host_port(addr_input, host, sizeof(host), port, sizeof(port));
    g_sock = net_connect_hostport(host, port);
    if (g_sock < 0) return -1;
    net_set_nonblocking(g_sock);
    // Simple hello
    const char *hello = "HELLO\n"; net_send_all(g_sock, hello, (int)strlen(hello));
    return 0;
}

void client_disconnect(void) {
    if (g_sock >= 0) { net_close(g_sock); g_sock = -1; }
    net_cleanup();
}

void client_send_input(int dx, int dy, int shoot) {
    if (g_sock < 0) return;
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "INPUT %d %d %d\n", dx, dy, shoot);
    net_send_all(g_sock, buf, n);
}

void client_poll_messages(void) {
    if (g_sock < 0) return;
    char buf[2048];
    int n = net_recv_nonblocking(g_sock, buf, sizeof(buf) - 1);
    if (n <= 0) return;
    buf[n] = '\0';
    // Reset remote bullets; server sends a full snapshot each tick
    for (int i = 0; i < MAX_REMOTE_BULLETS; ++i) g_remote_bullets[i].active = 0;
    // Protocol lines:
    //  - YOU id
    //  - PLAYER id wx wy x y color active
    //  - TILE wx wy x y ch
    //  - BULLET wx wy x y active
    char *p = buf;
    while (*p) {
        char *eol = strchr(p, '\n'); if (eol) *eol = '\0';
        if (strncmp(p, "YOU ", 4) == 0) {
            g_my_player_id = atoi(p + 4);
        } else if (strncmp(p, "PLAYER ", 7) == 0) {
            int id, wx, wy, x, y, color, active;
            if (sscanf(p + 7, "%d %d %d %d %d %d %d", &id, &wx, &wy, &x, &y, &color, &active) == 7) {
                if (id >= 0 && id < MAX_REMOTE_PLAYERS) {
                    g_remote_players[id].active = active;
                    if (active) {
                        g_remote_players[id].worldX = wx;
                        g_remote_players[id].worldY = wy;
                        g_remote_players[id].pos.x = x;
                        g_remote_players[id].pos.y = y;
                        g_remote_players[id].colorIndex = color;
                    }
                }
            }
        } else if (strncmp(p, "TILE ", 5) == 0) {
            int wx, wy, x, y; char ch;
            if (sscanf(p + 5, "%d %d %d %d %c", &wx, &wy, &x, &y, &ch) == 5) {
                game_mp_set_tile(wx, wy, x, y, ch);
            }
        } else if (strncmp(p, "BULLET ", 7) == 0) {
            int wx, wy, x, y, active;
            if (sscanf(p + 7, "%d %d %d %d %d", &wx, &wy, &x, &y, &active) == 5) {
                // naive compaction: place/update first matching slot or first free
                int slot = -1;
                for (int i = 0; i < MAX_REMOTE_BULLETS; ++i) {
                    if (g_remote_bullets[i].active && g_remote_bullets[i].worldX == wx && g_remote_bullets[i].worldY == wy && g_remote_bullets[i].pos.x == x && g_remote_bullets[i].pos.y == y) { slot = i; break; }
                }
                if (slot < 0) {
                    for (int i = 0; i < MAX_REMOTE_BULLETS; ++i) { if (!g_remote_bullets[i].active) { slot = i; break; } }
                }
                if (slot >= 0) {
                    g_remote_bullets[slot].active = active;
                    g_remote_bullets[slot].worldX = wx;
                    g_remote_bullets[slot].worldY = wy;
                    g_remote_bullets[slot].pos.x = x;
                    g_remote_bullets[slot].pos.y = y;
                }
            }
        }
        if (!eol) break;
        p = eol + 1;
    }
}

void client_send_bye(void) {
    if (g_sock < 0) return;
    const char *bye = "BYE\n";
    net_send_all(g_sock, bye, (int)strlen(bye));
}


