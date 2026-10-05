#pragma once
// Working out how the interface actually reaches the GPU.
//
// It never touches c0 -- zero two-dimensional passes in any capture, gameplay
// or menus -- and SetTransform is never called, so it arrives already in
// screen or clip space. *Which* of those decides what can be done about it,
// and they need opposite fixes. See uiprobe.cpp.

#include <windows.h>
#include <d3d9.h>

namespace trlvr
{
    void ui_note_fvf(DWORD fvf);
    void ui_note_vertex_shader(const void* shader);
    void ui_note_declaration(bool bound);
    void ui_note_render_state(D3DRENDERSTATETYPE state, DWORD value);
    void ui_note_draw(UINT primitives);

    // Which draws the same-frame stereo path duplicates, and which it does
    // not. This is the discriminator the earlier guesses were reaching for:
    // a draw issued once while the scene target is bound is one the engine
    // gave no projection for, and the menu screenshot shows those are exactly
    // the interface. Grouped by pass count and viewport, because where a
    // single-pass draw lands depends entirely on the viewport it inherits.
    void ui_note_pass(int passes, IDirect3DDevice9* device);

    // Called once a frame. Reports on a timer, not per draw.
    void ui_report();

    // True when the draw about to be issued has depth testing and writing
    // both off. Measured to separate the interface cleanly: with no UI up,
    // the depth-off draws are exactly the pre-transformed post-processing
    // quads at two primitives each; when the HUD or a menu appears, a
    // handful of shader draws carrying several hundred primitives join them.
    bool ui_depth_disabled();

    // Capture one frame's draws, grouped by which vertex shader issued them.
    // Two discriminators for the interface have now been wrong -- it is not
    // pre-transformed, and depth state does not separate it -- so instead of
    // a third guess, take a census with the interface up and another with it
    // down. Whatever only appears in the first is the interface.
    void ui_capture_frame();
}
