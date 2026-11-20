#pragma once
// Camera-synced reflections kept still while the head turns (2026-11-12).
//
// Some of the game's shiny surfaces (Croft Manor's glass wall) are drawn
// with a vertex shader that reflects the view about the vertex normal and
// looks the result up in a 2D texture through two camera axes:
//   u = R . c[k] + 0.5, v = R . c[k+1] + 0.5
// with c[k], c[k+1] the camera's right and down axes in the object's space
// (shaders 22E7C570 / 22E7B8F0, frame probe 2026-11-12; c40/c41, eye in
// c44). The game camera carries the head's rotation, so turning the head
// slid the texture across the surface. For these shaders the two axes are
// replaced by the game's own camera's, without the head: the reflection
// then moves with the player and the game camera, not the head.

#include <d3d9.h>

namespace trlvr
{
    // From the proxy: shader creation and binding, and the game's constant
    // uploads (before any change).
    void env_reflect_shader_created(IDirect3DVertexShader9* shader,
                                    const DWORD* function);
    void env_reflect_shader_set(IDirect3DVertexShader9* shader);
    void env_reflect_constants(UINT start, const float* data, UINT count);

    // Around a draw: true if the axes were replaced, then
    // env_reflect_after_draw puts the game's back.
    bool env_reflect_before_draw(IDirect3DDevice9* device);
    void env_reflect_after_draw(IDirect3DDevice9* device);
}
