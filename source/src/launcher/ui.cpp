/*
 * ui.cpp - Dear ImGui setup: Win32 + Direct3D 11 backends, a system font
 * with Swedish characters, scaling for the monitor's DPI.
 */
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"
#include "launcher/ui.h"
#include "platform/platform.h"
#include "stb_image.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wparam,
                                                             LPARAM lparam);

#define UI_FONT_SIZE            18.0f

static ImFont *s_font_bold;
static ImFont *s_font_display;           /* Bahnschrift: titles and the launcher menu */
static ID3D11SamplerState *s_trilinear;   /* for ui_draw_image_cover() */
static ID3D11SamplerState *s_point;       /* sharp game pixels */
static ID3D11Texture2D *s_video;          /* the game's picture */
static ID3D11ShaderResourceView *s_video_view;
static int s_video_w, s_video_h;
static ID3D11PixelShader *s_crt;          /* UI_VIDEO_CRT; NULL if it did not compile */
static ID3D11PixelShader *s_tv;           /* UI_VIDEO_TV */
static ID3D11PixelShader *s_soft_crt;     /* UI_VIDEO_SOFT_CRT */
static ID3D11PixelShader *s_composite;    /* UI_VIDEO_COMPOSITE */
static ID3D11PixelShader *s_pvm;          /* UI_VIDEO_PVM */
static ID3D11VertexShader *s_full_screen; /* anti-aliasing pass: a triangle over the target */
static ID3D11PixelShader *s_fxaa;         /* NULL if it did not compile: no anti-aliasing */
static ID3D11Texture2D *s_smooth;         /* the picture anti-aliased, with mipmaps */
static ID3D11ShaderResourceView *s_smooth_view;
static ID3D11RenderTargetView *s_smooth_target;
static int s_antialias;                   /* s_smooth holds this frame's picture */
static float s_crt_width;
static ID3D11Buffer *s_crt_constants;
static float s_crt_lines;
static float s_crt_strength;

/* CRT look for the game picture (ui_draw_video): the picture's own lines
 * as scanlines whose width grows with brightness, an aperture grille
 * mask by window pixel, a gentle curvature and darker corners.  The
 * input matches the vertex shader of the Dear ImGui backend. */
/* Soft TV look in the spirit of GTU v0.50 (RetroArch): the limited
 * bandwidth of a TV signal as a Gaussian low-pass measured in the
 * console's pixels (wider across than down), so the whole picture -
 * sprites and text too - is smoothed, whatever the internal resolution.
 * Linear light, no scanlines. */
static const char TV_SHADER[] = R"(
cbuffer Crt : register(b0)
{
    float4 crt;                         /* x: lines, y: pixels across (the console's), z: strength 0..1 */
};
struct PS_INPUT
{
    float4 pos : SV_POSITION;
    float4 col : COLOR0;
    float2 uv  : TEXCOORD0;
};
sampler sampler0;
Texture2D texture0;

static const float SIGMA_X = 0.62;      /* in console pixels (GTU: signal 256 for 320) */
static const float SIGMA_Y = 0.42;      /* in lines (GTU: TV resolution 250 for 240) */

float4 main(PS_INPUT input) : SV_Target
{
    float2 step = float2(1.0 / crt.y, 1.0 / crt.x);
    float3 sum = float3(0.0, 0.0, 0.0);
    float total = 0.0;
    /* taps half a console pixel apart: +-1.5 pixels across, +-1 line down */
    [unroll] for (int j = -2; j <= 2; j++) {
        float dy = j * 0.5;
        float wy = exp(-(dy * dy) / (2.0 * SIGMA_Y * SIGMA_Y));
        [unroll] for (int i = -3; i <= 3; i++) {
            float dx = i * 0.5;
            float w = wy * exp(-(dx * dx) / (2.0 * SIGMA_X * SIGMA_X));
            float3 c = texture0.Sample(sampler0, input.uv + float2(dx, dy) * step).rgb;
            sum += c * c * w;           /* roughly linear light */
            total += w;
        }
    }
    float3 plain = texture0.Sample(sampler0, input.uv).rgb;
    return float4(sqrt(lerp(plain * plain, sum / total, crt.z)), 1.0) * input.col;
}
)";

static const char CRT_SHADER[] = R"(
cbuffer Crt : register(b0)
{
    float4 crt;                         /* x: lines of the picture, z: strength 0..1 */
};
struct PS_INPUT
{
    float4 pos : SV_POSITION;
    float4 col : COLOR0;
    float2 uv  : TEXCOORD0;
};
sampler sampler0;
Texture2D texture0;

float3 fetch(float2 uv)
{
    float3 c = texture0.Sample(sampler0, uv).rgb;
    return c * c;                       /* roughly linear light */
}

float beam(float distance, float3 colour)
{
    float sigma = 0.22 + 0.16 * dot(colour, float3(0.3, 0.59, 0.11));
    return exp(-distance * distance / (2.0 * sigma * sigma));
}

float4 main(PS_INPUT input) : SV_Target
{
    float2 centre = input.uv - 0.5;
    float2 uv = input.uv + centre * dot(centre, centre) * 0.07 * crt.z;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
        return float4(0.0, 0.0, 0.0, 1.0);
    float lines = crt.x;
    float y = uv.y * lines - 0.5;
    float row = floor(y);
    float f = y - row;
    float3 c0 = fetch(float2(uv.x, (row + 0.5) / lines));
    float3 c1 = fetch(float2(uv.x, (row + 1.5) / lines));
    float3 colour = c0 * beam(f, c0) + c1 * beam(1.0 - f, c1);
    colour *= 1.45;
    float3 mask = float3(0.82, 0.82, 0.82);
    int column = (int) fmod(input.pos.x, 3.0);
    if (column == 0) mask.r = 1.12; else if (column == 1) mask.g = 1.12; else mask.b = 1.12;
    colour *= mask;
    float corner = 16.0 * uv.x * uv.y * (1.0 - uv.x) * (1.0 - uv.y);
    colour *= pow(saturate(corner), 0.18);
    colour = lerp(fetch(uv), colour, crt.z);
    return float4(sqrt(saturate(colour)), 1.0) * input.col;
}
)";

/* Soft CRT: a consumer TV.  The signal's limited bandwidth (a Gaussian
 * low-pass in the console's pixels, as the soft TV filter) smooths the
 * whole picture - 3D, sprites and text alike; then each of the picture's
 * lines is a beam whose width grows with brightness (soft scanlines that
 * blend into each other), a faint shadow mask, a glow around bright parts
 * (halation), a slight curvature and darker corners.  Linear light. */
static const char SOFT_CRT_SHADER[] = R"(
cbuffer Crt : register(b0)
{
    float4 crt;                         /* x: lines, y: pixels across (the console's), z: strength 0..1 */
};
struct PS_INPUT
{
    float4 pos : SV_POSITION;
    float4 col : COLOR0;
    float2 uv  : TEXCOORD0;
};
sampler sampler0;
Texture2D texture0;

static const float SIGMA_X = 0.70;      /* the signal's softness, in console pixels */
static const float GLOW_SIGMA = 1.6;    /* halation, in console pixels */
static const float GLOW = 0.16;
static const float CURVE = 0.045;

float3 fetch(float2 uv)
{
    float3 c = texture0.Sample(sampler0, uv).rgb;
    return c * c;                       /* roughly linear light */
}

/* one line of the picture, low-passed across, at height v (its centre) */
float3 line_colour(float u, float v)
{
    float step = 1.0 / crt.y;
    float3 sum = float3(0.0, 0.0, 0.0);
    float total = 0.0;
    [unroll] for (int i = -3; i <= 3; i++) {
        float dx = i * 0.5;
        float w = exp(-(dx * dx) / (2.0 * SIGMA_X * SIGMA_X));
        sum += fetch(float2(u + dx * step, v)) * w;
        total += w;
    }
    return sum / total;
}

float beam(float distance, float3 colour)
{
    float sigma = 0.235 + 0.15 * dot(colour, float3(0.3, 0.59, 0.11));
    return exp(-distance * distance / (2.0 * sigma * sigma));
}

float4 main(PS_INPUT input) : SV_Target
{
    float2 centre = input.uv - 0.5;
    float2 uv = input.uv + centre * dot(centre, centre) * CURVE * crt.z;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
        return float4(0.0, 0.0, 0.0, 1.0);
    float lines = crt.x;
    float y = uv.y * lines - 0.5;
    float row = floor(y);
    float f = y - row;
    float3 c0 = line_colour(uv.x, (row + 0.5) / lines);
    float3 c1 = line_colour(uv.x, (row + 1.5) / lines);
    float3 colour = c0 * beam(f, c0) + c1 * beam(1.0 - f, c1);
    colour *= 1.42;
    /* halation: a wide blur of the picture added on top */
    float2 step = float2(1.0 / crt.y, 1.0 / lines);
    float3 glow = float3(0.0, 0.0, 0.0);
    float total = 0.0;
    [unroll] for (int j = -2; j <= 2; j++) {
        [unroll] for (int i = -2; i <= 2; i++) {
            float2 d = float2(i, j) * GLOW_SIGMA * 0.8;
            float w = exp(-dot(d, d) / (2.0 * GLOW_SIGMA * GLOW_SIGMA));
            glow += fetch(uv + d * step) * w;
            total += w;
        }
    }
    colour += glow / total * GLOW;
    /* a faint shadow mask (triads of window pixels) */
    float3 mask = float3(0.94, 0.94, 0.94);
    int column = (int) fmod(input.pos.x + floor(input.pos.y / 2.0) * 1.5, 3.0);
    if (column == 0) mask.r = 1.06; else if (column == 1) mask.g = 1.06; else mask.b = 1.06;
    colour *= mask;
    float corner = 16.0 * uv.x * uv.y * (1.0 - uv.x) * (1.0 - uv.y);
    colour *= pow(saturate(corner), 0.15);
    colour = lerp(fetch(uv), colour, crt.z);
    return float4(sqrt(saturate(colour)), 1.0) * input.col;
}
)";

/* Composite CRT: a TV fed by a composite cable.  Brightness and colour
 * are low-passed apart (YIQ): the colour is blurred much wider than the
 * brightness, so it bleeds past edges; soft scanlines and a slight
 * curvature.  Linear light for the scanlines. */
static const char COMPOSITE_SHADER[] = R"(
cbuffer Crt : register(b0)
{
    float4 crt;                         /* x: lines, y: pixels across (the console's), z: strength 0..1 */
};
struct PS_INPUT
{
    float4 pos : SV_POSITION;
    float4 col : COLOR0;
    float2 uv  : TEXCOORD0;
};
sampler sampler0;
Texture2D texture0;

static const float LUMA_SIGMA = 0.65;   /* in console pixels */
static const float CHROMA_SIGMA = 1.9;
static const float CURVE = 0.04;

float3 to_yiq(float3 c)
{
    return float3(dot(c, float3(0.299, 0.587, 0.114)), dot(c, float3(0.596, -0.274, -0.322)),
                  dot(c, float3(0.211, -0.523, 0.312)));
}

float3 to_rgb(float3 c)
{
    return float3(dot(c, float3(1.0, 0.956, 0.621)), dot(c, float3(1.0, -0.272, -0.647)),
                  dot(c, float3(1.0, -1.106, 1.703)));
}

/* one line of the picture as the cable carries it, in linear light */
float3 line_colour(float u, float v)
{
    float step = 1.0 / crt.y;
    float luma = 0.0, luma_total = 0.0;
    float2 chroma = float2(0.0, 0.0);
    float chroma_total = 0.0;
    [unroll] for (int i = -6; i <= 6; i++) {
        float dx = i * 0.5;
        float3 yiq = to_yiq(texture0.Sample(sampler0, float2(u + dx * step, v)).rgb);
        float wl = exp(-(dx * dx) / (2.0 * LUMA_SIGMA * LUMA_SIGMA));
        float wc = exp(-(dx * dx) / (2.0 * CHROMA_SIGMA * CHROMA_SIGMA));
        luma += yiq.x * wl;
        luma_total += wl;
        chroma += yiq.yz * wc;
        chroma_total += wc;
    }
    float3 c = saturate(to_rgb(float3(luma / luma_total, chroma / chroma_total)));
    return c * c;
}

float4 main(PS_INPUT input) : SV_Target
{
    float2 centre = input.uv - 0.5;
    float2 uv = input.uv + centre * dot(centre, centre) * CURVE * crt.z;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
        return float4(0.0, 0.0, 0.0, 1.0);
    float lines = crt.x;
    float y = uv.y * lines - 0.5;
    float row = floor(y);
    float f = y - row;
    float3 c0 = line_colour(uv.x, (row + 0.5) / lines);
    float3 c1 = line_colour(uv.x, (row + 1.5) / lines);
    float s0 = 0.25 + 0.15 * dot(c0, float3(0.3, 0.59, 0.11));
    float s1 = 0.25 + 0.15 * dot(c1, float3(0.3, 0.59, 0.11));
    float3 colour = c0 * exp(-f * f / (2.0 * s0 * s0))
                    + c1 * exp(-(1.0 - f) * (1.0 - f) / (2.0 * s1 * s1));
    colour *= 1.38;
    float corner = 16.0 * uv.x * uv.y * (1.0 - uv.x) * (1.0 - uv.y);
    colour *= pow(saturate(corner), 0.15);
    float3 plain = texture0.Sample(sampler0, uv).rgb;
    colour = lerp(plain * plain, colour, crt.z);
    return float4(sqrt(saturate(colour)), 1.0) * input.col;
}
)";

/* PVM: a studio monitor.  Little softness across, narrow beams (clear,
 * dark scanlines), an aperture grille and a flat screen. */
static const char PVM_SHADER[] = R"(
cbuffer Crt : register(b0)
{
    float4 crt;                         /* x: lines, y: pixels across (the console's), z: strength 0..1 */
};
struct PS_INPUT
{
    float4 pos : SV_POSITION;
    float4 col : COLOR0;
    float2 uv  : TEXCOORD0;
};
sampler sampler0;
Texture2D texture0;

static const float SIGMA_X = 0.38;      /* in console pixels */

float3 line_colour(float u, float v)
{
    float step = 1.0 / crt.y;
    float3 sum = float3(0.0, 0.0, 0.0);
    float total = 0.0;
    [unroll] for (int i = -2; i <= 2; i++) {
        float dx = i * 0.5;
        float w = exp(-(dx * dx) / (2.0 * SIGMA_X * SIGMA_X));
        float3 c = texture0.Sample(sampler0, float2(u + dx * step, v)).rgb;
        sum += c * c * w;
        total += w;
    }
    return sum / total;
}

float4 main(PS_INPUT input) : SV_Target
{
    float lines = crt.x;
    float y = input.uv.y * lines - 0.5;
    float row = floor(y);
    float f = y - row;
    float3 c0 = line_colour(input.uv.x, (row + 0.5) / lines);
    float3 c1 = line_colour(input.uv.x, (row + 1.5) / lines);
    float s0 = 0.17 + 0.13 * dot(c0, float3(0.3, 0.59, 0.11));
    float s1 = 0.17 + 0.13 * dot(c1, float3(0.3, 0.59, 0.11));
    float3 colour = c0 * exp(-f * f / (2.0 * s0 * s0))
                    + c1 * exp(-(1.0 - f) * (1.0 - f) / (2.0 * s1 * s1));
    colour *= 1.7;
    float3 mask = float3(0.78, 0.78, 0.78);
    int column = (int) fmod(input.pos.x, 3.0);
    if (column == 0) mask.r = 1.15; else if (column == 1) mask.g = 1.15; else mask.b = 1.15;
    colour *= mask;
    float3 plain = texture0.Sample(sampler0, input.uv).rgb;
    colour = lerp(plain * plain, colour, crt.z);
    return float4(sqrt(saturate(colour)), 1.0) * input.col;
}
)";

/* Anti-aliasing (ui_video_update): FXAA on the picture at its own
 * resolution, before the picture filters.  Where the brightness changes
 * sharply, the edge's direction and its ends are found along it, and the
 * pixel is sampled across the edge by how far it is from the nearer end -
 * a stair step becomes a ramp.  Lone bright pixels are blended with their
 * neighbours (SUBPIXEL).  A triangle over the target, no vertex buffer. */
static const char FULL_SCREEN_SHADER[] = R"(
struct VS_OUTPUT
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};
VS_OUTPUT main(uint id : SV_VertexID)
{
    VS_OUTPUT output;
    output.uv = float2((id << 1) & 2, id & 2);
    output.pos = float4(output.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}
)";

static const char FXAA_SHADER[] = R"(
struct PS_INPUT
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};
sampler sampler0;
Texture2D texture0;

static const float EDGE_MIN = 0.0312;   /* darker contrasts are left alone */
static const float EDGE_SHARE = 0.125;  /* ... and those under 1/8 of the brightest */
static const float SUBPIXEL = 0.75;
static const float3 LUMA = float3(0.299, 0.587, 0.114);
#define SEARCH_STEPS 12
static const float STEP_SIZE[SEARCH_STEPS] = { 1.0, 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0 };

float luma_at(float2 uv)
{
    return dot(texture0.SampleLevel(sampler0, uv, 0).rgb, LUMA);
}

float neighbour(float2 uv, int2 offset)
{
    return dot(texture0.SampleLevel(sampler0, uv, 0, offset).rgb, LUMA);
}

float4 main(PS_INPUT input) : SV_Target
{
    float width, height;
    texture0.GetDimensions(width, height);
    float2 texel = float2(1.0 / width, 1.0 / height);
    float2 uv = input.uv;
    float3 centre = texture0.SampleLevel(sampler0, uv, 0).rgb;
    float m = dot(centre, LUMA);
    float n = neighbour(uv, int2(0, -1)), s = neighbour(uv, int2(0, 1));
    float w = neighbour(uv, int2(-1, 0)), e = neighbour(uv, int2(1, 0));
    float lo = min(m, min(min(n, s), min(w, e)));
    float hi = max(m, max(max(n, s), max(w, e)));
    float range = hi - lo;
    if (range < max(EDGE_MIN, hi * EDGE_SHARE)) {
        return float4(centre, 1.0);
    }
    float nw = neighbour(uv, int2(-1, -1)), ne = neighbour(uv, int2(1, -1));
    float sw = neighbour(uv, int2(-1, 1)), se = neighbour(uv, int2(1, 1));

    /* a horizontal edge changes down the picture */
    float across_x = abs(-2.0 * w + nw + sw) + 2.0 * abs(-2.0 * m + n + s) + abs(-2.0 * e + ne + se);
    float across_y = abs(-2.0 * n + nw + ne) + 2.0 * abs(-2.0 * m + w + e) + abs(-2.0 * s + sw + se);
    bool horizontal = across_x >= across_y;

    /* which side the edge is on: the steeper one */
    float luma1 = horizontal ? n : w, luma2 = horizontal ? s : e;
    float gradient1 = luma1 - m, gradient2 = luma2 - m;
    float side = horizontal ? texel.y : texel.x;
    float local;
    if (abs(gradient1) >= abs(gradient2)) {
        side = -side;
        local = 0.5 * (luma1 + m);
    } else {
        local = 0.5 * (luma2 + m);
    }
    float threshold = 0.25 * max(abs(gradient1), abs(gradient2));

    /* the edge's two ends, between the two rows (bilinear: their average) */
    float2 edge = uv + (horizontal ? float2(0.0, 0.5 * side) : float2(0.5 * side, 0.0));
    float2 along = horizontal ? float2(texel.x, 0.0) : float2(0.0, texel.y);
    float2 uv1 = edge - along, uv2 = edge + along;
    float end1 = luma_at(uv1) - local, end2 = luma_at(uv2) - local;
    bool done1 = abs(end1) >= threshold, done2 = abs(end2) >= threshold;
    [loop] for (int i = 1; i < SEARCH_STEPS && !(done1 && done2); i++) {
        if (!done1) {
            uv1 -= along * STEP_SIZE[i];
            end1 = luma_at(uv1) - local;
            done1 = abs(end1) >= threshold;
        }
        if (!done2) {
            uv2 += along * STEP_SIZE[i];
            end2 = luma_at(uv2) - local;
            done2 = abs(end2) >= threshold;
        }
    }
    float distance1 = horizontal ? uv.x - uv1.x : uv.y - uv1.y;
    float distance2 = horizontal ? uv2.x - uv.x : uv2.y - uv.y;
    bool nearer1 = distance1 < distance2;
    float offset = 0.5 - min(distance1, distance2) / (distance1 + distance2);
    /* only if the nearer end goes the other way than this pixel */
    if (((nearer1 ? end1 : end2) < 0.0) == (m < local)) {
        offset = 0.0;
    }
    float average = (2.0 * (n + s + w + e) + nw + ne + sw + se) / 12.0;
    float sub = saturate(abs(average - m) / range);
    sub = (-2.0 * sub + 3.0) * sub * sub;
    offset = max(offset, sub * sub * SUBPIXEL);
    uv += horizontal ? float2(0.0, offset * side) : float2(offset * side, 0.0);
    return float4(texture0.SampleLevel(sampler0, uv, 0).rgb, 1.0);
}
)";

static intptr_t ui_message_hook(void *hwnd, unsigned msg, uintptr_t wparam, intptr_t lparam)
{
    return ImGui_ImplWin32_WndProcHandler((HWND) hwnd, msg, wparam, lparam);
}

static void apply_style(float scale)
{
    ImGuiStyle &style = ImGui::GetStyle();
    ImGui::StyleColorsDark();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.FramePadding = ImVec2(10, 6);
    style.ItemSpacing = ImVec2(10, 8);
    ImVec4 *c = style.Colors;
    /* navy blue controls with orange accents: sky and desert sand */
    c[ImGuiCol_Button] = ImVec4(0.08f, 0.17f, 0.25f, 1.0f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.16f, 0.32f, 0.57f, 1.0f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.13f, 0.25f, 0.47f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.10f, 0.22f, 0.33f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.14f, 0.27f, 0.46f, 1.0f);
    c[ImGuiCol_SliderGrabActive] = ImVec4(1.00f, 0.68f, 0.27f, 1.0f);
    c[ImGuiCol_PopupBg] = ImVec4(0.04f, 0.07f, 0.13f, 0.97f);
    c[ImGuiCol_Separator] = ImVec4(1.0f, 1.0f, 1.0f, 0.10f);
    c[ImGuiCol_TableHeaderBg] = ImVec4(0.06f, 0.13f, 0.20f, 1.0f);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.03f);
    c[ImGuiCol_Header] = ImVec4(0.14f, 0.28f, 0.51f, 0.7f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.18f, 0.36f, 0.67f, 0.8f);
    c[ImGuiCol_Tab] = ImVec4(0.09f, 0.19f, 0.34f, 1.0f);
    c[ImGuiCol_TabHovered] = ImVec4(0.18f, 0.36f, 0.67f, 1.0f);
    c[ImGuiCol_TabSelected] = ImVec4(0.15f, 0.31f, 0.57f, 1.0f);
    c[ImGuiCol_FrameBg] = ImVec4(0.08f, 0.18f, 0.25f, 1.0f);
    c[ImGuiCol_SliderGrab] = ImVec4(1.00f, 0.54f, 0.20f, 1.0f);
    c[ImGuiCol_CheckMark] = ImVec4(1.00f, 0.65f, 0.23f, 1.0f);
    c[ImGuiCol_WindowBg] = ImVec4(0.03f, 0.06f, 0.10f, 1.0f);
    c[ImGuiCol_TitleBg] = ImVec4(0.06f, 0.13f, 0.24f, 1.0f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.14f, 0.28f, 0.51f, 1.0f);
    c[ImGuiCol_TitleBgCollapsed] = ImVec4(0.06f, 0.13f, 0.24f, 0.8f);
    c[ImGuiCol_ResizeGrip] = ImVec4(0.18f, 0.36f, 0.67f, 0.3f);
    c[ImGuiCol_ResizeGripHovered] = ImVec4(0.18f, 0.36f, 0.67f, 0.7f);
    c[ImGuiCol_ResizeGripActive] = ImVec4(1.00f, 0.65f, 0.23f, 0.9f);
    c[ImGuiCol_NavCursor] = ImVec4(1.00f, 0.65f, 0.23f, 1.0f);
    c[ImGuiCol_NavCursor] = ImVec4(0.17f, 0.69f, 1.00f, 0.9f);
    style.ScaleAllSizes(scale);
}

/* a pixel shader for the picture (input as the Dear ImGui vertex shader
 * gives it); NULL if it does not compile */
static ID3D11PixelShader *compile_pixel_shader(ID3D11Device *device, const char *source,
                                               size_t length, const char *name)
{
    ID3DBlob *code = NULL, *errors = NULL;
    ID3D11PixelShader *shader = NULL;
    if (SUCCEEDED(D3DCompile(source, length, name, NULL, NULL, "main", "ps_4_0",
                             D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors))) {
        device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), NULL, &shader);
        code->Release();
    } else if (errors) {
        OutputDebugStringA((const char *) errors->GetBufferPointer());
    }
    if (errors) {
        errors->Release();
    }
    return shader;
}

static void free_smooth(void)
{
    if (s_smooth) {
        s_smooth_target->Release();
        s_smooth_view->Release();
        s_smooth->Release();
        s_smooth_target = NULL;
        s_smooth_view = NULL;
        s_smooth = NULL;
    }
    s_antialias = 0;
}

/* The anti-aliasing pass: s_video through FXAA into s_smooth (as large),
 * then its mipmaps, so that a picture larger than the window is averaged
 * when shrunk rather than skipping pixels.  The render target is put back;
 * Dear ImGui sets the rest of the state itself when it draws. */
static int antialias(int w, int h)
{
    ID3D11Device *device = (ID3D11Device *) platform_d3d_device();
    ID3D11DeviceContext *context = (ID3D11DeviceContext *) platform_d3d_context();
    if (!s_fxaa || !s_full_screen) {
        return 0;
    }
    D3D11_TEXTURE2D_DESC desc = {};
    if (s_smooth) {
        s_smooth->GetDesc(&desc);
        if ((int) desc.Width != w || (int) desc.Height != h) {
            free_smooth();
        }
    }
    if (!s_smooth) {
        desc = {};
        desc.Width = w;
        desc.Height = h;
        desc.MipLevels = 0;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        desc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        if (FAILED(device->CreateTexture2D(&desc, NULL, &s_smooth))) {
            s_smooth = NULL;
            return 0;
        }
        if (FAILED(device->CreateShaderResourceView(s_smooth, NULL, &s_smooth_view))
            || FAILED(device->CreateRenderTargetView(s_smooth, NULL, &s_smooth_target))) {
            if (s_smooth_view) {
                s_smooth_view->Release();
                s_smooth_view = NULL;
            }
            s_smooth->Release();
            s_smooth = NULL;
            return 0;
        }
    }

    ID3D11RenderTargetView *old_target = NULL;
    ID3D11DepthStencilView *old_depth = NULL;
    context->OMGetRenderTargets(1, &old_target, &old_depth);
    D3D11_VIEWPORT viewport = { 0, 0, (float) w, (float) h, 0, 1 };
    context->OMSetRenderTargets(1, &s_smooth_target, NULL);
    context->RSSetViewports(1, &viewport);
    context->RSSetState(NULL);
    context->OMSetBlendState(NULL, NULL, 0xFFFFFFFF);
    context->OMSetDepthStencilState(NULL, 0);
    context->IASetInputLayout(NULL);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(s_full_screen, NULL, 0);
    context->GSSetShader(NULL, NULL, 0);
    context->PSSetShader(s_fxaa, NULL, 0);
    context->PSSetShaderResources(0, 1, &s_video_view);
    context->PSSetSamplers(0, 1, &s_trilinear);
    context->Draw(3, 0);
    context->OMSetRenderTargets(1, &old_target, old_depth);
    if (old_target) {
        old_target->Release();
    }
    if (old_depth) {
        old_depth->Release();
    }
    context->GenerateMips(s_smooth_view);
    return 1;
}

void ui_init(void)
{
    float scale = platform_dpi_scale();
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = NULL;                  /* no imgui.ini next to the exe */
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;

    /* Segoe UI from Windows (not shipped with the port); fallback: built-in font */
    wchar_t windir[MAX_PATH];
    char fonts[MAX_PATH * 3], font[MAX_PATH * 3];
    GetWindowsDirectoryW(windir, MAX_PATH);
    WideCharToMultiByte(CP_UTF8, 0, windir, -1, fonts, sizeof(fonts), NULL, NULL);
    snprintf(font, sizeof(font), "%s\\Fonts\\segoeui.ttf", fonts);
    ImFont *regular = NULL;
    if (GetFileAttributesA(font) != INVALID_FILE_ATTRIBUTES) {
        regular = io.Fonts->AddFontFromFileTTF(font, UI_FONT_SIZE * scale);
    }
    if (!regular) {
        ImFontConfig config;
        config.SizePixels = 13.0f * scale;
        regular = io.Fonts->AddFontDefault(&config);
    }
    snprintf(font, sizeof(font), "%s\\Fonts\\segoeuib.ttf", fonts);
    if (GetFileAttributesA(font) != INVALID_FILE_ATTRIBUTES) {
        s_font_bold = io.Fonts->AddFontFromFileTTF(font, UI_FONT_SIZE * scale);
    }
    if (!s_font_bold) {
        s_font_bold = regular;
    }
    snprintf(font, sizeof(font), "%s\\Fonts\\bahnschrift.ttf", fonts);
    if (GetFileAttributesA(font) != INVALID_FILE_ATTRIBUTES) {
        s_font_display = io.Fonts->AddFontFromFileTTF(font, UI_FONT_SIZE * scale);
    }
    if (!s_font_display) {
        s_font_display = s_font_bold;
    }
    apply_style(scale);
    ImGui_ImplWin32_Init(platform_hwnd());
    ImGui_ImplDX11_Init((ID3D11Device *) platform_d3d_device(),
                        (ID3D11DeviceContext *) platform_d3d_context());
    platform_set_message_hook(ui_message_hook);

    D3D11_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    ((ID3D11Device *) platform_d3d_device())->CreateSamplerState(&sampler, &s_trilinear);
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    ((ID3D11Device *) platform_d3d_device())->CreateSamplerState(&sampler, &s_point);

    ID3D11Device *device = (ID3D11Device *) platform_d3d_device();
    s_crt = compile_pixel_shader(device, CRT_SHADER, sizeof(CRT_SHADER) - 1, "crt");
    s_tv = compile_pixel_shader(device, TV_SHADER, sizeof(TV_SHADER) - 1, "tv");
    s_soft_crt = compile_pixel_shader(device, SOFT_CRT_SHADER, sizeof(SOFT_CRT_SHADER) - 1,
                                      "soft_crt");
    s_composite = compile_pixel_shader(device, COMPOSITE_SHADER, sizeof(COMPOSITE_SHADER) - 1,
                                       "composite");
    s_pvm = compile_pixel_shader(device, PVM_SHADER, sizeof(PVM_SHADER) - 1, "pvm");
    s_fxaa = compile_pixel_shader(device, FXAA_SHADER, sizeof(FXAA_SHADER) - 1, "fxaa");
    {
        ID3DBlob *code = NULL;
        if (SUCCEEDED(D3DCompile(FULL_SCREEN_SHADER, sizeof(FULL_SCREEN_SHADER) - 1, "full_screen",
                                 NULL, NULL, "main", "vs_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                 &code, NULL))) {
            device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), NULL,
                                       &s_full_screen);
            code->Release();
        }
    }
    {
        D3D11_BUFFER_DESC desc = {};
        desc.ByteWidth = 16;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        device->CreateBuffer(&desc, NULL, &s_crt_constants);
    }
}

void ui_shutdown(void)
{
    platform_set_message_hook(NULL);
    if (s_crt) {
        s_crt->Release();
        s_crt = NULL;
    }
    ID3D11PixelShader **shaders[] = { &s_tv, &s_soft_crt, &s_composite, &s_pvm, &s_fxaa };
    for (ID3D11PixelShader **shader : shaders) {
        if (*shader) {
            (*shader)->Release();
            *shader = NULL;
        }
    }
    if (s_full_screen) {
        s_full_screen->Release();
        s_full_screen = NULL;
    }
    free_smooth();
    if (s_crt_constants) {
        s_crt_constants->Release();
        s_crt_constants = NULL;
    }
    if (s_trilinear) {
        s_trilinear->Release();
        s_trilinear = NULL;
    }
    if (s_point) {
        s_point->Release();
        s_point = NULL;
    }
    if (s_video_view) {
        s_video_view->Release();
        s_video->Release();
        s_video_view = NULL;
        s_video = NULL;
    }
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
}

void ui_begin_frame(void)
{
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

void ui_end_frame(void)
{
    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

ImFont *ui_font_bold(void)
{
    return s_font_bold;
}

ImFont *ui_font_display(void)
{
    return s_font_display;
}

int ui_image_from_resource(int resource_id, UiImage *image)
{
    ID3D11Device *device = (ID3D11Device *) platform_d3d_device();
    ID3D11DeviceContext *context = (ID3D11DeviceContext *) platform_d3d_context();
    HRSRC res = FindResourceW(NULL, MAKEINTRESOURCEW(resource_id), RT_RCDATA);
    HGLOBAL mem = res ? LoadResource(NULL, res) : NULL;
    int w, h, channels;

    image->view = NULL;
    if (!mem) {
        return 0;
    }
    stbi_uc *pixels = stbi_load_from_memory((const stbi_uc *) LockResource(mem),
                                            (int) SizeofResource(NULL, res), &w, &h, &channels, 4);
    if (!pixels) {
        return 0;
    }
    /* full mip chain, generated on the GPU */
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 0;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    ID3D11Texture2D *texture = NULL;
    ID3D11ShaderResourceView *view = NULL;
    if (SUCCEEDED(device->CreateTexture2D(&desc, NULL, &texture))) {
        context->UpdateSubresource(texture, 0, NULL, pixels, w * 4, 0);
        if (SUCCEEDED(device->CreateShaderResourceView(texture, NULL, &view))) {
            context->GenerateMips(view);
        }
        texture->Release();
    }
    stbi_image_free(pixels);
    image->view = view;
    image->width = w;
    image->height = h;
    return view != NULL;
}

void ui_image_free(UiImage *image)
{
    if (image->view) {
        ((ID3D11ShaderResourceView *) image->view)->Release();
        image->view = NULL;
    }
}

/* draw callback: our sampler instead of the backend's (which has no mips) */
static void use_trilinear(const ImDrawList *, const ImDrawCmd *)
{
    ((ID3D11DeviceContext *) platform_d3d_context())->PSSetSamplers(0, 1, &s_trilinear);
}

static float clampf(float x, float lo, float hi)
{
    return x < lo ? lo : x > hi ? hi : x;
}

void ui_draw_image_cover(ImDrawList *draw, const UiImage *image, ImVec2 p0, ImVec2 p1,
                         ImVec2 focus)
{
    float w = p1.x - p0.x, h = p1.y - p0.y;
    if (!image->view || w <= 0 || h <= 0) {
        return;
    }
    float scale = w / image->width > h / image->height ? w / image->width : h / image->height;
    float uw = w / (image->width * scale), vh = h / (image->height * scale);
    float u0 = clampf(focus.x - uw / 2, 0.0f, 1.0f - uw);
    float v0 = clampf(focus.y - vh / 2, 0.0f, 1.0f - vh);
    draw->AddCallback(use_trilinear, NULL);
    draw->AddImage((ImTextureID) (uintptr_t) image->view, p0, p1, ImVec2(u0, v0),
                   ImVec2(u0 + uw, v0 + vh));
    draw->AddCallback(ImDrawCallback_ResetRenderState, NULL);
}

void ui_video_update(const uint32_t *rgba, int w, int h, int antialiased)
{
    ID3D11Device *device = (ID3D11Device *) platform_d3d_device();
    ID3D11DeviceContext *context = (ID3D11DeviceContext *) platform_d3d_context();
    D3D11_MAPPED_SUBRESOURCE mapped;

    if (w != s_video_w || h != s_video_h) {
        if (s_video_view) {
            s_video_view->Release();
            s_video->Release();
            s_video_view = NULL;
            s_video = NULL;
        }
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = w;
        desc.Height = h;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device->CreateTexture2D(&desc, NULL, &s_video))) {
            return;
        }
        device->CreateShaderResourceView(s_video, NULL, &s_video_view);
        s_video_w = w;
        s_video_h = h;
    }
    if (SUCCEEDED(context->Map(s_video, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        for (int y = 0; y < h; y++) {
            memcpy((uint8_t *) mapped.pData + y * mapped.RowPitch, rgba + y * w, w * 4);
        }
        context->Unmap(s_video, 0);
    }
    if (antialiased) {
        s_antialias = antialias(w, h);
    } else if (s_smooth) {
        free_smooth();
    }
}

/* draw callbacks: the game picture's sampler */
static void use_point(const ImDrawList *, const ImDrawCmd *)
{
    ((ID3D11DeviceContext *) platform_d3d_context())->PSSetSamplers(0, 1, &s_point);
}

static void use_linear(const ImDrawList *, const ImDrawCmd *)
{
    ((ID3D11DeviceContext *) platform_d3d_context())->PSSetSamplers(0, 1, &s_trilinear);
}

/* draw callback: a picture shader (the CRT or the TV one, `cmd`'s user
 * data), its constants and a bilinear sampler */
static void use_shader(const ImDrawList *, const ImDrawCmd *cmd)
{
    ID3D11DeviceContext *context = (ID3D11DeviceContext *) platform_d3d_context();
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(context->Map(s_crt_constants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        float constants[4] = { s_crt_lines, s_crt_width, s_crt_strength, 0 };
        memcpy(mapped.pData, constants, sizeof(constants));
        context->Unmap(s_crt_constants, 0);
    }
    context->PSSetShader((ID3D11PixelShader *) cmd->UserCallbackData, NULL, 0);
    context->PSSetConstantBuffers(0, 1, &s_crt_constants);
    context->PSSetSamplers(0, 1, &s_trilinear);
}

void ui_draw_video(ImDrawList *draw, ImVec2 p0, ImVec2 p1, int filter, int strength, int width,
                   int lines)
{
    ID3D11PixelShader *shader = filter == UI_VIDEO_CRT ? s_crt
                                : filter == UI_VIDEO_TV ? s_tv
                                : filter == UI_VIDEO_SOFT_CRT ? s_soft_crt
                                : filter == UI_VIDEO_COMPOSITE ? s_composite
                                : filter == UI_VIDEO_PVM ? s_pvm : NULL;
    if (!s_video_view) {
        return;
    }
    if (shader && s_crt_constants) {
        s_crt_lines = (float) (lines > 0 ? lines : 240);
        s_crt_width = (float) (width > 0 ? width : 320);
        s_crt_strength = (float) (strength < 0 ? 0 : strength > 100 ? 100 : strength) / 100.0f;
        draw->AddCallback(use_shader, shader);
    } else {
        draw->AddCallback(filter == UI_VIDEO_SHARP ? use_point : use_linear, NULL);
    }
    draw->AddImage((ImTextureID) (uintptr_t) (s_antialias ? s_smooth_view : s_video_view), p0, p1);
    draw->AddCallback(ImDrawCallback_ResetRenderState, NULL);
}
