/*
 * time_weather_ui.c — opt-in legacy Win32 control window (F8 when ui_enabled = true).
 *
 * Runs on a thread the plugin creates on first use. It never touches title
 * memory: it queues commands (tw_cmd_*) that the frame callback applies, and
 * stays synchronized with the status published by the frame callback
 * (tw_get_status), refreshed every 200 ms.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>

#include "time_weather.h"

#include <math.h>
#include <stdio.h>
#include <wchar.h>

#define TW_UI_CLASS      L"FireballTimeWeather"
#define WM_TW_TOGGLE     (WM_APP + 1)
#define WM_TW_QUIT       (WM_APP + 2)
#define ID_TIMER         1
#define ID_SLIDER        100
#define ID_HOUR_DOWN     101
#define ID_HOUR_UP       102
#define ID_PAUSE         103
#define ID_SCALE         104
#define ID_WEATHER_LIST  105
#define ID_WEATHER_SET   106
#define ID_WEATHER_BLEND 107
#define ID_LOCK          108

static HANDLE        g_ui_thread;
static DWORD         g_ui_thread_id;
static HWND          g_hwnd;
static HWND          g_status_label, g_slider, g_pause_btn, g_scale_combo, g_weather_list, g_lock_box, g_weather_label;
static volatile LONG g_ui_ready;
static int           g_dragging;
static int           g_last_scale_idx = -1;
static int32_t       g_last_weather_target = -1;

typedef struct tw_scale_choice {
    const wchar_t* label;
    double         scale; /* < 0: real time (1 game minute = 60 s) */
} tw_scale_choice_t;

static const tw_scale_choice_t k_scales[] = {
    {L"1x (game rate)", 1.0}, {L"0.25x", 0.25}, {L"0.5x", 0.5}, {L"2x", 2.0},   {L"5x", 5.0},
    {L"10x", 10.0},           {L"30x", 30.0},   {L"60x", 60.0}, {L"Real time", -1.0},
};

static int tw_ui_find_scale_idx(double scale) {
    double rt = tw_ms_per_game_minute() / 60000.0;
    int best = 0;
    double best_d = 1e30;
    for (size_t i = 0; i < sizeof(k_scales) / sizeof(k_scales[0]); ++i) {
        double v = k_scales[i].scale < 0.0 ? rt : k_scales[i].scale;
        double d = fabs(scale - v);
        if (d < best_d) {
            best_d = d;
            best = (int)i;
        }
    }
    return best;
}

static HWND tw_child(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h,
                     int id) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, parent,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    return c;
}

static void tw_ui_refresh(void) {
    tw_status_t st;
    tw_get_status(&st);
    wchar_t buf[256];
    if (st.clock_ok) {
        unsigned t = (unsigned)st.tod_sec;
        swprintf(buf, sizeof(buf) / sizeof(buf[0]), L"Game time %02u:%02u:%02u   %ls   scale %.2fx   writes %llu",
                 t / 3600u, (t % 3600u) / 60u, t % 60u, st.paused ? L"[paused]" : L"", st.scale,
                 (unsigned long long)st.writes);
        if (!g_dragging) {
            SendMessageW(g_slider, TBM_SETPOS, TRUE, (LPARAM)(t / 60u));
        }
    } else {
        swprintf(buf, sizeof(buf) / sizeof(buf[0]), L"Clock: not available (symbol unset or fields out of range)");
    }
    SetWindowTextW(g_status_label, buf);
    SetWindowTextW(g_pause_btn, st.paused ? L"Resume" : L"Pause");

    int sidx = tw_ui_find_scale_idx(st.scale);
    if (sidx != g_last_scale_idx) {
        g_last_scale_idx = sidx;
        SendMessageW(g_scale_combo, CB_SETCURSEL, (WPARAM)sidx, 0);
    }

    const tw_weather_entry_t* cur = st.weather_cur >= 0 ? tw_weather_at((uint32_t)st.weather_cur) : NULL;
    const tw_weather_entry_t* tgt = st.weather_target >= 0 ? tw_weather_at((uint32_t)st.weather_target) : NULL;
    if (st.weather_ok) {
        swprintf(buf, sizeof(buf) / sizeof(buf[0]), L"Weather now: %hs   held: %hs%ls%ls",
                 cur ? cur->name : "?", tgt ? tgt->name : "-", st.weather_locked ? L" [locked]" : L"",
                 st.blend_progress < 1.0 ? L" [blending]" : L"");
    } else {
        swprintf(buf, sizeof(buf) / sizeof(buf[0]), L"Weather: not available");
    }
    SetWindowTextW(g_weather_label, buf);
    SendMessageW(g_lock_box, BM_SETCHECK, st.weather_locked ? BST_CHECKED : BST_UNCHECKED, 0);
    if (st.weather_target >= 0 && st.weather_target != g_last_weather_target) {
        g_last_weather_target = st.weather_target;
        SendMessageW(g_weather_list, LB_SETCURSEL, (WPARAM)st.weather_target, 0);
    }
}

static void tw_apply_scale(void) {
    LRESULT sel = SendMessageW(g_scale_combo, CB_GETCURSEL, 0, 0);
    if (sel < 0 || (size_t)sel >= sizeof(k_scales) / sizeof(k_scales[0])) {
        return;
    }
    double s = k_scales[sel].scale;
    if (s < 0.0) {
        s = tw_ms_per_game_minute() / 60000.0;
    }
    g_last_scale_idx = (int)sel;
    tw_cmd_set_scale(s);
}

static void tw_apply_weather(int blend) {
    LRESULT sel = SendMessageW(g_weather_list, LB_GETCURSEL, 0, 0);
    if (sel >= 0) {
        g_last_weather_target = (int32_t)sel;
        tw_cmd_set_weather((int32_t)sel, blend);
    }
}

static LRESULT CALLBACK tw_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_last_scale_idx = -1;
        g_last_weather_target = -1;
        g_status_label = tw_child(hwnd, L"STATIC", L"", 0, 10, 10, 420, 18, 0);
        g_slider = tw_child(hwnd, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS, 10, 32, 420, 28, ID_SLIDER);
        SendMessageW(g_slider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 1439));
        SendMessageW(g_slider, TBM_SETPAGESIZE, 0, 60);
        tw_child(hwnd, L"BUTTON", L"-1h", BS_PUSHBUTTON, 10, 66, 60, 24, ID_HOUR_DOWN);
        tw_child(hwnd, L"BUTTON", L"+1h", BS_PUSHBUTTON, 76, 66, 60, 24, ID_HOUR_UP);
        g_pause_btn = tw_child(hwnd, L"BUTTON", L"Pause", BS_PUSHBUTTON, 142, 66, 80, 24, ID_PAUSE);
        g_scale_combo = tw_child(hwnd, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 230, 66, 200, 220, ID_SCALE);
        for (size_t i = 0; i < sizeof(k_scales) / sizeof(k_scales[0]); ++i) {
            SendMessageW(g_scale_combo, CB_ADDSTRING, 0, (LPARAM)k_scales[i].label);
        }
        SendMessageW(g_scale_combo, CB_SETCURSEL, 0, 0);
        g_weather_label = tw_child(hwnd, L"STATIC", L"", 0, 10, 100, 420, 18, 0);
        g_weather_list = tw_child(hwnd, L"LISTBOX", L"", WS_BORDER | WS_VSCROLL | LBS_NOTIFY, 10, 122, 300, 220,
                                  ID_WEATHER_LIST);
        uint32_t n = tw_weather_count();
        if (n == 0u) {
            for (uint32_t i = 0; i < 168u; ++i) {
                wchar_t w[16];
                swprintf(w, sizeof(w) / sizeof(w[0]), L"#%u", i);
                SendMessageW(g_weather_list, LB_ADDSTRING, 0, (LPARAM)w);
            }
        }
        for (uint32_t i = 0; i < n; ++i) {
            const tw_weather_entry_t* e = tw_weather_at(i);
            wchar_t w[TW_NAME_MAX + 8];
            swprintf(w, sizeof(w) / sizeof(w[0]), L"%3u  %hs", e->index, e->name);
            SendMessageW(g_weather_list, LB_ADDSTRING, 0, (LPARAM)w);
        }
        tw_child(hwnd, L"BUTTON", L"Set now", BS_PUSHBUTTON, 320, 122, 110, 24, ID_WEATHER_SET);
        tw_child(hwnd, L"BUTTON", L"Blend", BS_PUSHBUTTON, 320, 152, 110, 24, ID_WEATHER_BLEND);
        g_lock_box = tw_child(hwnd, L"BUTTON", L"Lock weather", BS_AUTOCHECKBOX, 320, 186, 110, 24, ID_LOCK);
        SetTimer(hwnd, ID_TIMER, 200, NULL);
        return 0;
    }
    case WM_TIMER:
        tw_ui_refresh();
        return 0;
    case WM_HSCROLL:
        if ((HWND)lp == g_slider) {
            int code = LOWORD(wp);
            g_dragging = code == TB_THUMBTRACK;
            LRESULT pos = SendMessageW(g_slider, TBM_GETPOS, 0, 0);
            if (code != TB_ENDTRACK) {
                tw_cmd_set_tod((double)pos * 60.0);
            }
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_HOUR_DOWN: tw_cmd_add_hours(-1); break;
        case ID_HOUR_UP: tw_cmd_add_hours(1); break;
        case ID_PAUSE: tw_cmd_toggle_pause(); break;
        case ID_SCALE:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                tw_apply_scale();
            }
            break;
        case ID_WEATHER_SET: tw_apply_weather(0); break;
        case ID_WEATHER_BLEND: tw_apply_weather(1); break;
        case ID_WEATHER_LIST:
            if (HIWORD(wp) == LBN_DBLCLK) {
                tw_apply_weather(0);
            }
            break;
        case ID_LOCK:
            tw_cmd_set_lock(SendMessageW(g_lock_box, BM_GETCHECK, 0, 0) == BST_CHECKED);
            break;
        default: break;
        }
        return 0;
    case WM_TW_TOGGLE:
        ShowWindow(hwnd, IsWindowVisible(hwnd) ? SW_HIDE : SW_SHOWNORMAL);
        return 0;
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE); /* F8 brings it back; plugin shutdown destroys it */
        return 0;
    case WM_TW_QUIT:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, ID_TIMER);
        PostQuitMessage(0);
        return 0;
    default: return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

static DWORD WINAPI tw_ui_main(LPVOID unused) {
    (void)unused;
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = tw_wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512)); /* IDC_ARROW */
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = TW_UI_CLASS;
    RegisterClassExW(&wc);
    RECT r = {0, 0, 440, 352};
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    g_hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, TW_UI_CLASS, L"Time & Weather (Fireball)",
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT,
                             r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    InterlockedExchange(&g_ui_ready, 1);
    if (!g_hwnd) {
        return 1;
    }
    ShowWindow(g_hwnd, SW_SHOWNORMAL);
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    UnregisterClassW(TW_UI_CLASS, wc.hInstance);
    g_hwnd = NULL;
    return 0;
}

void tw_ui_toggle(void) {
    if (!g_ui_thread) {
        g_ui_thread = CreateThread(NULL, 0, tw_ui_main, NULL, 0, &g_ui_thread_id);
        return; /* the new window opens shown */
    }
    if (g_hwnd) {
        PostMessageW(g_hwnd, WM_TW_TOGGLE, 0, 0);
    }
}

void tw_ui_shutdown(void) {
    if (!g_ui_thread) {
        return;
    }
    for (int i = 0; i < 100 && !InterlockedCompareExchange(&g_ui_ready, 0, 0); ++i) {
        Sleep(10);
    }
    if (g_hwnd) {
        PostMessageW(g_hwnd, WM_TW_QUIT, 0, 0);
    }
    WaitForSingleObject(g_ui_thread, 2000);
    CloseHandle(g_ui_thread);
    g_ui_thread = NULL;
    InterlockedExchange(&g_ui_ready, 0);
}
