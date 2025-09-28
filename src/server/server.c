#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <ctype.h>
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
#include <netinet/tcp.h>
typedef int sock_t;
#endif

#include "../types.h"

#define WORLD_W 9
#define WORLD_H 9
#define MAX_CLIENTS MAX_REMOTE_PLAYERS

typedef struct {
    char tiles[MAP_HEIGHT][MAP_WIDTH + 1];
    unsigned char wallDmg[MAP_HEIGHT][MAP_WIDTH];
} Map;

typedef struct {
    int active;
    int worldX;
    int worldY;
    Vec2 pos;
    Direction dir;
    int ownerId;
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
    int isWebSocket;
    int wsHandshakeDone;
    char wsBuf[8192];
    int wsBufLen;
    int worldX, worldY;
    Vec2 pos;
    int color;
    Direction facing;
    int hp;
    int invincibleTicks; // 3s at 20 ticks/sec => 60 ticks
    int superTicks; // 5s at 20 ticks/sec => 100 ticks
    int shootCooldown; // ticks until next allowed shot
    int score;
    time_t lastActive;
    char addr[64];
    char port[16];
    unsigned long long connId;
    // Lightweight input rate limiting
    int tokens; // leaky-bucket tokens
    int maxTokens; // capacity
    int refillTicks; // every N ticks, add tokens
    int refillAmount; // tokens added per refill
    int tickSinceRefill;
} Client;

static Map world[WORLD_H][WORLD_W];
static Client clients[MAX_CLIENTS];
static unsigned long long g_nextConnId = 1ULL;
static SrvBullet bullets[MAX_REMOTE_BULLETS];
static SrvEnemy enemies[WORLD_H][WORLD_W][MAX_ENEMIES];
static int g_tick_counter = 0; // global server tick counter (~20 ticks/sec)

// Simple WS connection limits
#define MAX_WS_PER_IP 2
#define WS_CONN_RATE_SLOTS 64
#define WS_CONN_WINDOW_SECONDS 10
#define WS_CONN_MAX_PER_WINDOW 3

typedef struct {
    char ip[64];
    time_t windowStart;
    int attemptsInWindow;
} WsIpRate;
static WsIpRate g_wsIpRates[WS_CONN_RATE_SLOTS];

static int ws_count_active_for_ip(const char *ip) {
    if (!ip || !*ip) return 0;
    int cnt = 0;
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (!clients[i].connected) continue;
        if (!clients[i].isWebSocket) continue;
        if (strcmp(clients[i].addr, ip) == 0) cnt++;
    }
    return cnt;
}

static int ws_rate_allow(const char *ip) {
    if (!ip) return 0;
    time_t now = time(NULL);
    int freeIdx = -1;
    for (int i = 0; i < WS_CONN_RATE_SLOTS; ++i) {
        if (g_wsIpRates[i].ip[0] == '\0') { if (freeIdx < 0) freeIdx = i; continue; }
        if (strcmp(g_wsIpRates[i].ip, ip) == 0) {
            if (now - g_wsIpRates[i].windowStart >= WS_CONN_WINDOW_SECONDS) {
                g_wsIpRates[i].windowStart = now; g_wsIpRates[i].attemptsInWindow = 0;
            }
            if (g_wsIpRates[i].attemptsInWindow >= WS_CONN_MAX_PER_WINDOW) return 0;
            g_wsIpRates[i].attemptsInWindow++;
            return 1;
        }
    }
    if (freeIdx >= 0) {
        strncpy(g_wsIpRates[freeIdx].ip, ip, sizeof(g_wsIpRates[freeIdx].ip)-1);
        g_wsIpRates[freeIdx].ip[sizeof(g_wsIpRates[freeIdx].ip)-1] = '\0';
        g_wsIpRates[freeIdx].windowStart = now;
        g_wsIpRates[freeIdx].attemptsInWindow = 1;
        return 1;
    }
    // No slot; allow by default
    return 1;
}

static int is_map_active(int wx, int wy) {
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (!clients[i].connected) continue;
        if (clients[i].worldX == wx && clients[i].worldY == wy) return 1;
    }
    return 0;
}

// --- Minimal Base64 encoding ---
static const char b64tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static int base64_encode(const uint8_t *in, int inlen, char *out, int outcap) {
    int o = 0;
    int i = 0;
    while (i + 2 < inlen) {
        if (o + 4 > outcap) return o;
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i+1] << 8) | in[i+2];
        out[o++] = b64tab[(v >> 18) & 63];
        out[o++] = b64tab[(v >> 12) & 63];
        out[o++] = b64tab[(v >> 6) & 63];
        out[o++] = b64tab[v & 63];
        i += 3;
    }
    int rem = inlen - i;
    if (rem == 1) {
        if (o + 4 > outcap) return o;
        uint32_t v = ((uint32_t)in[i]) << 16;
        out[o++] = b64tab[(v >> 18) & 63];
        out[o++] = b64tab[(v >> 12) & 63];
        out[o++] = '=';
        out[o++] = '=';
    } else if (rem == 2) {
        if (o + 4 > outcap) return o;
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i+1] << 8);
        out[o++] = b64tab[(v >> 18) & 63];
        out[o++] = b64tab[(v >> 12) & 63];
        out[o++] = b64tab[(v >> 6) & 63];
        out[o++] = '=';
    }
    if (o < outcap) out[o] = '\0';
    return o;
}

// --- Minimal SHA1 implementation ---
static uint32_t rol32(uint32_t v, int r) { return (v << r) | (v >> (32 - r)); }
static void sha1(const uint8_t *data, size_t len, uint8_t out[20]) {
    uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476, h4 = 0xC3D2E1F0;
    size_t newlen = len + 1; while ((newlen % 64) != 56) newlen++;
    size_t total = newlen + 8;
    uint8_t *msg = (uint8_t*)malloc(total);
    if (!msg) return;
    memcpy(msg, data, len);
    msg[len] = 0x80;
    memset(msg + len + 1, 0, newlen - (len + 1));
    uint64_t bits = (uint64_t)len * 8ULL;
    msg[newlen + 0] = (uint8_t)((bits >> 56) & 0xFF);
    msg[newlen + 1] = (uint8_t)((bits >> 48) & 0xFF);
    msg[newlen + 2] = (uint8_t)((bits >> 40) & 0xFF);
    msg[newlen + 3] = (uint8_t)((bits >> 32) & 0xFF);
    msg[newlen + 4] = (uint8_t)((bits >> 24) & 0xFF);
    msg[newlen + 5] = (uint8_t)((bits >> 16) & 0xFF);
    msg[newlen + 6] = (uint8_t)((bits >> 8) & 0xFF);
    msg[newlen + 7] = (uint8_t)((bits >> 0) & 0xFF);

    for (size_t off = 0; off < total; off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) {
            w[i] = ((uint32_t)msg[off + i*4 + 0] << 24) |
                   ((uint32_t)msg[off + i*4 + 1] << 16) |
                   ((uint32_t)msg[off + i*4 + 2] << 8)  |
                   ((uint32_t)msg[off + i*4 + 3]);
        }
        for (int i = 16; i < 80; i++) w[i] = rol32(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | ((~b) & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t temp = rol32(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol32(b, 30); b = a; a = temp;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }
    free(msg);
    out[0]= (h0>>24)&0xFF; out[1]=(h0>>16)&0xFF; out[2]=(h0>>8)&0xFF; out[3]=h0&0xFF;
    out[4]= (h1>>24)&0xFF; out[5]=(h1>>16)&0xFF; out[6]=(h1>>8)&0xFF; out[7]=h1&0xFF;
    out[8]= (h2>>24)&0xFF; out[9]=(h2>>16)&0xFF; out[10]=(h2>>8)&0xFF; out[11]=h2&0xFF;
    out[12]=(h3>>24)&0xFF; out[13]=(h3>>16)&0xFF; out[14]=(h3>>8)&0xFF; out[15]=h3&0xFF;
    out[16]=(h4>>24)&0xFF; out[17]=(h4>>16)&0xFF; out[18]=(h4>>8)&0xFF; out[19]=h4&0xFF;
}

// --- Minimal case-insensitive substring search (ASCII) ---
static const char *strcasestr_local(const char *haystack, const char *needle) {
    if (!*needle) return haystack;
    size_t nlen = strlen(needle);
    for (const char *p = haystack; *p; ++p) {
        size_t i = 0;
        while (i < nlen) {
            char a = p[i]; char b = needle[i];
            if (!a) return NULL;
            if (tolower((unsigned char)a) != tolower((unsigned char)b)) break;
            i++;
        }
        if (i == nlen) return p;
    }
    return NULL;
}

static int ws_send_text_frame(sock_t s, const char *data, int len) {
    // build a server-to-client unmasked text frame
    uint8_t hdr[10]; int hlen = 0;
    hdr[0] = 0x81; // FIN + text
    if (len < 126) { hdr[1] = (uint8_t)len; hlen = 2; }
    else if (len <= 0xFFFF) { hdr[1] = 126; hdr[2] = (len >> 8) & 0xFF; hdr[3] = len & 0xFF; hlen = 4; }
    else { hdr[1] = 127; // 64-bit length
           hdr[2]=hdr[3]=hdr[4]=hdr[5]=0; hdr[6]=(len>>24)&0xFF; hdr[7]=(len>>16)&0xFF; hdr[8]=(len>>8)&0xFF; hdr[9]=len&0xFF; hlen = 10; }
    int n1 = (int)send(s, (const char*)hdr, hlen, 0);
    if (n1 < 0) return n1;
    return (int)send(s, data, len, 0);
}

static int ws_handshake(Client *c) {
    // Expect HTTP GET with Sec-WebSocket-Key
    c->wsBuf[c->wsBufLen] = '\0';
    const char *end = strstr(c->wsBuf, "\r\n\r\n");
    if (!end) end = strstr(c->wsBuf, "\n\n"); // be tolerant
    if (!end) return 0; // need more
    // (debug logs removed)
    // Robust header parse: find Sec-WebSocket-Key case-insensitively, ignoring whitespace
    char key[128] = {0};
    const char *p = c->wsBuf;
    while (p < end) {
        const char *ln = p;
        const char *nl = strstr(ln, "\n");
        if (!nl || nl > end) nl = end;
        // Trim CRLF
        const char *lineEnd = nl;
        if (lineEnd > ln && *(lineEnd-1) == '\r') lineEnd--;
        // Find colon
        const char *colon = NULL;
        for (const char *q = ln; q < lineEnd; ++q) { if (*q == ':') { colon = q; break; } }
        if (colon) {
            // Header name
            int nameMatch = 1;
            const char *name = "sec-websocket-key";
            const char *q = ln; int idx = 0;
            while (q < colon && name[idx]) {
                char a = tolower((unsigned char)*q);
                char b = name[idx];
                if (a != b) { nameMatch = 0; break; }
                q++; idx++;
            }
            if (name[idx] != '\0') nameMatch = 0; // not full name
            // ignore extra spaces in header name area
            if (nameMatch) {
                const char *val = colon + 1;
                while (val < lineEnd && (*val==' '||*val=='\t')) val++;
                int ki = 0;
                while (val < lineEnd && ki < (int)sizeof(key)-1) key[ki++] = *val++;
                key[ki] = '\0';
                break;
            }
        }
        p = nl + 1;
    }
    if (key[0] == '\0') { return -1; }
    const char *GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    char concat[256]; snprintf(concat, sizeof(concat), "%s%s", key, GUID);
    uint8_t digest[20]; sha1((const uint8_t*)concat, strlen(concat), digest);
    char accept[64]; base64_encode(digest, 20, accept, sizeof(accept));
    char resp[256];
    int rn = snprintf(resp, sizeof(resp),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n\r\n", accept);
    if (send(c->sock, resp, rn, 0) < 0) { return -1; }
    c->wsHandshakeDone = 1;
    c->wsBufLen = 0;
    return 1;
}

static void send_text_to_client(int idx, const char *data, int len) {
    if (!clients[idx].connected) return;
    if (clients[idx].isWebSocket && clients[idx].wsHandshakeDone) {
        ws_send_text_frame(clients[idx].sock, data, len);
    } else {
        send(clients[idx].sock, data, len, 0);
    }
}

static void send_full_map_to(int clientIdx) {
    if (clientIdx < 0 || clientIdx >= MAX_CLIENTS) return;
    if (!clients[clientIdx].connected) return;
    char line[64];
    for (int wy = 0; wy < WORLD_H; ++wy) {
        for (int wx = 0; wx < WORLD_W; ++wx) {
            for (int y = 0; y < MAP_HEIGHT; ++y) {
                for (int x = 0; x < MAP_WIDTH; ++x) {
                    char ch = world[wy][wx].tiles[y][x];
                    int n = snprintf(line, sizeof(line), "TILE %d %d %d %d %c\n", wx, wy, x, y, ch);
                    send_text_to_client(clientIdx, line, n);
                }
            }
        }
    }
}

static void send_map_to(int clientIdx, int wx, int wy) {
    if (clientIdx < 0 || clientIdx >= MAX_CLIENTS) return;
    if (!clients[clientIdx].connected) return;
    if (wx < 0 || wx >= WORLD_W || wy < 0 || wy >= WORLD_H) return;
    char buf[32768]; int off = 0; char line[64];
    for (int y = 0; y < MAP_HEIGHT; ++y) {
        for (int x = 0; x < MAP_WIDTH; ++x) {
            char ch = world[wy][wx].tiles[y][x];
            int n = snprintf(line, sizeof(line), "TILE %d %d %d %d %c\n", wx, wy, x, y, ch);
            if (n <= 0) continue;
            if (off + n >= (int)sizeof(buf)) {
                send_text_to_client(clientIdx, buf, off);
                off = 0;
            }
            memcpy(buf + off, line, n);
            off += n;
        }
    }
    if (off > 0) send_text_to_client(clientIdx, buf, off);
}

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
        // Generate an all-dots map (including edges)
        for (int y = 0; y < MAP_HEIGHT; ++y) {
            for (int x = 0; x < MAP_WIDTH; ++x) m->tiles[y][x] = '.';
            m->tiles[y][MAP_WIDTH] = '\0';
        }
        memset(m->wallDmg, 0, sizeof(m->wallDmg));
        // Ensure inter-map connectivity on interior edges
        int midX = MAP_WIDTH / 2;
        int midY = MAP_HEIGHT / 2;
        if (mx > 0) m->tiles[midY][0] = '.';
        if (mx < WORLD_W - 1) m->tiles[midY][MAP_WIDTH - 1] = '.';
        if (my > 0) m->tiles[0][midX] = '.';
        if (my < WORLD_H - 1) m->tiles[MAP_HEIGHT - 1][midX] = '.';
        // Ensure a central spawn exists at world center
        if (mx == WORLD_W / 2 && my == WORLD_H / 2) {
            m->tiles[midY][midX] = 'S';
        }
        return;
    }
    char line[512];
    for (int y = 0; y < MAP_HEIGHT; ++y) {
        if (!fgets(line, sizeof(line), f)) { for (; y < MAP_HEIGHT; ++y) { for (int x = 0; x < MAP_WIDTH; ++x) m->tiles[y][x] = '#'; m->tiles[y][MAP_WIDTH] = '\0'; } break; }
        int len = (int)strcspn(line, "\r\n");
        for (int x = 0; x < MAP_WIDTH; ++x) { char c = (x < len) ? line[x] : '#'; if (c!='#'&&c!='.'&&c!='X'&&c!='W'&&c!='@'&&c!='S') c='.'; m->tiles[y][x] = c; }
        m->tiles[y][MAP_WIDTH] = '\0';
    }
    fclose(f);
    memset(m->wallDmg, 0, sizeof(m->wallDmg));
    // Ensure inter-map connectivity on interior edges
    int midX = MAP_WIDTH / 2;
    int midY = MAP_HEIGHT / 2;
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
}

static int is_open(Map *m, int x, int y) { if (x<0||x>=MAP_WIDTH||y<0||y>=MAP_HEIGHT) return 0; return m->tiles[y][x] != '#'; }
static int map_has_spawn(int mx, int my) { for (int y = 0; y < MAP_HEIGHT; ++y) for (int x = 0; x < MAP_WIDTH; ++x) if (world[my][mx].tiles[y][x] == 'S') return 1; return 0; }
static int find_spawn_in_map(int mx, int my, int *sx, int *sy) { for (int y = 0; y < MAP_HEIGHT; ++y) for (int x = 0; x < MAP_WIDTH; ++x) if (world[my][mx].tiles[y][x] == 'S') { *sx = x; *sy = y; return 1; } return 0; }

static void spawn_enemies_for_map(int mx, int my, int count) {
    if (map_has_spawn(mx, my)) { for (int i = 0; i < MAX_ENEMIES; ++i) enemies[my][mx][i].active = 0; return; }
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

static void place_near_spawn(Client *c) {
    int smx = 0, smy = 0, sx = 1, sy = 1; int foundMap = 0;
    for (int my = 0; my < WORLD_H && !foundMap; ++my) {
        for (int mx = 0; mx < WORLD_W && !foundMap; ++mx) {
            int tx, ty; if (find_spawn_in_map(mx, my, &tx, &ty)) { smx = mx; smy = my; sx = tx; sy = ty; foundMap = 1; }
        }
    }
    int bestx = sx, besty = sy;
    for (int r = 0; r <= MAP_WIDTH + MAP_HEIGHT; ++r) {
        for (int dy = -r; dy <= r; ++dy) {
            int dxs[2] = { -r, r };
            for (int k = 0; k < 2; ++k) {
                int dx = dxs[k]; int tx = sx + dx; int ty = sy + dy;
                if (tx < 0 || tx >= MAP_WIDTH || ty < 0 || ty >= MAP_HEIGHT) continue;
                if (!is_open(&world[smy][smx], tx, ty)) continue;
                int occupied = 0; for (int i = 0; i < MAX_CLIENTS; ++i) { if (!clients[i].connected) continue; if (clients[i].worldX == smx && clients[i].worldY == smy && clients[i].pos.x == tx && clients[i].pos.y == ty) { occupied = 1; break; } }
                if (!occupied) { bestx = tx; besty = ty; goto found; }
            }
        }
        for (int dx = -r+1; dx <= r-1; ++dx) {
            int dys[2] = { -r, r };
            for (int k = 0; k < 2; ++k) {
                int dy = dys[k]; int tx = sx + dx; int ty = sy + dy;
                if (tx < 0 || tx >= MAP_WIDTH || ty < 0 || ty >= MAP_HEIGHT) continue;
                if (!is_open(&world[smy][smx], tx, ty)) continue;
                int occupied = 0; for (int i = 0; i < MAX_CLIENTS; ++i) { if (!clients[i].connected) continue; if (clients[i].worldX == smx && clients[i].worldY == smy && clients[i].pos.x == tx && clients[i].pos.y == ty) { occupied = 1; break; } }
                if (!occupied) { bestx = tx; besty = ty; goto found; }
            }
        }
    }
found:
    c->worldX = smx; c->worldY = smy; c->pos.x = bestx; c->pos.y = besty;
}

static void broadcast_state(void) {
    char line[128]; char buf[8192]; int off = 0;
    // Prepend a tick marker so clients can align updates
    {
        int n0 = snprintf(line, sizeof(line), "TICK %d\n", g_tick_counter);
        if (n0 > 0 && off + n0 < (int)sizeof(buf)) { memcpy(buf + off, line, n0); off += n0; }
    }
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        int active = clients[i].connected ? 1 : 0;
        int n = snprintf(line, sizeof(line), "PLAYER %d %d %d %d %d %d %d %d %d %d %d\n", i, clients[i].worldX, clients[i].worldY, clients[i].pos.x, clients[i].pos.y, clients[i].color, active, clients[i].hp, clients[i].invincibleTicks, clients[i].superTicks, clients[i].score);
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
            if (!is_map_active(wx, wy)) continue; // skip maps without active players
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
        if (clients[i].isWebSocket && !clients[i].wsHandshakeDone) continue; // do not send before WS handshake
        if (clients[i].isWebSocket) ws_send_text_frame(clients[i].sock, buf, off);
        else send(clients[i].sock, buf, off, 0);
    }
}

static void broadcast_tile(int wx, int wy, int x, int y, char ch) {
    char line[64];
    int n = snprintf(line, sizeof(line), "TILE %d %d %d %d %c\n", wx, wy, x, y, ch);
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (!clients[i].connected) continue;
        if (clients[i].isWebSocket && !clients[i].wsHandshakeDone) continue; // wait for WS handshake
        if (clients[i].isWebSocket) ws_send_text_frame(clients[i].sock, line, n);
        else send(clients[i].sock, line, n, 0);
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
                if (e->hp <= 0) {
                    e->active = 0;
                    // award score to bullet owner
                    int owner = bullets[i].ownerId;
                    if (owner >= 0 && owner < MAX_CLIENTS && clients[owner].connected) {
                        clients[owner].score += 1;
                    }
                }
                bullets[i].active = 0;
                goto bullet_continue;
            }
        }
        // Check player hit (PvP)
        for (int ci = 0; ci < MAX_CLIENTS; ++ci) {
            if (!clients[ci].connected) continue;
            if (clients[ci].worldX != bullets[i].worldX || clients[ci].worldY != bullets[i].worldY) continue;
            if (clients[ci].pos.x == nx && clients[ci].pos.y == ny) {
                // Skip damage on spawn map; otherwise apply and respawn if needed
                if (!map_has_spawn(clients[ci].worldX, clients[ci].worldY)) {
                    if (clients[ci].invincibleTicks <= 0 && clients[ci].hp > 0) {
                        clients[ci].hp--;
                        clients[ci].invincibleTicks = 60; // 3s at 20 FPS (server tick ~50ms)
                        if (clients[ci].hp <= 0) {
                            // award 10 points to shooter on kill
                            int owner = bullets[i].ownerId;
                            if (owner >= 0 && owner < MAX_CLIENTS && clients[owner].connected) {
                                clients[owner].score += 10;
                            }
                            place_near_spawn(&clients[ci]);
                            clients[ci].hp = 3;
                            clients[ci].superTicks = 0;
                            clients[ci].shootCooldown = 0;
                            clients[ci].invincibleTicks = 60;
                        }
                    }
                }
                bullets[i].active = 0;
                goto bullet_continue;
            }
        }
        if (m->tiles[ny][nx] == '#') {
            if (m->wallDmg[ny][nx] < 4) {
                m->wallDmg[ny][nx]++;
            } else {
                m->tiles[ny][nx] = '.';
                m->wallDmg[ny][nx] = 0;
                broadcast_tile(bullets[i].worldX, bullets[i].worldY, nx, ny, '.');
            }
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
            if (!is_map_active(wx, wy)) continue; // optimize: only simulate maps with players
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

static void apply_enemy_contact_damage(void) {
    for (int ci = 0; ci < MAX_CLIENTS; ++ci) {
        if (!clients[ci].connected) continue;
        int wx = clients[ci].worldX;
        int wy = clients[ci].worldY;
        // Skip damage on spawn map
        if (map_has_spawn(wx, wy)) continue;
        // Check enemy collision on this map
        for (int ei = 0; ei < MAX_ENEMIES; ++ei) {
            SrvEnemy *e = &enemies[wy][wx][ei];
            if (!e->active) continue;
            if (e->pos.x == clients[ci].pos.x && e->pos.y == clients[ci].pos.y) {
                if (clients[ci].invincibleTicks <= 0 && clients[ci].hp > 0) {
                    clients[ci].hp--;
                    clients[ci].invincibleTicks = 60; // ~3s at 50ms tick
                    if (clients[ci].hp <= 0) {
                        place_near_spawn(&clients[ci]);
                        clients[ci].hp = 3;
                        clients[ci].superTicks = 0;
                        clients[ci].shootCooldown = 0;
                        clients[ci].invincibleTicks = 60;
                    }
                }
                break; // only one enemy contact per tick
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
    const char *wsport = (argc > 2) ? argv[2] : "5556"; // secondary port for WebSocket

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

    // Second listening socket for WebSocket clients
    struct addrinfo *res2 = NULL; if (getaddrinfo(NULL, wsport, &hints, &res2) != 0) { fprintf(stderr, "getaddrinfo failed (ws)\n"); return 1; }
    sock_t wslsock = (sock_t)socket(res2->ai_family, res2->ai_socktype, res2->ai_protocol);
    setsockopt(wslsock, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
    if (bind(wslsock, res2->ai_addr, (int)res2->ai_addrlen) != 0) { fprintf(stderr, "bind failed (ws)\n"); return 1; }
    if (listen(wslsock, 16) != 0) { fprintf(stderr, "listen failed (ws)\n"); return 1; }
    freeaddrinfo(res2);

    printf("[srv] Listening on port %s (TCP) and %s (WebSocket)\n", port, wsport);
    fflush(stdout);

    fd_set readfds;
    while (1) {
        FD_ZERO(&readfds); FD_SET(lsock, &readfds); FD_SET(wslsock, &readfds); sock_t maxfd = lsock; if (wslsock > maxfd) maxfd = wslsock;
        for (int i = 0; i < MAX_CLIENTS; ++i) { if (clients[i].connected) { FD_SET(clients[i].sock, &readfds); if (clients[i].sock > maxfd) maxfd = clients[i].sock; } }
        struct timeval tv; tv.tv_sec = 0; tv.tv_usec = 50000; // 50ms tick
        select((int)(maxfd+1), &readfds, NULL, NULL, &tv);

        if (FD_ISSET(lsock, &readfds)) {
            struct sockaddr_storage ss; socklen_t slen = sizeof(ss);
            sock_t cs = accept(lsock, (struct sockaddr*)&ss, &slen);
            if (cs >= 0) {
                // Set socket options to reduce latency and detect dead peers
                int one = 1; setsockopt(cs, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
                setsockopt(cs, SOL_SOCKET, SO_KEEPALIVE, (const char*)&one, sizeof(one));
                int idx = -1; for (int i = 0; i < MAX_CLIENTS; ++i) if (!clients[i].connected) { idx = i; break; }
                if (idx >= 0) {
                    clients[idx].connected = 1; clients[idx].sock = cs; clients[idx].color = idx; clients[idx].isWebSocket = 0; clients[idx].wsHandshakeDone = 0; clients[idx].wsBufLen = 0;
                    place_near_spawn(&clients[idx]);
                    clients[idx].facing = DIR_RIGHT;
                    clients[idx].hp = 3;
                    clients[idx].invincibleTicks = 0;
                    clients[idx].superTicks = 0;
                    clients[idx].shootCooldown = 0;
                    clients[idx].score = 0;
                    clients[idx].lastActive = time(NULL);
                    clients[idx].tokens = 10; // start with some burst allowance
                    clients[idx].maxTokens = 20;
                    clients[idx].refillTicks = 2; // every 2 server ticks (~100ms)
                    clients[idx].refillAmount = 1; // add 1 token
                    clients[idx].tickSinceRefill = 0;
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
                    send_text_to_client(idx, you, n);
                    // send an immediate state frame so clients can show themselves without waiting a tick
                    char line[128];
                    char buf[4096]; int off = 0;
                    int n0 = snprintf(line, sizeof(line), "TICK %d\n", g_tick_counter);
                    if (n0 > 0 && off + n0 < (int)sizeof(buf)) { memcpy(buf + off, line, n0); off += n0; }
                    for (int i = 0; i < MAX_CLIENTS; ++i) {
                        int active = clients[i].connected ? 1 : 0;
                        int pn = snprintf(line, sizeof(line), "PLAYER %d %d %d %d %d %d %d %d %d %d %d\n", i, clients[i].worldX, clients[i].worldY, clients[i].pos.x, clients[i].pos.y, clients[i].color, active, clients[i].hp, clients[i].invincibleTicks, clients[i].superTicks, clients[i].score);
                        if (off + pn < (int)sizeof(buf)) { memcpy(buf + off, line, pn); off += pn; }
                    }
                    for (int b = 0; b < MAX_REMOTE_BULLETS; ++b) {
                        if (!bullets[b].active) continue;
                        int bn = snprintf(line, sizeof(line), "BULLET %d %d %d %d %d\n", bullets[b].worldX, bullets[b].worldY, bullets[b].pos.x, bullets[b].pos.y, 1);
                        if (off + bn < (int)sizeof(buf)) { memcpy(buf + off, line, bn); off += bn; }
                    }
                    send_text_to_client(idx, buf, off);
                    // send only the current map snapshot to reduce initial burst
                    send_map_to(idx, clients[idx].worldX, clients[idx].worldY);
                    // signal client it can start accepting input/rendering
                    {
                        const char *ready = "READY\n";
                        send_text_to_client(idx, ready, (int)strlen(ready));
                    }
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

        // Accept WebSocket clients
        if (FD_ISSET(wslsock, &readfds)) {
            struct sockaddr_storage ss; socklen_t slen = sizeof(ss);
            sock_t cs = accept(wslsock, (struct sockaddr*)&ss, &slen);
            if (cs >= 0) {
                int one = 1; setsockopt(cs, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
                setsockopt(cs, SOL_SOCKET, SO_KEEPALIVE, (const char*)&one, sizeof(one));
                int idx = -1; for (int i = 0; i < MAX_CLIENTS; ++i) if (!clients[i].connected) { idx = i; break; }
                if (idx >= 0) {
                    // Determine client IP (for limits)
                    char host[64] = {0}, serv[16] = {0};
                    if (getnameinfo((struct sockaddr*)&ss, slen, host, sizeof(host), serv, sizeof(serv), NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
                        strncpy(host, "?", sizeof(host)-1); strncpy(serv, "?", sizeof(serv)-1);
                    }
                    // Per-IP concurrent limit (disabled)
                    // if (ws_count_active_for_ip(host) >= MAX_WS_PER_IP || !ws_rate_allow(host)) {
                    if (0) {
#ifdef _WIN32
                        closesocket(cs);
#else
                        close(cs);
#endif
                        continue;
                    }
                    clients[idx].connected = 1; clients[idx].sock = cs; clients[idx].color = idx; clients[idx].isWebSocket = 1; clients[idx].wsHandshakeDone = 0; clients[idx].wsBufLen = 0;
                    // Read HTTP headers synchronously (short timeout) and complete WS handshake
                    {
                        // Set a short recv timeout so we don't block the loop too long
                        struct timeval rtv; rtv.tv_sec = 0; rtv.tv_usec = 200000; // 200 ms
                        setsockopt(cs, SOL_SOCKET, SO_RCVTIMEO, (const char*)&rtv, sizeof(rtv));
                        int total = 0;
                        while (total < (int)sizeof(clients[idx].wsBuf) - 1) {
                            char tmp[1024]; int cap = (int)sizeof(tmp) - 1;
                            int rn = (int)recv(cs, tmp, cap, 0);
                            if (rn <= 0) break;
                            int room = (int)sizeof(clients[idx].wsBuf) - 1 - total;
                            if (rn > room) rn = room;
                            memcpy(clients[idx].wsBuf + total, tmp, rn);
                            total += rn; clients[idx].wsBuf[total] = '\0';
                            if (strstr(clients[idx].wsBuf, "\r\n\r\n") || strstr(clients[idx].wsBuf, "\n\n")) break;
                        }
                        clients[idx].wsBufLen = total;
                        int hs = ws_handshake(&clients[idx]);
                        if (hs <= 0) {
                            // Bad handshake; close
                            clients[idx].connected = 0;
#ifdef _WIN32
                            closesocket(cs);
#else
                            close(cs);
#endif
                            clients[idx].sock = 0;
                        } else {
                            // Initialize player state and send YOU + full map
                            clients[idx].facing = DIR_RIGHT;
                            clients[idx].hp = 3;
                            clients[idx].invincibleTicks = 0;
                            clients[idx].superTicks = 0;
                            clients[idx].shootCooldown = 0;
                            clients[idx].score = 0;
                            clients[idx].lastActive = time(NULL);
                            clients[idx].tokens = 10; clients[idx].maxTokens = 20; clients[idx].refillTicks = 2; clients[idx].refillAmount = 1; clients[idx].tickSinceRefill = 0;
                            strncpy(clients[idx].addr, host, sizeof(clients[idx].addr)-1);
                            strncpy(clients[idx].port, serv, sizeof(clients[idx].port)-1);
                            clients[idx].connId = g_nextConnId++;
                            place_near_spawn(&clients[idx]);
                            char you[32]; int yn = snprintf(you, sizeof(you), "YOU %d\n", idx);
                            send_text_to_client(idx, you, yn);
                            // immediate state frame for WS client (before tile snapshot)
                            char line[128];
                            char buf[4096]; int off = 0;
                            int n0 = snprintf(line, sizeof(line), "TICK %d\n", g_tick_counter);
                            if (n0 > 0 && off + n0 < (int)sizeof(buf)) { memcpy(buf + off, line, n0); off += n0; }
                            for (int i = 0; i < MAX_CLIENTS; ++i) {
                                int active = clients[i].connected ? 1 : 0;
                                int pn = snprintf(line, sizeof(line), "PLAYER %d %d %d %d %d %d %d %d %d %d %d\n", i, clients[i].worldX, clients[i].worldY, clients[i].pos.x, clients[i].pos.y, clients[i].color, active, clients[i].hp, clients[i].invincibleTicks, clients[i].superTicks, clients[i].score);
                                if (off + pn < (int)sizeof(buf)) { memcpy(buf + off, line, pn); off += pn; }
                            }
                            for (int b = 0; b < MAX_REMOTE_BULLETS; ++b) {
                                if (!bullets[b].active) continue;
                                int bn = snprintf(line, sizeof(line), "BULLET %d %d %d %d %d\n", bullets[b].worldX, bullets[b].worldY, bullets[b].pos.x, bullets[b].pos.y, 1);
                                if (off + bn < (int)sizeof(buf)) { memcpy(buf + off, line, bn); off += bn; }
                            }
                            send_text_to_client(idx, buf, off);
                    // now send only the current map snapshot (for WS clients)
                    send_map_to(idx, clients[idx].worldX, clients[idx].worldY);
                    // and signal READY
                    {
                        const char *ready = "READY\n";
                        send_text_to_client(idx, ready, (int)strlen(ready));
                    }
                        }
                    }
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

        // Read inputs / WS handshake/frames
        char buf[2048];
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
            // If WS client and not handshaken, accumulate and do handshake
            if (clients[i].isWebSocket && !clients[i].wsHandshakeDone) {
                if (clients[i].wsBufLen + n > (int)sizeof(clients[i].wsBuf)-1) clients[i].wsBufLen = 0; // reset on overflow
                memcpy(clients[i].wsBuf + clients[i].wsBufLen, buf, n);
                clients[i].wsBufLen += n;
                int hs = ws_handshake(&clients[i]);
                if (hs < 0) { // bad handshake
                    clients[i].connected = 0;
#ifdef _WIN32
                    closesocket(clients[i].sock);
#else
                    close(clients[i].sock);
#endif
                    clients[i].sock = 0;
                } else if (hs > 0) {
                    // complete: now send YOU and current map only, then READY
                    place_near_spawn(&clients[i]);
                    clients[i].facing = DIR_RIGHT; clients[i].hp = 3; clients[i].invincibleTicks=0; clients[i].superTicks=0; clients[i].shootCooldown=0; clients[i].score=0; clients[i].lastActive=time(NULL);
                    clients[i].tokens=10; clients[i].maxTokens=20; clients[i].refillTicks=2; clients[i].refillAmount=1; clients[i].tickSinceRefill=0;
                    char you[32]; int yn = snprintf(you, sizeof(you), "YOU %d\n", i);
                    send_text_to_client(i, you, yn);
                    send_map_to(i, clients[i].worldX, clients[i].worldY);
                    {
                        const char *ready = "READY\n";
                        send_text_to_client(i, ready, (int)strlen(ready));
                    }
                }
                continue;
            }

            // If WS framed, deframe text payload into buf
            if (clients[i].isWebSocket) {
                // Simple, single-frame text parser (FIN + TEXT, masked)
                unsigned char *db = (unsigned char*)buf;
                if (n < 2) continue;
                int masked = (db[1] & 0x80) != 0; size_t len = (size_t)(db[1] & 0x7F); size_t off = 2;
                if (len == 126) { if (n < 4) continue; len = (db[2]<<8)|db[3]; off = 4; }
                else if (len == 127) { if (n < 10) continue; len = (size_t)db[9]; off = 10; }
                unsigned char mask[4] = {0,0,0,0};
                if (masked) { if ((int)off + 4 > n) continue; memcpy(mask, db+off, 4); off += 4; }
                if ((int)(off + len) > n) continue;
                unsigned char *payload = db + off;
                for (size_t k = 0; k < len; ++k) payload[k] ^= mask[k & 3];
                n = (int)len; payload[n] = '\0';
                memmove(buf, payload, n + 1);
            }

            // parse simple commands: INPUT dx dy shoot | PING t
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
                } else if (strncmp(p, "PING ", 5) == 0) {
                    // Reflect back the timestamp/token for RTT measurement
                    char line[128]; int rn = snprintf(line, sizeof(line), "PONG %s\n", p + 5);
                    send_text_to_client(i, line, rn);
                } else if (sscanf(p, "INPUT %d %d %d", &dx, &dy, &shoot) == 3) {
                    clients[i].lastActive = time(NULL);
                    // Rate limit: consume one token per INPUT; if none, drop and optionally warn
                    if (clients[i].tokens <= 0) {
                        // send minimal soft warning once in a while
                        // (not strictly necessary for gameplay; keeps bandwidth tiny)
                        // char warn[] = "WARN slow down\n"; send(clients[i].sock, warn, (int)strlen(warn), 0);
                        goto parsed_continue;
                    } else {
                        clients[i].tokens--;
                    }
                    // update facing if a directional input was provided, even if movement is blocked
                    if (dx < 0) clients[i].facing = DIR_LEFT; else if (dx > 0) clients[i].facing = DIR_RIGHT; else if (dy < 0) clients[i].facing = DIR_UP; else if (dy > 0) clients[i].facing = DIR_DOWN;
                    int oldWX = clients[i].worldX;
                    int oldWY = clients[i].worldY;
                    int curx = clients[i].pos.x;
                    int cury = clients[i].pos.y;
                    int nx = curx + dx;
                    int ny = cury + dy;
                    // Preserve orthogonal axis on world transitions and avoid double-crossing on diagonals
                    int crossedX = 0;
                    if (nx < 0) {
                        int entryY = cury;
                        if (clients[i].worldX > 0 && is_open(&world[clients[i].worldY][clients[i].worldX-1], MAP_WIDTH-1, entryY)) {
                            clients[i].worldX--;
                            nx = MAP_WIDTH - 1;
                            ny = entryY;
                            crossedX = 1;
                        }
                    } else if (nx >= MAP_WIDTH) {
                        int entryY = cury;
                        if (clients[i].worldX < WORLD_W - 1 && is_open(&world[clients[i].worldY][clients[i].worldX+1], 0, entryY)) {
                            clients[i].worldX++;
                            nx = 0;
                            ny = entryY;
                            crossedX = 1;
                        }
                    }
                    if (!crossedX) {
                        if (ny < 0) {
                            int entryX = curx;
                            if (clients[i].worldY > 0 && is_open(&world[clients[i].worldY-1][clients[i].worldX], entryX, MAP_HEIGHT-1)) {
                                clients[i].worldY--;
                                ny = MAP_HEIGHT - 1;
                                nx = entryX;
                            }
                        } else if (ny >= MAP_HEIGHT) {
                            int entryX = curx;
                            if (clients[i].worldY < WORLD_H - 1 && is_open(&world[clients[i].worldY+1][clients[i].worldX], entryX, 0)) {
                                clients[i].worldY++;
                                ny = 0;
                                nx = entryX;
                            }
                        }
                    }
                    if (nx >= 0 && nx < MAP_WIDTH && ny >= 0 && ny < MAP_HEIGHT && is_open(&world[clients[i].worldY][clients[i].worldX], nx, ny)) {
                        // Disallow stepping into a tile occupied by another player in the same map
                        int occupied = 0;
                        for (int pj = 0; pj < MAX_CLIENTS; ++pj) {
                            if (pj == i) continue;
                            if (!clients[pj].connected) continue;
                            if (clients[pj].worldX == clients[i].worldX && clients[pj].worldY == clients[i].worldY && clients[pj].pos.x == nx && clients[pj].pos.y == ny) {
                                occupied = 1; break;
                            }
                        }
                        if (!occupied) {
                            clients[i].pos.x = nx; clients[i].pos.y = ny;
                        }
                    }
                    // If world tile changed, send the new map snapshot to this client
                    if (clients[i].worldX != oldWX || clients[i].worldY != oldWY) {
                        // send state first so client can show entities immediately
                        char line[128]; char buf[4096]; int off = 0;
                        int n0 = snprintf(line, sizeof(line), "TICK %d\n", g_tick_counter);
                        if (n0 > 0 && off + n0 < (int)sizeof(buf)) { memcpy(buf + off, line, n0); off += n0; }
                        for (int pj = 0; pj < MAX_CLIENTS; ++pj) {
                            int active = clients[pj].connected ? 1 : 0;
                            int pn = snprintf(line, sizeof(line), "PLAYER %d %d %d %d %d %d %d %d %d %d %d\n", pj, clients[pj].worldX, clients[pj].worldY, clients[pj].pos.x, clients[pj].pos.y, clients[pj].color, active, clients[pj].hp, clients[pj].invincibleTicks, clients[pj].superTicks, clients[pj].score);
                            if (off + pn < (int)sizeof(buf)) { memcpy(buf + off, line, pn); off += pn; }
                        }
                        for (int b = 0; b < MAX_REMOTE_BULLETS; ++b) {
                            if (!bullets[b].active) continue;
                            int bn = snprintf(line, sizeof(line), "BULLET %d %d %d %d %d\n", bullets[b].worldX, bullets[b].worldY, bullets[b].pos.x, bullets[b].pos.y, 1);
                            if (off + bn < (int)sizeof(buf)) { memcpy(buf + off, line, bn); off += bn; }
                        }
                        send_text_to_client(i, buf, off);
                        send_map_to(i, clients[i].worldX, clients[i].worldY);
                    }
                    if (shoot) {
                        // spawn a server bullet in player's facing; if dx/dy provided, infer and override
                        int allow = 0;
                        if (clients[i].superTicks > 0) {
                            allow = 1; // spammable during super
                        } else if (clients[i].shootCooldown <= 0) {
                            allow = 1;
                            clients[i].shootCooldown = 6; // ~300ms at 50ms tick
                        }
                        if (allow) {
                            Direction dir = clients[i].facing;
                            if (dx < 0) dir = DIR_LEFT; else if (dx > 0) dir = DIR_RIGHT; else if (dy < 0) dir = DIR_UP; else if (dy > 0) dir = DIR_DOWN;
                            int slot = -1; for (int bi = 0; bi < MAX_REMOTE_BULLETS; ++bi) if (!bullets[bi].active) { slot = bi; break; }
                            if (slot >= 0) { bullets[slot].active = 1; bullets[slot].worldX = clients[i].worldX; bullets[slot].worldY = clients[i].worldY; bullets[slot].pos = clients[i].pos; bullets[slot].dir = dir; bullets[slot].ownerId = i; }
                        }
                    }
                }
parsed_continue:
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

        if ((g_tick_counter % 2) == 0) step_bullets(); // ~10 steps/sec
        if ((g_tick_counter % 3) == 0) step_enemies(); // ~6-7 steps/sec
        apply_enemy_contact_damage();
        // handle pickups like 'X'
        for (int ci = 0; ci < MAX_CLIENTS; ++ci) {
            if (!clients[ci].connected) continue;
            int wx = clients[ci].worldX;
            int wy = clients[ci].worldY;
            Map *m = &world[wy][wx];
            int x = clients[ci].pos.x;
            int y = clients[ci].pos.y;
            if (x < 0 || x >= MAP_WIDTH || y < 0 || y >= MAP_HEIGHT) continue;
            if (m->tiles[y][x] == 'X') {
                if (clients[ci].hp < 3) clients[ci].hp = 3;
                clients[ci].superTicks = 100; // 5s at 20 ticks/sec
                clients[ci].invincibleTicks = 60; // 3s at 20 ticks/sec
                m->tiles[y][x] = '.';
                broadcast_tile(wx, wy, x, y, '.');
            }
        }
        // tick down timers and refill input tokens
        for (int i = 0; i < MAX_CLIENTS; ++i) {
            if (!clients[i].connected) continue;
            if (clients[i].invincibleTicks > 0) clients[i].invincibleTicks--;
            if (clients[i].superTicks > 0) clients[i].superTicks--;
            if (clients[i].shootCooldown > 0) clients[i].shootCooldown--;
            clients[i].tickSinceRefill++;
            if (clients[i].tickSinceRefill >= clients[i].refillTicks) {
                clients[i].tickSinceRefill = 0;
                clients[i].tokens += clients[i].refillAmount;
                if (clients[i].tokens > clients[i].maxTokens) clients[i].tokens = clients[i].maxTokens;
            }
        }
        broadcast_state();
        g_tick_counter++;
    }

    return 0;
}


