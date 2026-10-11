#include "reflection_stereo.h"
#include "stereo.h"

#include "../common/config.h"
#include "../common/log.h"

#include <windows.h>
#include <cstdint>
#include <cstring>

namespace trlvr
{
    namespace
    {
        // PCDeviceManager's colour-target bind (thiscall, one argument, the
        // target object). The previous-gen end scene binds the reflection
        // target with it at 0x0041581E and the main target again at
        // 0x00415933; everything drawn between is the reflection.
        const uintptr_t kSetTarget = 0x00EC5870;
        const uintptr_t kBeginCall = 0x0041581E;
        const uintptr_t kEndCall = 0x00415933;
        // The gates that FINDINGS 41 forced shut (left as the game has them
        // when the reflection is on).
        const uintptr_t kBeginSceneGate = 0x0041A2E9;
        const uintptr_t kEndSceneGate = 0x0041580F;

        typedef void (__fastcall* SetTargetFn)(void*, void*, void*);

        bool g_enabled = false;
        bool g_in_pass = false;
        unsigned g_passes = 0;

        IDirect3DDevice9* g_device = nullptr;     // owns the twins
        // The game's reflection target, texture and depth -- compared only,
        // never held.
        IDirect3DSurface9* g_refl_surface = nullptr;
        IDirect3DTexture9* g_refl_texture = nullptr;
        IDirect3DSurface9* g_refl_depth = nullptr;
        IDirect3DSurface9* g_current_rt = nullptr;
        IDirect3DSurface9* g_current_ds = nullptr;
        // The right eye's.
        IDirect3DTexture9* g_twin = nullptr;
        IDirect3DSurface9* g_twin_surface = nullptr;
        IDirect3DSurface9* g_twin_depth = nullptr;
        UINT g_twin_w = 0, g_twin_h = 0;
        UINT g_twin_depth_w = 0, g_twin_depth_h = 0;

        IDirect3DBaseTexture9* g_bound[16]{};
        bool g_swapped[16]{};
        int g_refl_mode = 0;   // 0 per eye, 1 one image, 2 not drawn
        int g_copy_mode = 0;   // 0 per eye, 1 eyes swapped, 2 whole

        // The whole-screen copy (both eyes side by side) and its halves.
        IDirect3DTexture9* g_copy = nullptr;           // compared only
        IDirect3DSurface9* g_copy_surface = nullptr;   // compared only
        bool g_copy_dirty = false;
        IDirect3DTexture9* g_half[2] = { nullptr, nullptr };
        IDirect3DSurface9* g_half_surface[2] = { nullptr, nullptr };
        UINT g_half_w = 0, g_half_h = 0;
        D3DFORMAT g_half_format = D3DFMT_UNKNOWN;

        void release_halves()
        {
            for (int e = 0; e < 2; ++e)
            {
                if (g_half_surface[e])
                    g_half_surface[e]->Release();
                if (g_half[e])
                    g_half[e]->Release();
                g_half_surface[e] = nullptr;
                g_half[e] = nullptr;
            }
            g_half_w = g_half_h = 0;
            g_half_format = D3DFMT_UNKNOWN;
        }

        // The copy's halves, refreshed if it was written since.
        bool split_copy(IDirect3DDevice9* device)
        {
            if (!g_copy || !g_copy_surface)
                return false;
            D3DSURFACE_DESC desc{};
            if (FAILED(g_copy_surface->GetDesc(&desc)) || desc.Width < 2)
                return false;
            const UINT w = desc.Width / 2, h = desc.Height;
            if (!g_half[0] || w != g_half_w || h != g_half_h ||
                desc.Format != g_half_format)
            {
                release_halves();
                for (int e = 0; e < 2; ++e)
                {
                    const HRESULT hr = device->CreateTexture(
                        w, h, 1, D3DUSAGE_RENDERTARGET, desc.Format,
                        D3DPOOL_DEFAULT, &g_half[e], nullptr);
                    if (FAILED(hr) || !g_half[e] ||
                        FAILED(g_half[e]->GetSurfaceLevel(
                            0, &g_half_surface[e])))
                    {
                        log("water effects: could not create the eye copies "
                            "(%ux%u, hr 0x%08X)", w, h, (unsigned)hr);
                        release_halves();
                        return false;
                    }
                }
                g_half_w = w;
                g_half_h = h;
                g_half_format = desc.Format;
                g_copy_dirty = true;
                log("water effects: screen copy %ux%u split into two %ux%u "
                    "eye copies", desc.Width, desc.Height, w, h);
            }
            if (g_copy_dirty)
            {
                for (int e = 0; e < 2; ++e)
                {
                    const RECT half = { (LONG)(e * w), 0, (LONG)((e + 1) * w),
                                        (LONG)h };
                    device->StretchRect(g_copy_surface, &half,
                                        g_half_surface[e], nullptr,
                                        D3DTEXF_POINT);
                }
                g_copy_dirty = false;
            }
            return true;
        }

        void release_twin_colour()
        {
            if (g_twin_surface)
                g_twin_surface->Release();
            if (g_twin)
                g_twin->Release();
            g_twin_surface = nullptr;
            g_twin = nullptr;
            g_twin_w = g_twin_h = 0;
        }

        void release_twin_depth()
        {
            if (g_twin_depth)
                g_twin_depth->Release();
            g_twin_depth = nullptr;
            g_twin_depth_w = g_twin_depth_h = 0;
        }

        void __fastcall begin_thunk(void* self, void*, void* target)
        {
            if (!g_passes++)
                log("water reflection: reflection pass running (right eye "
                    "drawn into a twin target)");
            g_in_pass = true;
            reinterpret_cast<SetTargetFn>(kSetTarget)(self, nullptr, target);
        }

        void __fastcall end_thunk(void* self, void*, void* target)
        {
            g_in_pass = false;
            reinterpret_cast<SetTargetFn>(kSetTarget)(self, nullptr, target);
        }

        bool redirect_call(uintptr_t at, void* to)
        {
            unsigned char* p = reinterpret_cast<unsigned char*>(at);
            if (p[0] != 0xE8)
                return false;
            const intptr_t target = (intptr_t)at + 5 +
                *reinterpret_cast<const int32_t*>(p + 1);
            if ((uintptr_t)target != kSetTarget)
                return false;
            DWORD old = 0;
            if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old))
                return false;
            const int32_t rel = (int32_t)((intptr_t)to - (intptr_t)at - 5);
            memcpy(p + 1, &rel, 4);
            VirtualProtect(p, 5, old, &old);
            FlushInstructionCache(GetCurrentProcess(), p, 5);
            return true;
        }

        bool call_targets_set_target(uintptr_t at)
        {
            const unsigned char* p = reinterpret_cast<const unsigned char*>(at);
            return p[0] == 0xE8 &&
                (uintptr_t)((intptr_t)at + 5 +
                            *reinterpret_cast<const int32_t*>(p + 1)) ==
                    kSetTarget;
        }
    }

    bool reflection_stereo_init()
    {
        if (!config().water_reflection)
        {
            log("water reflection: off ([vr] water_reflection = 0); the "
                "game's reflection stays suppressed");
            return false;
        }
        const unsigned char begin_gate[] = { 0x0F, 0x84, 0x15, 0x01, 0x00, 0x00 };
        const unsigned char end_gate[] = { 0x0F, 0x84, 0x38, 0x01, 0x00, 0x00 };
        if (memcmp((const void*)kBeginSceneGate, begin_gate,
                   sizeof(begin_gate)) != 0 ||
            memcmp((const void*)kEndSceneGate, end_gate, sizeof(end_gate)) != 0 ||
            !call_targets_set_target(kBeginCall) ||
            !call_targets_set_target(kEndCall))
        {
            log("water reflection: executable signature mismatch -- the "
                "reflection stays suppressed");
            return false;
        }
        if (!redirect_call(kBeginCall, (void*)&begin_thunk))
        {
            log("water reflection: could not mark the reflection pass -- "
                "the reflection stays suppressed");
            return false;
        }
        if (!redirect_call(kEndCall, (void*)&end_thunk))
        {
            // Put the first one back: a pass that never ends is worse.
            redirect_call(kBeginCall, (void*)kSetTarget);
            log("water reflection: could not mark the end of the reflection "
                "pass -- the reflection stays suppressed");
            return false;
        }
        g_enabled = true;
        log("water reflection: on -- the game's planar reflection is drawn "
            "per eye (needs Reflections on in the game's Setup dialog)");
        return true;
    }

    bool reflection_stereo_pass_active()
    {
        return g_enabled && g_in_pass;
    }

    void reflection_stereo_note_render_target(IDirect3DDevice9* device,
                                              DWORD index,
                                              IDirect3DSurface9* surface)
    {
        if (!g_enabled || index != 0)
            return;
        g_current_rt = surface;
        if (!g_in_pass || !surface || !device)
            return;
        if (device != g_device)
        {
            reflection_stereo_release();
            g_device = device;
        }
        D3DSURFACE_DESC desc{};
        if (FAILED(surface->GetDesc(&desc)))
            return;
        if (surface == g_refl_surface && g_twin_surface &&
            desc.Width == g_twin_w && desc.Height == g_twin_h)
            return;
        g_refl_surface = surface;
        g_refl_texture = nullptr;
        IDirect3DTexture9* texture = nullptr;
        if (SUCCEEDED(surface->GetContainer(IID_IDirect3DTexture9,
                                            (void**)&texture)) && texture)
        {
            g_refl_texture = texture;
            texture->Release();
        }
        release_twin_colour();
        if (!g_refl_texture)
        {
            log("water reflection: the reflection target is not a texture "
                "(%ux%u) -- right eye not drawn", desc.Width, desc.Height);
            return;
        }
        const HRESULT hr = device->CreateTexture(
            desc.Width, desc.Height, g_refl_texture->GetLevelCount(),
            desc.Usage, desc.Format, D3DPOOL_DEFAULT, &g_twin, nullptr);
        if (FAILED(hr) || !g_twin ||
            FAILED(g_twin->GetSurfaceLevel(0, &g_twin_surface)))
        {
            log("water reflection: could not create the right eye's target "
                "(%ux%u, format %u, hr 0x%08X)", desc.Width, desc.Height,
                (unsigned)desc.Format, (unsigned)hr);
            release_twin_colour();
            return;
        }
        g_twin_w = desc.Width;
        g_twin_h = desc.Height;
        log("water reflection: target %ux%u (format %u, %u levels); right "
            "eye twin created", desc.Width, desc.Height,
            (unsigned)desc.Format, g_refl_texture->GetLevelCount());
    }

    void reflection_stereo_note_depth(IDirect3DSurface9* surface)
    {
        if (!g_enabled)
            return;
        g_current_ds = surface;
        if (!g_in_pass || !surface || !g_device)
            return;
        D3DSURFACE_DESC desc{};
        if (FAILED(surface->GetDesc(&desc)))
            return;
        if (surface == g_refl_depth && g_twin_depth &&
            desc.Width == g_twin_depth_w && desc.Height == g_twin_depth_h)
            return;
        g_refl_depth = surface;
        release_twin_depth();
        const HRESULT hr = g_device->CreateDepthStencilSurface(
            desc.Width, desc.Height, desc.Format, desc.MultiSampleType,
            desc.MultiSampleQuality, FALSE, &g_twin_depth, nullptr);
        if (FAILED(hr) || !g_twin_depth)
        {
            log("water reflection: could not create the right eye's depth "
                "(%ux%u, hr 0x%08X)", desc.Width, desc.Height, (unsigned)hr);
            release_twin_depth();
            return;
        }
        g_twin_depth_w = desc.Width;
        g_twin_depth_h = desc.Height;
        log("water reflection: depth %ux%u (format %u); right eye twin "
            "created", desc.Width, desc.Height, (unsigned)desc.Format);
    }

    void reflection_stereo_note_texture(DWORD stage,
                                        IDirect3DBaseTexture9* texture)
    {
        if (stage < 16)
        {
            g_bound[stage] = texture;
            g_swapped[stage] = false;
        }
    }

    bool reflection_stereo_draw_here(IDirect3DDevice9* device)
    {
        return g_enabled && g_in_pass && device == g_device &&
            g_refl_surface && g_current_rt == g_refl_surface &&
            g_twin_surface &&
            (!g_refl_depth || g_current_ds != g_refl_depth || g_twin_depth);
    }

    void reflection_stereo_bind_twin(IDirect3DDevice9* device, bool twin)
    {
        if (!device || !g_twin_surface)
            return;
        device->SetRenderTarget(0, twin ? g_twin_surface : g_refl_surface);
        if (g_current_ds && g_current_ds == g_refl_depth && g_twin_depth)
            device->SetDepthStencilSurface(twin ? g_twin_depth : g_refl_depth);
    }

    void reflection_stereo_after_clear(IDirect3DDevice9* device, DWORD count,
                                       const D3DRECT* rects, DWORD flags,
                                       D3DCOLOR colour, float z,
                                       DWORD stencil)
    {
        if (!reflection_stereo_draw_here(device))
            return;
        D3DVIEWPORT9 viewport{};
        device->GetViewport(&viewport);
        reflection_stereo_bind_twin(device, true);
        device->SetViewport(&viewport);
        device->Clear(count, rects, flags, colour, z, stencil);
        reflection_stereo_bind_twin(device, false);
        device->SetViewport(&viewport);
    }

    void reflection_stereo_note_copy(IDirect3DDevice9* device,
                                     IDirect3DSurface9* destination,
                                     const RECT* source_rect,
                                     const RECT* destination_rect)
    {
        if (!config().water_effects || !destination || !device ||
            source_rect || destination_rect || !stereo_same_frame_active())
            return;
        if (destination == g_copy_surface)
        {
            g_copy_dirty = true;
            return;
        }
        D3DSURFACE_DESC desc{};
        if (FAILED(destination->GetDesc(&desc)) ||
            desc.Width != stereo_eye_width() * 2 ||
            desc.Height != stereo_eye_height())
            return;
        IDirect3DTexture9* texture = nullptr;
        if (FAILED(destination->GetContainer(IID_IDirect3DTexture9,
                                             (void**)&texture)) || !texture)
            return;
        texture->Release();
        if (device != g_device)
        {
            reflection_stereo_release();
            g_device = device;
        }
        g_copy = texture;
        g_copy_surface = destination;
        g_copy_dirty = true;
        log("water effects: whole-screen copy %ux%u found; each eye samples "
            "its own half", desc.Width, desc.Height);
    }

    void reflection_stereo_eye_textures(IDirect3DDevice9* device, int eye)
    {
        if (!device || device != g_device)
            return;
        if (g_copy)
        {
            bool split = false, tried = false;
            for (DWORD s = 0; s < 16; ++s)
            {
                if (g_bound[s] != (IDirect3DBaseTexture9*)g_copy)
                    continue;
                if (!tried)
                {
                    tried = true;
                    split = split_copy(device);
                }
                if (!split || g_copy_mode == 2)
                    break;
                const int half = (eye == 1) != (g_copy_mode == 1) ? 1 : 0;
                device->SetTexture(s, g_half[half]);
                g_swapped[s] = true;
            }
        }
        if (!g_enabled || !g_twin || !g_refl_texture || g_refl_mode != 0)
            return;
        for (DWORD s = 0; s < 16; ++s)
        {
            if (g_bound[s] != (IDirect3DBaseTexture9*)g_refl_texture)
                continue;
            device->SetTexture(s, eye == 1
                ? (IDirect3DBaseTexture9*)g_twin
                : (IDirect3DBaseTexture9*)g_refl_texture);
            g_swapped[s] = eye == 1;
        }
    }

    void reflection_stereo_restore_textures(IDirect3DDevice9* device)
    {
        if (!device)
            return;
        for (DWORD s = 0; s < 16; ++s)
            if (g_swapped[s])
            {
                device->SetTexture(s, g_bound[s]);
                g_swapped[s] = false;
            }
    }

    void reflection_stereo_cycle_mode(bool copy)
    {
        if (copy)
        {
            g_copy_mode = (g_copy_mode + 1) % 3;
            static const char* const kNames[3] = {
                "split by eye", "split, eyes SWAPPED", "whole (retail)" };
            log("water effects: Shift+F8 -- screen copy %s",
                kNames[g_copy_mode]);
        }
        else
        {
            g_refl_mode = (g_refl_mode + 1) % 3;
            static const char* const kNames[3] = {
                "per eye", "one image for both eyes (retail)", "NOT drawn" };
            log("water reflection: Shift+F7 -- reflection %s",
                kNames[g_refl_mode]);
        }
    }

    int reflection_stereo_reflection_mode()
    {
        return g_refl_mode;
    }

    void reflection_stereo_note_unsubstituted()
    {
        static unsigned count = 0;
        if (count++ < 4)
            log("water reflection: a reflection draw kept the game's single "
                "camera (no per-eye substitution) -- %u so far", count);
    }

    void reflection_stereo_note_draw(IDirect3DDevice9* device, bool stereo)
    {
        if (!device || device != g_device)
            return;
        int which = -1;
        DWORD stage = 0;
        for (DWORD s = 0; s < 16 && which < 0; ++s)
        {
            if (g_refl_texture &&
                g_bound[s] == (IDirect3DBaseTexture9*)g_refl_texture)
                which = 0, stage = s;
            else if (g_copy && g_bound[s] == (IDirect3DBaseTexture9*)g_copy)
                which = 1, stage = s;
        }
        if (which < 0)
            return;
        IDirect3DVertexShader9* vs = nullptr;
        IDirect3DPixelShader9* ps = nullptr;
        device->GetVertexShader(&vs);
        device->GetPixelShader(&ps);
        if (vs)
            vs->Release();
        if (ps)
            ps->Release();
        UINT rt_w = 0, rt_h = 0;
        IDirect3DSurface9* rt = nullptr;
        if (SUCCEEDED(device->GetRenderTarget(0, &rt)) && rt)
        {
            D3DSURFACE_DESC desc{};
            if (SUCCEEDED(rt->GetDesc(&desc)))
            {
                rt_w = desc.Width;
                rt_h = desc.Height;
            }
            rt->Release();
        }
        struct Seen { void* vs; void* ps; UINT w; int which; bool stereo; };
        static Seen seen[24];
        static int seen_count = 0;
        for (int i = 0; i < seen_count; ++i)
            if (seen[i].vs == vs && seen[i].ps == ps && seen[i].w == rt_w &&
                seen[i].which == which && seen[i].stereo == stereo)
                return;
        if (seen_count >= 24)
            return;
        seen[seen_count++] = { vs, ps, rt_w, which, stereo };
        D3DVIEWPORT9 vp{};
        device->GetViewport(&vp);
        float c[16 * 4]{};
        device->GetVertexShaderConstantF(4, c, 16);
        log("water draw: samples the %s at stage %u, %s, target %ux%u, "
            "viewport %u,%u %ux%u, VS %p PS %p",
            which ? "SCREEN COPY" : "REFLECTION", (unsigned)stage,
            stereo ? "both eyes" : "ONE pass (not per eye)", rt_w, rt_h,
            (unsigned)vp.X, (unsigned)vp.Y, (unsigned)vp.Width,
            (unsigned)vp.Height, (void*)vs, (void*)ps);
        for (int r = 0; r < 16; r += 2)
            log("water draw:   c%-2d %9.4f %9.4f %9.4f %9.4f | c%-2d %9.4f "
                "%9.4f %9.4f %9.4f", 4 + r, c[r * 4], c[r * 4 + 1],
                c[r * 4 + 2], c[r * 4 + 3], 5 + r, c[r * 4 + 4],
                c[r * 4 + 5], c[r * 4 + 6], c[r * 4 + 7]);
    }

    void reflection_stereo_release()
    {
        release_halves();
        g_copy = nullptr;
        g_copy_surface = nullptr;
        g_copy_dirty = false;
        release_twin_colour();
        release_twin_depth();
        g_refl_surface = nullptr;
        g_refl_texture = nullptr;
        g_refl_depth = nullptr;
        g_current_rt = nullptr;
        g_current_ds = nullptr;
        g_device = nullptr;
        for (DWORD s = 0; s < 16; ++s)
            g_swapped[s] = false;
    }
}
