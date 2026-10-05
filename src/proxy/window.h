#pragma once
// Making the game's window behave like a window.
//
// Setting D3DPRESENT_PARAMETERS::Windowed gives you a windowed *device*. It
// does nothing to the window, which the game creates as a borderless popup
// with WS_EX_TOPMOST covering the whole screen -- so it still sits over
// everything and you cannot alt-tab away from it.
//
// This gives it a caption and a border, sizes it, centres it, and takes the
// topmost bit off. The game re-asserts topmost from time to time, so there is
// also a cheap periodic check.

#include <windows.h>

namespace trlvr
{
    // Pick a client size: whatever the config pins, or about two thirds of the
    // monitor keeping the aspect the game asked for. The game asks for the
    // whole desktop, which is useless as a window.
    void window_choose_size(HWND hwnd, UINT asked_w, UINT asked_h,
                            UINT* out_w, UINT* out_h);

    // Restyle and reposition. Call with the client size you want. The window
    // is remembered, so window_tick needs no argument.
    void window_configure(HWND hwnd, UINT width, UINT height);

    // Called every frame; does real work only occasionally. Undoes a topmost
    // the game has re-applied, and releases the cursor when the window is not
    // in the foreground so alt-tab actually gets you out.
    void window_tick();

    // Before a synthesised click: if the cursor has left the (foreground)
    // game window, put it back inside, so the click cannot land on -- and
    // give focus to -- another window.
    void window_keep_cursor_inside();
}
