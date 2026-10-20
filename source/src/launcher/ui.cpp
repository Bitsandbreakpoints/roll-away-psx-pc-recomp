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
static float s_crt_width;
static ID3D11Buffer *s_crt_constants;
static float s_crt_lines;

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
    float4 crt;                         /* x: lines, y: pixels across (the console's) */
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
    return float4(sqrt(sum / total), 1.0) * input.col;
}
)";

static const char CRT_SHADER[] = R"(
cbuffer Crt : register(b0)
{
    float4 crt;                         /* x: lines of the picture */
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
    float2 uv = input.uv + centre * dot(centre, centre) * 0.07;
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
    return float4(sqrt(saturate(colour)), 1.0) * input.col;
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
    if (s_tv) {
        s_tv->Release();
        s_tv = NULL;
    }
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

void ui_video_update(const uint32_t *rgba, int w, int h)
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
        float constants[4] = { s_crt_lines, s_crt_width, 0, 0 };
        memcpy(mapped.pData, constants, sizeof(constants));
        context->Unmap(s_crt_constants, 0);
    }
    context->PSSetShader((ID3D11PixelShader *) cmd->UserCallbackData, NULL, 0);
    context->PSSetConstantBuffers(0, 1, &s_crt_constants);
    context->PSSetSamplers(0, 1, &s_trilinear);
}

void ui_draw_video(ImDrawList *draw, ImVec2 p0, ImVec2 p1, int filter, int width, int lines)
{
    ID3D11PixelShader *shader = filter == UI_VIDEO_CRT ? s_crt : filter == UI_VIDEO_TV ? s_tv : NULL;
    if (!s_video_view) {
        return;
    }
    if (shader && s_crt_constants) {
        s_crt_lines = (float) (lines > 0 ? lines : 240);
        s_crt_width = (float) (width > 0 ? width : 320);
        draw->AddCallback(use_shader, shader);
    } else {
        draw->AddCallback(filter == UI_VIDEO_SHARP ? use_point : use_linear, NULL);
    }
    draw->AddImage((ImTextureID) (uintptr_t) s_video_view, p0, p1);
    draw->AddCallback(ImDrawCallback_ResetRenderState, NULL);
}
