#pragma once
// One-frame GPU profile (F3, 2026-10-05). The water area ran at 20-30 ms of
// GPU time per frame with 8x and with 2x MSAA alike, so something else is
// expensive there. F3 times the next frame with GPU timestamp queries,
// split wherever the render target, the drawing code or the blend state
// changes, and around every copy (StretchRect) and clear, then logs the
// costliest groups: "gpu profile: ...".

#include <d3d9.h>

namespace trlvr
{
    void gpu_profile_request();

    // The device every timestamp is issued on (the real one, not the proxy).
    void gpu_profile_set_device(IDirect3DDevice9* device);

    // Per draw, from the proxy's dispatch_draw (cheap when not profiling).
    void gpu_profile_draw(void* caller, unsigned primitives);

    // Render target 0 changed.
    void gpu_profile_render_target(IDirect3DSurface9* surface);

    // A copy or clear: begin before the call, end after it.
    void gpu_profile_event_begin(const char* what, IDirect3DSurface9* source,
                                 IDirect3DSurface9* destination);
    void gpu_profile_event_end();

    // Mod work at the frame's end (VR submit: resolve, overlays, hands).
    void gpu_profile_mark(const char* what);

    // The frame boundary: starts a requested capture, closes the one being
    // recorded, and collects finished results.
    void gpu_profile_frame_end();
}
