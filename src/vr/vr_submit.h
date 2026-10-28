#pragma once
// Handing finished frames to the headset.
//
// OpenXR has no Direct3D 9 binding. Our DXVK fork exports Direct3DCreateVR9,
// which gives the Vulkan device behind the D3D9 one -- what the OpenXR
// session is created on -- and copies the back buffer the game just drew
// into the runtime's swapchain image.
#include <d3d9.h>

namespace trlvr
{
    // Needs the *real* DXVK device, not our wrapper: Direct3DCreateVR9 casts
    // its argument straight to DXVK's own device type.
    bool vr_submit_init(IDirect3DDevice9* real_device);
    void vr_submit_shutdown();
    // Before the real device is released: the OpenXR session lives on its
    // Vulkan device and has to go first.
    void vr_submit_device_closing(IDirect3DDevice9* real_device);
    bool vr_submit_ready();

    // Call with the swap chain's back buffer, before presenting it. In stereo
    // this copies the finished frame into whichever eye it was drawn for and
    // submits both; in mono it submits the one image to both eyes.
    void vr_submit_frame(IDirect3DSurface9* back_buffer);

    // Blocks until the runtime wants the next frame (xrWaitFrame), begins it
    // and refreshes the poses.
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
    // The main hand points at another mod's ImGui menu panel: its trigger
    // is the menu's mouse button, not the game's.
    bool vr_submit_imgui_pointing();

    // The gamma ramp the game asks the device for (its brightness and
    // contrast sliders): a D3DGAMMARAMP. Windows ignores it in a window,
    // so the submit path applies it to the frame instead.
    void vr_submit_set_game_ramp(const void* ramp);
}
