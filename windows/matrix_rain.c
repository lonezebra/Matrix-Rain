/* Matrix Rain screensaver — Win32 shell (rendering, modes, settings dialog).
 * Simulation lives in rain.c and is display-independent.
 */
#include <windows.h>
#include <commctrl.h>

#include <stdio.h>
#include <wchar.h>
#include <wctype.h>

#include "rain.h"

#define APP_CLASS     L"MatrixRainSaver"
#define CFG_CLASS     L"MatrixRainConfig"
#define REG_PATH      L"Software\\MatrixRain"
#define TIMER_ID      1
#define FRAME_MS      33          /* ~30 FPS; animation itself is dt-based */
#define RAMP_STEPS    32
#define MOUSE_SLOP    3           /* px of mouse travel that ends the saver */

/* ---------------------------------------------------------------- settings */

typedef struct Settings {
    COLORREF color;    /* 0x00BBGGRR */
    DWORD    density;  /* percent, 5..100  (spec 0.05..1.0 stored *100) */
    DWORD    size;     /* px, 8..64 */
    DWORD    speed;    /* percent, 50..300 (spec 0.5..3.0 stored *100) */
} Settings;

typedef struct Preset {
    const wchar_t *name;
    COLORREF       color;
} Preset;

static const Preset PRESETS[] = {
    { L"Matrix Green", RGB(0x00, 0xFF, 0x41) },
    { L"Amber",        RGB(0xFF, 0xB0, 0x00) },
    { L"Ice Blue",     RGB(0x00, 0xC8, 0xFF) },
    { L"Crimson",      RGB(0xFF, 0x20, 0x20) },
    { L"Violet",       RGB(0xB0, 0x40, 0xFF) },
    { L"White",        RGB(0xE0, 0xE0, 0xE0) },
};
#define PRESET_COUNT   ((int)(sizeof(PRESETS) / sizeof(PRESETS[0])))
#define CUSTOM_INDEX   PRESET_COUNT

static HINSTANCE g_hInst;
static Settings  g_settings;

static DWORD clamp_dw(DWORD v, DWORD lo, DWORD hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static void load_settings(Settings *s)
{
    HKEY hKey;

    s->color   = RGB(0x00, 0xFF, 0x41);
    s->density = 75;
    s->size    = 18;
    s->speed   = 100;

    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_PATH, 0, KEY_READ, &hKey)
            == ERROR_SUCCESS) {
        DWORD val, cb, type;

        cb = sizeof(val);
        if (RegQueryValueExW(hKey, L"color", NULL, &type, (BYTE *)&val, &cb)
                == ERROR_SUCCESS && type == REG_DWORD)
            s->color = (COLORREF)(val & 0x00FFFFFF);
        cb = sizeof(val);
        if (RegQueryValueExW(hKey, L"density", NULL, &type, (BYTE *)&val, &cb)
                == ERROR_SUCCESS && type == REG_DWORD)
            s->density = clamp_dw(val, 5, 100);
        cb = sizeof(val);
        if (RegQueryValueExW(hKey, L"size", NULL, &type, (BYTE *)&val, &cb)
                == ERROR_SUCCESS && type == REG_DWORD)
            s->size = clamp_dw(val, 8, 64);
        cb = sizeof(val);
        if (RegQueryValueExW(hKey, L"speed", NULL, &type, (BYTE *)&val, &cb)
                == ERROR_SUCCESS && type == REG_DWORD)
            s->speed = clamp_dw(val, 50, 300);
        RegCloseKey(hKey);
    }
}

static void save_settings(const Settings *s)
{
    HKEY hKey;

    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_PATH, 0, NULL, 0,
                        KEY_WRITE, NULL, &hKey, NULL) != ERROR_SUCCESS)
        return;

    {
        DWORD v;
        v = (DWORD)s->color;
        RegSetValueExW(hKey, L"color", 0, REG_DWORD, (const BYTE *)&v, sizeof(v));
        v = s->density;
        RegSetValueExW(hKey, L"density", 0, REG_DWORD, (const BYTE *)&v, sizeof(v));
        v = s->size;
        RegSetValueExW(hKey, L"size", 0, REG_DWORD, (const BYTE *)&v, sizeof(v));
        v = s->speed;
        RegSetValueExW(hKey, L"speed", 0, REG_DWORD, (const BYTE *)&v, sizeof(v));
    }
    RegCloseKey(hKey);
}

/* --------------------------------------------------------------- rendering */

typedef struct RainWindow {
    RainSim       sim;
    BOOL          preview;
    int           width, height;
    int           cellW, cellH;
    HDC           memDC;
    HBITMAP       bmp, oldBmp;
    HFONT         font, oldFont;
    COLORREF      ramp[RAMP_STEPS]; /* rain color -> black */
    COLORREF      headColor;
    LARGE_INTEGER lastTick;
} RainWindow;

static LARGE_INTEGER g_qpf;
static BOOL          g_mouseInit;
static POINT         g_mousePt;
static RainWindow   *g_pendingRw; /* ownership handoff: creator -> WM_CREATE */

/* "MS Gothic" first (has half-width katakana), then "Consolas", then the
 * stock fixed font. The font mapper always returns *something*, so verify
 * the face actually resolved before accepting it. */
static HFONT create_glyph_font(int size)
{
    static const wchar_t *faces[] = { L"MS Gothic", L"Consolas" };
    HDC screen = GetDC(NULL);

    for (int i = 0; i < 2; i++) {
        HFONT f = CreateFontW(-size, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                              FIXED_PITCH | FF_MODERN, faces[i]);
        if (f) {
            wchar_t got[LF_FACESIZE] = L"";
            HFONT   old = (HFONT)SelectObject(screen, f);
            GetTextFaceW(screen, LF_FACESIZE, got);
            SelectObject(screen, old);
            if (_wcsicmp(got, faces[i]) == 0) {
                ReleaseDC(NULL, screen);
                return f;
            }
            DeleteObject(f);
        }
    }
    ReleaseDC(NULL, screen);
    return (HFONT)GetStockObject(ANSI_FIXED_FONT);
}

static void build_palette(RainWindow *rw, COLORREF rain)
{
    int r = GetRValue(rain), g = GetGValue(rain), b = GetBValue(rain);

    for (int i = 0; i < RAMP_STEPS; i++) {
        float f = (float)i / (float)(RAMP_STEPS - 1);
        rw->ramp[i] = RGB((int)(r * f), (int)(g * f), (int)(b * f));
    }
    /* head = rain + (white - rain) * 0.75 */
    rw->headColor = RGB(r + (int)((255 - r) * 0.75f),
                        g + (int)((255 - g) * 0.75f),
                        b + (int)((255 - b) * 0.75f));
}

static void destroy_surface(RainWindow *rw)
{
    if (rw->memDC) {
        if (rw->oldFont)
            SelectObject(rw->memDC, rw->oldFont);
        if (rw->oldBmp)
            SelectObject(rw->memDC, rw->oldBmp);
        DeleteDC(rw->memDC);
        rw->memDC = NULL;
    }
    if (rw->bmp) {
        DeleteObject(rw->bmp);
        rw->bmp = NULL;
    }
    if (rw->font) {
        DeleteObject(rw->font); /* no-op if it is the stock font */
        rw->font = NULL;
    }
    rain_free(&rw->sim);
}

static BOOL build_surface(RainWindow *rw, HWND hwnd, int width, int height)
{
    HDC screen;
    int size = (int)g_settings.size;
    int cols, rows;

    destroy_surface(rw);
    if (width < 1 || height < 1)
        return FALSE;

    /* Keep the preview grid a few rows tall even though the window is tiny. */
    if (rw->preview) {
        int maxSize = height / 6;
        if (size > maxSize)
            size = maxSize;
        if (size < 6)
            size = 6;
    }

    rw->width  = width;
    rw->height = height;
    rw->cellW  = (int)(size * 0.62f);
    rw->cellH  = (int)(size * 1.05f);
    if (rw->cellW < 1)
        rw->cellW = 1;
    if (rw->cellH < 1)
        rw->cellH = 1;
    cols = width / rw->cellW;
    rows = height / rw->cellH;

    if (rain_init(&rw->sim, cols, rows,
                  (float)g_settings.density / 100.0f,
                  (float)g_settings.speed / 100.0f,
                  GetTickCount() ^ (unsigned)(UINT_PTR)hwnd) != 0)
        return FALSE;

    screen = GetDC(hwnd);
    rw->memDC = CreateCompatibleDC(screen);
    rw->bmp   = CreateCompatibleBitmap(screen, width, height); /* NOT memDC:
                                that would yield a 1-bpp monochrome bitmap */
    ReleaseDC(hwnd, screen);
    if (!rw->memDC || !rw->bmp) {
        destroy_surface(rw);
        return FALSE;
    }
    rw->oldBmp  = (HBITMAP)SelectObject(rw->memDC, rw->bmp);
    rw->font    = create_glyph_font(size);
    rw->oldFont = (HFONT)SelectObject(rw->memDC, rw->font);
    SetBkMode(rw->memDC, TRANSPARENT);

    build_palette(rw, g_settings.color);
    QueryPerformanceCounter(&rw->lastTick);
    return TRUE;
}

static void render_frame(RainWindow *rw)
{
    RECT     rc = { 0, 0, rw->width, rw->height };
    RainSim *s  = &rw->sim;

    FillRect(rw->memDC, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));

    for (int c = 0; c < s->cols; c++) {
        const float   *colB = &s->bright[(size_t)c * s->rows];
        const wchar_t *colG = &s->glyph[(size_t)c * s->rows];
        int            head = rain_head_cell(s, c);
        int            x    = c * rw->cellW;

        for (int r = 0; r < s->rows; r++) {
            float b = colB[r];
            int   shade;

            if (b <= 0.0f || r == head)
                continue;
            shade = (int)(b * (RAMP_STEPS - 1) + 0.5f);
            if (shade >= RAMP_STEPS)
                shade = RAMP_STEPS - 1;
            SetTextColor(rw->memDC, rw->ramp[shade]);
            ExtTextOutW(rw->memDC, x, r * rw->cellH, 0, NULL, &colG[r], 1, NULL);
        }
        if (head >= 0) {
            SetTextColor(rw->memDC, rw->headColor);
            ExtTextOutW(rw->memDC, x, head * rw->cellH, 0, NULL,
                        &colG[head], 1, NULL);
        }
    }
}

/* ------------------------------------------------------------ saver window */

static LRESULT CALLBACK SaverWndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                     LPARAM lParam)
{
    RainWindow *rw = (RainWindow *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lParam;
        RECT           rc;

        rw = (RainWindow *)cs->lpCreateParams;
        g_pendingRw = NULL; /* WM_DESTROY owns the struct from here on */
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)rw);
        GetClientRect(hwnd, &rc);
        if (!build_surface(rw, hwnd, rc.right, rc.bottom))
            return -1;
        SetTimer(hwnd, TIMER_ID, FRAME_MS, NULL);
        return 0;
    }

    case WM_TIMER:
        if (rw && rw->memDC && wParam == TIMER_ID) {
            LARGE_INTEGER now;
            float         dt;

            QueryPerformanceCounter(&now);
            dt = (float)(now.QuadPart - rw->lastTick.QuadPart)
                 / (float)g_qpf.QuadPart;
            rw->lastTick = now;
            rain_step(&rw->sim, dt);
            render_frame(rw);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC         hdc = BeginPaint(hwnd, &ps);

        if (rw && rw->memDC)
            BitBlt(hdc, 0, 0, rw->width, rw->height, rw->memDC, 0, 0, SRCCOPY);
        else
            FillRect(hdc, &ps.rcPaint, (HBRUSH)GetStockObject(BLACK_BRUSH));
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1; /* the blit covers everything; avoids flicker */

    case WM_SIZE:
        if (rw && (LOWORD(lParam) != rw->width || HIWORD(lParam) != rw->height))
            build_surface(rw, hwnd, LOWORD(lParam), HIWORD(lParam));
        return 0;

    case WM_SETCURSOR:
        if (rw && !rw->preview) {
            SetCursor(NULL);
            return TRUE;
        }
        break;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN:
    case WM_MOUSEWHEEL:
        if (rw && !rw->preview)
            PostQuitMessage(0);
        return 0;

    case WM_MOUSEMOVE:
        if (rw && !rw->preview) {
            POINT pt;

            GetCursorPos(&pt); /* screen coords: shared across monitors */
            if (!g_mouseInit) {
                g_mouseInit = TRUE;
                g_mousePt   = pt;
            } else if (pt.x - g_mousePt.x > MOUSE_SLOP ||
                       g_mousePt.x - pt.x > MOUSE_SLOP ||
                       pt.y - g_mousePt.y > MOUSE_SLOP ||
                       g_mousePt.y - pt.y > MOUSE_SLOP) {
                PostQuitMessage(0);
            }
        }
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, TIMER_ID);
        if (rw) {
            destroy_surface(rw);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            HeapFree(GetProcessHeap(), 0, rw);
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static ATOM register_saver_class(void)
{
    WNDCLASSW wc = {0};

    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = SaverWndProc;
    wc.hInstance     = g_hInst;
    wc.hCursor       = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = APP_CLASS;
    return RegisterClassW(&wc);
}

static HWND create_saver_window(const RECT *rc, HWND parent, BOOL preview)
{
    RainWindow *rw = (RainWindow *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                             sizeof(RainWindow));
    DWORD       style, exStyle;
    HWND        hwnd;

    if (!rw)
        return NULL;
    rw->preview = preview;
    if (preview) {
        style   = WS_CHILD | WS_VISIBLE;
        exStyle = 0;
    } else {
        style   = WS_POPUP | WS_VISIBLE;
        exStyle = WS_EX_TOPMOST;
    }
    g_pendingRw = rw;
    hwnd = CreateWindowExW(exStyle, APP_CLASS, L"Matrix Rain", style,
                           rc->left, rc->top,
                           rc->right - rc->left, rc->bottom - rc->top,
                           parent, NULL, g_hInst, rw);
    /* On failure after WM_CREATE ran, WM_DESTROY already freed the struct;
     * free here only if creation failed before WM_CREATE adopted it. */
    if (!hwnd && g_pendingRw)
        HeapFree(GetProcessHeap(), 0, g_pendingRw);
    g_pendingRw = NULL;
    return hwnd;
}

static BOOL CALLBACK MonitorEnumProc(HMONITOR mon, HDC hdc, LPRECT rc,
                                     LPARAM lParam)
{
    int *count = (int *)lParam;

    (void)mon;
    (void)hdc;
    if (create_saver_window(rc, NULL, FALSE))
        (*count)++;
    return TRUE;
}

static int message_loop(void)
{
    MSG msg;

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}

static int run_fullscreen(void)
{
    int count = 0;

    if (!register_saver_class())
        return 1;
    EnumDisplayMonitors(NULL, NULL, MonitorEnumProc, (LPARAM)&count);
    if (count == 0) {
        RECT rc = { 0, 0, GetSystemMetrics(SM_CXSCREEN),
                    GetSystemMetrics(SM_CYSCREEN) };
        if (!create_saver_window(&rc, NULL, FALSE))
            return 1;
    }
    return message_loop();
}

static int run_preview(HWND parent)
{
    RECT rc;

    if (!IsWindow(parent))
        return 0;
    if (!register_saver_class())
        return 1;
    GetClientRect(parent, &rc);
    if (!create_saver_window(&rc, parent, TRUE))
        return 1;
    return message_loop();
}

/* --------------------------------------------------------- settings dialog */

enum {
    CID_PRESET = 100,
    CID_RED, CID_GREEN, CID_BLUE,
    CID_DENSITY, CID_DENSITY_VAL,
    CID_SIZE, CID_SIZE_VAL,
    CID_SPEED, CID_SPEED_VAL
};

static HFONT g_uiFont;
static BOOL  g_syncingRgb; /* suppress EN_CHANGE from programmatic SetText */
static BOOL  g_cfgDone;

static HWND make_ctrl(HWND dlg, const wchar_t *cls, const wchar_t *text,
                      DWORD style, int x, int y, int w, int h, int id)
{
    HWND ctrl = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                                x, y, w, h, dlg, (HMENU)(INT_PTR)id,
                                g_hInst, NULL);
    if (ctrl && g_uiFont)
        SendMessageW(ctrl, WM_SETFONT, (WPARAM)g_uiFont, TRUE);
    return ctrl;
}

static DWORD read_edit(HWND dlg, int id, DWORD hi)
{
    wchar_t buf[16];
    DWORD   v;

    GetWindowTextW(GetDlgItem(dlg, id), buf, 16);
    v = (DWORD)wcstoul(buf, NULL, 10);
    return v > hi ? hi : v;
}

static void set_rgb_edits(HWND dlg, COLORREF c)
{
    wchar_t buf[16];

    g_syncingRgb = TRUE;
    wsprintfW(buf, L"%d", GetRValue(c));
    SetWindowTextW(GetDlgItem(dlg, CID_RED), buf);
    wsprintfW(buf, L"%d", GetGValue(c));
    SetWindowTextW(GetDlgItem(dlg, CID_GREEN), buf);
    wsprintfW(buf, L"%d", GetBValue(c));
    SetWindowTextW(GetDlgItem(dlg, CID_BLUE), buf);
    g_syncingRgb = FALSE;
}

static COLORREF read_rgb_edits(HWND dlg)
{
    return RGB(read_edit(dlg, CID_RED, 255),
               read_edit(dlg, CID_GREEN, 255),
               read_edit(dlg, CID_BLUE, 255));
}

static int preset_for_color(COLORREF c)
{
    for (int i = 0; i < PRESET_COUNT; i++) {
        if (PRESETS[i].color == c)
            return i;
    }
    return CUSTOM_INDEX;
}

static void update_slider_label(HWND dlg, int sliderId, int labelId,
                                const wchar_t *suffix)
{
    wchar_t buf[32];
    int     pos = (int)SendMessageW(GetDlgItem(dlg, sliderId), TBM_GETPOS, 0, 0);

    wsprintfW(buf, L"%d%s", pos, suffix);
    SetWindowTextW(GetDlgItem(dlg, labelId), buf);
}

static void update_all_labels(HWND dlg)
{
    update_slider_label(dlg, CID_DENSITY, CID_DENSITY_VAL, L"%");
    update_slider_label(dlg, CID_SIZE, CID_SIZE_VAL, L" px");
    update_slider_label(dlg, CID_SPEED, CID_SPEED_VAL, L"%");
}

static void init_slider(HWND dlg, int id, int lo, int hi, int pos)
{
    HWND s = GetDlgItem(dlg, id);

    SendMessageW(s, TBM_SETRANGE, TRUE, MAKELPARAM(lo, hi));
    SendMessageW(s, TBM_SETPOS, TRUE, pos);
}

static void create_config_controls(HWND dlg)
{
    HWND combo;

    make_ctrl(dlg, L"STATIC", L"Preset:", 0, 12, 18, 70, 20, 0);
    combo = make_ctrl(dlg, L"COMBOBOX", NULL,
                      CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                      90, 14, 270, 220, CID_PRESET);
    for (int i = 0; i < PRESET_COUNT; i++)
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)PRESETS[i].name);
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"Custom");
    SendMessageW(combo, CB_SETCURSEL, preset_for_color(g_settings.color), 0);

    make_ctrl(dlg, L"STATIC", L"Red:", 0, 12, 53, 40, 20, 0);
    make_ctrl(dlg, L"EDIT", NULL,
              ES_NUMBER | WS_BORDER | WS_TABSTOP, 54, 50, 50, 22, CID_RED);
    make_ctrl(dlg, L"STATIC", L"Green:", 0, 128, 53, 50, 20, 0);
    make_ctrl(dlg, L"EDIT", NULL,
              ES_NUMBER | WS_BORDER | WS_TABSTOP, 180, 50, 50, 22, CID_GREEN);
    make_ctrl(dlg, L"STATIC", L"Blue:", 0, 256, 53, 44, 20, 0);
    make_ctrl(dlg, L"EDIT", NULL,
              ES_NUMBER | WS_BORDER | WS_TABSTOP, 302, 50, 50, 22, CID_BLUE);
    set_rgb_edits(dlg, g_settings.color);

    make_ctrl(dlg, L"STATIC", L"Density (5–100%):", 0, 12, 90, 200, 18, 0);
    make_ctrl(dlg, TRACKBAR_CLASSW, NULL, TBS_HORZ | TBS_AUTOTICKS | WS_TABSTOP,
              12, 110, 290, 30, CID_DENSITY);
    make_ctrl(dlg, L"STATIC", NULL, 0, 308, 116, 55, 20, CID_DENSITY_VAL);
    init_slider(dlg, CID_DENSITY, 5, 100, (int)g_settings.density);

    make_ctrl(dlg, L"STATIC", L"Glyph size (8–64 px):", 0, 12, 150, 200, 18, 0);
    make_ctrl(dlg, TRACKBAR_CLASSW, NULL, TBS_HORZ | TBS_AUTOTICKS | WS_TABSTOP,
              12, 170, 290, 30, CID_SIZE);
    make_ctrl(dlg, L"STATIC", NULL, 0, 308, 176, 55, 20, CID_SIZE_VAL);
    init_slider(dlg, CID_SIZE, 8, 64, (int)g_settings.size);

    make_ctrl(dlg, L"STATIC", L"Speed (50–300%):", 0, 12, 210, 200, 18, 0);
    make_ctrl(dlg, TRACKBAR_CLASSW, NULL, TBS_HORZ | TBS_AUTOTICKS | WS_TABSTOP,
              12, 230, 290, 30, CID_SPEED);
    make_ctrl(dlg, L"STATIC", NULL, 0, 308, 236, 55, 20, CID_SPEED_VAL);
    init_slider(dlg, CID_SPEED, 50, 300, (int)g_settings.speed);

    make_ctrl(dlg, L"BUTTON", L"OK", BS_DEFPUSHBUTTON | WS_TABSTOP,
              196, 272, 80, 26, IDOK);
    make_ctrl(dlg, L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP,
              284, 272, 80, 26, IDCANCEL);

    update_all_labels(dlg);
}

static void config_apply_and_save(HWND dlg)
{
    g_settings.color   = read_rgb_edits(dlg);
    g_settings.density = clamp_dw((DWORD)SendMessageW(
        GetDlgItem(dlg, CID_DENSITY), TBM_GETPOS, 0, 0), 5, 100);
    g_settings.size    = clamp_dw((DWORD)SendMessageW(
        GetDlgItem(dlg, CID_SIZE), TBM_GETPOS, 0, 0), 8, 64);
    g_settings.speed   = clamp_dw((DWORD)SendMessageW(
        GetDlgItem(dlg, CID_SPEED), TBM_GETPOS, 0, 0), 50, 300);
    save_settings(&g_settings);
}

static LRESULT CALLBACK ConfigWndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                      LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE:
        create_config_controls(hwnd);
        return 0;

    case WM_COMMAND: {
        int id   = LOWORD(wParam);
        int code = HIWORD(wParam);

        /* Not a real dialog (no DM_GETDEFID), so the Enter key may deliver
         * IDOK with a notification code other than BN_CLICKED — accept any. */
        if (id == IDOK) {
            config_apply_and_save(hwnd);
            DestroyWindow(hwnd);
        } else if (id == IDCANCEL) {
            DestroyWindow(hwnd);
        } else if (id == CID_PRESET && code == CBN_SELCHANGE) {
            int sel = (int)SendMessageW(GetDlgItem(hwnd, CID_PRESET),
                                        CB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < PRESET_COUNT)
                set_rgb_edits(hwnd, PRESETS[sel].color);
        } else if ((id == CID_RED || id == CID_GREEN || id == CID_BLUE)
                   && code == EN_CHANGE && !g_syncingRgb) {
            SendMessageW(GetDlgItem(hwnd, CID_PRESET), CB_SETCURSEL,
                         preset_for_color(read_rgb_edits(hwnd)), 0);
        }
        return 0;
    }

    case WM_HSCROLL:
        update_all_labels(hwnd);
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        g_cfgDone = TRUE;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static int run_config(HWND owner)
{
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES };
    NONCLIENTMETRICSW    ncm = {0};
    WNDCLASSW            wc  = {0};
    RECT                 rc  = { 0, 0, 376, 310 };
    DWORD                style = WS_CAPTION | WS_SYSMENU | WS_POPUP;
    int                  x, y;
    HWND                 dlg;
    MSG                  msg;

    InitCommonControlsEx(&icc);

    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
        g_uiFont = CreateFontIndirectW(&ncm.lfMessageFont);

    wc.lpfnWndProc   = ConfigWndProc;
    wc.hInstance     = g_hInst;
    wc.hCursor       = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = CFG_CLASS;
    if (!RegisterClassW(&wc))
        return 1;

    AdjustWindowRect(&rc, style, FALSE);
    {
        RECT area = { 0, 0, GetSystemMetrics(SM_CXSCREEN),
                      GetSystemMetrics(SM_CYSCREEN) };
        if (owner && IsWindow(owner))
            GetWindowRect(owner, &area);
        x = area.left + ((area.right - area.left)
                         - (rc.right - rc.left)) / 2;
        y = area.top + ((area.bottom - area.top)
                        - (rc.bottom - rc.top)) / 2;
        if (x < 0)
            x = 0;
        if (y < 0)
            y = 0;
    }

    dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, CFG_CLASS,
                          L"Matrix Rain Settings", style, x, y,
                          rc.right - rc.left, rc.bottom - rc.top,
                          owner, NULL, g_hInst, NULL);
    if (!dlg)
        return 1;
    ShowWindow(dlg, SW_SHOW);

    while (!g_cfgDone && GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (g_uiFont) {
        DeleteObject(g_uiFont);
        g_uiFont = NULL;
    }
    return 0;
}

/* -------------------------------------------------------- headless test /t */

static int run_test(void)
{
    RainSim s;
    int     cols = 160, rows = 50;
    float   dt   = 1.0f / 30.0f;

    AllocConsole();
    {
        FILE *fp;
        freopen_s(&fp, "CONOUT$", "w", stdout);
    }

    printf("Matrix Rain headless test: %dx%d grid, density=%lu%%, "
           "speed=%lu%%, 300 frames @ 30 FPS\n",
           cols, rows, (unsigned long)g_settings.density,
           (unsigned long)g_settings.speed);

    if (rain_init(&s, cols, rows,
                  (float)g_settings.density / 100.0f,
                  (float)g_settings.speed / 100.0f, 12345u) != 0) {
        printf("rain_init failed\n");
        return 1;
    }

    for (int frame = 1; frame <= 300; frame++) {
        rain_step(&s, dt);
        if (frame % 30 == 0) {
            printf("frame %3d: active=%3d/%d lit=%5d/%d spawned=%lu\n",
                   frame, rain_active_streams(&s),
                   (int)(s.density * (float)cols),
                   rain_lit_cells(&s), cols * rows, s.spawned);
        }
    }

    {
        int active = rain_active_streams(&s);
        int target = (int)(s.density * (float)cols);
        printf("done: active=%d (target %d), lit=%d, spawned=%lu — %s\n",
               active, target, rain_lit_cells(&s), s.spawned,
               (active > 0 && active <= target) ? "OK" : "SUSPECT");
    }
    rain_free(&s);
    return 0;
}

/* ------------------------------------------------------------ command line */

typedef enum { MODE_CONFIG, MODE_SAVER, MODE_PREVIEW, MODE_TEST } Mode;

/* Screensaver convention: /s /p <hwnd> /c /c:<hwnd>, case-insensitive,
 * '-' accepted in place of '/'. No arguments means configure. */
static Mode parse_cmdline(const wchar_t *cmd, HWND *hwndOut)
{
    const wchar_t *p = cmd;
    wchar_t        c;

    *hwndOut = NULL;
    while (*p == L' ' || *p == L'\t')
        p++;
    if (!*p)
        return MODE_CONFIG;
    if (*p == L'/' || *p == L'-')
        p++;
    c = towlower(*p);
    if (c)
        p++;
    if (*p == L':')
        p++;
    while (*p == L' ' || *p == L'\t')
        p++;
    if (*p >= L'0' && *p <= L'9')
        *hwndOut = (HWND)(UINT_PTR)wcstoull(p, NULL, 10);

    switch (c) {
    case L's': return MODE_SAVER;
    case L'p': return MODE_PREVIEW;
    case L't': return MODE_TEST;
    case L'c': /* fall through */
    default:   return MODE_CONFIG;
    }
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                    PWSTR lpCmdLine, int nCmdShow)
{
    HWND arg;
    Mode mode;

    (void)hPrevInstance;
    (void)nCmdShow;

    g_hInst = hInstance;
    QueryPerformanceFrequency(&g_qpf);
    load_settings(&g_settings);

    mode = parse_cmdline(lpCmdLine, &arg);
    switch (mode) {
    case MODE_SAVER:   return run_fullscreen();
    case MODE_PREVIEW: return run_preview(arg);
    case MODE_TEST:    return run_test();
    case MODE_CONFIG:
    default:           return run_config(arg);
    }
}
