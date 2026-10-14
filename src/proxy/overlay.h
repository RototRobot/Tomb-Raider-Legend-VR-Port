#pragma once
// The settings overlay: a Dear ImGui window on the desktop mirror, opened
// and closed with Insert. It edits the live values in config() and saves
// each change to trlvr.ini, so the settings it covers no longer need the
// game closed and the file edited.
//
// It is drawn onto the back buffer after the frame has gone to the headset,
// so it is on the monitor only. Mouse and keyboard go to it, not the game,
// while it is open.

#include <d3d9.h>

namespace trlvr
{
    // Once a frame, just before the real Present.
    void overlay_frame(IDirect3DSwapChain9* chain);
    // The overlay's device objects do not survive a Reset.
    void overlay_before_reset();
    // It holds a reference on the device; drop it before the device goes.
    void overlay_device_closing(IDirect3DDevice9* real_device);
    bool overlay_open();
}
