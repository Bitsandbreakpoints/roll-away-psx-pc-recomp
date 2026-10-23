/*
 * editor_view.cpp - the level editor's 3D view: the level drawn with its
 * world's textures, the camera (right button turns, middle button or
 * Shift + right moves, the wheel zooms), picking through the grid and
 * the tools under the left button.  A new block snaps to the side of the
 * block it is put against (or to the build plane); things go on the open
 * sides of blocks.  The things are drawn as icons over the view.
 */
#include <math.h>
#include <stdio.h>
#include <algorithm>

#include "editor/editor.h"

#define PI                      3.14159265f

/* the variants' colours: yellow, blue, green, red (transporters, buttons, lasers) */
static const ImU32 SIGNAL_COLOURS[4] = {
    IM_COL32(255, 220, 40, 255), IM_COL32(60, 140, 255, 255), IM_COL32(60, 220, 90, 255),
    IM_COL32(240, 60, 50, 255)
};

static int s_hover_cell = -1, s_hover_side = 0;     /* the block under the mouse */
static int s_hover_new = -1;                        /* where the build tool would put one */
static bool s_dragging, s_drag_changed;
static int s_drag_axis = -1, s_drag_coord;          /* build: stay on this plane */
static int s_last_action_cell = -1, s_last_action_side = -1;

static Vec3 cell_lo(int c)
{
    int x, y, z;
    Level::position(c, &x, &y, &z);
    return v3((float) x, (float) y, (float) z);
}

static Vec3 side_centre(int c, int side, float lift)
{
    int n[3];
    Level::side_normal(side, n);
    return cell_lo(c) + v3(0.5f, 0.5f, 0.5f) + v3((float) n[0], (float) n[1], (float) n[2]) * (0.5f + lift);
}

static Vec3 axis_vector(int side)
{
    int n[3];
    Level::side_normal(side, n);
    return v3((float) n[0], (float) n[1], (float) n[2]);
}

/* the top's directions: rotation 1 +Z, 2 -X, 3 -Z, 4 +X; on the other
 * sides the top's +X and +Z become these (tried in the game with the
 * start on every side) */
Vec3 thing_direction(int side, int rotation)
{
    static const float top[4][2] = { { 0, 1 }, { -1, 0 }, { 0, -1 }, { 1, 0 } };
    int r = ((rotation - 1) % 4 + 4) % 4;
    float a = top[r][0], b = top[r][1];
    Vec3 X, Z;
    switch (side) {
    case SIDE_BOTTOM: X = v3(-1, 0, 0), Z = v3(0, 0, 1); break;
    case SIDE_RIGHT: X = v3(0, -1, 0), Z = v3(0, 0, 1); break;
    case SIDE_LEFT: X = v3(0, 1, 0), Z = v3(0, 0, 1); break;
    case SIDE_FRONT: X = v3(-1, 0, 0), Z = v3(0, 1, 0); break;
    case SIDE_BACK: X = v3(-1, 0, 0), Z = v3(0, -1, 0); break;
    default: X = v3(1, 0, 0), Z = v3(0, 0, 1); break;
    }
    return X * a + Z * b;
}

int rotation_facing_camera(int side)
{
    Vec3 eye = g_ed.camera.eye();
    Vec3 forward = g_ed.camera.target - eye;
    int best = 1;
    float best_dot = -1e9f;
    for (int r = 1; r <= 4; r++) {
        Vec3 d = thing_direction(side, r);
        float dd = d.x * forward.x + d.y * forward.y + d.z * forward.z;
        if (dd > best_dot) {
            best_dot = dd;
            best = r;
        }
    }
    return best;
}

/* Walks the grid along the ray; the first block hit and the side it was
 * entered by.  `max_t` limits the walk. */
static bool pick(Vec3 o, Vec3 d, float max_t, int *cell, int *side, float *at)
{
    float t0 = 0, t1 = max_t;
    float ov[3] = { o.x, o.y, o.z }, dv[3] = { d.x, d.y, d.z };
    int entry_axis = -1;
    for (int a = 0; a < 3; a++) {
        if (fabsf(dv[a]) < 1e-9f) {
            if (ov[a] < 0 || ov[a] > GRID) {
                return false;
            }
            continue;
        }
        float ta = (0 - ov[a]) / dv[a], tb = (GRID - ov[a]) / dv[a];
        if (ta > tb) {
            float t = ta;
            ta = tb;
            tb = t;
        }
        if (ta > t0) {
            t0 = ta;
            entry_axis = a;
        }
        if (tb < t1) {
            t1 = tb;
        }
    }
    if (t0 > t1) {
        return false;
    }
    int ix[3], step[3];
    float tmax[3], tdelta[3];
    for (int a = 0; a < 3; a++) {
        float p = ov[a] + dv[a] * (t0 + 1e-4f);
        ix[a] = (int) floorf(p);
        ix[a] = ix[a] < 0 ? 0 : ix[a] >= GRID ? GRID - 1 : ix[a];
        step[a] = dv[a] > 0 ? 1 : -1;
        tdelta[a] = fabsf(dv[a]) > 1e-9f ? fabsf(1.0f / dv[a]) : 1e30f;
        float boundary = (float) (dv[a] > 0 ? ix[a] + 1 : ix[a]);
        tmax[a] = fabsf(dv[a]) > 1e-9f ? (boundary - ov[a]) / dv[a] : 1e30f;
    }
    /* the side entered by: against the step on that axis */
    static const int enter_pos[3] = { SIDE_LEFT, SIDE_BOTTOM, SIDE_BACK };
    static const int enter_neg[3] = { SIDE_RIGHT, SIDE_TOP, SIDE_FRONT };
    int entered = entry_axis < 0 ? SIDE_TOP : (step[entry_axis] > 0 ? enter_pos : enter_neg)[entry_axis];
    float t = t0;
    while (t <= t1) {
        int c = Level::cell(ix[0], ix[1], ix[2]);
        if (g_ed.level.block(c)) {
            *cell = c;
            *side = entered;
            if (at) {
                *at = t;
            }
            return true;
        }
        int a = tmax[0] < tmax[1] ? (tmax[0] < tmax[2] ? 0 : 2) : (tmax[1] < tmax[2] ? 1 : 2);
        t = tmax[a];
        tmax[a] += tdelta[a];
        ix[a] += step[a];
        if (ix[a] < 0 || ix[a] >= GRID) {
            return false;
        }
        entered = (step[a] > 0 ? enter_pos : enter_neg)[a];
    }
    return false;
}

/* the cell of the build plane under the ray, -1 none */
static int pick_plane(Vec3 o, Vec3 d)
{
    if (fabsf(d.y) < 1e-6f) {
        return -1;
    }
    float t = (g_ed.plane_y - o.y) / d.y;
    if (t <= 0) {
        return -1;
    }
    Vec3 p = o + d * t;
    int x = (int) floorf(p.x), z = (int) floorf(p.z);
    return Level::inside(x, g_ed.plane_y, z) ? Level::cell(x, g_ed.plane_y, z) : -1;
}

/* TRUE if the point can be seen (no block of another cell in the way) */
static bool visible(Vec3 p, int own)
{
    Vec3 eye = g_ed.camera.eye();
    Vec3 d = p - eye;
    float len = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
    if (len < 1e-4f) {
        return true;
    }
    d = d * (1.0f / len);
    int c, s;
    float at;
    if (pick(eye, d, len - 0.05f, &c, &s, &at)) {
        return c == own && at > len - 0.6f;
    }
    return true;
}

/* ---- the icons ---- */

static ImU32 shade(ImU32 c, float f)
{
    int r = (int) ((c & 255) * f), g = (int) ((c >> 8 & 255) * f), b = (int) ((c >> 16 & 255) * f);
    r = r > 255 ? 255 : r, g = g > 255 ? 255 : g, b = b > 255 ? 255 : b;
    return IM_COL32(r, g, b, c >> 24);
}

static ImU32 with_alpha(ImU32 c, int a)
{
    return (c & 0x00FFFFFF) | (ImU32) a << 24;
}

static void star(ImDrawList *dl, ImVec2 c, float r, int points, ImU32 col)
{
    ImVec2 p[24];
    for (int i = 0; i < points * 2; i++) {
        float a = -PI / 2 + i * PI / points, rr = i & 1 ? r * 0.45f : r;
        p[i] = ImVec2(c.x + cosf(a) * rr, c.y + sinf(a) * rr);
    }
    dl->AddConcavePolyFilled(p, points * 2, col);
    dl->AddPolyline(p, points * 2, IM_COL32(0, 0, 0, 160), ImDrawFlags_Closed, 1.0f);
}

void draw_thing_icon(ImDrawList *dl, ImVec2 c, float r, int id, int variant, int alpha)
{
    const ThingInfo *info = thing_info(id);
    ImU32 col = with_alpha(info ? info->colour : 0xFFFFFFFF, alpha);
    ImU32 dark = with_alpha(IM_COL32(20, 20, 30, 255), alpha * 3 / 4);
    ImU32 white = with_alpha(IM_COL32(255, 255, 255, 255), alpha);
    switch (id) {
    case THING_START: {
        dl->AddCircleFilled(c, r, white);
        dl->AddQuadFilled(ImVec2(c.x - r * 0.35f, c.y - r * 0.93f), ImVec2(c.x + r * 0.35f, c.y - r * 0.93f),
                          ImVec2(c.x + r * 0.35f, c.y + r * 0.93f), ImVec2(c.x - r * 0.35f, c.y + r * 0.93f),
                          with_alpha(IM_COL32(40, 120, 255, 255), alpha));
        dl->AddCircle(c, r, dark, 0, 1.5f);
        break;
    }
    case THING_KEY:
        dl->AddCircle(ImVec2(c.x - r * 0.45f, c.y), r * 0.38f, col, 0, r * 0.22f);
        dl->AddLine(ImVec2(c.x - r * 0.1f, c.y), ImVec2(c.x + r * 0.9f, c.y), col, r * 0.22f);
        dl->AddLine(ImVec2(c.x + r * 0.55f, c.y), ImVec2(c.x + r * 0.55f, c.y + r * 0.35f), col, r * 0.2f);
        dl->AddLine(ImVec2(c.x + r * 0.85f, c.y), ImVec2(c.x + r * 0.85f, c.y + r * 0.4f), col, r * 0.2f);
        break;
    case THING_EXIT:
    case THING_HIDDEN_EXIT:
        dl->AddRectFilled(ImVec2(c.x - r * 0.7f, c.y - r), ImVec2(c.x + r * 0.7f, c.y + r), col, r * 0.25f);
        dl->AddRect(ImVec2(c.x - r * 0.7f, c.y - r), ImVec2(c.x + r * 0.7f, c.y + r), dark, r * 0.25f, 0, 1.5f);
        dl->AddTriangleFilled(ImVec2(c.x - r * 0.3f, c.y - r * 0.45f), ImVec2(c.x + r * 0.4f, c.y),
                              ImVec2(c.x - r * 0.3f, c.y + r * 0.45f), white);
        break;
    case THING_FRUIT:
        dl->AddCircleFilled(ImVec2(c.x, c.y + r * 0.1f), r * 0.85f, col);
        dl->AddCircleFilled(ImVec2(c.x - r * 0.3f, c.y - r * 0.2f), r * 0.25f, with_alpha(IM_COL32(255, 200, 200, 255), alpha / 2));
        dl->AddTriangleFilled(ImVec2(c.x, c.y - r * 0.6f), ImVec2(c.x + r * 0.7f, c.y - r * 1.0f),
                              ImVec2(c.x + r * 0.3f, c.y - r * 0.5f), with_alpha(IM_COL32(60, 200, 60, 255), alpha));
        break;
    case THING_COIN: {
        static const ImU32 coins[3] = { IM_COL32(255, 200, 40, 255), IM_COL32(210, 215, 225, 255),
                                        IM_COL32(205, 127, 50, 255) };
        ImU32 cc = with_alpha(coins[variant >= 0 && variant < 3 ? variant : 0], alpha);
        dl->AddCircleFilled(c, r * 0.85f, cc);
        dl->AddCircle(c, r * 0.55f, shade(cc, 0.7f), 0, r * 0.12f);
        dl->AddCircle(c, r * 0.85f, dark, 0, 1.0f);
        break;
    }
    case THING_GEM: {
        static const ImU32 gems[3] = { IM_COL32(60, 120, 255, 255), IM_COL32(50, 220, 90, 255),
                                       IM_COL32(240, 50, 60, 255) };
        ImU32 gc = with_alpha(gems[variant >= 0 && variant < 3 ? variant : 0], alpha);
        dl->AddQuadFilled(ImVec2(c.x, c.y - r), ImVec2(c.x + r * 0.75f, c.y), ImVec2(c.x, c.y + r),
                          ImVec2(c.x - r * 0.75f, c.y), gc);
        dl->AddTriangleFilled(ImVec2(c.x, c.y - r), ImVec2(c.x + r * 0.75f, c.y), ImVec2(c.x, c.y), shade(gc, 1.4f));
        dl->AddQuad(ImVec2(c.x, c.y - r), ImVec2(c.x + r * 0.75f, c.y), ImVec2(c.x, c.y + r),
                    ImVec2(c.x - r * 0.75f, c.y), dark, 1.0f);
        break;
    }
    case THING_SUNGLASSES:
        dl->AddCircleFilled(ImVec2(c.x - r * 0.45f, c.y), r * 0.42f, with_alpha(IM_COL32(25, 25, 25, 255), alpha));
        dl->AddCircleFilled(ImVec2(c.x + r * 0.45f, c.y), r * 0.42f, with_alpha(IM_COL32(25, 25, 25, 255), alpha));
        dl->AddLine(ImVec2(c.x - r * 0.1f, c.y - r * 0.1f), ImVec2(c.x + r * 0.1f, c.y - r * 0.1f), white, 2.0f);
        break;
    case THING_BOUNCE_PILL:
    case THING_INVINCIBILITY:
    case THING_LETHARGY:
        dl->AddRectFilled(ImVec2(c.x - r * 0.9f, c.y - r * 0.4f), ImVec2(c.x + r * 0.9f, c.y + r * 0.4f), col, r * 0.4f);
        dl->AddRectFilled(ImVec2(c.x, c.y - r * 0.4f), ImVec2(c.x + r * 0.9f, c.y + r * 0.4f), white, r * 0.4f,
                          ImDrawFlags_RoundCornersRight);
        dl->AddRect(ImVec2(c.x - r * 0.9f, c.y - r * 0.4f), ImVec2(c.x + r * 0.9f, c.y + r * 0.4f), dark, r * 0.4f);
        break;
    case THING_TIMER_FLIP:
        dl->AddTriangleFilled(ImVec2(c.x - r * 0.6f, c.y - r), ImVec2(c.x + r * 0.6f, c.y - r), c, col);
        dl->AddTriangleFilled(ImVec2(c.x - r * 0.6f, c.y + r), ImVec2(c.x + r * 0.6f, c.y + r), c, shade(col, 0.7f));
        dl->AddLine(ImVec2(c.x - r * 0.7f, c.y - r), ImVec2(c.x + r * 0.7f, c.y - r), dark, 2.0f);
        dl->AddLine(ImVec2(c.x - r * 0.7f, c.y + r), ImVec2(c.x + r * 0.7f, c.y + r), dark, 2.0f);
        break;
    case THING_TIME_STOP:
        dl->AddCircleFilled(c, r * 0.9f, col);
        dl->AddCircle(c, r * 0.9f, dark, 0, 1.5f);
        dl->AddLine(c, ImVec2(c.x, c.y - r * 0.6f), dark, 2.0f);
        dl->AddLine(c, ImVec2(c.x + r * 0.45f, c.y), dark, 2.0f);
        break;
    case THING_SPIKES:
    case THING_MOVING_SPIKES:
        for (int i = -1; i <= 1; i++) {
            dl->AddTriangleFilled(ImVec2(c.x + i * r * 0.6f - r * 0.3f, c.y + r * 0.7f),
                                  ImVec2(c.x + i * r * 0.6f, c.y - r * 0.8f),
                                  ImVec2(c.x + i * r * 0.6f + r * 0.3f, c.y + r * 0.7f), col);
        }
        dl->AddLine(ImVec2(c.x - r, c.y + r * 0.7f), ImVec2(c.x + r, c.y + r * 0.7f), dark, 2.0f);
        break;
    case THING_FIRE_PATCH:
    case THING_ICE_PATCH:
    case THING_INVISIBLE_PATCH:
    case THING_ACID_PATCH:
        dl->AddRectFilled(ImVec2(c.x - r * 0.8f, c.y - r * 0.8f), ImVec2(c.x + r * 0.8f, c.y + r * 0.8f),
                          with_alpha(col, alpha * 3 / 4), r * 0.2f);
        dl->AddRect(ImVec2(c.x - r * 0.8f, c.y - r * 0.8f), ImVec2(c.x + r * 0.8f, c.y + r * 0.8f), dark, r * 0.2f);
        if (id == THING_FIRE_PATCH) {
            dl->AddTriangleFilled(ImVec2(c.x - r * 0.4f, c.y + r * 0.5f), ImVec2(c.x, c.y - r * 0.6f),
                                  ImVec2(c.x + r * 0.4f, c.y + r * 0.5f), with_alpha(IM_COL32(255, 230, 80, 255), alpha));
        }
        break;
    case THING_BOUNCE_PAD:
        for (int i = 0; i < 4; i++) {
            float y0 = c.y + r * 0.7f - i * r * 0.4f;
            dl->AddLine(ImVec2(c.x - r * 0.6f, y0), ImVec2(c.x + r * 0.6f, y0 - r * 0.2f), col, 2.5f);
        }
        dl->AddRectFilled(ImVec2(c.x - r * 0.8f, c.y - r * 0.9f), ImVec2(c.x + r * 0.8f, c.y - r * 0.6f), col);
        break;
    case THING_ARROW:
        dl->AddTriangleFilled(ImVec2(c.x, c.y - r), ImVec2(c.x + r * 0.8f, c.y + r * 0.2f),
                              ImVec2(c.x - r * 0.8f, c.y + r * 0.2f), col);
        dl->AddRectFilled(ImVec2(c.x - r * 0.3f, c.y + r * 0.2f), ImVec2(c.x + r * 0.3f, c.y + r), col);
        break;
    case THING_TRANSPORTER: {
        ImU32 tc = with_alpha(SIGNAL_COLOURS[variant & 3], alpha);
        dl->AddCircle(c, r * 0.85f, tc, 0, r * 0.3f);
        dl->AddCircle(c, r * 0.4f, white, 0, 1.5f);
        break;
    }
    case THING_BUTTON: {
        ImU32 bc = with_alpha(SIGNAL_COLOURS[variant & 3], alpha);
        dl->AddCircleFilled(c, r * 0.8f, bc);
        dl->AddCircle(c, r * 0.8f, white, 0, 2.0f);
        dl->AddCircleFilled(ImVec2(c.x - r * 0.2f, c.y - r * 0.2f), r * 0.2f, with_alpha(white, alpha / 2));
        break;
    }
    case THING_SLOW_STAR:
    case THING_FAST_STAR:
        star(dl, c, r, 5, col);
        break;
    case THING_TIRE:
        dl->AddCircle(c, r * 0.7f, col, 0, r * 0.45f);
        dl->AddCircle(c, r * 0.3f, with_alpha(IM_COL32(150, 150, 150, 255), alpha), 0, 2.0f);
        break;
    case THING_CAPTURE_POD:
        dl->PathArcTo(ImVec2(c.x, c.y + r * 0.4f), r * 0.9f, PI, 2 * PI);
        dl->PathFillConvex(col);
        dl->AddLine(ImVec2(c.x - r, c.y + r * 0.4f), ImVec2(c.x + r, c.y + r * 0.4f), dark, 2.0f);
        break;
    case THING_CAPTIVATOR:
        star(dl, c, r, 8, col);
        break;
    case THING_HEDGEHOG:
        star(dl, c, r, 10, col);
        dl->AddCircleFilled(c, r * 0.5f, with_alpha(IM_COL32(160, 110, 70, 255), alpha));
        break;
    default:
        dl->AddCircleFilled(c, r * 0.7f, col);
        break;
    }
}

/* an arrow in the plane of a side: where a thing faces */
static void direction_arrow(ImDrawList *dl, int c, int side, int rotation, ImU32 col)
{
    Vec3 centre = side_centre(c, side, 0.02f);
    Vec3 d = thing_direction(side, rotation);
    Vec3 tip = centre + d * 0.45f, base = centre + d * 0.1f;
    Vec3 n = axis_vector(side);
    Vec3 across = v3(d.y * n.z - d.z * n.y, d.z * n.x - d.x * n.z, d.x * n.y - d.y * n.x) * 0.14f;
    float x0, y0, x1, y1, x2, y2, x3, y3;
    if (renderer_project(base, &x0, &y0) && renderer_project(tip, &x1, &y1)
        && renderer_project(base + d * 0.2f + across, &x2, &y2)
        && renderer_project(base + d * 0.2f - across, &x3, &y3)) {
        dl->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), col, 3.0f);
        dl->AddTriangleFilled(ImVec2(x1, y1), ImVec2(x2, y2), ImVec2(x3, y3), col);
    }
}

/* a reference drawn as an arrow from a to b */
static void link_line(ImDrawList *dl, Vec3 a, Vec3 b, ImU32 col)
{
    float x0, y0, x1, y1;
    if (!renderer_project(a, &x0, &y0) || !renderer_project(b, &x1, &y1)) {
        return;
    }
    /* a curve: lifted in the middle */
    Vec3 mid = (a + b) * 0.5f + v3(0, 0.8f + 0.15f * sqrtf((b.x - a.x) * (b.x - a.x) + (b.z - a.z) * (b.z - a.z)), 0);
    float xm, ym;
    if (!renderer_project(mid, &xm, &ym)) {
        dl->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), col, 2.0f);
        return;
    }
    dl->AddBezierQuadratic(ImVec2(x0, y0), ImVec2(xm, ym), ImVec2(x1, y1), col, 2.0f);
    float dx = x1 - xm, dy = y1 - ym, l = sqrtf(dx * dx + dy * dy);
    if (l > 1) {
        dx /= l, dy /= l;
        dl->AddTriangleFilled(ImVec2(x1, y1), ImVec2(x1 - dx * 10 - dy * 5, y1 - dy * 10 + dx * 5),
                              ImVec2(x1 - dx * 10 + dy * 5, y1 - dy * 10 - dx * 5), col);
    }
    dl->AddCircleFilled(ImVec2(x0, y0), 3.5f, col);
}

static Vec3 link_point(const Link &l)
{
    if (l.side == LINK_SELF) {
        return cell_lo(l.cell) + v3(0.5f, 0.5f, 0.5f);
    }
    return side_centre(l.cell, l.side, 0.2f);
}

/* ---- drawing the level ---- */

static uint32_t to_abgr(ImU32 c)
{
    return c;                           /* (ImU32 is 0xAABBGGRR, as the vertices) */
}

static void draw_level(ImDrawList *dl, int width, int height)
{
    Level &lv = g_ed.level;
    /* the grid's bounds */
    if (g_ed.show_grid) {
        renderer_box_lines(v3(0, 0, 0), v3(GRID, GRID, GRID), 0x30FFFFFFu);
    }
    for (auto &kv : lv.blocks()) {
        const Block &b = kv.second;
        Vec3 lo = cell_lo(kv.first);
        uint32_t tint = g_block_kinds[b.kind].colour;
        if (b.kind == BLOCK_INVISIBLE) {
            tint = (tint & 0x00FFFFFF) | 0x70000000u;
        }
        renderer_cube(lo, lo + v3(1, 1, 1), tint, true);
        if (b.kind != BLOCK_NORMAL) {
            /* a mark of its kind on every side */
            for (int s = 0; s < 6; s++) {
                if (lv.side_open(kv.first, s) && !(b.kind <= BLOCK_ACID && b.sides[s].id())) {
                    int x, y, z;
                    Level::position(kv.first, &x, &y, &z);
                    renderer_decal(x, y, z, s, 0.38f, (g_block_kinds[b.kind].colour & 0x00FFFFFF) | 0xA0000000u);
                }
            }
        }
        if (b.kind == BLOCK_MOVING || b.kind == BLOCK_LASER) {
            /* the path */
            int side = b.prop[1] < 6 ? b.prop[1] : SIDE_RIGHT;
            int far_x = b.prop[7], far_z = b.prop[8], far_y = GRID - 1 - b.prop[9];
            int near_x = b.prop[4], near_z = b.prop[5], near_y = GRID - 1 - b.prop[6];
            Vec3 a = v3(near_x + 0.5f, near_y + 0.5f, near_z + 0.5f), e = v3(far_x + 0.5f, far_y + 0.5f, far_z + 0.5f);
            if (b.kind == BLOCK_MOVING) {
                renderer_line(a, e, 0xC0FFB070u);
                Vec3 lo2 = v3((float) (a.x < e.x ? a.x : e.x), (float) (a.y < e.y ? a.y : e.y),
                              (float) (a.z < e.z ? a.z : e.z)) - v3(0.5f, 0.5f, 0.5f);
                Vec3 hi2 = v3((float) (a.x > e.x ? a.x : e.x), (float) (a.y > e.y ? a.y : e.y),
                              (float) (a.z > e.z ? a.z : e.z)) + v3(0.5f, 0.5f, 0.5f);
                renderer_box_lines(lo2, hi2, 0x80FFB070u);
            } else {
                /* the beam: from the laser's side to the other end of the path */
                ImU32 col = SIGNAL_COLOURS[b.prop[22] & 3];
                Vec3 from = lo + v3(0.5f, 0.5f, 0.5f) + axis_vector(side) * 0.5f;
                Vec3 to = kv.first == Level::cell(near_x, near_y, near_z) ? e : a;
                Vec3 lo3 = v3(fminf(from.x, to.x), fminf(from.y, to.y), fminf(from.z, to.z)) - v3(0.05f, 0.05f, 0.05f);
                Vec3 hi3 = v3(fmaxf(from.x, to.x), fmaxf(from.y, to.y), fmaxf(from.z, to.z)) + v3(0.05f, 0.05f, 0.05f);
                renderer_cube(lo3, hi3, (to_abgr(col) & 0x00FFFFFF) | (b.prop[3] == 1 ? 0xA0000000u : 0x40000000u), false);
            }
        }
        if (b.kind > BLOCK_ACID) {
            continue;
        }
        for (int s = 0; s < 6; s++) {
            const Thing &t = b.sides[s];
            if (!t.id()) {
                continue;
            }
            const ThingInfo *info = thing_info(t.id());
            int x, y, z;
            Level::position(kv.first, &x, &y, &z);
            renderer_decal(x, y, z, s, 0.14f, ((info ? info->colour : 0xFFFFFFFF) & 0x00FFFFFF) | 0x90000000u);
        }
    }
    /* the build plane */
    if (g_ed.plane || lv.blocks().empty()) {
        float y = (float) g_ed.plane_y;
        for (int i = 0; i <= GRID; i++) {
            renderer_line(v3((float) i, y, 0), v3((float) i, y, GRID), 0x40FFD8A0u);
            renderer_line(v3(0, y, (float) i), v3(GRID, y, (float) i), 0x40FFD8A0u);
        }
    }

    /* icons, arrows and references over the view */
    struct Icon {
        float depth;
        ImVec2 at;
        float r;
        int id, variant, cell, side, rotation;
    };
    std::vector<Icon> icons;
    for (auto &kv : lv.blocks()) {
        const Block &b = kv.second;
        if (b.kind > BLOCK_ACID) {
            continue;
        }
        for (int s = 0; s < 6; s++) {
            const Thing &t = b.sides[s];
            if (!t.id()) {
                continue;
            }
            Vec3 p = side_centre(kv.first, s, 0.3f);
            float sx, sy, depth;
            if (!renderer_project(p, &sx, &sy, &depth) || !visible(side_centre(kv.first, s, 0.05f), kv.first)) {
                continue;
            }
            float r = height * 0.045f * 10.0f / (depth + 4.0f);
            r = r < 6 ? 6 : r > 30 ? 30 : r;
            icons.push_back({ depth, ImVec2(sx, sy), r, t.id(), t.w[2], kv.first, s, t.w[1] });
        }
    }
    std::sort(icons.begin(), icons.end(), [](const Icon &a, const Icon &b) { return a.depth > b.depth; });
    for (const Icon &ic : icons) {
        const ThingInfo *info = thing_info(ic.id);
        if (info && (info->flags & THING_ROTATES)) {
            direction_arrow(dl, ic.cell, ic.side, ic.rotation, IM_COL32(255, 255, 255, 220));
        }
        if (g_ed.show_icons) {
            dl->AddCircleFilled(ImVec2(ic.at.x + 1.5f, ic.at.y + 2), ic.r * 0.9f, IM_COL32(0, 0, 0, 60));
            draw_thing_icon(dl, ic.at, ic.r, ic.id, ic.variant, 255);
        }
    }
    if (g_ed.show_links) {
        for (auto &kv : lv.blocks()) {
            const Block &b = kv.second;
            if (b.kind == BLOCK_LASER && b.laser_power.valid()) {
                link_line(dl, cell_lo(kv.first) + v3(0.5f, 0.5f, 0.5f), link_point(b.laser_power),
                          IM_COL32(255, 140, 60, 220));
            }
            if (b.kind > BLOCK_ACID) {
                continue;
            }
            for (int s = 0; s < 6; s++) {
                const Thing &t = b.sides[s];
                if (t.power.valid()) {
                    link_line(dl, side_centre(kv.first, s, 0.2f), link_point(t.power), IM_COL32(255, 140, 60, 220));
                }
                if (t.target.valid()) {
                    link_line(dl, side_centre(kv.first, s, 0.2f), link_point(t.target),
                              t.id() == THING_TRANSPORTER ? IM_COL32(80, 220, 255, 230) : IM_COL32(255, 140, 60, 220));
                }
            }
        }
    }
    (void) width;
}

/* ---- the tools ---- */

static void say_dropped(int before)
{
    int after = 0;
    for (auto &kv : g_ed.level.blocks()) {
        for (int s = 0; s < 6; s++) {
            after += kv.second.kind <= BLOCK_ACID && kv.second.sides[s].id() != 0;
        }
    }
    if (after < before) {
        editor_say("%d thing(s) under the new block were taken away.", before - after);
    }
}

static int thing_total()
{
    int n = 0;
    for (auto &kv : g_ed.level.blocks()) {
        for (int s = 0; s < 6; s++) {
            n += kv.second.kind <= BLOCK_ACID && kv.second.sides[s].id() != 0;
        }
    }
    return n;
}

static void change_once()
{
    if (!s_drag_changed) {
        editor_snapshot();
        s_drag_changed = true;
    }
}

static void put_thing(int cell, int side)
{
    Level &lv = g_ed.level;
    Block *b = lv.block(cell);
    if (!b) {
        return;
    }
    if (b->kind > BLOCK_ACID) {
        editor_say("Things can only go on normal, fire, ice, invisible and acid blocks.");
        return;
    }
    if (!lv.side_open(cell, side)) {
        return;
    }
    Thing &t = b->sides[side];
    if (t.id() == g_ed.thing_id && t.w[2] == g_ed.thing_variant) {
        return;
    }
    change_once();
    if (g_ed.thing_id == THING_START) {
        /* one start: it moves here */
        for (auto &kv : lv.blocks()) {
            for (int s = 0; s < 6; s++) {
                if (kv.second.kind <= BLOCK_ACID && kv.second.sides[s].id() == THING_START) {
                    Level::set_thing(&kv.second.sides[s], THING_NONE);
                }
            }
        }
    }
    Level::set_thing(&t, g_ed.thing_id);
    const ThingInfo *info = thing_info(g_ed.thing_id);
    if (info && (info->flags & (THING_KINDS | THING_COLOURED | THING_TIMED))) {
        t.w[2] = (uint16_t) g_ed.thing_variant;
    }
    if (info && (info->flags & THING_ROTATES)) {
        t.w[1] = (uint16_t) rotation_facing_camera(side);
    }
    g_ed.sel_cell = cell;
    g_ed.sel_side = side;
    editor_changed();
}

/* a build drag: the cell of the first block's layer under the mouse, if
 * it touches a block (new blocks snap on, they do not float) */
static int drag_cell()
{
    if (s_drag_axis < 0) {
        return -1;
    }
    Vec3 o, d;
    ImGuiIO &io = ImGui::GetIO();
    renderer_ray(io.MousePos.x, io.MousePos.y, &o, &d);
    float ov[3] = { o.x, o.y, o.z }, dv[3] = { d.x, d.y, d.z };
    if (fabsf(dv[s_drag_axis]) < 1e-5f) {
        return -1;
    }
    float t = (s_drag_coord + 0.5f - ov[s_drag_axis]) / dv[s_drag_axis];
    if (t <= 0) {
        return -1;
    }
    int p[3];
    for (int a = 0; a < 3; a++) {
        p[a] = a == s_drag_axis ? s_drag_coord : (int) floorf(ov[a] + dv[a] * t);
    }
    if (!Level::inside(p[0], p[1], p[2])) {
        return -1;
    }
    int c = Level::cell(p[0], p[1], p[2]);
    for (int s = 0; s < 6; s++) {
        int n = Level::neighbour(c, s);
        if (n >= 0 && g_ed.level.block(n)) {
            return c;
        }
    }
    return -1;
}

static void act(bool first)
{
    Level &lv = g_ed.level;
    switch (g_ed.tool) {
    case TOOL_BUILD: {
        int c = first ? s_hover_new : drag_cell();
        if (c < 0 || lv.block(c)) {
            return;
        }
        if (first) {
            /* a drag keeps to the layer of the first block (a floor, a wall) */
            int x, y, z, n[3];
            Level::position(c, &x, &y, &z);
            int coords[3] = { x, y, z };
            Level::side_normal(s_hover_cell >= 0 ? s_hover_side : SIDE_TOP, n);
            s_drag_axis = n[0] ? 0 : n[1] ? 1 : 2;
            s_drag_coord = coords[s_drag_axis];
        }
        change_once();
        int before = thing_total();
        lv.add_block(c, g_ed.block_kind);
        say_dropped(before);
        g_ed.sel_cell = c;
        editor_changed();
        break;
    }
    case TOOL_ERASE:
        if (s_hover_cell >= 0) {
            change_once();
            lv.remove_block(s_hover_cell);
            if (g_ed.sel_cell == s_hover_cell) {
                g_ed.sel_cell = -1;
            }
            editor_changed();
        }
        break;
    case TOOL_PAINT: {
        Block *b = s_hover_cell >= 0 ? lv.block(s_hover_cell) : NULL;
        if (!b || b->kind == g_ed.block_kind) {
            return;
        }
        change_once();
        Block keep = *b;
        lv.add_block(s_hover_cell, g_ed.block_kind);
        Block *nb = lv.block(s_hover_cell);
        if (keep.kind <= BLOCK_ACID && g_ed.block_kind <= BLOCK_ACID) {
            for (int s = 0; s < 6; s++) {
                nb->sides[s] = keep.sides[s];
            }
        }
        lv.update_paths();
        g_ed.sel_cell = s_hover_cell;
        editor_changed();
        break;
    }
    case TOOL_THING:
        if (s_hover_cell >= 0) {
            put_thing(s_hover_cell, s_hover_side);
        }
        break;
    case TOOL_SELECT:
        if (first) {
            g_ed.sel_cell = s_hover_cell;
            g_ed.sel_side = s_hover_side;
        }
        break;
    }
}

static void finish_pick()
{
    Level &lv = g_ed.level;
    Block *from = lv.block(g_ed.sel_cell);
    if (!from || s_hover_cell < 0) {
        g_ed.link_pick = PICK_NONE;
        return;
    }
    Block *to = lv.block(s_hover_cell);
    Link l;
    l.cell = s_hover_cell;
    l.side = to->kind == BLOCK_LASER ? LINK_SELF : s_hover_side;
    bool ok = to->kind == BLOCK_LASER
              || (to->kind <= BLOCK_ACID && (to->sides[l.side].id() == THING_TRANSPORTER
                                             || to->sides[l.side].id() == THING_BUTTON));
    if (g_ed.link_pick == PICK_TARGET && from->sides[g_ed.sel_side].id() == THING_TRANSPORTER) {
        ok = to->kind <= BLOCK_ACID && to->sides[l.side].id() == THING_TRANSPORTER;
    }
    if (!ok) {
        editor_say("Choose a transporter, a button or a laser block.");
        return;
    }
    if (l.cell == g_ed.sel_cell && l.side == g_ed.sel_side && g_ed.link_pick != PICK_LASER_POWER) {
        editor_say("A thing cannot refer to itself.");
        return;
    }
    editor_snapshot();
    if (g_ed.link_pick == PICK_LASER_POWER) {
        from->laser_power = l;
    } else if (g_ed.link_pick == PICK_POWER) {
        from->sides[g_ed.sel_side].power = l;
    } else {
        from->sides[g_ed.sel_side].target = l;
    }
    g_ed.link_pick = PICK_NONE;
    editor_changed();
}

void view_focus()
{
    Level &lv = g_ed.level;
    if (g_ed.sel_cell >= 0 && lv.block(g_ed.sel_cell)) {
        g_ed.camera.target = cell_lo(g_ed.sel_cell) + v3(0.5f, 0.5f, 0.5f);
        return;
    }
    if (lv.blocks().empty()) {
        g_ed.camera.target = v3(17, (float) g_ed.plane_y, 17);
        return;
    }
    Vec3 lo = v3(1e9f, 1e9f, 1e9f), hi = v3(-1e9f, -1e9f, -1e9f);
    for (auto &kv : lv.blocks()) {
        Vec3 p = cell_lo(kv.first);
        lo = v3(fminf(lo.x, p.x), fminf(lo.y, p.y), fminf(lo.z, p.z));
        hi = v3(fmaxf(hi.x, p.x + 1), fmaxf(hi.y, p.y + 1), fmaxf(hi.z, p.z + 1));
    }
    g_ed.camera.target = (lo + hi) * 0.5f;
    Vec3 size = hi - lo;
    float extent = fmaxf(size.x, fmaxf(size.y, size.z));
    g_ed.camera.distance = fmaxf(6.0f, extent * 1.4f + 3.0f);
}

void view_frame(int width, int height)
{
    ImGuiIO &io = ImGui::GetIO();
    Camera &cam = g_ed.camera;
    renderer_begin(cam, width, height);

    /* the camera */
    bool in_view = !io.WantCaptureMouse;
    static bool turning, moving;
    if (in_view && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        turning = !io.KeyShift;
        moving = io.KeyShift;
    }
    if (in_view && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
        moving = true;
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Right) && !ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
        turning = moving = false;
    }
    if (turning) {
        cam.yaw -= io.MouseDelta.x * 0.008f;
        cam.pitch += io.MouseDelta.y * 0.008f;
        cam.pitch = fmaxf(-1.5f, fminf(1.5f, cam.pitch));
    }
    if (moving) {
        Vec3 eye = cam.eye();
        Vec3 f = cam.target - eye;
        float l = sqrtf(f.x * f.x + f.y * f.y + f.z * f.z);
        f = f * (1.0f / l);
        Vec3 right = v3(-f.z, 0, f.x);
        float rl = sqrtf(right.x * right.x + right.z * right.z);
        right = rl > 0 ? right * (1.0f / rl) : v3(1, 0, 0);
        Vec3 up = v3(right.y * f.z - right.z * f.y, right.z * f.x - right.x * f.z, right.x * f.y - right.y * f.x);
        float k = cam.distance * 0.0016f;
        cam.target = cam.target - right * (io.MouseDelta.x * k) + up * (io.MouseDelta.y * k);
    }
    if (in_view && io.MouseWheel != 0) {
        cam.distance *= powf(0.88f, io.MouseWheel);
        cam.distance = fmaxf(2.0f, fminf(80.0f, cam.distance));
    }
    if (!io.WantCaptureKeyboard && !io.KeyCtrl) {
        /* WASD / arrows move the camera's point over the level */
        Vec3 eye = cam.eye();
        Vec3 f = cam.target - eye;
        Vec3 fwd = v3(f.x, 0, f.z);
        float fl = sqrtf(fwd.x * fwd.x + fwd.z * fwd.z);
        fwd = fl > 0 ? fwd * (1.0f / fl) : v3(0, 0, 1);
        Vec3 right = v3(-fwd.z, 0, fwd.x);
        float step = io.DeltaTime * (6.0f + cam.distance * 0.5f);
        if (ImGui::IsKeyDown(ImGuiKey_W) || ImGui::IsKeyDown(ImGuiKey_UpArrow)) cam.target = cam.target + fwd * step;
        if (ImGui::IsKeyDown(ImGuiKey_S) || ImGui::IsKeyDown(ImGuiKey_DownArrow)) cam.target = cam.target - fwd * step;
        if (ImGui::IsKeyDown(ImGuiKey_D) || ImGui::IsKeyDown(ImGuiKey_RightArrow)) cam.target = cam.target + right * step;
        if (ImGui::IsKeyDown(ImGuiKey_A) || ImGui::IsKeyDown(ImGuiKey_LeftArrow)) cam.target = cam.target - right * step;
        if (ImGui::IsKeyDown(ImGuiKey_Q)) cam.target.y -= step;
        if (ImGui::IsKeyDown(ImGuiKey_E)) cam.target.y += step;
    }
    renderer_begin(cam, width, height);

    /* what is under the mouse */
    s_hover_cell = -1;
    s_hover_new = -1;
    if (in_view || s_dragging) {
        Vec3 o, d;
        renderer_ray(io.MousePos.x, io.MousePos.y, &o, &d);
        int c, s;
        if (pick(o, d, 1000, &c, &s, NULL)) {
            s_hover_cell = c;
            s_hover_side = s;
            s_hover_new = Level::neighbour(c, s);
        } else if (g_ed.plane || g_ed.level.blocks().empty()) {
            s_hover_new = pick_plane(o, d);
            s_hover_side = SIDE_TOP;
        }
    }

    /* the left button */
    if (in_view && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyAlt) {
        if (g_ed.link_pick != PICK_NONE) {
            finish_pick();
        } else {
            s_dragging = true;
            s_drag_changed = false;
            s_drag_axis = -1;
            act(true);
            s_last_action_cell = s_hover_cell;
            s_last_action_side = s_hover_side;
        }
    } else if (s_dragging && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (g_ed.tool == TOOL_BUILD) {
            act(false);
        } else if (s_hover_cell != s_last_action_cell || s_hover_side != s_last_action_side) {
            if (g_ed.tool != TOOL_SELECT) {
                act(false);
            }
            s_last_action_cell = s_hover_cell;
            s_last_action_side = s_hover_side;
        }
    } else if (s_dragging) {
        s_dragging = false;
    }

    ImDrawList *dl = ImGui::GetBackgroundDrawList();
    draw_level(dl, width, height);

    /* what the tool would do */
    Level &lv = g_ed.level;
    if (g_ed.link_pick != PICK_NONE) {
        if (s_hover_cell >= 0) {
            renderer_box_lines(cell_lo(s_hover_cell) - v3(0.02f, 0.02f, 0.02f),
                               cell_lo(s_hover_cell) + v3(1.02f, 1.02f, 1.02f), 0xFFFFE050u);
        }
    } else if (g_ed.tool == TOOL_BUILD && (s_dragging ? drag_cell() : s_hover_new) >= 0
               && !lv.block(s_dragging ? drag_cell() : s_hover_new)) {
        Vec3 lo = cell_lo(s_dragging ? drag_cell() : s_hover_new);
        uint32_t tint = (g_block_kinds[g_ed.block_kind].colour & 0x00FFFFFF) | 0x60000000u;
        renderer_cube(lo + v3(0.02f, 0.02f, 0.02f), lo + v3(0.98f, 0.98f, 0.98f), tint, true);
        renderer_box_lines(lo, lo + v3(1, 1, 1), 0xFF40A8FFu);
    } else if ((g_ed.tool == TOOL_ERASE || g_ed.tool == TOOL_PAINT || g_ed.tool == TOOL_SELECT)
               && s_hover_cell >= 0) {
        uint32_t col = g_ed.tool == TOOL_ERASE ? 0xFF3246FFu : 0xFF40A8FFu;
        renderer_box_lines(cell_lo(s_hover_cell) - v3(0.02f, 0.02f, 0.02f),
                           cell_lo(s_hover_cell) + v3(1.02f, 1.02f, 1.02f), col);
    } else if (g_ed.tool == TOOL_THING && s_hover_cell >= 0) {
        const Block *b = lv.block(s_hover_cell);
        bool ok = b && b->kind <= BLOCK_ACID && lv.side_open(s_hover_cell, s_hover_side);
        int x, y, z;
        Level::position(s_hover_cell, &x, &y, &z);
        renderer_decal(x, y, z, s_hover_side, 0.05f, ok ? 0x7040A8FFu : 0x603246FFu);
        float sx, sy, depth;
        if (ok && renderer_project(side_centre(s_hover_cell, s_hover_side, 0.3f), &sx, &sy, &depth)) {
            float r = height * 0.045f * 10.0f / (depth + 4.0f);
            r = r < 6 ? 6 : r > 30 ? 30 : r;
            draw_thing_icon(dl, ImVec2(sx, sy), r, g_ed.thing_id, g_ed.thing_variant, 150);
        }
    }
    /* the selection */
    if (g_ed.sel_cell >= 0 && lv.block(g_ed.sel_cell)) {
        Vec3 lo = cell_lo(g_ed.sel_cell);
        renderer_box_lines(lo - v3(0.03f, 0.03f, 0.03f), lo + v3(1.03f, 1.03f, 1.03f), 0xFF40A8FFu);
        int x, y, z;
        Level::position(g_ed.sel_cell, &x, &y, &z);
        renderer_decal(x, y, z, g_ed.sel_side, 0.0f, 0x5040A8FFu);
    }
}

/* (editor.cpp) the hovered block's description for the status bar */
bool view_hover(int *cell, int *side)
{
    *cell = s_hover_cell >= 0 ? s_hover_cell : s_hover_new;
    *side = s_hover_side;
    return *cell >= 0;
}
