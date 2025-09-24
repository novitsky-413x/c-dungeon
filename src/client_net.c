#include "client_net.h"
#include "game.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif

static net_socket_t g_sock = -1;
static char g_recv_buf[8192];
static int g_recv_len = 0;

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
    if (strcasecmp(host, "localhost") == 0) {
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
    g_recv_len = 0;
}

void client_send_input(int dx, int dy, int shoot) {
    if (g_sock < 0) return;
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "INPUT %d %d %d\n", dx, dy, shoot);
    net_send_all(g_sock, buf, n);
}

int client_poll_messages(void) {
    int changed = 0;
    if (g_sock < 0) return 0;
    char tmp[2048];
    int n = net_recv_nonblocking(g_sock, tmp, sizeof(tmp));
    if (n <= 0) return 0;
    // Reset snapshots on new data chunk
    for (int i = 0; i < MAX_REMOTE_BULLETS; ++i) g_remote_bullets[i].active = 0;
    for (int i = 0; i < MAX_REMOTE_ENEMIES; ++i) g_remote_enemies[i].active = 0;
    // Append to rolling buffer, clamp if necessary (drop oldest on overflow)
    int cap = (int)sizeof(g_recv_buf) - 1;
    if (g_recv_len + n > cap) {
        int over = g_recv_len + n - cap;
        if (over >= g_recv_len) {
            int keep = cap;
            if (keep > n) keep = n;
            memcpy(g_recv_buf, tmp + (n - keep), keep);
            g_recv_len = keep;
        } else {
            memmove(g_recv_buf, g_recv_buf + over, g_recv_len - over);
            g_recv_len -= over;
            memcpy(g_recv_buf + g_recv_len, tmp, n);
            g_recv_len += n;
        }
    } else {
        memcpy(g_recv_buf + g_recv_len, tmp, n);
        g_recv_len += n;
    }
    g_recv_buf[g_recv_len] = '\0';

    // Process complete lines
    while (1) {
        char *eol = memchr(g_recv_buf, '\n', g_recv_len);
        if (!eol) break;
        int linelen = (int)(eol - g_recv_buf);
        char line[256];
        if (linelen >= (int)sizeof(line)) linelen = (int)sizeof(line) - 1;
        memcpy(line, g_recv_buf, linelen);
        line[linelen] = '\0';

        // Shift remaining buffer
        int remain = g_recv_len - (linelen + 1);
        if (remain > 0) memmove(g_recv_buf, eol + 1, remain);
        g_recv_len = remain;
        g_recv_buf[g_recv_len] = '\0';

        // Parse one line
        if (line[0] == '\0') continue;
        if (strncmp(line, "YOU ", 4) == 0) {
            g_my_player_id = atoi(line + 4);
            changed = 1;
        } else if (strncmp(line, "PLAYER ", 7) == 0) {
            int id, wx, wy, x, y, color, active;
            if (sscanf(line + 7, "%d %d %d %d %d %d %d", &id, &wx, &wy, &x, &y, &color, &active) == 7) {
                if (id >= 0 && id < MAX_REMOTE_PLAYERS) {
                    g_remote_players[id].active = active;
                    if (active) {
                        g_remote_players[id].worldX = wx;
                        g_remote_players[id].worldY = wy;
                        g_remote_players[id].pos.x = x;
                        g_remote_players[id].pos.y = y;
                        g_remote_players[id].colorIndex = color;
                        if (id == g_my_player_id) {
                            game_mp_set_self(wx, wy, x, y);
                        }
                        changed = 1;
                    }
                }
            }
        } else if (strncmp(line, "TILE ", 5) == 0) {
            int wx, wy, x, y; char ch;
            if (sscanf(line + 5, "%d %d %d %d %c", &wx, &wy, &x, &y, &ch) == 5) {
                game_mp_set_tile(wx, wy, x, y, ch);
                changed = 1;
            }
        } else if (strncmp(line, "BULLET ", 7) == 0) {
            int wx, wy, x, y, active;
            if (sscanf(line + 7, "%d %d %d %d %d", &wx, &wy, &x, &y, &active) == 5) {
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
                    changed = 1;
                }
            }
        } else if (strncmp(line, "ENEMY ", 7) == 0) {
            int wx, wy, x, y, hp;
            if (sscanf(line + 7, "%d %d %d %d %d", &wx, &wy, &x, &y, &hp) == 5) {
                int slot = -1;
                for (int i = 0; i < MAX_REMOTE_ENEMIES; ++i) {
                    if (g_remote_enemies[i].active && g_remote_enemies[i].worldX == wx && g_remote_enemies[i].worldY == wy && g_remote_enemies[i].pos.x == x && g_remote_enemies[i].pos.y == y) { slot = i; break; }
                }
                if (slot < 0) {
                    for (int i = 0; i < MAX_REMOTE_ENEMIES; ++i) { if (!g_remote_enemies[i].active) { slot = i; break; } }
                }
                if (slot >= 0) {
                    g_remote_enemies[slot].active = (hp > 0);
                    g_remote_enemies[slot].worldX = wx;
                    g_remote_enemies[slot].worldY = wy;
                    g_remote_enemies[slot].pos.x = x;
                    g_remote_enemies[slot].pos.y = y;
                    g_remote_enemies[slot].hp = hp;
                    changed = 1;
                }
            }
        }
    }
    return changed;
}

void client_send_bye(void) {
    if (g_sock < 0) return;
    const char *bye = "BYE\n";
    net_send_all(g_sock, bye, (int)strlen(bye));
}


