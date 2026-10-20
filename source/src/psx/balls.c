/*
 * balls.c - (from the American Pool port: round billiard balls drawn in
 * high resolution.)  Roll Away has no balls to mark: never found.
 */
#include "psx/balls.h"

int g_smooth_balls = 0;

BallRecord *balls_find(const void *word)
{
    (void) word;
    return 0;
}

void balls_clear(void)
{
}
