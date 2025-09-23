#ifndef TYPES_H
#define TYPES_H

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

#endif // TYPES_H


