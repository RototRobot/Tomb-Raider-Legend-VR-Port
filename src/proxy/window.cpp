#include "window.h"

#include <intrin.h>

#include "../common/config.h"
#include "../common/log.h"

namespace trlvr
{
    namespace
    {
        unsigned g_tick = 0;
        bool g_cursor_released = false;
        HWND g_hwnd = nullptr;

        // Retail mouse-look recentre (window procedure, WM_MOUSEMOVE at
        // 0x004040FB): the game still believes it is fullscreen (the proxy
        // forces a window behind its back), so it measures the cursor from
        // the MONITOR centre in client coordinates (lParam - monitor size/2,
        // monitor rect from GetAdapterMonitor at 0x00EC6090) and then
        // SetCursorPos()es the monitor centre in SCREEN coordinates. With a
        // window elsewhere and of another size those are different points,
        // so every recentre left the same residual delta: the cursor crept
        // to the top-left, eventually outside the window, where a trigger
        // click took focus away (user report 2026-10-01). The game's call at
        // 0x0040425F (returning to 0x00404265) is redirected to the screen
        // position of that same client point.
        const uintptr_t kGameSetCursorPosIat = 0x00EFD210;
        const uintptr_t kRecentreReturn = 0x00404265;
        using PFN_SetCursorPos = BOOL(WINAPI*)(int, int);
        PFN_SetCursorPos g_real_set_cursor_pos = nullptr;
        bool g_cursor_fix_installed = false;

        BOOL WINAPI game_set_cursor_pos(int x, int y)
        {
            HWND hwnd = g_hwnd;
            if (_ReturnAddress() == reinterpret_cast<void*>(kRecentreReturn) &&
                hwnd && IsWindow(hwnd))
            {
                const POINT centre = { x, y };
                MONITORINFO mi{ sizeof(mi) };
                if (GetMonitorInfoW(MonitorFromPoint(centre,
                                        MONITOR_DEFAULTTONEAREST), &mi))
                {
                    POINT client = { x - mi.rcMonitor.left,
                                     y - mi.rcMonitor.top };
                    POINT screen = client;
                    if (ClientToScreen(hwnd, &screen))
                    {
                        static unsigned reports = 0;
                        if (reports++ < 2)
                            log("window: game mouse recentre (%d, %d) -> "
                                "client (%ld, %ld) at screen (%ld, %ld)",
                                x, y, client.x, client.y, screen.x,
                                screen.y);
                        return g_real_set_cursor_pos(screen.x, screen.y);
                    }
                }
            }
            return g_real_set_cursor_pos(x, y);
        }

        void install_cursor_fix()
        {
            if (g_cursor_fix_installed)
                return;
            g_cursor_fix_installed = true;
            HMODULE user32 = GetModuleHandleW(L"user32.dll");
            void* real = user32
                ? reinterpret_cast<void*>(GetProcAddress(user32,
                                                         "SetCursorPos"))
                : nullptr;
            void** slot = reinterpret_cast<void**>(kGameSetCursorPosIat);
            __try
            {
                if (!real || *slot != real)
                {
                    log("window: game SetCursorPos import not as expected "
                        "(%p vs %p); mouse recentre fix not installed",
                        *slot, real);
                    return;
                }
                DWORD old = 0;
                if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old))
                    return;
                g_real_set_cursor_pos = reinterpret_cast<PFN_SetCursorPos>(real);
                *slot = reinterpret_cast<void*>(&game_set_cursor_pos);
                VirtualProtect(slot, sizeof(void*), old, &old);
                log("window: game mouse recentre redirected to the window's "
                    "own client centre (SetCursorPos import patched)");
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                log("window: mouse recentre fix failed to install");
            }
        }

        DWORD wanted_style(bool borderless)
        {
            // WS_POPUP is what makes it fill the screen with no frame; drop it.
            return borderless ? (WS_POPUP | WS_VISIBLE)
                              : (WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        }

        void centre(HWND hwnd, DWORD style, UINT width, UINT height)
        {
            RECT r = { 0, 0, (LONG)width, (LONG)height };
            AdjustWindowRect(&r, style, FALSE);
            const int ww = r.right - r.left;
            const int wh = r.bottom - r.top;

            MONITORINFO mi{ sizeof(mi) };
            HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            int x = 0, y = 0;
            if (GetMonitorInfoW(mon, &mi))
            {
                const int mw = mi.rcWork.right - mi.rcWork.left;
                const int mh = mi.rcWork.bottom - mi.rcWork.top;
                x = mi.rcWork.left + (mw - ww) / 2;
                y = mi.rcWork.top + (mh - wh) / 2;
                if (x < mi.rcWork.left) x = mi.rcWork.left;
                if (y < mi.rcWork.top)  y = mi.rcWork.top;
            }

            SetWindowPos(hwnd, HWND_NOTOPMOST, x, y, ww, wh,
                         SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOACTIVATE);
        }
    }

    void window_choose_size(HWND hwnd, UINT asked_w, UINT asked_h,
                            UINT* out_w, UINT* out_h)
    {
        if (!out_w || !out_h)
            return;

        if (config().window_width > 0 && config().window_height > 0)
        {
            *out_w = (UINT)config().window_width;
            *out_h = (UINT)config().window_height;
            return;
        }

        *out_w = asked_w;
        *out_h = asked_h;
        if (!asked_w || !asked_h)
            return;

        MONITORINFO mi{ sizeof(mi) };
        HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
        if (!GetMonitorInfoW(mon, &mi))
            return;

        const int work_w = mi.rcWork.right - mi.rcWork.left;
        const int work_h = mi.rcWork.bottom - mi.rcWork.top;

        // Two thirds of the work area, capped at 1080p. Without the cap a 4K
        // desktop gives a window so large it is fullscreen in all but name,
        // which is the thing we are trying to get away from. On a 1080p
        // desktop the cap never binds and two thirds is what you get.
        const double kMaxW = 1920.0, kMaxH = 1080.0;

        double w = work_w * 2.0 / 3.0;
        if (w > kMaxW)
            w = kMaxW;
        double h = w * (double)asked_h / (double)asked_w;

        double max_h = work_h * 2.0 / 3.0;
        if (max_h > kMaxH)
            max_h = kMaxH;
        if (h > max_h)
        {
            h = max_h;
            w = h * (double)asked_w / (double)asked_h;
        }

        *out_w = (UINT)((long)w & ~1L);      // even, some drivers dislike odd
        *out_h = (UINT)((long)h & ~1L);
    }

    void window_configure(HWND hwnd, UINT width, UINT height)
    {
        if (!hwnd || !IsWindow(hwnd))
            return;
        g_hwnd = hwnd;

        const bool borderless = config().window_borderless;
        const DWORD style = wanted_style(borderless);

        DWORD ex = (DWORD)GetWindowLongW(hwnd, GWL_EXSTYLE);
        ex &= ~WS_EX_TOPMOST;

        SetWindowLongW(hwnd, GWL_STYLE, (LONG)style);
        SetWindowLongW(hwnd, GWL_EXSTYLE, (LONG)ex);

        // It can arrive minimised, in which case sizing it does nothing visible.
        if (IsIconic(hwnd))
            ShowWindow(hwnd, SW_RESTORE);

        centre(hwnd, style, width, height);
        SetForegroundWindow(hwnd);

        log("window: %ux%u client, %s, topmost cleared",
            width, height, borderless ? "borderless" : "bordered");
        install_cursor_fix();
    }

    void window_keep_cursor_inside()
    {
        HWND hwnd = g_hwnd;
        if (!hwnd || !IsWindow(hwnd) || GetForegroundWindow() != hwnd)
            return;
        POINT cursor{};
        RECT client{};
        if (!GetCursorPos(&cursor) || !GetClientRect(hwnd, &client))
            return;
        POINT origin = { 0, 0 };
        ClientToScreen(hwnd, &origin);
        if (cursor.x >= origin.x && cursor.y >= origin.y &&
            cursor.x < origin.x + client.right &&
            cursor.y < origin.y + client.bottom)
            return;
        POINT centre = { client.right / 2, client.bottom / 2 };
        ClientToScreen(hwnd, &centre);
        // Keep the target on a real monitor even if the window is larger.
        MONITORINFO mi{ sizeof(mi) };
        if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST),
                            &mi))
        {
            if (centre.x >= mi.rcMonitor.right) centre.x = mi.rcMonitor.right - 1;
            if (centre.y >= mi.rcMonitor.bottom) centre.y = mi.rcMonitor.bottom - 1;
            if (centre.x < mi.rcMonitor.left) centre.x = mi.rcMonitor.left;
            if (centre.y < mi.rcMonitor.top) centre.y = mi.rcMonitor.top;
        }
        const PFN_SetCursorPos set = g_real_set_cursor_pos
            ? g_real_set_cursor_pos : &SetCursorPos;
        set(centre.x, centre.y);
        static unsigned reports = 0;
        if (reports++ < 6)
            log("window: cursor was outside the game window at (%ld, %ld); "
                "moved inside before a click", cursor.x, cursor.y);
    }

    void window_tick()
    {
        HWND hwnd = g_hwnd;
        if (!hwnd || !IsWindow(hwnd))
            return;

        // Once every couple of seconds is plenty, and it keeps this off the
        // hot path.
        if ((++g_tick % 120) != 0)
            return;

        const DWORD ex = (DWORD)GetWindowLongW(hwnd, GWL_EXSTYLE);
        if (ex & WS_EX_TOPMOST)
        {
            SetWindowLongW(hwnd, GWL_EXSTYLE, (LONG)(ex & ~WS_EX_TOPMOST));
            SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }

        // The game clips the cursor to its window. Left alone, that follows you
        // out of the game when you alt-tab.
        if (config().release_cursor_unfocused)
        {
            const bool focused = (GetForegroundWindow() == hwnd);
            if (!focused && !g_cursor_released)
            {
                ClipCursor(nullptr);
                g_cursor_released = true;
            }
            else if (focused && g_cursor_released)
            {
                g_cursor_released = false;
            }
        }
    }
}
