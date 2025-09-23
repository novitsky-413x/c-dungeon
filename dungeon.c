#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>

#ifdef _WIN32
#include <conio.h>
#include <windows.h>
#else
#include <unistd.h>
#include <termios.h>
#include <fcntl.h>
#include <time.h>
#endif

// Game configuration
#define MAP_WIDTH 40
#define MAP_HEIGHT 18
#define MAX_ENEMIES 5

typedef struct {
    int x;
    int y;
} Vec2;

typedef struct {
    int isAlive;
    Vec2 pos;
} Enemy;

static char mapData[MAP_HEIGHT][MAP_WIDTH + 1];
static Vec2 playerPos;
static Vec2 exitPos;
static Enemy enemies[MAX_ENEMIES];
static int numEnemies = 0;
static int gameRunning = 1;
static int playerWon = 0;
static int tickCount = 0;
static int needsRedraw = 1;

// Forward declarations
static int isBlocked(int x, int y);

#ifdef _WIN32
static DWORD originalConsoleMode = 0;
#else
static struct termios originalTermios;
#endif

// Utility
static int clamp(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// Terminal control
static void enableAnsiIfNeeded(void) {
#ifdef _WIN32
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == INVALID_HANDLE_VALUE) return;
    DWORD mode = 0;
    if (!GetConsoleMode(hOut, &mode)) return;
    originalConsoleMode = mode;
    mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    SetConsoleMode(hOut, mode);
#endif
}

static void restoreConsoleMode(void) {
#ifdef _WIN32
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE && originalConsoleMode != 0) {
        SetConsoleMode(hOut, originalConsoleMode);
    }
#else
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &originalTermios);
#endif
    // Show cursor again
    printf("\x1b[?25h");
    fflush(stdout);
}

static void clearScreen(void) {
    // Clear and move cursor to home
    printf("\x1b[2J\x1b[H");
}

static void hideCursor(void) {
    printf("\x1b[?25l");
}

// Input
#ifndef _WIN32
static void enableRawMode(void) {
    tcgetattr(STDIN_FILENO, &originalTermios);
    struct termios raw = originalTermios;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0; // non-blocking
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
}
#endif

// Timing
static double now_ms(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq;
    static int initialized = 0;
    if (!initialized) {
        QueryPerformanceFrequency(&freq);
        initialized = 1;
    }
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart * 1000.0 / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
#endif
}

static int readInputNonBlocking(void) {
#ifdef _WIN32
    if (_kbhit()) {
        int c = _getch();
        if (c == 0 || c == 224) {
            // Arrow keys
            int c2 = _getch();
            switch (c2) {
                case 72: return 'w'; // up
                case 80: return 's'; // down
                case 75: return 'a'; // left
                case 77: return 'd'; // right
                default: return 0;
            }
        }
        return c;
    }
    return 0;
#else
    unsigned char c;
    if (read(STDIN_FILENO, &c, 1) == 1) {
        if (c == '\x1b') {
            unsigned char seq[2];
            if (read(STDIN_FILENO, &seq[0], 1) == 1 && read(STDIN_FILENO, &seq[1], 1) == 1) {
                if (seq[0] == '[') {
                    switch (seq[1]) {
                        case 'A': return 'w';
                        case 'B': return 's';
                        case 'D': return 'a';
                        case 'C': return 'd';
                    }
                }
            }
            return 0;
        }
        return c;
    }
    return 0;
#endif
}

// Map setup
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

static void initMap(void) {
    int foundPlayer = 0;
    int foundExit = 0;
    for (int y = 0; y < MAP_HEIGHT; ++y) {
        // Default fill with walls to avoid garbage if defaultMap line is short
        for (int x = 0; x < MAP_WIDTH; ++x) mapData[y][x] = '#';
        mapData[y][MAP_WIDTH] = '\0';

        // Copy up to MAP_WIDTH characters from defaultMap into mapData
        const char *src = defaultMap[y];
        for (int x = 0; x < MAP_WIDTH && src[x] != '\0'; ++x) {
            mapData[y][x] = src[x];
        }

        // Scan for special markers and normalize to floor
        for (int x = 0; x < MAP_WIDTH; ++x) {
            if (mapData[y][x] == '@') {
                playerPos.x = x;
                playerPos.y = y;
                foundPlayer = 1;
                mapData[y][x] = '.';
            } else if (mapData[y][x] == 'X') {
                exitPos.x = x;
                exitPos.y = y;
                foundExit = 1;
                mapData[y][x] = '.';
            }
        }
    }
    if (!foundPlayer) {
        // Find first open tile for player, else fallback to (1,1)
        int placed = 0;
        for (int y = 1; y < MAP_HEIGHT - 1 && !placed; ++y) {
            for (int x = 1; x < MAP_WIDTH - 1 && !placed; ++x) {
                if (!isBlocked(x, y)) { playerPos.x = x; playerPos.y = y; placed = 1; }
            }
        }
        if (!placed) { playerPos.x = 1; playerPos.y = 1; }
    }
    if (!foundExit) {
        // Place exit near bottom-right on first open tile
        int placed = 0;
        for (int y = MAP_HEIGHT - 2; y >= 1 && !placed; --y) {
            for (int x = MAP_WIDTH - 2; x >= 1 && !placed; --x) {
                if (!isBlocked(x, y) && (x != playerPos.x || y != playerPos.y)) { exitPos.x = x; exitPos.y = y; placed = 1; }
            }
        }
        if (!placed) { exitPos.x = MAP_WIDTH - 2; exitPos.y = MAP_HEIGHT - 2; }
    }
}

static int isBlocked(int x, int y) {
    if (x < 0 || x >= MAP_WIDTH || y < 0 || y >= MAP_HEIGHT) return 1;
    char c = mapData[y][x];
    return c == '#';
}

// Enemies
static void spawnEnemies(int count) {
    if (count > MAX_ENEMIES) count = MAX_ENEMIES;
    numEnemies = count;
    for (int i = 0; i < numEnemies; ++i) {
        enemies[i].isAlive = 1;
        // Find random free tile not on player or exit
        for (int attempt = 0; attempt < 1000; ++attempt) {
            int x = rand() % MAP_WIDTH;
            int y = rand() % MAP_HEIGHT;
            if (isBlocked(x, y)) continue;
            if ((x == playerPos.x && y == playerPos.y) || (x == exitPos.x && y == exitPos.y)) continue;
            // Avoid placing right next to player
            if (abs(x - playerPos.x) + abs(y - playerPos.y) < 6) continue;
            enemies[i].pos.x = x;
            enemies[i].pos.y = y;
            break;
        }
    }
}

static int isEnemyAt(int x, int y) {
    for (int i = 0; i < numEnemies; ++i) {
        if (enemies[i].isAlive && enemies[i].pos.x == x && enemies[i].pos.y == y) return 1;
    }
    return 0;
}

static int moveEnemies(void) {
    int moved = 0;
    for (int i = 0; i < numEnemies; ++i) {
        if (!enemies[i].isAlive) continue;
        int dir = rand() % 4;
        int dx = 0, dy = 0;
        switch (dir) {
            case 0: dy = -1; break; // up
            case 1: dy = 1; break;  // down
            case 2: dx = -1; break; // left
            case 3: dx = 1; break;  // right
        }
        int nx = clamp(enemies[i].pos.x + dx, 0, MAP_WIDTH - 1);
        int ny = clamp(enemies[i].pos.y + dy, 0, MAP_HEIGHT - 1);
        if (!isBlocked(nx, ny)) {
            // Avoid stacking: if another enemy is there, skip
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

// Rendering
static void draw(void) {
    char frame[4096];
    int pos = 0;
    int cap = (int)sizeof(frame);

    // Clear and move home in one go
    int n = snprintf(frame + pos, cap - pos, "\x1b[2J\x1b[H");
    if (n < 0) n = 0; pos += n; if (pos < 0) pos = 0; if (pos > cap) pos = cap;

    for (int y = 0; y < MAP_HEIGHT; ++y) {
        for (int x = 0; x < MAP_WIDTH; ++x) {
            char out;
            if (x == playerPos.x && y == playerPos.y) {
                out = '@';
            } else if (x == exitPos.x && y == exitPos.y) {
                out = 'X';
            } else if (isEnemyAt(x, y)) {
                out = 'E';
            } else {
                char c = mapData[y][x];
                out = (c == '#') ? '#' : (c == '.' ? '.' : ' ');
            }
            if (pos < cap) frame[pos++] = out;
        }
        if (pos < cap) frame[pos++] = '\n';
    }
    n = snprintf(frame + pos, cap - pos, "\nUse WASD or Arrow Keys to move. Reach X, avoid E. Press Q to quit.\n");
    if (n < 0) n = 0; pos += n; if (pos > cap) pos = cap;

    fwrite(frame, 1, (size_t)(pos < cap ? pos : cap), stdout);
    fflush(stdout);
}

// Game logic
static int attemptMovePlayer(int dx, int dy) {
    int nx = clamp(playerPos.x + dx, 0, MAP_WIDTH - 1);
    int ny = clamp(playerPos.y + dy, 0, MAP_HEIGHT - 1);
    if (!isBlocked(nx, ny)) {
        if (playerPos.x != nx || playerPos.y != ny) {
            playerPos.x = nx;
            playerPos.y = ny;
            return 1;
        }
    }
    return 0;
}

static void handleInput(void) {
    int c = readInputNonBlocking();
    if (!c) return;
    switch (c) {
        case 'w': case 'W': if (attemptMovePlayer(0, -1)) needsRedraw = 1; break;
        case 's': case 'S': if (attemptMovePlayer(0, 1)) needsRedraw = 1; break;
        case 'a': case 'A': if (attemptMovePlayer(-1, 0)) needsRedraw = 1; break;
        case 'd': case 'D': if (attemptMovePlayer(1, 0)) needsRedraw = 1; break;
        case 'q': case 'Q': gameRunning = 0; break;
        default: break;
    }
}

static void checkWinLose(void) {
    // Lose if any enemy on player
    if (isEnemyAt(playerPos.x, playerPos.y)) {
        gameRunning = 0;
        playerWon = 0;
        return;
    }
    // Win if at exit
    if (playerPos.x == exitPos.x && playerPos.y == exitPos.y) {
        gameRunning = 0;
        playerWon = 1;
    }
}

int main(void) {
    srand((unsigned int)time(NULL));
    enableAnsiIfNeeded();
    atexit(restoreConsoleMode);
    hideCursor();

#ifndef _WIN32
    enableRawMode();
#endif

    initMap();
    spawnEnemies(4);

    // Main loop
    while (gameRunning) {
        double frameStart = now_ms();
        handleInput();

        if ((tickCount % 6) == 0) {
            if (moveEnemies()) needsRedraw = 1;
        }

        checkWinLose();
        if (needsRedraw) {
            draw();
            needsRedraw = 0;
        }

        // 60 FPS cap (~16.67 ms per frame)
        const double targetFrameMs = 16.6667;
        double elapsed = now_ms() - frameStart;
        double remaining = targetFrameMs - elapsed;
        if (remaining > 0) {
#ifdef _WIN32
            Sleep((DWORD)(remaining + 0.5));
#else
            struct timespec req;
            long ms = (long)remaining;
            long ns = (long)((remaining - (double)ms) * 1e6) * 1000L; // convert to nanoseconds
            if (ns < 0) ns = 0;
            req.tv_sec = ms / 1000;
            req.tv_nsec = (ms % 1000) * 1000000L + ns;
            if (req.tv_nsec >= 1000000000L) { req.tv_sec += 1; req.tv_nsec -= 1000000000L; }
            nanosleep(&req, NULL);
#endif
        }
        tickCount++;
    }

    clearScreen();
    if (playerWon) {
        printf("You escaped the dungeon!\n");
    } else {
        printf("You were caught by an enemy. Game Over.\n");
    }
    printf("Thanks for playing.\n");
    return 0;
}


