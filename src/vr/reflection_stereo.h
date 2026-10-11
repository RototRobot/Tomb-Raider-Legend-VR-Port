#pragma once
// Stereo planar water reflection (2026-10-11).
//
// The game renders its water reflection with a mirrored copy of its camera
// into a half-size target (created at 0x00414838), during the previous-gen
// end scene (0x00415815..0x0041594D), and the water samples it by screen
// position. One image for both eyes cannot match either, so the reflection
// was switched off (FINDINGS 40-42). Here each reflection draw is issued
// twice: the left eye into the game's target, the right eye into a twin
// target with its own depth buffer, each with that eye's camera (the c0
// substitution works in the camera's own view space, so a mirrored camera
// gets the mirrored eye). In the main scene the right eye's draws sample the
// twin wherever the game bound the reflection.

#include <d3d9.h>

namespace trlvr
{
    // Re-enables the reflection pass (when [vr] water_reflection = 1) and
    // marks it. Returns false if it left the reflection suppressed.
    bool reflection_stereo_init();

    // Inside the reflection pass, with its target and twin ready.
    bool reflection_stereo_pass_active();

    // Device tracking, from the proxy (real device).
    void reflection_stereo_note_render_target(IDirect3DDevice9* device,
                                              DWORD index,
                                              IDirect3DSurface9* surface);
    void reflection_stereo_note_depth(IDirect3DSurface9* surface);
    void reflection_stereo_note_texture(DWORD stage,
                                        IDirect3DBaseTexture9* texture);

    // The reflection draw: true if this draw goes to the reflection target
    // (both eyes are then issued by the caller through the bind calls).
    bool reflection_stereo_draw_here(IDirect3DDevice9* device);
    // Bind the twin (right eye) target and depth, or the game's again.
    void reflection_stereo_bind_twin(IDirect3DDevice9* device, bool twin);
    // After a Clear on the reflection target: clear the twin the same way.
    void reflection_stereo_after_clear(IDirect3DDevice9* device, DWORD count,
                                       const D3DRECT* rects, DWORD flags,
                                       D3DCOLOR colour, float z,
                                       DWORD stencil);

    // Main scene, per eye: the right eye samples the twin.
    void reflection_stereo_eye_textures(IDirect3DDevice9* device, int eye);
    void reflection_stereo_restore_textures(IDirect3DDevice9* device);

    // Water Effects (2026-10-11): the game copies the whole screen into a
    // texture (0x4FE, by StretchRect at 0x00414170) and draws its water
    // distortion grid from it with 0..1 coordinates. In stereo the screen
    // holds both eyes side by side, so each eye showed both squeezed. Each
    // such copy is split into one texture per eye, and each eye's pass
    // samples its own half.
    void reflection_stereo_note_copy(IDirect3DDevice9* device,
                                     IDirect3DSurface9* destination,
                                     const RECT* source_rect,
                                     const RECT* destination_rect);

    // Diagnosis (2026-10-11, user test: the water effect showed in the
    // right eye only and moved with the head). Shift+F7 cycles the
    // reflection: per eye / one image (retail) / not drawn. Shift+F8 cycles
    // the screen copy: split by eye / split, eyes swapped / whole (retail).
    void reflection_stereo_cycle_mode(bool copy);
    int reflection_stereo_reflection_mode();
    // Once per kind of draw: what samples the reflection or the copy.
    void reflection_stereo_note_draw(IDirect3DDevice9* device, bool stereo);
    // A reflection draw kept the game's one camera.
    void reflection_stereo_note_unsubstituted();

    // Device going away or resetting: drop the twin resources.
    void reflection_stereo_release();
}
