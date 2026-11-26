/*
 * fpv_drone_osd.c — Opt-in pure C11 Win32 GDI live Betaflight OSD & Telemetry preview window.
 *
 * Off by default (`osd_window_enabled = false` in `mod.toml`). When enabled, displays the
 * rasterized MAX7456 OSD from `fpv_osd_render_rgba()` alongside live flight telemetry
 * (armed/failsafe state, speed, altitude, battery voltage, RSSI, Betaflight Configurator URL).
 * Zero third-party dependencies (`user32`, `gdi32` only).
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "fpv_drone.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DRONE_OSD_CLASS L"FireballFpvDroneOsdWindow"
#define DRONE_OSD_TIMER 1u

typedef struct drone_osd_ui {
    SRWLOCK              lock;
    int                  lock_init;
    HANDLE               thread;
    HANDLE               ready_ev;
    volatile LONG        running;
    volatile LONG        visible;
    HWND                 hwnd;
    drone_osd_snapshot_t snap;
    uint8_t              bgra[DRONE_OSD_W * DRONE_OSD_H * 4u];
} drone_osd_ui_t;

static drone_osd_ui_t g_osd_ui;

static const char* drone_view_label(uint32_t v) {
    switch (v) {
    case (uint32_t)FPV_VIEW_FPV:   return "FPV";
    case (uint32_t)FPV_VIEW_CHASE: return "Chase";
    case (uint32_t)FPV_VIEW_LOS:   return "LOS";
    case (uint32_t)FPV_VIEW_PILOT: return "Pilot";
    default:                       return "Unknown";
    }
}

static void drone_osd_paint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    if (!hdc) {
        return;
    }

    RECT rc;
    GetClientRect(hwnd, &rc);
    int cw = rc.right - rc.left;
    int ch = rc.bottom - rc.top;

    HBRUSH bg = CreateSolidBrush(RGB(14, 16, 22));
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);

    drone_osd_snapshot_t local;
    AcquireSRWLockShared(&g_osd_ui.lock);
    local = g_osd_ui.snap;
    ReleaseSRWLockShared(&g_osd_ui.lock);

    int header_h = 44;
    if (local.valid && local.has_rgba && ch > header_h + 16) {
        /* Convert RGBA -> BGRA premultiplied over dark backdrop for GDI StretchDIBits */
        size_t px_count = (size_t)DRONE_OSD_W * (size_t)DRONE_OSD_H;
        for (size_t i = 0; i < px_count; ++i) {
            uint8_t r = local.rgba[i * 4u + 0u];
            uint8_t g = local.rgba[i * 4u + 1u];
            uint8_t b = local.rgba[i * 4u + 2u];
            uint8_t a = local.rgba[i * 4u + 3u];
            uint32_t inv = 255u - (uint32_t)a;
            g_osd_ui.bgra[i * 4u + 0u] = (uint8_t)(((uint32_t)b * a + 22u * inv) / 255u);
            g_osd_ui.bgra[i * 4u + 1u] = (uint8_t)(((uint32_t)g * a + 16u * inv) / 255u);
            g_osd_ui.bgra[i * 4u + 2u] = (uint8_t)(((uint32_t)r * a + 14u * inv) / 255u);
            g_osd_ui.bgra[i * 4u + 3u] = 255u;
        }

        BITMAPINFO bmi;
        memset(&bmi, 0, sizeof(bmi));
        bmi.bmiHeader.biSize = (DWORD)sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = (LONG)DRONE_OSD_W;
        bmi.bmiHeader.biHeight = -(LONG)DRONE_OSD_H; /* top-down DIB */
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        SetStretchBltMode(hdc, HALFTONE);
        (void)StretchDIBits(
            hdc,
            8,
            header_h,
            cw - 16,
            ch - header_h - 8,
            0,
            0,
            (int)DRONE_OSD_W,
            (int)DRONE_OSD_H,
            g_osd_ui.bgra,
            &bmi,
            DIB_RGB_COLORS,
            SRCCOPY);
    }

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(226, 232, 240));
    char line1[256];
    char line2[256];
    if (!local.valid) {
        snprintf(line1, sizeof(line1), "Fireball FPV Drone (libfpv) — Waiting for drone launch (F7)");
        snprintf(line2, sizeof(line2), "Betaflight Configurator: %s",
                 local.configurator_url[0] ? local.configurator_url : "inactive");
    } else {
        const char* state_str = (local.flags & FPV_DRONE_CRASHED)
                                    ? "CRASHED"
                                    : ((local.flags & FPV_DRONE_ARMED) ? "ARMED" : "DISARMED");
        snprintf(
            line1,
            sizeof(line1),
            "%s | View:%s | %.1f km/h | Alt:%.1fm | %.2fV (%uS) | Link:%d%%",
            state_str,
            drone_view_label(local.view),
            (double)local.speed_kmh,
            (double)local.altitude,
            (double)local.voltage,
            (unsigned)local.cell_count,
            (int)(local.signal_strength * 100.0f));
        snprintf(
            line2,
            sizeof(line2),
            "Arm:%s | BF:%s",
            local.arming_disable_text[0] ? local.arming_disable_text : "READY",
            local.configurator_url[0] ? local.configurator_url : "off");
    }

    TextOutA(hdc, 10, 6, line1, (int)strlen(line1));
    SetTextColor(hdc, RGB(148, 163, 184));
    TextOutA(hdc, 10, 24, line2, (int)strlen(line2));

    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK drone_osd_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT:
        drone_osd_paint(hwnd);
        return 0;
    case WM_TIMER:
        if (wp == DRONE_OSD_TIMER) {
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        InterlockedExchange(&g_osd_ui.visible, 0);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, DRONE_OSD_TIMER);
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

static DWORD WINAPI drone_osd_thread(LPVOID param) {
    (void)param;
    HINSTANCE hinst = GetModuleHandleW(NULL);
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = drone_osd_wndproc;
    wc.hInstance = hinst;
    wc.lpszClassName = DRONE_OSD_CLASS;
    wc.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512)); /* IDC_ARROW */
    (void)RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        DRONE_OSD_CLASS,
        L"Fireball FPV Drone — Betaflight OSD",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        520,
        420,
        NULL,
        NULL,
        hinst,
        NULL);
    g_osd_ui.hwnd = hwnd;
    if (hwnd) {
        SetTimer(hwnd, DRONE_OSD_TIMER, 50, NULL);
        if (InterlockedCompareExchange(&g_osd_ui.visible, 0, 0)) {
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        }
    }
    if (g_osd_ui.ready_ev) {
        SetEvent(g_osd_ui.ready_ev);
    }
    if (!hwnd) {
        return 1;
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    g_osd_ui.hwnd = NULL;
    return 0;
}

int drone_osd_window_start(void) {
    if (InterlockedCompareExchange(&g_osd_ui.running, 0, 0)) {
        return 1;
    }
    wchar_t headless[16];
    if (GetEnvironmentVariableW(L"FIREBALL_HEADLESS", headless, (DWORD)(sizeof(headless) / sizeof(headless[0]))) > 0u &&
        headless[0] == L'1')
    {
        return 0;
    }
    if (!g_osd_ui.lock_init) {
        InitializeSRWLock(&g_osd_ui.lock);
        g_osd_ui.lock_init = 1;
    }
    g_osd_ui.ready_ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_osd_ui.ready_ev) {
        return 0;
    }
    InterlockedExchange(&g_osd_ui.visible, 1);
    g_osd_ui.thread = CreateThread(NULL, 0, drone_osd_thread, NULL, 0, NULL);
    if (!g_osd_ui.thread) {
        CloseHandle(g_osd_ui.ready_ev);
        g_osd_ui.ready_ev = NULL;
        return 0;
    }
    (void)WaitForSingleObject(g_osd_ui.ready_ev, 2000);
    CloseHandle(g_osd_ui.ready_ev);
    g_osd_ui.ready_ev = NULL;
    InterlockedExchange(&g_osd_ui.running, 1);
    return g_osd_ui.hwnd != NULL;
}

void drone_osd_window_stop(void) {
    if (!InterlockedCompareExchange(&g_osd_ui.running, 0, 0)) {
        return;
    }
    HWND h = g_osd_ui.hwnd;
    if (h) {
        PostMessageW(h, WM_DESTROY, 0, 0);
    }
    if (g_osd_ui.thread) {
        (void)WaitForSingleObject(g_osd_ui.thread, 2000);
        CloseHandle(g_osd_ui.thread);
        g_osd_ui.thread = NULL;
    }
    InterlockedExchange(&g_osd_ui.visible, 0);
    InterlockedExchange(&g_osd_ui.running, 0);
}

int drone_osd_window_is_open(void) {
    return InterlockedCompareExchange(&g_osd_ui.running, 0, 0) != 0 &&
           InterlockedCompareExchange(&g_osd_ui.visible, 0, 0) != 0;
}

void drone_osd_window_set_visible(int visible) {
    if (visible) {
        if (!InterlockedCompareExchange(&g_osd_ui.running, 0, 0)) {
            (void)drone_osd_window_start();
            return;
        }
        InterlockedExchange(&g_osd_ui.visible, 1);
        if (g_osd_ui.hwnd) {
            ShowWindow(g_osd_ui.hwnd, SW_SHOWNOACTIVATE);
        }
    } else {
        InterlockedExchange(&g_osd_ui.visible, 0);
        if (g_osd_ui.hwnd) {
            ShowWindow(g_osd_ui.hwnd, SW_HIDE);
        }
    }
}

void drone_osd_window_push_snapshot(const drone_osd_snapshot_t* snap) {
    if (!snap || !g_osd_ui.lock_init) {
        return;
    }
    AcquireSRWLockExclusive(&g_osd_ui.lock);
    g_osd_ui.snap = *snap;
    ReleaseSRWLockExclusive(&g_osd_ui.lock);
}
