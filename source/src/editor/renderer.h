/*
 * renderer.h - the level editor's 3D view (Direct3D 11): textured,
 * shaded cubes, see-through ghosts and decals, lines; the orbiting
 * camera and its rays for picking.
 */
#ifndef EDITOR_RENDERER_H
#define EDITOR_RENDERER_H

#include <stdint.h>
#include <vector>

struct ID3D11Device;
struct ID3D11DeviceContext;

struct Vec3 {
    float x, y, z;
};

static inline Vec3 v3(float x, float y, float z) { return Vec3{ x, y, z }; }
static inline Vec3 operator+(Vec3 a, Vec3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline Vec3 operator-(Vec3 a, Vec3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline Vec3 operator*(Vec3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }

/* texture layers: the world's top and side, then plain white */
enum RenderLayer {
    LAYER_TOP = 0,
    LAYER_SIDE = 1,
    LAYER_WHITE = 2,
    LAYER_COUNT = 3
};

struct Camera {
    Vec3 target = { 16.5f, 16.5f, 16.5f };
    float yaw = 0.7f, pitch = 0.6f, distance = 14.0f;
    float fov = 0.9f;
    Vec3 eye() const;
};

bool renderer_init(ID3D11Device *device, ID3D11DeviceContext *context);
void renderer_shutdown();

/* the world's textures (64 x 64, 0xAABBGGRR); NULL for the editor's own */
void renderer_set_textures(const uint32_t *top, const uint32_t *side);

/* one frame: the view is `w` x `h` pixels */
void renderer_begin(const Camera &camera, int w, int h);

/* a cube from `lo` to `hi`; `colour` 0xAABBGGRR tints it (alpha: see-through) */
void renderer_cube(Vec3 lo, Vec3 hi, uint32_t colour, bool textured);
/* a square on side `side` (level_model.h Side) of the cell at (x, y, z), lifted a little */
void renderer_decal(int x, int y, int z, int side, float inset, uint32_t colour);
void renderer_line(Vec3 a, Vec3 b, uint32_t colour);
/* the box's edges */
void renderer_box_lines(Vec3 lo, Vec3 hi, uint32_t colour);

/* draws everything queued since renderer_begin (opaque, then see-through, lines) */
void renderer_flush();

/* screen position of a point; FALSE behind the camera */
bool renderer_project(Vec3 p, float *sx, float *sy, float *depth = 0);
/* the ray through a pixel */
void renderer_ray(float sx, float sy, Vec3 *origin, Vec3 *direction);

#endif /* EDITOR_RENDERER_H */
