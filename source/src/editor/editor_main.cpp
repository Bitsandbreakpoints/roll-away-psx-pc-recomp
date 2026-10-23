/*
 * editor_main.cpp - entry point of the Roll Away level editor
 * (roll_away_editor.exe, next to roll_away.exe): the window, Direct3D 11
 * and Dear ImGui around editor_frame().
 *
 *   roll_away_editor.exe [level.rlv]
 */
#include <windows.h>
#include <shellapi.h>
#include <d3d11.h>
#include <dxgi.h>

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"
#include "editor/editor.h"
#include "level/level_file.h"
#include <stdio.h>
#include "resources.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wparam,
                                                             LPARAM lparam);

#define CLEAR_TOP               0.36f, 0.55f, 0.78f
#define FONT_SIZE               16.0f

static ID3D11Device *s_device;
static ID3D11DeviceContext *s_context;
static IDXGISwapChain *s_swap;
static ID3D11RenderTargetView *s_target;
static ID3D11DepthStencilView *s_depth;
static int s_width, s_height;
static bool s_closing;

static void release_targets()
{
    if (s_target) {
        s_target->Release();
        s_target = NULL;
    }
    if (s_depth) {
        s_depth->Release();
        s_depth = NULL;
    }
}

static void make_targets()
{
    ID3D11Texture2D *back = NULL;
    s_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **) &back);
    s_device->CreateRenderTargetView(back, NULL, &s_target);
    D3D11_TEXTURE2D_DESC bd;
    back->GetDesc(&bd);
    back->Release();
    s_width = (int) bd.Width;
    s_height = (int) bd.Height;
    D3D11_TEXTURE2D_DESC dd = {};
    dd.Width = bd.Width;
    dd.Height = bd.Height;
    dd.MipLevels = 1;
    dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dd.SampleDesc.Count = 1;
    dd.Usage = D3D11_USAGE_DEFAULT;
    dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ID3D11Texture2D *depth = NULL;
    s_device->CreateTexture2D(&dd, NULL, &depth);
    s_device->CreateDepthStencilView(depth, NULL, &s_depth);
    depth->Release();
}

static bool s_test;                     /* (tests/editor_drive.py: RA_EDITOR_TEST=1) */

static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    /* (tests send mouse messages while the real pointer is elsewhere) */
    if (s_test && msg == WM_MOUSELEAVE) {
        return 0;
    }
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) {
        return 1;
    }
    switch (msg) {
    case WM_SIZE:
        if (s_swap && wparam != SIZE_MINIMIZED) {
            release_targets();
            s_swap->ResizeBuffers(0, LOWORD(lparam), HIWORD(lparam), DXGI_FORMAT_UNKNOWN, 0);
            make_targets();
        }
        return 0;
    case WM_CLOSE:
        if (editor_close_request()) {
            s_closing = true;
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_DROPFILES: {
        wchar_t path[MAX_PATH];
        if (DragQueryFileW((HDROP) wparam, 0, path, MAX_PATH) && editor_close_request()) {
            char utf8[MAX_PATH * 3];
            WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, sizeof(utf8), NULL, NULL);
            editor_open_path(utf8);
        }
        DragFinish((HDROP) wparam);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

/* the launcher's colours: navy with orange */
static void style()
{
    ImGuiStyle &st = ImGui::GetStyle();
    ImGui::StyleColorsDark();
    st.WindowRounding = 0;
    st.FrameRounding = 4;
    st.GrabRounding = 4;
    st.ItemSpacing = ImVec2(6, 5);
    ImVec4 *c = st.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.03f, 0.06f, 0.10f, 0.94f);
    c[ImGuiCol_PopupBg] = ImVec4(0.04f, 0.07f, 0.13f, 0.97f);
    c[ImGuiCol_MenuBarBg] = ImVec4(0.05f, 0.10f, 0.18f, 1.0f);
    c[ImGuiCol_Button] = ImVec4(0.08f, 0.17f, 0.25f, 1.0f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.16f, 0.32f, 0.57f, 1.0f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.13f, 0.25f, 0.47f, 1.0f);
    c[ImGuiCol_FrameBg] = ImVec4(0.08f, 0.18f, 0.25f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.10f, 0.22f, 0.33f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.14f, 0.27f, 0.46f, 1.0f);
    c[ImGuiCol_SliderGrab] = ImVec4(1.00f, 0.54f, 0.20f, 1.0f);
    c[ImGuiCol_SliderGrabActive] = ImVec4(1.00f, 0.68f, 0.27f, 1.0f);
    c[ImGuiCol_CheckMark] = ImVec4(1.00f, 0.65f, 0.23f, 1.0f);
    c[ImGuiCol_Header] = ImVec4(0.14f, 0.28f, 0.51f, 0.7f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.18f, 0.36f, 0.67f, 0.8f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.85f, 0.50f, 0.15f, 0.8f);
    c[ImGuiCol_TitleBg] = ImVec4(0.06f, 0.13f, 0.24f, 1.0f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.14f, 0.28f, 0.51f, 1.0f);
    c[ImGuiCol_Separator] = ImVec4(1.0f, 1.0f, 1.0f, 0.10f);
}

/* the screen's scale (1.0 at 96 DPI) */
static float dpi_scale()
{
    HDC dc = GetDC(NULL);
    int dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(NULL, dc);
    return dpi > 0 ? dpi / 96.0f : 1.0f;
}

static void fonts()
{
    ImGuiIO &io = ImGui::GetIO();
    float scale = dpi_scale();
    wchar_t windows[MAX_PATH];
    GetWindowsDirectoryW(windows, MAX_PATH);
    char path[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, windows, -1, path, sizeof(path), NULL, NULL);
    std::string font = std::string(path) + "\\Fonts\\segoeui.ttf";
    if (GetFileAttributesA(font.c_str()) != INVALID_FILE_ATTRIBUTES) {
        io.Fonts->AddFontFromFileTTF(font.c_str(), FONT_SIZE * scale);
    } else {
        io.Fonts->AddFontDefault();
    }
}

/* (tests: `--convert <in> <out>` reads a level and writes the game's data
 * the editor makes of it, raw, without a window) */
static int convert(const wchar_t *in, const wchar_t *out)
{
    char path[MAX_PATH * 3], error[256];
    WideCharToMultiByte(CP_UTF8, 0, in, -1, path, sizeof(path), NULL, NULL);
    LevelFile lf;
    if (!level_file_load(path, &lf, error, sizeof(error))) {
        return 2;
    }
    Level level;
    std::string reason;
    bool ok = level.from_game(lf.data, lf.size, &reason);
    level_file_free(&lf);
    if (!ok) {
        return 3;
    }
    level.update_paths();
    std::vector<uint8_t> data = level.to_game();
    FILE *f = _wfopen(out, L"wb");
    if (!f) {
        return 4;
    }
    fwrite(data.data(), 1, data.size(), f);
    fclose(f);
    return 0;
}

/* (tests: `--showcase <out.rlv>` builds a level with every kind of block
 * through the editor's own model, to be tried in the game) */
static int showcase(const wchar_t *out)
{
    Level lv;
    lv.clear();
    for (int x = 13; x <= 20; x++) {
        for (int z = 14; z <= 18; z++) {
            lv.add_block(Level::cell(x, 16, z), BLOCK_NORMAL);
        }
    }
    static const int kinds[] = { BLOCK_FIRE, BLOCK_ICE, BLOCK_ACID, BLOCK_INVISIBLE, BLOCK_CRUMBLING,
                                 BLOCK_FLASHING };
    for (int i = 0; i < 6; i++) {
        lv.add_block(Level::cell(14 + i, 16, 18), kinds[i]);
    }
    lv.add_block(Level::cell(16, 16, 13), BLOCK_MOVING);
    Block *moving = lv.block(Level::cell(16, 16, 13));
    moving->prop[1] = SIDE_RIGHT;
    moving->length = 3;
    lv.add_block(Level::cell(21, 17, 17), BLOCK_LASER);
    Block *laser = lv.block(Level::cell(21, 17, 17));
    laser->prop[1] = SIDE_LEFT;
    laser->prop[22] = 3;
    lv.update_paths();
    auto put = [&](int x, int y, int z, int side, int id, int rot) {
        Thing *t = &lv.block(Level::cell(x, y, z))->sides[side];
        Level::set_thing(t, id);
        if (rot) {
            t->w[1] = (uint16_t) rot;
        }
        return t;
    };
    put(13, 16, 16, SIDE_TOP, THING_START, 4);
    put(15, 16, 14, SIDE_TOP, THING_KEY, 0);
    put(20, 16, 15, SIDE_TOP, THING_EXIT, 0);
    put(14, 16, 16, SIDE_TOP, THING_COIN, 0);
    put(15, 16, 16, SIDE_TOP, THING_GEM, 0);
    put(16, 16, 16, SIDE_TOP, THING_FRUIT, 0);
    put(17, 16, 14, SIDE_TOP, THING_SPIKES, 0);
    Thing *a = put(18, 16, 16, SIDE_TOP, THING_TRANSPORTER, 4);
    Thing *b = put(13, 16, 14, SIDE_TOP, THING_TRANSPORTER, 4);
    a->target.cell = Level::cell(13, 16, 14);
    a->target.side = SIDE_TOP;
    b->target.cell = Level::cell(18, 16, 16);
    b->target.side = SIDE_TOP;
    put(19, 16, 17, SIDE_TOP, THING_BOUNCE_PAD, 0);
    lv.settings.seconds = 120;
    std::string error;
    std::vector<uint8_t> data = lv.to_game();
    LevelFile lf = {};
    lf.theme = 3;
    snprintf(lf.name, sizeof(lf.name), "Showcase");
    lf.data = data.data();
    lf.size = (int) data.size();
    char path[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, out, -1, path, sizeof(path), NULL, NULL);
    return level_file_save(path, &lf) ? 0 : 1;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
    {
        int argc = 0;
        wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv && argc == 4 && !wcscmp(argv[1], L"--convert")) {
            return convert(argv[2], argv[3]);
        }
        if (argv && argc == 3 && !wcscmp(argv[1], L"--showcase")) {
            return showcase(argv[2]);
        }
    }
    SetProcessDPIAware();
    s_test = GetEnvironmentVariableA("RA_EDITOR_TEST", NULL, 0) != 0;
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
    wc.lpszClassName = L"RollAwayLevelEditor";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Roll Away level editor", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, 1440, 900, NULL, NULL, instance, NULL);
    if (!hwnd) {
        return 1;
    }
    DragAcceptFiles(hwnd, TRUE);

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    if (FAILED(D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, levels, 2,
                                             D3D11_SDK_VERSION, &sd, &s_swap, &s_device, NULL, &s_context))
        && FAILED(D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, levels, 2,
                                                D3D11_SDK_VERSION, &sd, &s_swap, &s_device, NULL,
                                                &s_context))) {
        MessageBoxW(hwnd, L"Direct3D 11 could not be started.", L"Roll Away level editor", MB_ICONERROR);
        return 1;
    }
    make_targets();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = NULL;
    style();
    fonts();
    ImGui::GetStyle().ScaleAllSizes(dpi_scale());
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(s_device, s_context);
    if (!editor_init(s_device, s_context, hwnd)) {
        MessageBoxW(hwnd, L"The 3D view could not be made (shaders).", L"Roll Away level editor", MB_ICONERROR);
        return 1;
    }
    /* a level given on the command line (or by opening a .rlv with the editor) */
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc > 1) {
        char utf8[MAX_PATH * 3];
        WideCharToMultiByte(CP_UTF8, 0, argv[1], -1, utf8, sizeof(utf8), NULL, NULL);
        editor_open_path(utf8);
    }
    if (argv) {
        LocalFree(argv);
    }
    ShowWindow(hwnd, show == SW_SHOWDEFAULT ? SW_SHOWMAXIMIZED : show);

    MSG msg;
    while (!s_closing) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) {
                s_closing = true;
            }
        }
        if (s_closing) {
            break;
        }
        if (g_ed.quit) {
            g_ed.quit = false;
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        if (IsIconic(hwnd)) {
            Sleep(30);
            continue;
        }
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        editor_frame(s_width, s_height);
        ImGui::Render();
        float sky[4] = { CLEAR_TOP, 1.0f };
        s_context->OMSetRenderTargets(1, &s_target, s_depth);
        s_context->ClearRenderTargetView(s_target, sky);
        s_context->ClearDepthStencilView(s_depth, D3D11_CLEAR_DEPTH, 1.0f, 0);
        renderer_flush();
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        s_swap->Present(1, 0);
    }
    editor_shutdown();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    release_targets();
    s_swap->Release();
    s_context->Release();
    s_device->Release();
    return 0;
}

/* (disc.cpp's disc_check() needs these; the editor only reads files) */
extern "C" int disc_check_executables(char *detail, int detail_size)
{
    (void) detail;
    (void) detail_size;
    return 0;
}
