#pragma once
// Handing finished frames to the compositor.
//
// OpenVR will not take a Direct3D 9 texture. Our DXVK fork exports
// Direct3DCreateVR9, which gives back the VkImage and the device/queue handles
// behind any D3D9 surface, so the back buffer the game just drew can go
// straight to the compositor with no copy.
//
// This stage submits the same image to both eyes -- mono. It exists to prove
// the interop, the Scene session and the compositor timing all work before
// stereo depends on them.

#include <d3d9.h>

namespace trlvr
{
    // Needs the *real* DXVK device, not our wrapper: Direct3DCreateVR9 casts
    // its argument straight to DXVK's own device type.
    bool vr_submit_init(IDirect3DDevice9* real_device);
    void vr_submit_shutdown();
    bool vr_submit_ready();

    // Call with the swap chain's back buffer, before presenting it. In stereo
    // this copies the finished frame into whichever eye it was drawn for and
    // submits both; in mono it submits the one image to both eyes.
    void vr_submit_frame(IDirect3DSurface9* back_buffer);

    // Blocks until the compositor wants the next frame, and refreshes poses.
    // This is what paces the game to the headset.
    void vr_submit_wait();
    // Counts one game draw call for the 5 s performance line.
    void vr_submit_note_draw();

    // Default-pool targets the submit path owns; released before a device
    // Reset (which fails while any exist) and re-made on demand.
    void vr_submit_release_targets();

    // A short message panel in front of the player for a few seconds, for
    // menu actions that cannot start (user 2026-10-06: a tester's holster
    // setup "did nothing" with no way to tell why).
    void vr_submit_notice(const wchar_t* title, const wchar_t* body);
}
