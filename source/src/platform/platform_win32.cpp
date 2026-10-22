/*
 * platform_win32.cpp - window, Direct3D 11 swap chain, 60 Hz frame pacing
 * and file drop / open dialog on Windows.
 */
#include <windows.h>
#include <shellapi.h>
#include <commdlg.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dwmapi.h>
#include <windowsx.h>              /* GET_X_LPARAM */
#include <stdio.h>
#include <string.h>
#include <mmsystem.h>          /* timeBeginPeriod */
#include "platform/platform.h"
#include "resources.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

static const wchar_t WINDOW_CLASS[] = L"RollAwayPC";
static const wchar_t WINDOW_TITLE[] = L"Roll Away";
static const DWORD WINDOWED_STYLE = WS_OVERLAPPEDWINDOW;
static const DWORD BORDERLESS_STYLE = WS_POPUP;
/* (a sizing frame, removed again in WM_NCCALCSIZE: resizable, with a shadow) */
static const DWORD SPLASH_STYLE = WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX;
#define SPLASH_EDGE             7       /* resize grip at the edges, at 96 DPI */

static HWND s_window;
static ID3D11Device *s_device;
static ID3D11DeviceContext *s_context;
static IDXGISwapChain *s_swap_chain;
static ID3D11RenderTargetView *s_back_buffer;
static PlatformConfig s_config;
static int s_quit;
static PlatformMessageHook s_hook;
static wchar_t s_dropped[MAX_PATH];
static int s_has_dropped;

/* frame pacing */
static LARGE_INTEGER s_freq;
static LARGE_INTEGER s_next_frame;
static HANDLE s_timer;
static int s_frames;
static LARGE_INTEGER s_fps_start;
static double s_fps;

static void utf8_to_wide(const char *in, wchar_t *out, int size)
{
    MultiByteToWideChar(CP_UTF8, 0, in, -1, out, size);
}

static void wide_to_utf8(const wchar_t *in, char *out, int size)
{
    WideCharToMultiByte(CP_UTF8, 0, in, -1, out, size, NULL, NULL);
}

void platform_message(const char *title, const char *text)
{
    wchar_t wtitle[256], wtext[2048];
    utf8_to_wide(title, wtitle, 256);
    utf8_to_wide(text, wtext, 2048);
    MessageBoxW(s_window, wtext, wtitle, MB_OK | MB_ICONINFORMATION);
}

void platform_set_title(const char *title)
{
    wchar_t wtitle[256];
    utf8_to_wide(title, wtitle, 256);
    SetWindowTextW(s_window, wtitle);
}

void *platform_hwnd(void) { return s_window; }
void *platform_d3d_device(void) { return s_device; }
void *platform_d3d_context(void) { return s_context; }
void platform_set_message_hook(PlatformMessageHook hook) { s_hook = hook; }

void platform_client_size(int *width, int *height)
{
    RECT rect;
    GetClientRect(s_window, &rect);
    *width = rect.right - rect.left;
    *height = rect.bottom - rect.top;
}

float platform_dpi_scale(void)
{
    UINT dpi = 96;
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    typedef UINT (WINAPI *GetDpiForWindowFn)(HWND);
    GetDpiForWindowFn get_dpi = (GetDpiForWindowFn) (void *) GetProcAddress(user32, "GetDpiForWindow");
    if (get_dpi && s_window) {
        dpi = get_dpi(s_window);
    }
    return dpi / 96.0f;
}

static void create_back_buffer_view(void)
{
    ID3D11Texture2D *buffer = NULL;
    s_swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **) &buffer);
    s_device->CreateRenderTargetView(buffer, NULL, &s_back_buffer);
    buffer->Release();
}

static void resize_swap_chain(void)
{
    if (!s_swap_chain) {
        return;
    }
    if (s_back_buffer) {
        s_context->OMSetRenderTargets(0, NULL, NULL);
        s_back_buffer->Release();
        s_back_buffer = NULL;
    }
    s_swap_chain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
    create_back_buffer_view();
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (s_hook && s_hook(hwnd, msg, wparam, lparam)) {
        return TRUE;
    }
    int splash = s_config.window_mode == WINDOW_MODE_SPLASH;
    switch (msg) {
    case WM_NCCALCSIZE:
        if (splash && wparam) {
            return 0;                   /* the client area is the whole window */
        }
        break;
    case WM_NCHITTEST:
        if (splash) {
            /* edges and corners resize; the rest is the client (the
             * launcher moves the window itself, platform_begin_window_drag) */
            POINT pt = { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
            RECT rc;
            ScreenToClient(hwnd, &pt);
            GetClientRect(hwnd, &rc);
            int edge = (int) (SPLASH_EDGE * platform_dpi_scale());
            int left = pt.x < edge, right = pt.x >= rc.right - edge;
            int top = pt.y < edge, bottom = pt.y >= rc.bottom - edge;
            if (top && left) return HTTOPLEFT;
            if (top && right) return HTTOPRIGHT;
            if (bottom && left) return HTBOTTOMLEFT;
            if (bottom && right) return HTBOTTOMRIGHT;
            if (left) return HTLEFT;
            if (right) return HTRIGHT;
            if (top) return HTTOP;
            if (bottom) return HTBOTTOM;
            return HTCLIENT;
        }
        break;
    case WM_GETMINMAXINFO:
        if (splash && s_window) {
            MINMAXINFO *info = (MINMAXINFO *) lparam;
            float scale = platform_dpi_scale();
            info->ptMinTrackSize.x = (LONG) (PLATFORM_SPLASH_MIN_W * scale);
            info->ptMinTrackSize.y = (LONG) (PLATFORM_SPLASH_MIN_H * scale);
            return 0;
        }
        break;
    case WM_SIZE:
        if (wparam != SIZE_MINIMIZED) {
            resize_swap_chain();
        }
        return 0;
    case WM_DROPFILES: {
        HDROP drop = (HDROP) wparam;
        if (DragQueryFileW(drop, 0, s_dropped, MAX_PATH)) {
            s_has_dropped = 1;
        }
        DragFinish(drop);
        return 0;
    }
    case WM_CLOSE:
        s_quit = 1;
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

#define DWMWA_CORNERS           33      /* DWMWA_WINDOW_CORNER_PREFERENCE (Windows 11) */
#define DWM_CORNERS_ROUND       2
#define DWM_CORNERS_DEFAULT     0

/* Places the window for the current s_config (windowed and splash:
 * centred in the work area). */
static void place_window(void)
{
    /* rounded corners for the splash (ignored before Windows 11) */
    DWORD corners = s_config.window_mode == WINDOW_MODE_SPLASH ? DWM_CORNERS_ROUND : DWM_CORNERS_DEFAULT;
    DwmSetWindowAttribute(s_window, DWMWA_CORNERS, &corners, sizeof(corners));
    if (s_config.window_mode == WINDOW_MODE_SPLASH) {
        HMONITOR monitor = MonitorFromWindow(s_window, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info = { sizeof(info) };
        GetMonitorInfoW(monitor, &info);
        float scale = platform_dpi_scale();
        int w = (int) (s_config.width * scale), h = (int) (s_config.height * scale);
        int x = info.rcWork.left + ((info.rcWork.right - info.rcWork.left) - w) / 2;
        int y = info.rcWork.top + ((info.rcWork.bottom - info.rcWork.top) - h) / 2;
        SetWindowLongW(s_window, GWL_STYLE, SPLASH_STYLE | WS_VISIBLE);
        SetWindowPos(s_window, HWND_TOP, x, y, w, h, SWP_FRAMECHANGED);
    } else if (s_config.window_mode == WINDOW_MODE_BORDERLESS) {
        HMONITOR monitor = MonitorFromWindow(s_window, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info = { sizeof(info) };
        GetMonitorInfoW(monitor, &info);
        SetWindowLongW(s_window, GWL_STYLE, BORDERLESS_STYLE | WS_VISIBLE);
        SetWindowPos(s_window, HWND_TOP, info.rcMonitor.left, info.rcMonitor.top,
                     info.rcMonitor.right - info.rcMonitor.left,
                     info.rcMonitor.bottom - info.rcMonitor.top, SWP_FRAMECHANGED);
    } else {
        HMONITOR monitor = MonitorFromWindow(s_window, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info = { sizeof(info) };
        RECT rect = { 0, 0, s_config.width, s_config.height };
        GetMonitorInfoW(monitor, &info);
        SetWindowLongW(s_window, GWL_STYLE, WINDOWED_STYLE | WS_VISIBLE);
        AdjustWindowRect(&rect, WINDOWED_STYLE, FALSE);
        int w = rect.right - rect.left, h = rect.bottom - rect.top;
        int x = info.rcWork.left + ((info.rcWork.right - info.rcWork.left) - w) / 2;
        int y = info.rcWork.top + ((info.rcWork.bottom - info.rcWork.top) - h) / 2;
        SetWindowPos(s_window, HWND_TOP, x, y, w, h, SWP_FRAMECHANGED);
    }
}

static int create_window(void)
{
    HINSTANCE instance = GetModuleHandleW(NULL);
    WNDCLASSEXW wc = {};

    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIcon(instance, MAKEINTRESOURCE(IDI_APP));
    wc.hIconSm = (HICON) LoadImage(instance, MAKEINTRESOURCE(IDI_APP), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wc.lpszClassName = WINDOW_CLASS;
    if (!RegisterClassExW(&wc)) {
        return 0;
    }
    s_window = CreateWindowExW(WS_EX_ACCEPTFILES, WINDOW_CLASS, WINDOW_TITLE, WINDOWED_STYLE,
                               CW_USEDEFAULT, CW_USEDEFAULT, s_config.width, s_config.height,
                               NULL, NULL, instance, NULL);
    if (!s_window) {
        return 0;
    }
    DragAcceptFiles(s_window, TRUE);
    place_window();
    ShowWindow(s_window, SW_SHOW);
    return 1;
}

static int create_device(void)
{
    DXGI_SWAP_CHAIN_DESC desc = {};
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                   D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL got;
    HRESULT hr;

    desc.BufferCount = 2;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = s_window;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, levels, 3,
                                       D3D11_SDK_VERSION, &desc, &s_swap_chain, &s_device, &got,
                                       &s_context);
    if (FAILED(hr)) {
        /* older Windows: no flip model */
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        desc.BufferCount = 1;
        hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, levels, 3,
                                           D3D11_SDK_VERSION, &desc, &s_swap_chain, &s_device,
                                           &got, &s_context);
    }
    if (FAILED(hr)) {
        return 0;
    }
    create_back_buffer_view();
    return 1;
}

static void enable_dpi_awareness(void)
{
    typedef BOOL (WINAPI *SetContextFn)(HANDLE);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    SetContextFn set_context =
        (SetContextFn) (void *) GetProcAddress(user32, "SetProcessDpiAwarenessContext");
    if (set_context) {
        set_context((HANDLE) -4);       /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */
    } else {
        SetProcessDPIAware();
    }
}

int platform_init(const PlatformConfig *config)
{
    s_config = *config;
    enable_dpi_awareness();
    if (!create_window()) {
        platform_message("Roll Away", "Could not open the window.");
        return 0;
    }
    if (!create_device()) {
        platform_message("Roll Away", "Could not start Direct3D 11.");
        return 0;
    }
    timeBeginPeriod(1);
    s_timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                     TIMER_ALL_ACCESS);
    if (!s_timer) {
        s_timer = CreateWaitableTimerW(NULL, TRUE, NULL);
    }
    QueryPerformanceFrequency(&s_freq);
    QueryPerformanceCounter(&s_next_frame);
    s_fps_start = s_next_frame;
    return 1;
}

void platform_begin_window_drag(void)
{
    ReleaseCapture();
    SendMessageW(s_window, WM_NCLBUTTONDOWN, HTCAPTION, 0);
}

void platform_minimize(void)
{
    ShowWindow(s_window, SW_MINIMIZE);
}

void platform_apply_config(const PlatformConfig *config)
{
    int moved = config->window_mode != s_config.window_mode
                || (config->window_mode != WINDOW_MODE_BORDERLESS
                    && (config->width != s_config.width || config->height != s_config.height));
    s_config = *config;
    if (moved) {
        place_window();
    }
}

void platform_shutdown(void)
{
    if (s_back_buffer) s_back_buffer->Release();
    if (s_swap_chain) s_swap_chain->Release();
    if (s_context) s_context->Release();
    if (s_device) s_device->Release();
    if (s_timer) CloseHandle(s_timer);
    timeEndPeriod(1);
}

int platform_pump(void)
{
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            s_quit = 1;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return !s_quit;
}

void platform_clear(uint32_t rgb)
{
    float colour[4] = { ((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f,
                        (rgb & 0xFF) / 255.0f, 1.0f };
    int w, h;
    D3D11_VIEWPORT vp = {};
    platform_client_size(&w, &h);
    vp.Width = (float) w;
    vp.Height = (float) h;
    vp.MaxDepth = 1.0f;
    s_context->RSSetViewports(1, &vp);
    s_context->OMSetRenderTargets(1, &s_back_buffer, NULL);
    s_context->ClearRenderTargetView(s_back_buffer, colour);
}

static int s_refresh_hz = PLATFORM_REFRESH_HZ;

void platform_set_refresh_hz(int hz)
{
    s_refresh_hz = hz > 0 ? hz : PLATFORM_REFRESH_HZ;
}

int platform_refresh_hz(void)
{
    return s_refresh_hz;
}

/* Sleeps until the next 1/60 s (PAL: 1/50 s) tick (timer for most of it,
 * then a short spin). */
static void wait_next_frame(void)
{
    LARGE_INTEGER now;
    LONGLONG period = s_freq.QuadPart / s_refresh_hz;

    s_next_frame.QuadPart += period;
    QueryPerformanceCounter(&now);
    if (now.QuadPart > s_next_frame.QuadPart + period) {
        s_next_frame = now;             /* fell behind (e.g. window dragged): resync */
        return;
    }
    LONGLONG remaining = s_next_frame.QuadPart - now.QuadPart;
    LONGLONG spin = s_freq.QuadPart / 1000;     /* last ~1 ms */
    if (remaining > spin && s_timer) {
        LARGE_INTEGER due;
        due.QuadPart = -((remaining - spin) * 10000000LL / s_freq.QuadPart);
        SetWaitableTimer(s_timer, &due, 0, NULL, NULL, FALSE);
        WaitForSingleObject(s_timer, INFINITE);
    }
    do {
        QueryPerformanceCounter(&now);
    } while (now.QuadPart < s_next_frame.QuadPart);
}

static int s_fast_forward;

void platform_set_fast_forward(int on)
{
    if (s_fast_forward && !on) {
        QueryPerformanceCounter(&s_next_frame);     /* (the pace starts again from now) */
    }
    s_fast_forward = on;
}

void platform_present(void)
{
    LARGE_INTEGER now;
    /* (tests: RA_FAST=1 runs as fast as it can, showing every 16th frame) */
    static int fast = -1;
    if (fast < 0) {
        fast = GetEnvironmentVariableA("RA_FAST", NULL, 0) != 0;
    }
    if (fast || s_fast_forward) {
        if ((s_frames & (fast ? 15 : 3)) == 0) {
            s_swap_chain->Present(0, 0);
        }
    } else {
        /* (vsync only at 60 Hz: a 50 Hz PAL game waiting for a 60 Hz monitor
         * as well would show every other refresh; the timer paces it) */
        s_swap_chain->Present(s_config.vsync && s_refresh_hz == PLATFORM_REFRESH_HZ ? 1 : 0, 0);
        wait_next_frame();
    }
    s_frames++;
    QueryPerformanceCounter(&now);
    if (now.QuadPart - s_fps_start.QuadPart >= s_freq.QuadPart) {
        s_fps = s_frames * (double) s_freq.QuadPart / (double) (now.QuadPart - s_fps_start.QuadPart);
        s_frames = 0;
        s_fps_start = now;
    }
}

double platform_fps(void)
{
    return s_fps;
}

int platform_take_dropped_file(char *path, int size)
{
    if (!s_has_dropped) {
        return 0;
    }
    s_has_dropped = 0;
    wide_to_utf8(s_dropped, path, size);
    return 1;
}

int platform_open_file_dialog(char *path, int size)
{
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = s_window;
    ofn.lpstrFilter = L"PS1 disc image (*.bin, *.cue, *.chd, *.iso)\0*.bin;*.cue;*.chd;*.iso\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Choose your Roll Away, Kula World or KulaQuest disc";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) {
        return 0;
    }
    wide_to_utf8(file, path, size);
    return 1;
}
