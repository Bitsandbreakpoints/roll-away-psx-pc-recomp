/*
 * balls.h - smooth balls (an enhancement of the PC port).
 *
 * The game draws each ball as a camera-facing picture of a ball (48 x 48
 * texels) with a small HMD model over it that carries the details: the
 * stripe, the number, the red dots of the cue ball.  Details turned away
 * from the camera give no polygons at all, so a solid ball's model is
 * often empty.  With the high-resolution copy on, the PC port marks the
 * packets of both (pc_ball_begin / pc_ball_picture_begin / pc_ball_end
 * around them in src/game/scene.c), skips them in the copy and draws a
 * sphere there per pixel instead: the outline from the ball's projected
 * centre and radius, the stripe and the number discs from the ball's
 * rotation, lit from above with a highlight, antialiased.  The centre
 * and rotation come from the ball's and the camera's matrices, not from
 * the polygons.  The original VRAM still gets the original ball.
 */
#ifndef PSX_BALLS_H
#define PSX_BALLS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const uint32_t *model_start, *model_end;        /* the model's packets (maybe none) */
    const uint32_t *picture_start, *picture_end;    /* the picture's */
    int number;                     /* 0 cue ball, 1..15, 16 the second cue ball (lag) */
    int visible;                    /* in front of the camera: drawn as a sphere */
    float rot[3][3];                /* ball -> view (rows normalised) */
    float cx, cy;                   /* projected centre (screen, without the draw offset) */
    float rx, ry;                   /* projected radius */
    float light[3];                 /* towards the light, in view space */
    unsigned queued;                /* hires queue generation it was queued in, + 1 */
    int command;                    /* ... and its place in the queue */
} BallRecord;

extern int g_smooth_balls;

/* The ball whose model or picture packets contain `word`, or NULL. */
BallRecord *balls_find(const void *word);

/* A new frame's packets begin (GsSetWorkBase): forget the old balls. */
void balls_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* PSX_BALLS_H */
