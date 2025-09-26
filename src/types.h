#ifndef TYPES_H
#define TYPES_H

#define MAP_WIDTH 40
#define MAP_HEIGHT 18
#define MAX_ENEMIES 5
#define MAX_PROJECTILES 32
#define MAX_REMOTE_PLAYERS 16
#define MAX_REMOTE_BULLETS 64
#define MAX_REMOTE_ENEMIES 128

// Client-side smoothing configuration (kept in sync across clients)
// Interpolate between last and current snapshot for this many ticks
#define REMOTE_INTERP_TICKS 4
// Extrapolate one step beyond current in last movement direction up to this many ticks
#define REMOTE_EXTRAP_TICKS 15

typedef struct {
    int x;
    int y;
} Vec2;

typedef struct {
    int isAlive;
    int hp; // enemies take 2 shots
    Vec2 pos;
} Enemy;

typedef enum {
    DIR_UP = 0,
    DIR_DOWN = 1,
    DIR_LEFT = 2,
    DIR_RIGHT = 3
} Direction;

typedef struct {
    int active;
    Vec2 pos;
    Direction dir;
} Projectile;

typedef struct {
    int active;
    int worldX;
    int worldY;
    Vec2 pos;
    int colorIndex;
    int hp;
    int invincibleTicks;
    int superTicks;
    int score;
    // client-side smoothing
    Vec2 lastPos;
    int lastWorldX;
    int lastWorldY;
    int lastUpdateTick; // client tick when we processed last snapshot
} RemotePlayer;

typedef struct {
    int active;
    int worldX;
    int worldY;
    Vec2 pos;
    // smoothing
    Vec2 lastPos;
    int lastWorldX;
    int lastWorldY;
    int lastUpdateTick;
} RemoteBullet;

typedef struct {
    int active;
    int worldX;
    int worldY;
    Vec2 pos;
    int hp;
} RemoteEnemy;

#endif // TYPES_H


