/*
 * renderer.cpp - the level editor's 3D view (renderer.h).
 *
 * Everything is queued as triangles or lines in three lists (opaque,
 * see-through, lines) and drawn at renderer_flush() from one dynamic
 * vertex buffer, with one shader: a texture array layer (or none),
 * tinted by the vertex colour, shaded by the face's normal.
 */
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <math.h>
#include <string.h>

#include "editor/renderer.h"
#include "editor/level_model.h"

#define TEX_SIZE                64
#define TEX_MIPS                7       /* 64 .. 1 */

struct Vertex {
    float x, y, z;
    float nx, ny, nz;
    float u, v, layer;
    uint32_t colour;
};

static ID3D11Device *s_device;
static ID3D11DeviceContext *s_context;
static ID3D11VertexShader *s_vs;
static ID3D11PixelShader *s_ps;
static ID3D11InputLayout *s_layout;
static ID3D11Buffer *s_vb, *s_cb;
static UINT s_vb_capacity;
static ID3D11Texture2D *s_texture;
static ID3D11ShaderResourceView *s_texture_view;
static ID3D11SamplerState *s_sampler;
static ID3D11BlendState *s_blend_alpha;
static ID3D11DepthStencilState *s_depth_write, *s_depth_read;
static ID3D11RasterizerState *s_raster;

static std::vector<Vertex> s_opaque, s_clear, s_lines;
static float s_view_proj[16];
static Vec3 s_eye;
static int s_width = 1, s_height = 1;
static Camera s_camera;

static const char *const SHADER =
    "cbuffer C : register(b0) { row_major float4x4 vp; float4 light; };\n"
    "Texture2DArray tex : register(t0);\n"
    "SamplerState smp : register(s0);\n"
    "struct VI { float3 p : POSITION; float3 n : NORMAL; float3 uvl : TEXCOORD; float4 c : COLOR; };\n"
    "struct VO { float4 p : SV_POSITION; float3 n : NORMAL; float3 uvl : TEXCOORD; float4 c : COLOR;\n"
    "            float3 w : WORLD; };\n"
    "VO vs(VI i) { VO o; o.p = mul(float4(i.p, 1), vp); o.n = i.n; o.uvl = i.uvl; o.c = i.c;\n"
    "              o.w = i.p; return o; }\n"
    "float4 ps(VO i) : SV_Target {\n"
    "    float4 t = i.uvl.z < 0 ? float4(1, 1, 1, 1) : tex.Sample(smp, i.uvl);\n"
    "    float shade = 1;\n"
    "    if (dot(i.n, i.n) > 0.25) {\n"
    "        shade = 0.5 + 0.5 * saturate(dot(normalize(i.n), light.xyz));\n"
    "    }\n"
    "    return float4(t.rgb * i.c.rgb * shade, t.a * i.c.a);\n"
    "}\n";

Vec3 Camera::eye() const
{
    return v3(target.x + distance * cosf(pitch) * sinf(yaw), target.y + distance * sinf(pitch),
              target.z + distance * cosf(pitch) * cosf(yaw));
}

static Vec3 cross(Vec3 a, Vec3 b)
{
    return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}

static float dot(Vec3 a, Vec3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static Vec3 normalize(Vec3 a)
{
    float l = sqrtf(dot(a, a));
    return l > 0 ? a * (1.0f / l) : a;
}

static void multiply(const float *a, const float *b, float *out)
{
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            float s = 0;
            for (int k = 0; k < 4; k++) {
                s += a[r * 4 + k] * b[k * 4 + c];
            }
            out[r * 4 + c] = s;
        }
    }
}

static bool compile(const char *entry, const char *target, ID3DBlob **blob)
{
    ID3DBlob *errors = NULL;
    HRESULT hr = D3DCompile(SHADER, strlen(SHADER), "editor", NULL, NULL, entry, target, 0, 0, blob,
                            &errors);
    if (errors) {
        OutputDebugStringA((const char *) errors->GetBufferPointer());
        errors->Release();
    }
    return SUCCEEDED(hr);
}

/* the editor's own block textures (no disc): sandstone with a rim */
static void own_texture(int layer, uint32_t *out)
{
    for (int y = 0; y < TEX_SIZE; y++) {
        for (int x = 0; x < TEX_SIZE; x++) {
            uint32_t h = (uint32_t) (x * 73856093u ^ y * 19349663u ^ layer * 83492791u);
            h = (h ^ (h >> 13)) * 1274126177u;
            int noise = (int) (h >> 28) - 8;
            int edge = x < 3 || y < 3 || x > TEX_SIZE - 4 || y > TEX_SIZE - 4;
            int r = 214 + noise, g = 168 + noise, b = 108 + noise;
            if (layer == LAYER_SIDE) {
                r -= 30, g -= 30, b -= 22;
                if (y % 16 == 0) {
                    r -= 40, g -= 40, b -= 30;
                }
            }
            if (edge) {
                r -= 60, g -= 55, b -= 40;
            }
            if (layer == LAYER_WHITE) {
                r = g = b = 255;
            }
            out[y * TEX_SIZE + x] = 0xFF000000u | (uint32_t) b << 16 | (uint32_t) g << 8 | (uint32_t) r;
        }
    }
}

void renderer_set_textures(const uint32_t *top, const uint32_t *side)
{
    static uint32_t level[LAYER_COUNT][TEX_SIZE * TEX_SIZE];
    for (int l = 0; l < LAYER_COUNT; l++) {
        const uint32_t *given = l == LAYER_TOP ? top : l == LAYER_SIDE ? side : NULL;
        if (given) {
            memcpy(level[l], given, sizeof(level[l]));
        } else {
            own_texture(l, level[l]);
        }
        /* the mipmaps: averages of 2 x 2 */
        static uint32_t mip[TEX_SIZE * TEX_SIZE];
        memcpy(mip, level[l], sizeof(mip));
        int size = TEX_SIZE;
        for (int m = 0; m < TEX_MIPS; m++) {
            s_context->UpdateSubresource(s_texture, D3D11CalcSubresource(m, l, TEX_MIPS), NULL, mip,
                                         size * 4, 0);
            int half = size / 2;
            if (!half) {
                break;
            }
            for (int y = 0; y < half; y++) {
                for (int x = 0; x < half; x++) {
                    uint32_t sum[4] = { 0, 0, 0, 0 };
                    for (int k = 0; k < 4; k++) {
                        uint32_t p = mip[(2 * y + k / 2) * size + 2 * x + k % 2];
                        for (int c = 0; c < 4; c++) {
                            sum[c] += p >> (8 * c) & 255;
                        }
                    }
                    mip[y * half + x] = (sum[0] / 4) | (sum[1] / 4) << 8 | (sum[2] / 4) << 16
                                        | (sum[3] / 4) << 24;
                }
            }
            size = half;
        }
    }
}

bool renderer_init(ID3D11Device *device, ID3D11DeviceContext *context)
{
    s_device = device;
    s_context = context;
    ID3DBlob *vs = NULL, *ps = NULL;
    if (!compile("vs", "vs_4_0", &vs) || !compile("ps", "ps_4_0", &ps)) {
        return false;
    }
    device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), NULL, &s_vs);
    device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), NULL, &s_ps);
    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 36, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    device->CreateInputLayout(layout, 4, vs->GetBufferPointer(), vs->GetBufferSize(), &s_layout);
    vs->Release();
    ps->Release();

    D3D11_BUFFER_DESC cb = {};
    cb.ByteWidth = 80;
    cb.Usage = D3D11_USAGE_DEFAULT;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    device->CreateBuffer(&cb, NULL, &s_cb);

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = td.Height = TEX_SIZE;
    td.MipLevels = TEX_MIPS;
    td.ArraySize = LAYER_COUNT;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    device->CreateTexture2D(&td, NULL, &s_texture);
    device->CreateShaderResourceView(s_texture, NULL, &s_texture_view);
    renderer_set_textures(NULL, NULL);

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    device->CreateSamplerState(&sd, &s_sampler);

    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    device->CreateBlendState(&bd, &s_blend_alpha);

    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    device->CreateDepthStencilState(&dd, &s_depth_write);
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    device->CreateDepthStencilState(&dd, &s_depth_read);

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    rd.AntialiasedLineEnable = TRUE;
    device->CreateRasterizerState(&rd, &s_raster);
    return true;
}

void renderer_shutdown()
{
    ID3D11DeviceChild *all[] = { s_vs, s_ps, s_layout, s_vb, s_cb, s_texture, s_texture_view,
                                 s_sampler, s_blend_alpha, s_depth_write, s_depth_read, s_raster };
    for (ID3D11DeviceChild *p : all) {
        if (p) {
            p->Release();
        }
    }
    s_vb = NULL;
}

void renderer_begin(const Camera &camera, int w, int h)
{
    s_camera = camera;
    s_width = w > 0 ? w : 1;
    s_height = h > 0 ? h : 1;
    s_opaque.clear();
    s_clear.clear();
    s_lines.clear();
    Vec3 eye = camera.eye();
    s_eye = eye;
    /* (as the game shows it: looking along +X, +Z is to the right) */
    Vec3 zaxis = normalize(camera.target - eye);
    Vec3 xaxis = normalize(cross(zaxis, v3(0, 1, 0)));
    Vec3 yaxis = cross(xaxis, zaxis);
    float view[16] = {
        xaxis.x, yaxis.x, zaxis.x, 0,
        xaxis.y, yaxis.y, zaxis.y, 0,
        xaxis.z, yaxis.z, zaxis.z, 0,
        -dot(xaxis, eye), -dot(yaxis, eye), -dot(zaxis, eye), 1
    };
    float zn = 0.1f, zf = 300.0f;
    float ys = 1.0f / tanf(camera.fov / 2), xs = ys * s_height / s_width;
    float proj[16] = {
        xs, 0, 0, 0,
        0, ys, 0, 0,
        0, 0, zf / (zf - zn), 1,
        0, 0, -zn * zf / (zf - zn), 0
    };
    multiply(view, proj, s_view_proj);
}

static void quad(std::vector<Vertex> &list, Vec3 a, Vec3 b, Vec3 c, Vec3 d, Vec3 n, float layer,
                 uint32_t colour)
{
    Vertex v[4] = {
        { a.x, a.y, a.z, n.x, n.y, n.z, 0, 0, layer, colour },
        { b.x, b.y, b.z, n.x, n.y, n.z, 1, 0, layer, colour },
        { c.x, c.y, c.z, n.x, n.y, n.z, 1, 1, layer, colour },
        { d.x, d.y, d.z, n.x, n.y, n.z, 0, 1, layer, colour },
    };
    int order[6] = { 0, 1, 2, 0, 2, 3 };
    for (int i : order) {
        list.push_back(v[i]);
    }
}

/* the 4 corners of side `side` of the box lo..hi, counter-clockwise seen from outside */
static void side_corners(Vec3 lo, Vec3 hi, int side, Vec3 out[4])
{
    switch (side) {
    case SIDE_TOP:
        out[0] = v3(lo.x, hi.y, hi.z), out[1] = v3(hi.x, hi.y, hi.z);
        out[2] = v3(hi.x, hi.y, lo.z), out[3] = v3(lo.x, hi.y, lo.z);
        break;
    case SIDE_BOTTOM:
        out[0] = v3(lo.x, lo.y, lo.z), out[1] = v3(hi.x, lo.y, lo.z);
        out[2] = v3(hi.x, lo.y, hi.z), out[3] = v3(lo.x, lo.y, hi.z);
        break;
    case SIDE_RIGHT:
        out[0] = v3(hi.x, hi.y, lo.z), out[1] = v3(hi.x, hi.y, hi.z);
        out[2] = v3(hi.x, lo.y, hi.z), out[3] = v3(hi.x, lo.y, lo.z);
        break;
    case SIDE_LEFT:
        out[0] = v3(lo.x, hi.y, hi.z), out[1] = v3(lo.x, hi.y, lo.z);
        out[2] = v3(lo.x, lo.y, lo.z), out[3] = v3(lo.x, lo.y, hi.z);
        break;
    case SIDE_FRONT:
        out[0] = v3(hi.x, hi.y, hi.z), out[1] = v3(lo.x, hi.y, hi.z);
        out[2] = v3(lo.x, lo.y, hi.z), out[3] = v3(hi.x, lo.y, hi.z);
        break;
    default:
        out[0] = v3(lo.x, hi.y, lo.z), out[1] = v3(hi.x, hi.y, lo.z);
        out[2] = v3(hi.x, lo.y, lo.z), out[3] = v3(lo.x, lo.y, lo.z);
        break;
    }
}

void renderer_cube(Vec3 lo, Vec3 hi, uint32_t colour, bool textured)
{
    std::vector<Vertex> &list = (colour >> 24) < 255 ? s_clear : s_opaque;
    for (int s = 0; s < 6; s++) {
        Vec3 c[4];
        side_corners(lo, hi, s, c);
        int n[3];
        Level::side_normal(s, n);
        float layer = !textured ? LAYER_WHITE : s == SIDE_TOP || s == SIDE_BOTTOM ? LAYER_TOP : LAYER_SIDE;
        quad(list, c[0], c[1], c[2], c[3], v3((float) n[0], (float) n[1], (float) n[2]), layer, colour);
    }
}

void renderer_decal(int x, int y, int z, int side, float inset, uint32_t colour)
{
    int n[3];
    Level::side_normal(side, n);
    Vec3 lift = v3((float) n[0], (float) n[1], (float) n[2]) * 0.01f;
    Vec3 lo = v3(x + inset, y + inset, z + inset), hi = v3(x + 1 - inset, y + 1 - inset, z + 1 - inset);
    /* the side's plane stays the cube's */
    if (n[0]) {
        lo.x = hi.x = n[0] > 0 ? x + 1.0f : (float) x;
    } else if (n[1]) {
        lo.y = hi.y = n[1] > 0 ? y + 1.0f : (float) y;
    } else {
        lo.z = hi.z = n[2] > 0 ? z + 1.0f : (float) z;
    }
    Vec3 c[4];
    side_corners(lo, hi, side, c);
    quad(s_clear, c[0] + lift, c[1] + lift, c[2] + lift, c[3] + lift, v3(0, 0, 0), -1, colour);
}

void renderer_line(Vec3 a, Vec3 b, uint32_t colour)
{
    s_lines.push_back({ a.x, a.y, a.z, 0, 0, 0, 0, 0, -1, colour });
    s_lines.push_back({ b.x, b.y, b.z, 0, 0, 0, 0, 0, -1, colour });
}

void renderer_box_lines(Vec3 lo, Vec3 hi, uint32_t colour)
{
    Vec3 p[8];
    for (int i = 0; i < 8; i++) {
        p[i] = v3(i & 1 ? hi.x : lo.x, i & 2 ? hi.y : lo.y, i & 4 ? hi.z : lo.z);
    }
    int edges[12][2] = { { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 0, 2 }, { 1, 3 },
                         { 4, 6 }, { 5, 7 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
    for (auto &e : edges) {
        renderer_line(p[e[0]], p[e[1]], colour);
    }
}

static void draw(const std::vector<Vertex> &list, UINT first, D3D11_PRIMITIVE_TOPOLOGY topology)
{
    if (list.empty()) {
        return;
    }
    s_context->IASetPrimitiveTopology(topology);
    s_context->Draw((UINT) list.size(), first);
}

void renderer_flush()
{
    UINT total = (UINT) (s_opaque.size() + s_clear.size() + s_lines.size());
    if (!total) {
        return;
    }
    if (total > s_vb_capacity) {
        if (s_vb) {
            s_vb->Release();
        }
        s_vb_capacity = total + total / 2 + 1024;
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = s_vb_capacity * sizeof(Vertex);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        s_device->CreateBuffer(&bd, NULL, &s_vb);
    }
    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(s_context->Map(s_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        return;
    }
    Vertex *out = (Vertex *) map.pData;
    memcpy(out, s_opaque.data(), s_opaque.size() * sizeof(Vertex));
    memcpy(out + s_opaque.size(), s_clear.data(), s_clear.size() * sizeof(Vertex));
    memcpy(out + s_opaque.size() + s_clear.size(), s_lines.data(), s_lines.size() * sizeof(Vertex));
    s_context->Unmap(s_vb, 0);

    float constants[20];
    memcpy(constants, s_view_proj, sizeof(s_view_proj));
    Vec3 light = normalize(v3(0.35f, 1.0f, 0.55f));
    constants[16] = light.x;
    constants[17] = light.y;
    constants[18] = light.z;
    constants[19] = 0;
    s_context->UpdateSubresource(s_cb, 0, NULL, constants, 0, 0);

    D3D11_VIEWPORT vp = { 0, 0, (float) s_width, (float) s_height, 0, 1 };
    s_context->RSSetViewports(1, &vp);
    s_context->RSSetState(s_raster);
    UINT stride = sizeof(Vertex), offset = 0;
    s_context->IASetVertexBuffers(0, 1, &s_vb, &stride, &offset);
    s_context->IASetInputLayout(s_layout);
    s_context->VSSetShader(s_vs, NULL, 0);
    s_context->VSSetConstantBuffers(0, 1, &s_cb);
    s_context->PSSetShader(s_ps, NULL, 0);
    s_context->PSSetConstantBuffers(0, 1, &s_cb);
    s_context->PSSetShaderResources(0, 1, &s_texture_view);
    s_context->PSSetSamplers(0, 1, &s_sampler);

    float blend[4] = { 0, 0, 0, 0 };
    s_context->OMSetBlendState(NULL, blend, 0xFFFFFFFF);
    s_context->OMSetDepthStencilState(s_depth_write, 0);
    draw(s_opaque, 0, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    s_context->OMSetBlendState(s_blend_alpha, blend, 0xFFFFFFFF);
    s_context->OMSetDepthStencilState(s_depth_read, 0);
    draw(s_lines, (UINT) (s_opaque.size() + s_clear.size()), D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    draw(s_clear, (UINT) s_opaque.size(), D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    s_context->OMSetBlendState(NULL, blend, 0xFFFFFFFF);
}

bool renderer_project(Vec3 p, float *sx, float *sy, float *depth)
{
    const float *m = s_view_proj;
    float x = p.x * m[0] + p.y * m[4] + p.z * m[8] + m[12];
    float y = p.x * m[1] + p.y * m[5] + p.z * m[9] + m[13];
    float w = p.x * m[3] + p.y * m[7] + p.z * m[11] + m[15];
    if (w < 0.1f) {
        return false;
    }
    *sx = (x / w * 0.5f + 0.5f) * s_width;
    *sy = (0.5f - y / w * 0.5f) * s_height;
    if (depth) {
        *depth = w;
    }
    return true;
}

void renderer_ray(float sx, float sy, Vec3 *origin, Vec3 *direction)
{
    Vec3 eye = s_camera.eye();
    Vec3 zaxis = normalize(s_camera.target - eye);
    Vec3 xaxis = normalize(cross(zaxis, v3(0, 1, 0)));
    Vec3 yaxis = cross(xaxis, zaxis);
    float t = tanf(s_camera.fov / 2);
    float nx = (sx / s_width * 2 - 1) * t * s_width / s_height;
    float ny = (1 - sy / s_height * 2) * t;
    *origin = eye;
    *direction = normalize(zaxis + xaxis * nx + yaxis * ny);
}
