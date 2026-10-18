#include "imgui_overlay.h"

#include "../common/config.h"
#include "../common/log.h"

#include <cmath>

namespace trlvr
{
    namespace
    {
        IDirect3DTexture9* g_target = nullptr;
        UINT g_width = 0, g_height = 0;
        bool g_cleared = false;      // this frame
        bool g_any = false;          // drawn on this frame
        RECT g_drawn{};
        bool g_reported = false;
    }

    ImguiState& imgui_state()
    {
        static ImguiState state;
        return state;
    }

    void imgui_state_reset()
    {
        imgui_state() = ImguiState{};
    }

    bool imgui_overlay_matches(const ImguiState& s)
    {
        if (!config().imgui_panel || s.vertex_shader ||
            s.fvf != (D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1))
            return false;
        // imgui_impl_dx9's projection: 2/(R-L), 2/(T-B) (T < B, so
        // negative), z 0.5, translation row, w 1.
        const D3DMATRIX& m = s.projection;
        return m._11 > 0.0f && m._22 < 0.0f && fabsf(m._33 - 0.5f) < 1e-3f &&
               m._34 == 0.0f && m._44 == 1.0f && m._12 == 0.0f &&
               m._21 == 0.0f;
    }

    HRESULT imgui_overlay_draw(IDirect3DDevice9* device,
                               const ImguiState& state,
                               const std::function<HRESULT()>& draw)
    {
        D3DVIEWPORT9 viewport{};
        if (!device || FAILED(device->GetViewport(&viewport)) ||
            viewport.Width < 16 || viewport.Height < 16)
            return draw();
        if (!g_target || g_width != viewport.Width ||
            g_height != viewport.Height)
        {
            if (g_target)
                g_target->Release();
            g_target = nullptr;
            if (FAILED(device->CreateTexture(viewport.Width, viewport.Height,
                    1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                    D3DPOOL_DEFAULT, &g_target, nullptr)) || !g_target)
            {
                g_target = nullptr;
                return draw();
            }
            g_width = viewport.Width;
            g_height = viewport.Height;
            g_cleared = false;
            log("imgui: another mod's ImGui menu found -- drawn into a "
                "%ux%u overlay and shown as a VR panel", g_width, g_height);
        }
        IDirect3DSurface9* surface = nullptr;
        IDirect3DSurface9* old_target = nullptr;
        IDirect3DSurface9* old_depth = nullptr;
        if (FAILED(g_target->GetSurfaceLevel(0, &surface)) || !surface)
            return draw();
        device->GetRenderTarget(0, &old_target);
        device->GetDepthStencilSurface(&old_depth);
        device->SetRenderTarget(0, surface);
        device->SetDepthStencilSurface(nullptr);
        device->SetViewport(&viewport);
        device->SetScissorRect(&state.scissor);
        if (!g_cleared)
        {
            device->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 0.0f, 0);
            g_cleared = true;
            g_any = false;
        }
        RECT r = state.scissor;
        if (r.left < 0) r.left = 0;
        if (r.top < 0) r.top = 0;
        if (r.right > (LONG)g_width) r.right = (LONG)g_width;
        if (r.bottom > (LONG)g_height) r.bottom = (LONG)g_height;
        // A clip covering (nearly) the whole window is ImGui's background
        // / foreground layer, not a menu window (user 2026-10-18: the
        // panel took the whole 3844x2135 window and the menu was out of
        // view); only window-sized clips make the panel's area.
        const bool whole = r.right - r.left >= (LONG)g_width * 9 / 10 &&
                           r.bottom - r.top >= (LONG)g_height * 9 / 10;
        if (!whole && r.right > r.left && r.bottom > r.top)
        {
            if (!g_any)
                g_drawn = r;
            else
            {
                if (r.left < g_drawn.left) g_drawn.left = r.left;
                if (r.top < g_drawn.top) g_drawn.top = r.top;
                if (r.right > g_drawn.right) g_drawn.right = r.right;
                if (r.bottom > g_drawn.bottom) g_drawn.bottom = r.bottom;
            }
            g_any = true;
        }
        const HRESULT hr = draw();
        device->SetRenderTarget(0, old_target);
        device->SetDepthStencilSurface(old_depth);
        device->SetViewport(&viewport);
        device->SetScissorRect(&state.scissor);
        if (old_target)
            old_target->Release();
        if (old_depth)
            old_depth->Release();
        surface->Release();
        if (!g_reported)
        {
            g_reported = true;
            log("imgui: first menu draw redirected (scissor %ld,%ld-%ld,%ld)",
                state.scissor.left, state.scissor.top, state.scissor.right,
                state.scissor.bottom);
        }
        return hr;
    }

    bool imgui_overlay_take(IDirect3DSurface9** surface, RECT* drawn)
    {
        const bool any = g_cleared && g_any && g_target;
        g_cleared = false;
        if (!any || !surface || !drawn)
            return false;
        *drawn = g_drawn;
        g_any = false;
        return SUCCEEDED(g_target->GetSurfaceLevel(0, surface)) && *surface;
    }

    void imgui_overlay_release()
    {
        if (g_target)
            g_target->Release();
        g_target = nullptr;
        g_width = g_height = 0;
        g_cleared = g_any = false;
    }
}
