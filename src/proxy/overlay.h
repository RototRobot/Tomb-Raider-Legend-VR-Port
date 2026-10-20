#pragma once
// The settings overlay: a Dear ImGui window opened and closed with Insert,
// or in the headset by holding the controller menu button (vr_input.cpp,
// [vr] overlay_vr_open_hold). It edits the live values in config() and
// saves each change to trlvr.ini, so the settings it covers no longer need
// the game closed and the file edited.
//
// On the desktop mirror it is drawn onto the back buffer after the frame
// has gone to the headset. With VR up it is also laid out in a square
// texture (overlay_vr_texture) that vr_submit.cpp puts in the room as a
// panel. Mouse, keyboard and controllers go to it, not the game, while it
// is open.

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
    // Open or close it as Insert does. False if it is not up yet.
    bool overlay_request_open(bool open);
    // How many times it has opened (the panel is placed afresh each time).
    unsigned overlay_open_count();

    // The controllers as a gamepad for the overlay: buttons as XInput's
    // XINPUT_GAMEPAD_* bits, sticks -1..1 (x right, y up). Given once a frame
    // while it is open (vr_input.cpp); a frame without one reads as nothing
    // held.
    struct OverlayPad
    {
        unsigned short buttons;
        float lx, ly, rx, ry;
    };
    void overlay_set_pad(const OverlayPad& pad);

    // While it is open: builds this frame's window laid out for the headset
    // and renders it into a square, premultiplied-alpha texture, which it
    // returns (the overlay keeps it). The desktop mirror then reuses the
    // same frame. Null when closed or if the texture cannot be made.
    IDirect3DTexture9* overlay_vr_texture();
}
