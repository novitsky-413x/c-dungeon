#ifndef TYPES_H
#define TYPES_H

#define MAP_WIDTH 40
#define MAP_HEIGHT 18
#define MAX_ENEMIES 5
#define MAX_PROJECTILES 32

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

#endif // TYPES_H


