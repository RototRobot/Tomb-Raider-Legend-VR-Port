#pragma once
// Same-frame stereo: both eyes drawn in the same frame, in step.
//
// Alternating eyes gave real depth cheaply, but each eye only refreshes at
// half the frame rate and the two are taken a frame apart. See stereo.cpp.

#include <windows.h>
#include <d3d9.h>

namespace trlvr
{
    // On, and everything it needs is in place.
    bool stereo_same_frame_active();

    // The size of one eye's half of the target. The surface itself is twice
    // this wide.
    void stereo_set_eye_size(unsigned width, unsigned height);
    unsigned stereo_eye_width();
    unsigned stereo_eye_height();

    // Double the width of a surface the game is asking for, if it is one the
    // scene is drawn into.
    void stereo_widen(UINT* width, UINT height);

    // How many times the draw about to be issued should be sent: two while the
    // scene is being drawn, one otherwise.
    int stereo_passes();

    // Point the next draw at one eye's half of the target.
    void stereo_begin_pass(IDirect3DDevice9* device, int pass);

    // The passes leave the viewport and c0..c3 as the last eye set them; the
    // game's own values are put back only when something needs them. Call
    // this before anything that reads or depends on the device's viewport or
    // c0..c3 (a draw that is not stereo, a Get*, Clear, a target change,
    // Present, mod drawing on the real device).
    void stereo_restore_game_state(IDirect3DDevice9* device);

    // The game itself set the viewport / vertex constants [start, start +
    // count): its value now stands, so there is nothing of ours to restore.
    void stereo_note_game_viewport();
    void stereo_note_game_constants(UINT start, UINT count);

    // Something other than begin_pass changed the viewport after it.
    void stereo_note_viewport_modified();
}
