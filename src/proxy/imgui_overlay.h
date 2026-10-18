#pragma once

// Other mods' Dear ImGui menus (TR7-Menu-Hook / TRLAU-menu-hook, user
// 2026-10-18) as a VR panel.
//
// imgui_impl_dx9 draws straight into the back buffer just before the game
// presents, at window pixels. In VR the back buffer is both eyes side by
// side at headset resolution, so the menu came out tiny in the top-left of
// the left eye. Its draws are recognised by ImGui's fixed-function state
// (no vertex shader, FVF XYZ | DIFFUSE | TEX1, an orthographic projection)
// and drawn into an overlay target of their own instead; the submit stage
// shows the part that was drawn on as a panel in both eyes.

#include <d3d9.h>
#include <functional>

namespace trlvr
{
    // ImGui's pipeline state, as last set through the proxy device.
    struct ImguiState
    {
        DWORD fvf = 0;
        bool vertex_shader = false;
        D3DMATRIX projection{};
        RECT scissor{};
    };

    // The tracked state. Reset at each present: ImGui restores the game's
    // state with a state block, which bypasses the proxy, and the game's
    // own state cache may then never re-send it.
    ImguiState& imgui_state();
    void imgui_state_reset();

    // Whether a draw with this state is an ImGui draw.
    bool imgui_overlay_matches(const ImguiState& state);

    // Draws into the overlay target instead of the current one (restoring
    // render target, depth and viewport after). Returns the draw's result.
    HRESULT imgui_overlay_draw(IDirect3DDevice9* device,
                               const ImguiState& state,
                               const std::function<HRESULT()>& draw);

    // For the submit stage, once a frame: the overlay surface and the
    // window-pixel rectangle drawn on this frame. False when ImGui drew
    // nothing. Resets for the next frame.
    bool imgui_overlay_take(IDirect3DSurface9** surface, RECT* drawn);

    // Device reset / shutdown.
    void imgui_overlay_release();
}
