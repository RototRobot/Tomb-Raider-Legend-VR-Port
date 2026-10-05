#pragma once
#include <d3d9.h>

namespace trlvr
{
    // Developer capture (F9): for about three seconds every texture bound at
    // stage 0 for an interface draw (ui_drawing_interface) is collected, then
    // each distinct one is written as a .dds file to <game>\trlvr_hud_dump\,
    // with a list of what was saved in trlvr.log. Used to find the game's
    // own gear-cross icons.
    void hud_capture_start();
    // From every draw call, on the render thread. True when the draw should
    // be skipped: in third-person immersive, an interface draw of one of
    // the game's gear-cross icons or its tile frame (the 3D cross replaces
    // them, whatever fade group or state shows them).
    bool hud_capture_note_draw(IDirect3DDevice9* device);
    // Once per presented frame, on the render thread.
    void hud_capture_frame();
    // F4 (skybox hunt, 2026-10-03): log every draw of the next frame (matrix
    // kind, depth state, shaders, texture) and dump the textures of the
    // non-perspective, non-interface draws.
    void hud_scene_capture_start();
    // The game code address that issued the next draw, and its primitive
    // count, for the F4 capture (decal/culling diagnosis 2026-10-04).
    void hud_capture_set_call(void* caller, unsigned primitives);
    // The draw call being made (its caller in the game and primitive count).
    void* hud_capture_current_call(unsigned* primitives);
    // The HUD drawable being drawn (ui_space draw hook): its texture page,
    // index count and class (0 SimpleDrawable, 1 HUDDrawable), so the F9
    // capture can list which drawable uses which texture. page 0 clears.
    void hud_capture_set_drawable(uintptr_t page, unsigned indices,
                                  int kind);

    // The game's own gear-cross icons (white on transparent, 64x64), found
    // by content in interface draws (FNV-1a of the pixels, from the F9
    // capture of 2026-10-01) and kept as the mod's own premultiplied copy.
    // Once seen they are cached to <game>\trlvr_gear_icons\ and loaded
    // from there in later sessions. Item: 0 medipack, 1 binoculars,
    // 2 weapon switch, 3 flashlight, 4 grenade. Null until available.
    IDirect3DTexture9* hud_gear_icon(IDirect3DDevice9* device, int item);
    // The game made a new device (in-game AA toggle): drop textures made on
    // the old one and forget the game's old texture pointers.
    void hud_capture_device_changed();

    // True while the game is drawing its binocular overlay (the 256x512
    // interface texture, FNV-1a CD443973 from the F9 capture): the
    // binocular view is open, whoever opened it.
    bool hud_binocular_view_active();
}
