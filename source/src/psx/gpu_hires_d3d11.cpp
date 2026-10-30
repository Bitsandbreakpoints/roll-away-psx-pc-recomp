/*
 * gpu_hires_d3d11.cpp - the high-resolution copy drawn by the graphics
 * card (see gpu_hires_d3d11.h).
 *
 * Coordinates as in gpu_hires.c: a vertex at original x lies at x * scale
 * in the copy, and pixel X is sampled at its centre, original
 * (X + 0.5) / scale - the rasterizer's own pixel centres.  Textures that
 * are perspective-correct in gpu_hires.c get the GTE's depth as w; colours
 * stay linear on the screen (noperspective).
 */
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdlib.h>
#include <string.h>
#include "psx/gpu.h"
#include "psx/gpu_hires_d3d11.h"
#include "psx/log.h"
#include "platform/platform.h"

/* ---- shaders ------------------------------------------------------------ */

static const char COMMON[] = R"(
cbuffer Frame : register(b0)
{
    float2 to_ndc;          /* 2 / the target's size */
    int scale;
    int true_color;
    int2 size;              /* of the copy */
    int2 unused;
};

struct VS_INPUT
{
    float3 pos : POSITION;  /* pixels of the target, w */
    float3 col : COLOR0;    /* 0..255 */
    float2 uv  : TEXCOORD0;
    uint4 a    : TEXCOORD1; /* page, clut, flags, 0 */
    uint4 b    : TEXCOORD2; /* u_min | u_max << 16, v_min | v_max << 16 (rectangles: u0, v0) */
};

struct PS_INPUT
{
    float4 pos : SV_Position;
    noperspective float3 col : COLOR0;
    float2 uv : TEXCOORD0;
    nointerpolation uint4 a : TEXCOORD1;
    nointerpolation uint4 b : TEXCOORD2;
};
)";

static const char VERTEX_SHADER[] = R"(
PS_INPUT main(VS_INPUT v)
{
    PS_INPUT o;
    float w = v.pos.z;
    o.pos = float4((v.pos.x * to_ndc.x - 1.0) * w, (1.0 - v.pos.y * to_ndc.y) * w, 0.0, w);
    o.col = v.col;
    o.uv = v.uv;
    o.a = v.a;
    o.b = v.b;
    return o;
}
)";

/* plot() and plot_filtered() of gpu_hires.c */
static const char DRAW_SHADER[] = R"(
Texture2D<uint> vram : register(t0);

static const int DITHER[16] = { -4, 0, -3, 1, 2, -2, 3, -1, -3, 1, -4, 0, 3, -1, 2, -2 };

uint fetch(int x, int y)
{
    return vram.Load(int3(x & 1023, y & 511, 0));
}

int expand5(int c)
{
    return (c << 3) | (c >> 2);
}

int3 channels(uint t)
{
    return int3(t & 31, (t >> 5) & 31, (t >> 10) & 31);
}

uint texel(uint4 a, int u, int v)
{
    uint flags = a.z;
    int mx = (flags >> 12) & 31, my = (flags >> 17) & 31;
    int ox = (flags >> 22) & 31, oy = (flags >> 27) & 31;
    u &= 255;
    v &= 255;
    u = (u & ~(mx * 8)) | ((ox & mx) * 8);
    v = (v & ~(my * 8)) | ((oy & my) * 8);
    int px = a.x & 0xFFFF, py = a.x >> 16, cx = a.y & 0xFFFF, cy = a.y >> 16;
    uint depth = flags & 3;
    if (depth == 0) {
        uint word = fetch(px + u / 4, py + v);
        return fetch(cx + ((word >> ((u & 3) * 4)) & 15), cy);
    }
    if (depth == 1) {
        uint word = fetch(px + u / 2, py + v);
        return fetch(cx + ((word >> ((u & 1) * 8)) & 255), cy);
    }
    return fetch(px + u, py + v);
}

/* pack_rgb(): kept in true colour, otherwise 15 bits (dithered) */
float3 pack(int3 c, bool dither, int2 p)
{
    if (true_color) {
        return float3(clamp(c, 0, 255)) / 255.0;
    }
    if (dither) {
        int2 o = p / scale;
        c += DITHER[(o.y & 3) * 4 + (o.x & 3)];
    }
    c = clamp(c, 0, 255) >> 3;
    return float3(expand5(c.r), expand5(c.g), expand5(c.b)) / 255.0;
}

float4 main(PS_INPUT i) : SV_Target
{
    uint flags = i.a.z;
    int2 p = int2(i.pos.xy);
    int3 rgb = clamp(int3(i.col), 0, 255);
    bool dither = (flags & 16) != 0;
    uint which = (flags >> 6) & 3;
    uint mask = (flags >> 8) & 1;
    bool semi = true;           /* (untextured: all of it, if the primitive is) */
    float3 colour;

    if ((flags & 4) == 0) {
        colour = pack(rgb, dither, p);
    } else if (flags & 32) {
        /* bilinear: the four texels around the sample point, inside the
         * polygon's texture area; the nearest decides whether and how */
        int u_min = i.b.x & 0xFFFF, u_max = i.b.x >> 16, v_min = i.b.y & 0xFFFF, v_max = i.b.y >> 16;
        int uf = int(floor((i.uv.x - 0.5) * 256.0)), vf = int(floor((i.uv.y - 0.5) * 256.0));
        int fu = uf & 255, fv = vf & 255;
        int u0 = uf >> 8, v0 = vf >> 8;
        int u1 = clamp(u0 + 1, u_min, u_max), v1 = clamp(v0 + 1, v_min, v_max);
        u0 = clamp(u0, u_min, u_max);
        v0 = clamp(v0, v_min, v_max);
        uint t[4] = { texel(i.a, u0, v0), texel(i.a, u1, v0), texel(i.a, u0, v1), texel(i.a, u1, v1) };
        int w[4] = { (256 - fu) * (256 - fv), fu * (256 - fv), (256 - fu) * fv, fu * fv };
        int n = 0;
        [unroll] for (int k = 1; k < 4; k++) {
            n = w[k] > w[n] ? k : n;
        }
        uint nearest = t[n];
        if (nearest == 0) {
            discard;
        }
        int3 sum = int3(0, 0, 0);
        int weight = 0;
        [unroll] for (int j = 0; j < 4; j++) {
            if (t[j] != 0) {
                int3 c = channels(t[j]);
                if (true_color) {
                    c = int3(expand5(c.r), expand5(c.g), expand5(c.b));
                }
                sum += w[j] * c;
                weight += w[j];
            }
        }
        int3 tc;
        if (true_color) {
            tc = weight == 65536 ? sum >> 16 : sum / weight;
        } else {
            tc = weight == 65536 ? sum >> 13 : sum * 8 / weight;
        }
        semi = (nearest & 0x8000) != 0;
        mask |= nearest >> 15;
        colour = (flags & 8) ? pack(tc, false, p) : pack((tc * rgb) >> 7, dither, p);
    } else {
        int u, v;
        if (flags & 512) {
            /* a rectangle: whole texels from its corner */
            int col = int(floor(i.uv.x)), row = int(floor(i.uv.y));
            int u0 = i.b.x & 0xFFFF, v0 = i.b.y & 0xFFFF;
            u = (flags & 1024) ? u0 - col : u0 + col;
            v = (flags & 2048) ? v0 - row : v0 + row;
        } else {
            u = clamp(int(floor(i.uv.x)), int(i.b.x & 0xFFFF), int(i.b.x >> 16));
            v = clamp(int(floor(i.uv.y)), int(i.b.y & 0xFFFF), int(i.b.y >> 16));
        }
        uint t = texel(i.a, u, v);
        if (t == 0) {
            discard;
        }
        semi = (t & 0x8000) != 0;
        mask |= t >> 15;
        int3 c = channels(t);
        if (flags & 8) {
            colour = float3(expand5(c.r), expand5(c.g), expand5(c.b)) / 255.0;
        } else if (true_color) {
            colour = pack((int3(expand5(c.r), expand5(c.g), expand5(c.b)) * rgb) >> 7, false, p);
        } else {
            colour = pack((c * rgb) >> 4, dither, p);
        }
    }
    if ((which == 1 && semi) || (which == 2 && !semi)) {
        discard;
    }
    return float4(colour, (float) mask);
}
)";

/* fills: the colour as given (15 bits), the mask bit from the flags */
static const char FILL_SHADER[] = R"(
float4 main(PS_INPUT i) : SV_Target
{
    return float4(i.col / 255.0, (float) ((i.a.z >> 8) & 1));
}
)";

/* the original VRAM enlarged */
static const char COPY_SHADER[] = R"(
Texture2D<uint> vram : register(t0);
float4 main(PS_INPUT i) : SV_Target
{
    int2 o = int2(i.pos.xy) / scale;
    uint t = vram.Load(int3(o.x & 1023, o.y & 511, 0));
    uint3 c = uint3(t & 31, (t >> 5) & 31, (t >> 10) & 31);
    return float4(float3((c << 3) | (c >> 2)) / 255.0, (float) (t >> 15));
}
)";

/* a texture (pixels at uv, wrapping around the copy's size) as it is;
 * the mask bit added (moves), or opaque (the picture shown) */
static const char MOVE_SHADER[] = R"(
Texture2D<float4> source : register(t1);
float4 main(PS_INPUT i) : SV_Target
{
    int2 at = int2(floor(i.uv)) % size;
    float4 c = source.Load(int3(at, 0));
    /* (b.z: 1 opaque, 2 set the mask bit) */
    return float4(c.rgb, (i.b.z & 1) ? 1.0 : max(c.a, (float) ((i.b.z >> 1) & 1)));
}
)";

/* ---- state -------------------------------------------------------------- */

typedef struct {
    float x, y, w;                      /* pixels of the target; w: depth or 1 */
    float r, g, b;
    float u, v;
    uint32_t prim[4];                   /* page, clut, flags, 0 */
    uint32_t area[4];                   /* texture area / rectangle corner, options */
} Vertex;

/* Vertex.prim[2] (the drawing shader) */
#define F_TEXTURED              (1u << 2)
#define F_RAW                   (1u << 3)
#define F_DITHER                (1u << 4)
#define F_FILTERED              (1u << 5)
#define F_PASS_OPAQUE           (1u << 6)       /* only the texels that are not semi-transparent */
#define F_PASS_SEMI             (2u << 6)       /* only the semi-transparent ones */
#define F_SET_MASK              (1u << 8)
#define F_RECTANGLE             (1u << 9)
#define F_FLIP_X                (1u << 10)
#define F_FLIP_Y                (1u << 11)

enum { PS_DRAW, PS_FILL, PS_COPY, PS_MOVE, PS_COUNT };
enum { BLEND_OPAQUE, BLEND_HALF, BLEND_ADD, BLEND_SUBTRACT, BLEND_QUARTER, BLEND_CHECK_MASK,
       BLEND_COUNT };
static const float BLEND_FACTOR[BLEND_COUNT] = { 0, 0.5f, 0, 0, 0.25f, 0 };

typedef struct {
    int first, count;
    int shader, blend;
    D3D11_RECT scissor;
} Batch;

typedef struct {
    float to_ndc[2];
    int scale, true_color;
    int size[2];
    int unused[2];
} Constants;

static ID3D11Device *s_device;
static ID3D11DeviceContext *s_context;
static int s_scale, s_width, s_height;
static int s_true_color, s_texture_filter;

static ID3D11VertexShader *s_vs;
static ID3D11InputLayout *s_layout;
static ID3D11PixelShader *s_ps[PS_COUNT];
static ID3D11BlendState *s_blend[BLEND_COUNT];
static ID3D11RasterizerState *s_raster;
static ID3D11DepthStencilState *s_no_depth;
static ID3D11Buffer *s_constants;
static ID3D11Buffer *s_vertex_buffer;
static int s_vertex_buffer_size;        /* vertices */

static ID3D11Texture2D *s_target;       /* the copy */
static ID3D11RenderTargetView *s_target_rtv;
static ID3D11ShaderResourceView *s_target_srv;
static ID3D11Texture2D *s_vram;         /* the original VRAM (textures) */
static ID3D11ShaderResourceView *s_vram_srv;
static ID3D11Texture2D *s_scratch;      /* moves */
static ID3D11ShaderResourceView *s_scratch_srv;
static int s_scratch_w, s_scratch_h;
static ID3D11Texture2D *s_picture;      /* the area shown */
static ID3D11RenderTargetView *s_picture_rtv;
static ID3D11ShaderResourceView *s_picture_srv;
static int s_picture_w, s_picture_h;
static ID3D11Texture2D *s_staging;      /* hwr_read() */
static int s_staging_w, s_staging_h;

static Vertex *s_vertices;
static int s_count, s_capacity;
static Batch *s_batches;
static int s_batch_count, s_batch_capacity;

template <typename T> static void release(T *&object)
{
    if (object) {
        object->Release();
        object = NULL;
    }
}

/* ---- the pipeline ------------------------------------------------------- */

static ID3DBlob *compile(const char *source, const char *name, const char *profile)
{
    size_t common = sizeof(COMMON) - 1, length = strlen(source);
    char *text = (char *) malloc(common + length + 1);
    ID3DBlob *code = NULL, *errors = NULL;
    if (!text) {
        return NULL;
    }
    memcpy(text, COMMON, common);
    memcpy(text + common, source, length + 1);
    if (FAILED(D3DCompile(text, common + length, name, NULL, NULL, "main", profile,
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors))) {
        psx_log("GPU renderer: shader %s: %s", name,
                errors ? (const char *) errors->GetBufferPointer() : "does not compile");
        code = NULL;
    }
    release(errors);
    free(text);
    return code;
}

static int create_pipeline(void)
{
    static const struct { const char *source, *name; } PIXEL[PS_COUNT] = {
        { DRAW_SHADER, "draw" }, { FILL_SHADER, "fill" }, { COPY_SHADER, "copy" },
        { MOVE_SHADER, "move" },
    };
    static const D3D11_INPUT_ELEMENT_DESC LAYOUT[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_UINT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_UINT, 0, 48, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    ID3DBlob *code = compile(VERTEX_SHADER, "vertex", "vs_4_0");
    if (!code) {
        return 0;
    }
    HRESULT hr = s_device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), NULL,
                                              &s_vs);
    if (SUCCEEDED(hr)) {
        hr = s_device->CreateInputLayout(LAYOUT, 5, code->GetBufferPointer(), code->GetBufferSize(),
                                         &s_layout);
    }
    code->Release();
    if (FAILED(hr)) {
        return 0;
    }
    for (int i = 0; i < PS_COUNT; i++) {
        code = compile(PIXEL[i].source, PIXEL[i].name, "ps_4_0");
        if (!code) {
            return 0;
        }
        hr = s_device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), NULL,
                                         &s_ps[i]);
        code->Release();
        if (FAILED(hr)) {
            return 0;
        }
    }

    /* semi-transparency (back B, front F): (B + F) / 2, B + F, B - F, B + F / 4;
     * the mask bit (alpha) as the shader gives it.  Check mask: the back
     * stays where its mask bit is set. */
    for (int i = 0; i < BLEND_COUNT; i++) {
        D3D11_BLEND_DESC desc = {};
        D3D11_RENDER_TARGET_BLEND_DESC *rt = &desc.RenderTarget[0];
        rt->BlendEnable = i != BLEND_OPAQUE;
        rt->SrcBlend = D3D11_BLEND_ONE;
        rt->DestBlend = D3D11_BLEND_ONE;
        rt->BlendOp = D3D11_BLEND_OP_ADD;
        rt->SrcBlendAlpha = D3D11_BLEND_ONE;
        rt->DestBlendAlpha = D3D11_BLEND_ZERO;
        rt->BlendOpAlpha = D3D11_BLEND_OP_ADD;
        rt->RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        switch (i) {
        case BLEND_HALF:
            rt->SrcBlend = rt->DestBlend = D3D11_BLEND_BLEND_FACTOR;
            break;
        case BLEND_SUBTRACT:
            rt->BlendOp = D3D11_BLEND_OP_REV_SUBTRACT;
            break;
        case BLEND_QUARTER:
            rt->SrcBlend = D3D11_BLEND_BLEND_FACTOR;
            break;
        case BLEND_CHECK_MASK:
            rt->SrcBlend = rt->SrcBlendAlpha = D3D11_BLEND_INV_DEST_ALPHA;
            rt->DestBlend = rt->DestBlendAlpha = D3D11_BLEND_DEST_ALPHA;
            break;
        default:
            break;
        }
        if (FAILED(s_device->CreateBlendState(&desc, &s_blend[i]))) {
            return 0;
        }
    }
    D3D11_RASTERIZER_DESC raster = {};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = FALSE;
    raster.ScissorEnable = TRUE;
    D3D11_DEPTH_STENCIL_DESC depth = {};
    depth.DepthEnable = FALSE;
    depth.StencilEnable = FALSE;
    D3D11_BUFFER_DESC buffer = {};
    buffer.ByteWidth = sizeof(Constants);
    buffer.Usage = D3D11_USAGE_DYNAMIC;
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return SUCCEEDED(s_device->CreateRasterizerState(&raster, &s_raster))
           && SUCCEEDED(s_device->CreateDepthStencilState(&depth, &s_no_depth))
           && SUCCEEDED(s_device->CreateBuffer(&buffer, NULL, &s_constants));
}

static void free_pipeline(void)
{
    release(s_vs);
    release(s_layout);
    for (int i = 0; i < PS_COUNT; i++) {
        release(s_ps[i]);
    }
    for (int i = 0; i < BLEND_COUNT; i++) {
        release(s_blend[i]);
    }
    release(s_raster);
    release(s_no_depth);
    release(s_constants);
    release(s_vertex_buffer);
    s_vertex_buffer_size = 0;
}

/* a texture of RGBA pixels with the views asked for; FALSE if not */
static int create_texture(int w, int h, UINT bind, ID3D11Texture2D **texture,
                          ID3D11ShaderResourceView **srv, ID3D11RenderTargetView **rtv)
{
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = (UINT) w;
    desc.Height = (UINT) h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = bind;
    if (FAILED(s_device->CreateTexture2D(&desc, NULL, texture))) {
        *texture = NULL;
        return 0;
    }
    if ((srv && FAILED(s_device->CreateShaderResourceView(*texture, NULL, srv)))
        || (rtv && FAILED(s_device->CreateRenderTargetView(*texture, NULL, rtv)))) {
        if (srv) {
            release(*srv);
        }
        release(*texture);
        return 0;
    }
    return 1;
}

/* ---- batches ------------------------------------------------------------ */

static D3D11_RECT whole_target(void)
{
    D3D11_RECT rect = { 0, 0, s_width, s_height };
    return rect;
}

/* the drawing area in pixels of the target */
static D3D11_RECT clip_rect(const GpuEnv *e)
{
    D3D11_RECT rect = { e->clip_x1 * s_scale, e->clip_y1 * s_scale, (e->clip_x2 + 1) * s_scale,
                        (e->clip_y2 + 1) * s_scale };
    return rect;
}

/* room for `n` vertices drawn with this shader, blending and scissor:
 * the last batch if it has the same, else a new one */
static Vertex *add_vertices(int n, int shader, int blend, const D3D11_RECT *scissor)
{
    if (s_count + n > s_capacity) {
        int capacity = s_capacity ? s_capacity * 2 : 65536;
        while (capacity < s_count + n) {
            capacity *= 2;
        }
        Vertex *vertices = (Vertex *) realloc(s_vertices, sizeof(Vertex) * (size_t) capacity);
        if (!vertices) {
            return NULL;
        }
        s_vertices = vertices;
        s_capacity = capacity;
    }
    Batch *last = s_batch_count ? &s_batches[s_batch_count - 1] : NULL;
    if (!last || last->shader != shader || last->blend != blend
        || memcmp(&last->scissor, scissor, sizeof(*scissor)) != 0) {
        if (s_batch_count == s_batch_capacity) {
            int capacity = s_batch_capacity ? s_batch_capacity * 2 : 1024;
            Batch *batches = (Batch *) realloc(s_batches, sizeof(Batch) * (size_t) capacity);
            if (!batches) {
                return NULL;
            }
            s_batches = batches;
            s_batch_capacity = capacity;
        }
        last = &s_batches[s_batch_count++];
        last->first = s_count;
        last->count = 0;
        last->shader = shader;
        last->blend = blend;
        last->scissor = *scissor;
    }
    last->count += n;
    Vertex *v = &s_vertices[s_count];
    s_count += n;
    memset(v, 0, sizeof(Vertex) * (size_t) n);
    return v;
}

/* two triangles over (x0, y0)-(x1, y1), uv likewise, all else from `like` */
static void quad(Vertex *out, const Vertex *like, float x0, float y0, float x1, float y1,
                 float u0, float v0, float u1, float v1)
{
    static const int CORNERS[6] = { 0, 1, 2, 1, 3, 2 };
    for (int i = 0; i < 6; i++) {
        int k = CORNERS[i];
        out[i] = *like;
        out[i].x = k & 1 ? x1 : x0;
        out[i].y = k & 2 ? y1 : y0;
        out[i].w = 1;
        out[i].u = k & 1 ? u1 : u0;
        out[i].v = k & 2 ? v1 : v0;
    }
}

static void set_constants(int w, int h)
{
    D3D11_MAPPED_SUBRESOURCE mapped;
    Constants c = {};
    c.to_ndc[0] = 2.0f / (float) w;
    c.to_ndc[1] = 2.0f / (float) h;
    c.scale = s_scale;
    c.true_color = s_true_color;
    c.size[0] = s_width;
    c.size[1] = s_height;
    if (SUCCEEDED(s_context->Map(s_constants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, &c, sizeof(c));
        s_context->Unmap(s_constants, 0);
    }
}

/* Draws the batches into `target` (w x h) with `source` as the second
 * texture, and forgets them.  The window's render target and viewport are
 * put back; Dear ImGui and the picture filters set the rest themselves. */
static void run_batches(ID3D11RenderTargetView *target, int w, int h,
                        ID3D11ShaderResourceView *source)
{
    if (s_batch_count == 0) {
        return;
    }
    if (s_count > s_vertex_buffer_size) {
        release(s_vertex_buffer);
        int size = s_vertex_buffer_size ? s_vertex_buffer_size : 65536;
        while (size < s_count) {
            size *= 2;
        }
        D3D11_BUFFER_DESC desc = {};
        desc.ByteWidth = (UINT) (sizeof(Vertex) * (size_t) size);
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(s_device->CreateBuffer(&desc, NULL, &s_vertex_buffer))) {
            s_vertex_buffer_size = 0;
            s_count = s_batch_count = 0;
            return;
        }
        s_vertex_buffer_size = size;
    }
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(s_context->Map(s_vertex_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        s_count = s_batch_count = 0;
        return;
    }
    memcpy(mapped.pData, s_vertices, sizeof(Vertex) * (size_t) s_count);
    s_context->Unmap(s_vertex_buffer, 0);

    ID3D11RenderTargetView *old_target = NULL;
    ID3D11DepthStencilView *old_depth = NULL;
    D3D11_VIEWPORT old_viewport;
    UINT viewports = 1;
    s_context->OMGetRenderTargets(1, &old_target, &old_depth);
    s_context->RSGetViewports(&viewports, &old_viewport);

    set_constants(w, h);
    UINT stride = sizeof(Vertex), offset = 0;
    D3D11_VIEWPORT viewport = { 0, 0, (float) w, (float) h, 0, 1 };
    ID3D11ShaderResourceView *views[2] = { s_vram_srv, source };
    s_context->IASetInputLayout(s_layout);
    s_context->IASetVertexBuffers(0, 1, &s_vertex_buffer, &stride, &offset);
    s_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    s_context->VSSetShader(s_vs, NULL, 0);
    s_context->VSSetConstantBuffers(0, 1, &s_constants);
    s_context->GSSetShader(NULL, NULL, 0);
    s_context->PSSetConstantBuffers(0, 1, &s_constants);
    s_context->OMSetRenderTargets(1, &target, NULL);
    s_context->PSSetShaderResources(0, 2, views);
    s_context->RSSetViewports(1, &viewport);
    s_context->RSSetState(s_raster);
    s_context->OMSetDepthStencilState(s_no_depth, 0);
    int shader = -1, blend = -1;
    for (int i = 0; i < s_batch_count; i++) {
        const Batch *b = &s_batches[i];
        if (b->shader != shader) {
            shader = b->shader;
            s_context->PSSetShader(s_ps[shader], NULL, 0);
        }
        if (b->blend != blend) {
            blend = b->blend;
            float f = BLEND_FACTOR[blend];
            float factor[4] = { f, f, f, f };
            s_context->OMSetBlendState(s_blend[blend], factor, 0xFFFFFFFF);
        }
        s_context->RSSetScissorRects(1, &b->scissor);
        s_context->Draw((UINT) b->count, (UINT) b->first);
    }
    ID3D11ShaderResourceView *none[2] = { NULL, NULL };
    s_context->PSSetShaderResources(0, 2, none);
    s_context->OMSetRenderTargets(1, &old_target, old_depth);
    if (viewports) {
        s_context->RSSetViewports(1, &old_viewport);
    }
    release(old_target);
    release(old_depth);
    s_count = s_batch_count = 0;
}

static void upload_vram(int x, int y, int w, int h)
{
    D3D11_BOX box = { (UINT) x, (UINT) y, 0, (UINT) (x + w), (UINT) (y + h), 1 };
    s_context->UpdateSubresource(s_vram, 0, &box, &g_vram[y][x], sizeof(g_vram[0]), 0);
}

/* ---- setup -------------------------------------------------------------- */

void hwr_shutdown(void)
{
    s_count = s_batch_count = 0;
    release(s_target_srv);
    release(s_target_rtv);
    release(s_target);
    release(s_vram_srv);
    release(s_vram);
    release(s_scratch_srv);
    release(s_scratch);
    release(s_picture_srv);
    release(s_picture_rtv);
    release(s_picture);
    release(s_staging);
    s_scratch_w = s_scratch_h = s_picture_w = s_picture_h = s_staging_w = s_staging_h = 0;
    free_pipeline();
    s_scale = 0;
}

int hwr_init(int scale)
{
    hwr_shutdown();
    s_device = (ID3D11Device *) platform_d3d_device();
    s_context = (ID3D11DeviceContext *) platform_d3d_context();
    if (!s_device || !s_context) {
        return 0;
    }
    if (!create_pipeline()) {
        psx_log("GPU renderer: the shaders could not be made");
        hwr_shutdown();
        return 0;
    }
    s_scale = scale;
    s_width = GPU_VRAM_WIDTH * scale;
    s_height = GPU_VRAM_HEIGHT * scale;
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = GPU_VRAM_WIDTH;
    desc.Height = GPU_VRAM_HEIGHT;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R16_UINT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (!create_texture(s_width, s_height, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
                        &s_target, &s_target_srv, &s_target_rtv)
        || FAILED(s_device->CreateTexture2D(&desc, NULL, &s_vram))
        || FAILED(s_device->CreateShaderResourceView(s_vram, NULL, &s_vram_srv))) {
        psx_log("GPU renderer: no %d x %d render target", s_width, s_height);
        hwr_shutdown();
        return 0;
    }
    hwr_copy_from_original(0, 0, GPU_VRAM_WIDTH, GPU_VRAM_HEIGHT);
    psx_log("GPU: internal resolution %dx drawn by the graphics card", scale);
    return 1;
}

void hwr_set_true_color(int on)
{
    hwr_flush();
    s_true_color = on;
}

void hwr_set_texture_filter(int on)
{
    s_texture_filter = on;
}

/* ---- primitives --------------------------------------------------------- */

static uint32_t prim_flags(const GpuPrim *p, const GpuEnv *e)
{
    return (uint32_t) (p->depth & 3) | (p->textured ? F_TEXTURED : 0) | (p->raw ? F_RAW : 0)
           | (p->dither ? F_DITHER : 0) | (e->set_mask ? F_SET_MASK : 0)
           | (uint32_t) (e->win_mask_x & 31) << 12 | (uint32_t) (e->win_mask_y & 31) << 17
           | (uint32_t) (e->win_off_x & 31) << 22 | (uint32_t) (e->win_off_y & 31) << 27;
}

static void prim_words(const GpuPrim *p, const GpuEnv *e, Vertex *v)
{
    v->prim[0] = (uint32_t) p->page_x | (uint32_t) p->page_y << 16;
    v->prim[1] = (uint32_t) p->clut_x | (uint32_t) p->clut_y << 16;
    v->prim[2] = prim_flags(p, e);
}

/* Queues `n` vertices of a primitive: semi-transparent textured ones twice,
 * the opaque texels and then the semi-transparent ones blended. */
static void emit(const GpuPrim *p, const GpuEnv *e, const Vertex *v, int n)
{
    D3D11_RECT clip = clip_rect(e);
    int opaque = e->check_mask ? BLEND_CHECK_MASK : BLEND_OPAQUE;
    static int logged;
    if (p->semi && e->check_mask && !logged) {
        logged = 1;
        psx_log("GPU renderer: semi-transparency with \"check mask\" (drawn without the check)");
    }
    for (int pass = 0; pass < 2; pass++) {
        int blend;
        uint32_t flags;
        if (!p->semi) {
            if (pass) {
                break;
            }
            blend = opaque, flags = 0;
        } else if (!p->textured) {
            if (pass) {
                break;
            }
            blend = BLEND_HALF + (p->blend & 3), flags = 0;
        } else {
            blend = pass ? BLEND_HALF + (p->blend & 3) : opaque;
            flags = pass ? F_PASS_SEMI : F_PASS_OPAQUE;
        }
        Vertex *out = add_vertices(n, PS_DRAW, blend, &clip);
        if (!out) {
            return;
        }
        for (int i = 0; i < n; i++) {
            out[i] = v[i];
            out[i].prim[2] |= flags;
        }
    }
}

void hwr_triangle(const GpuPrim *p, const GpuEnv *e, const HiresVertex *v0,
                  const HiresVertex *v1, const HiresVertex *v2)
{
    const HiresVertex *in[3] = { v0, v1, v2 };
    Vertex v[3];
    if (!s_scale) {
        return;
    }
    /* (as gpu_hires.c: textures perspective-correct where the GTE gave depths) */
    int perspective = v0->z > 0 && v1->z > 0 && v2->z > 0 && p->textured;
    int u_min = v0->u, u_max = v0->u, v_min = v0->v, v_max = v0->v;
    for (int k = 1; k < 3; k++) {
        u_min = in[k]->u < u_min ? in[k]->u : u_min;
        u_max = in[k]->u > u_max ? in[k]->u : u_max;
        v_min = in[k]->v < v_min ? in[k]->v : v_min;
        v_max = in[k]->v > v_max ? in[k]->v : v_max;
    }
    for (int k = 0; k < 3; k++) {
        const HiresVertex *h = in[k], *c = p->gouraud ? h : v0;
        memset(&v[k], 0, sizeof(v[k]));
        v[k].x = h->x * (float) s_scale;
        v[k].y = h->y * (float) s_scale;
        v[k].w = perspective ? h->z : 1.0f;
        v[k].r = (float) c->r;
        v[k].g = (float) c->g;
        v[k].b = (float) c->b;
        v[k].u = (float) h->u;
        v[k].v = (float) h->v;
        prim_words(p, e, &v[k]);
        if (s_texture_filter && perspective) {
            v[k].prim[2] |= F_FILTERED;
        }
        v[k].area[0] = (uint32_t) (uint16_t) u_min | (uint32_t) (uint16_t) u_max << 16;
        v[k].area[1] = (uint32_t) (uint16_t) v_min | (uint32_t) (uint16_t) v_max << 16;
    }
    emit(p, e, v, 3);
}

void hwr_rectangle(const GpuPrim *p, const GpuEnv *e, int x, int y, int w, int h,
                   int r, int g, int b, int u0, int v0, int flip_x, int flip_y)
{
    Vertex like = {}, v[6];
    if (!s_scale || w <= 0 || h <= 0) {
        return;
    }
    like.r = (float) r;
    like.g = (float) g;
    like.b = (float) b;
    prim_words(p, e, &like);
    like.prim[2] |= F_RECTANGLE | (flip_x ? F_FLIP_X : 0) | (flip_y ? F_FLIP_Y : 0);
    like.area[0] = (uint32_t) (u0 & 0xFFFF);
    like.area[1] = (uint32_t) (v0 & 0xFFFF);
    quad(v, &like, (float) (x * s_scale), (float) (y * s_scale), (float) ((x + w) * s_scale),
         (float) ((y + h) * s_scale), 0, 0, (float) w, (float) h);
    emit(p, e, v, 6);
}

static int lerp(int a, int b, int i, int n)
{
    return n ? a + ((b - a) * i * 2 + (b >= a ? n : -n)) / (2 * n) : a;
}

/* (the original's pixels, each scale x scale, as gpu_hires.c) */
void hwr_line(const GpuPrim *p, const GpuEnv *e, const HiresVertex *a, const HiresVertex *b)
{
    if (!s_scale) {
        return;
    }
    int ax = (int) a->x, ay = (int) a->y, bx = (int) b->x, by = (int) b->y;
    int dx = bx - ax, dy = by - ay;
    int n = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
    Vertex like = {}, v[6];
    prim_words(p, e, &like);
    for (int i = 0; i <= n; i++) {
        int x = lerp(ax, bx, i, n), y = lerp(ay, by, i, n);
        if (x < e->clip_x1 || x > e->clip_x2 || y < e->clip_y1 || y > e->clip_y2) {
            continue;
        }
        like.r = (float) (p->gouraud ? lerp(a->r, b->r, i, n) : a->r);
        like.g = (float) (p->gouraud ? lerp(a->g, b->g, i, n) : a->g);
        like.b = (float) (p->gouraud ? lerp(a->b, b->b, i, n) : a->b);
        quad(v, &like, (float) (x * s_scale), (float) (y * s_scale), (float) ((x + 1) * s_scale),
             (float) ((y + 1) * s_scale), 0, 0, 0, 0);
        emit(p, e, v, 6);
    }
}

/* ---- VRAM operations ---------------------------------------------------- */

/* Calls fn for the pieces of a w x h area at (x, y) that do not cross the
 * edges of VRAM (areas wrap around them), in original pixels. */
static void pieces(int x, int y, int w, int h, void (*fn)(int x, int y, int w, int h, void *),
                   void *context)
{
    for (int row = 0; row < h;) {
        int py = (y + row) & (GPU_VRAM_HEIGHT - 1);
        int ph = GPU_VRAM_HEIGHT - py < h - row ? GPU_VRAM_HEIGHT - py : h - row;
        for (int col = 0; col < w;) {
            int px = (x + col) & (GPU_VRAM_WIDTH - 1);
            int pw = GPU_VRAM_WIDTH - px < w - col ? GPU_VRAM_WIDTH - px : w - col;
            fn(px, py, pw, ph, context);
            col += pw;
        }
        row += ph;
    }
}

static void fill_piece(int x, int y, int w, int h, void *context)
{
    const Vertex *like = (const Vertex *) context;
    D3D11_RECT all = whole_target();
    Vertex *v = add_vertices(6, PS_FILL, BLEND_OPAQUE, &all);
    if (v) {
        quad(v, like, (float) (x * s_scale), (float) (y * s_scale), (float) ((x + w) * s_scale),
             (float) ((y + h) * s_scale), 0, 0, 0, 0);
    }
}

void hwr_fill(int x, int y, int w, int h, uint16_t color)
{
    Vertex like = {};
    if (!s_scale || w <= 0 || h <= 0) {
        return;
    }
    like.r = (float) (((color & 31) << 3) | ((color & 31) >> 2));
    like.g = (float) ((((color >> 5) & 31) << 3) | (((color >> 5) & 31) >> 2));
    like.b = (float) ((((color >> 10) & 31) << 3) | (((color >> 10) & 31) >> 2));
    like.prim[2] = color & 0x8000 ? F_SET_MASK : 0;
    pieces(x, y, w, h, fill_piece, &like);
}

static void copy_piece(int x, int y, int w, int h, void *)
{
    D3D11_RECT all = whole_target();
    Vertex like = {};
    upload_vram(x, y, w, h);
    Vertex *v = add_vertices(6, PS_COPY, BLEND_OPAQUE, &all);
    if (v) {
        quad(v, &like, (float) (x * s_scale), (float) (y * s_scale), (float) ((x + w) * s_scale),
             (float) ((y + h) * s_scale), 0, 0, 0, 0);
    }
}

void hwr_copy_from_original(int x, int y, int w, int h)
{
    if (!s_scale || w <= 0 || h <= 0) {
        return;
    }
    hwr_flush();
    pieces(x, y, w, h, copy_piece, NULL);
    run_batches(s_target_rtv, s_width, s_height, NULL);
}

typedef struct {
    int sx, sy, dx, dy;                 /* the move's corners */
    int set_mask, check_mask;
} Move;

/* one piece of a move whose destination does not wrap; split again where
 * its source wraps */
static void move_source_piece(int x, int y, int w, int h, void *context)
{
    const Move *m = (const Move *) context;
    /* (x, y): the source; where it lands */
    int tx = (m->dx + (x - m->sx + GPU_VRAM_WIDTH) % GPU_VRAM_WIDTH) & (GPU_VRAM_WIDTH - 1);
    int ty = (m->dy + (y - m->sy + GPU_VRAM_HEIGHT) % GPU_VRAM_HEIGHT) & (GPU_VRAM_HEIGHT - 1);
    int pw = w * s_scale, ph = h * s_scale;
    if (pw > s_scratch_w || ph > s_scratch_h) {
        int sw = pw > s_scratch_w ? pw : s_scratch_w, sh = ph > s_scratch_h ? ph : s_scratch_h;
        release(s_scratch_srv);
        release(s_scratch);
        s_scratch_w = s_scratch_h = 0;
        if (!create_texture(sw, sh, D3D11_BIND_SHADER_RESOURCE, &s_scratch, &s_scratch_srv, NULL)) {
            return;
        }
        s_scratch_w = sw;
        s_scratch_h = sh;
    }
    D3D11_BOX from = { (UINT) (x * s_scale), (UINT) (y * s_scale), 0, (UINT) ((x + w) * s_scale),
                       (UINT) ((y + h) * s_scale), 1 };
    s_context->CopySubresourceRegion(s_scratch, 0, 0, 0, 0, s_target, 0, &from);
    if (!m->set_mask && !m->check_mask) {
        D3D11_BOX back = { 0, 0, 0, (UINT) pw, (UINT) ph, 1 };
        s_context->CopySubresourceRegion(s_target, 0, (UINT) (tx * s_scale), (UINT) (ty * s_scale),
                                         0, s_scratch, 0, &back);
        return;
    }
    D3D11_RECT all = whole_target();
    Vertex like = {};
    like.area[2] = m->set_mask ? 2u : 0u;
    Vertex *v = add_vertices(6, PS_MOVE, m->check_mask ? BLEND_CHECK_MASK : BLEND_OPAQUE, &all);
    if (v) {
        quad(v, &like, (float) (tx * s_scale), (float) (ty * s_scale), (float) ((tx + w) * s_scale),
             (float) ((ty + h) * s_scale), 0, 0, (float) pw, (float) ph);
        run_batches(s_target_rtv, s_width, s_height, s_scratch_srv);
    }
}

static void move_piece(int x, int y, int w, int h, void *context)
{
    const Move *m = (const Move *) context;
    Move part = *m;
    /* (x, y): the destination; its source */
    part.sx = (m->sx + (x - m->dx + GPU_VRAM_WIDTH) % GPU_VRAM_WIDTH) & (GPU_VRAM_WIDTH - 1);
    part.sy = (m->sy + (y - m->dy + GPU_VRAM_HEIGHT) % GPU_VRAM_HEIGHT) & (GPU_VRAM_HEIGHT - 1);
    part.dx = x;
    part.dy = y;
    pieces(part.sx, part.sy, w, h, move_source_piece, &part);
}

void hwr_move(int sx, int sy, int dx, int dy, int w, int h, int set_mask, int check_mask)
{
    Move m = { sx, sy, dx, dy, set_mask, check_mask };
    if (!s_scale || w <= 0 || h <= 0) {
        return;
    }
    hwr_flush();
    pieces(dx, dy, w, h, move_piece, &m);
}

/* ---- drawing, the picture ----------------------------------------------- */

void hwr_flush(void)
{
    if (!s_scale || s_batch_count == 0) {
        s_count = s_batch_count = 0;
        return;
    }
    /* (the textures as they are now, as gpu_hires.c reads them when it draws) */
    upload_vram(0, 0, GPU_VRAM_WIDTH, GPU_VRAM_HEIGHT);
    run_batches(s_target_rtv, s_width, s_height, NULL);
}

void hwr_discard(void)
{
    s_count = s_batch_count = 0;
}

void *hwr_picture(int x, int y, int w, int h)
{
    int pw = w * s_scale, ph = h * s_scale;
    if (!s_scale || w <= 0 || h <= 0) {
        return NULL;
    }
    hwr_flush();
    if (pw != s_picture_w || ph != s_picture_h) {
        release(s_picture_srv);
        release(s_picture_rtv);
        release(s_picture);
        s_picture_w = s_picture_h = 0;
        if (!create_texture(pw, ph, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, &s_picture,
                            &s_picture_srv, &s_picture_rtv)) {
            return NULL;
        }
        s_picture_w = pw;
        s_picture_h = ph;
    }
    D3D11_RECT all = { 0, 0, pw, ph };
    Vertex like = {};
    like.area[2] = 1;                      /* opaque */
    Vertex *v = add_vertices(6, PS_MOVE, BLEND_OPAQUE, &all);
    if (!v) {
        return NULL;
    }
    quad(v, &like, 0, 0, (float) pw, (float) ph, (float) (x * s_scale), (float) (y * s_scale),
         (float) ((x + w) * s_scale), (float) ((y + h) * s_scale));
    run_batches(s_picture_rtv, pw, ph, s_target_srv);
    return s_picture_srv;
}

void hwr_read(uint32_t *out, int x, int y, int w, int h)
{
    int pw = w * s_scale, ph = h * s_scale;
    if (!hwr_picture(x, y, w, h)) {
        memset(out, 0, sizeof(uint32_t) * (size_t) pw * (size_t) ph);
        return;
    }
    if (pw != s_staging_w || ph != s_staging_h) {
        release(s_staging);
        s_staging_w = s_staging_h = 0;
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = (UINT) pw;
        desc.Height = (UINT) ph;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(s_device->CreateTexture2D(&desc, NULL, &s_staging))) {
            s_staging = NULL;
            return;
        }
        s_staging_w = pw;
        s_staging_h = ph;
    }
    s_context->CopyResource(s_staging, s_picture);
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(s_context->Map(s_staging, 0, D3D11_MAP_READ, 0, &mapped))) {
        for (int row = 0; row < ph; row++) {
            memcpy(out + (size_t) row * pw, (const uint8_t *) mapped.pData + (size_t) row * mapped.RowPitch,
                   sizeof(uint32_t) * (size_t) pw);
        }
        s_context->Unmap(s_staging, 0);
    }
}
