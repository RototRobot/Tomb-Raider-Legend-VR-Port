#include "hud_capture.h"
#include "ui_space.h"
#include "camera_head.h"
#include "vr_math.h"

#include "../common/config.h"

#include "../common/log.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace trlvr
{
    namespace
    {
        struct Seen
        {
            IDirect3DTexture9* texture;
            unsigned draws;
        };
        Seen g_seen[256];
        unsigned g_seen_count = 0;

        // Game icon finder (always on; see hud_gear_icon).
        struct KnownIcon
        {
            unsigned hash;
            const wchar_t* file;
        };
        const KnownIcon kKnownIcons[5] = {
            { 0xF59EF1DAu, L"medipack" },
            { 0xC44C906Du, L"binoculars" },
            { 0x4F1082E3u, L"weapon_switch" },
            { 0x0FD849E4u, L"flashlight" },
            { 0x1F0475F7u, L"grenade" },
        };
        const unsigned kIconSize = 64;
        unsigned char g_icon_pixels[5][64 * 64 * 4];
        bool g_icon_have[5] = {};
        bool g_icon_cache_tried = false;
        IDirect3DTexture9* g_icon_texture[5] = {};
        IDirect3DTexture9* g_icon_checked[256] = {};
        const unsigned kBinocularOverlayHash = 0xCD443973u;
        IDirect3DTexture9* g_binocular_overlay = nullptr;
        // Retail gear-cross textures found by content: the five icons and
        // the 128x128 rounded tile frame (FNV-1a 8A7D0047).
        const unsigned kGearFrameHash = 0x8A7D0047u;
        IDirect3DTexture9* g_gear_hud_texture[6] = {};
        unsigned g_gear_hud_skipped = 0;
        ULONGLONG g_binocular_seen_at = 0;
        unsigned g_icon_checked_next = 0;
        unsigned g_icon_frames = 0;

        unsigned fnv1a(const unsigned char* data, unsigned size,
                       unsigned hash)
        {
            for (unsigned i = 0; i < size; ++i)
            {
                hash ^= data[i];
                hash *= 0x01000193u;
            }
            return hash;
        }

        void icon_cache_path(int item, wchar_t* path, size_t count)
        {
            swprintf_s(path, count, L"%strlvr_gear_icons\\%s.bin",
                       exe_dir(), kKnownIcons[item].file);
        }

        void load_icon_cache()
        {
            if (g_icon_cache_tried)
                return;
            g_icon_cache_tried = true;
            unsigned loaded = 0;
            for (int item = 0; item < 5; ++item)
            {
                wchar_t path[MAX_PATH]{};
                icon_cache_path(item, path, MAX_PATH);
                FILE* file = _wfopen(path, L"rb");
                if (!file)
                    continue;
                const size_t got = fread(g_icon_pixels[item], 1,
                                         sizeof(g_icon_pixels[item]), file);
                fclose(file);
                g_icon_have[item] = got == sizeof(g_icon_pixels[item]);
                loaded += g_icon_have[item] ? 1 : 0;
            }
            log("gear icons: %u of 5 game icons loaded from the cache%s",
                loaded, loaded < 5 ? " (the rest appear once the game "
                "shows its gear cross)" : "");
        }

        // A 64x64 A8R8G8B8 interface texture: hash it once per pointer and
        // keep a premultiplied copy if it is one of the known icons.
        void check_icon(IDirect3DTexture9* texture)
        {
            for (IDirect3DTexture9* seen : g_icon_checked)
                if (seen == texture)
                    return;
            g_icon_checked[g_icon_checked_next++ % 256] = texture;
            D3DSURFACE_DESC desc{};
            if (FAILED(texture->GetLevelDesc(0, &desc)) ||
                desc.Format != D3DFMT_A8R8G8B8)
                return;
            if (desc.Width == 128 && desc.Height == 128)
            {
                D3DLOCKED_RECT frame{};
                if (FAILED(texture->LockRect(0, &frame, nullptr,
                                             D3DLOCK_READONLY)))
                    return;
                unsigned hash = 0x811C9DC5u;
                for (unsigned y = 0; y < 128; ++y)
                    hash = fnv1a(static_cast<const unsigned char*>(
                        frame.pBits) + y * frame.Pitch, 128 * 4, hash);
                texture->UnlockRect(0);
                if (hash == kGearFrameHash)
                {
                    g_gear_hud_texture[5] = texture;
                    log("gear icons: the game's gear tile frame is texture "
                        "%p", texture);
                }
                return;
            }
            if (desc.Width == 256 && desc.Height == 512)
            {
                D3DLOCKED_RECT overlay{};
                if (FAILED(texture->LockRect(0, &overlay, nullptr,
                                             D3DLOCK_READONLY)))
                    return;
                unsigned hash = 0x811C9DC5u;
                for (unsigned y = 0; y < 512; ++y)
                    hash = fnv1a(static_cast<const unsigned char*>(
                        overlay.pBits) + y * overlay.Pitch, 256 * 4, hash);
                texture->UnlockRect(0);
                if (hash == kBinocularOverlayHash)
                {
                    g_binocular_overlay = texture;
                    log("binoculars: the game's binocular overlay is "
                        "texture %p", texture);
                }
                return;
            }
            if (desc.Width != kIconSize || desc.Height != kIconSize)
                return;
            D3DLOCKED_RECT locked{};
            if (FAILED(texture->LockRect(0, &locked, nullptr,
                                         D3DLOCK_READONLY)))
                return;
            unsigned hash = 0x811C9DC5u;
            for (unsigned y = 0; y < kIconSize; ++y)
                hash = fnv1a(static_cast<const unsigned char*>(locked.pBits) +
                             y * locked.Pitch, kIconSize * 4, hash);
            int item = -1;
            for (int k = 0; k < 5; ++k)
                if (kKnownIcons[k].hash == hash)
                    item = k;
            if (item >= 0)
                g_gear_hud_texture[item] = texture;
            else
            {
                // Unknown 64x64 interface icons, to identify any variant
                // (e.g. a selected/white gear icon) from the log.
                static unsigned unknown_reports = 0;
                if (unknown_reports++ < 24)
                    log("gear icons: unrecognised 64x64 interface texture "
                        "%p hash %08X", texture, hash);
            }
            if (item >= 0 && !g_icon_have[item])
            {
                unsigned char* out = g_icon_pixels[item];
                for (unsigned y = 0; y < kIconSize; ++y)
                {
                    const unsigned char* row =
                        static_cast<const unsigned char*>(locked.pBits) +
                        y * locked.Pitch;
                    for (unsigned x = 0; x < kIconSize; ++x)
                    {
                        const unsigned a = row[x * 4 + 3];
                        for (int c = 0; c < 3; ++c)
                            out[(y * kIconSize + x) * 4 + c] =
                                (unsigned char)(row[x * 4 + c] * a / 255);
                        out[(y * kIconSize + x) * 4 + 3] = (unsigned char)a;
                    }
                }
                g_icon_have[item] = true;
                wchar_t dir[MAX_PATH]{}, path[MAX_PATH]{};
                swprintf_s(dir, L"%strlvr_gear_icons", exe_dir());
                CreateDirectoryW(dir, nullptr);
                icon_cache_path(item, path, MAX_PATH);
                FILE* file = _wfopen(path, L"wb");
                if (file)
                {
                    fwrite(out, 1, sizeof(g_icon_pixels[item]), file);
                    fclose(file);
                }
                log("gear icons: found the game's %ls icon (texture %p); "
                    "cached", kKnownIcons[item].file, texture);
            }
            texture->UnlockRect(0);
        }
        unsigned g_frames_left = 0;     // >0 while collecting

        // F4 scene capture state.
        bool g_scene_requested = false;
        bool g_scene_active = false;
        unsigned g_scene_draws = 0;
        unsigned g_scene_index = 0;
        IDirect3DTexture9* g_scene_dump[48];
        unsigned g_scene_dump_count = 0;
        bool g_requested = false;
        unsigned g_capture_index = 0;

        // F9: (texture page, indices, class, texture) of interface draws.
        struct PageSeen
        {
            uintptr_t page;
            unsigned indices;
            int kind;
            void* texture;
            unsigned draws;
        };
        PageSeen g_pages[256];
        unsigned g_page_count = 0;
        uintptr_t g_current_page = 0;
        unsigned g_current_indices = 0;
        int g_current_kind = -1;

        bool write_dds(const wchar_t* path, IDirect3DTexture9* texture,
                       D3DSURFACE_DESC* out_desc)
        {
            D3DSURFACE_DESC desc{};
            if (FAILED(texture->GetLevelDesc(0, &desc)))
                return false;
            *out_desc = desc;
            DWORD fourcc = 0, bits = 0, pf_flags = 0;
            DWORD masks[4] = { 0, 0, 0, 0 };
            bool compressed = false;
            unsigned block = 0;
            switch (desc.Format)
            {
            case D3DFMT_DXT1: fourcc = '1TXD'; compressed = true; block = 8; break;
            case D3DFMT_DXT2: fourcc = '2TXD'; compressed = true; block = 16; break;
            case D3DFMT_DXT3: fourcc = '3TXD'; compressed = true; block = 16; break;
            case D3DFMT_DXT4: fourcc = '4TXD'; compressed = true; block = 16; break;
            case D3DFMT_DXT5: fourcc = '5TXD'; compressed = true; block = 16; break;
            case D3DFMT_A8R8G8B8:
                pf_flags = 0x41; bits = 32;
                masks[0] = 0x00FF0000; masks[1] = 0x0000FF00;
                masks[2] = 0x000000FF; masks[3] = 0xFF000000; break;
            case D3DFMT_X8R8G8B8:
                pf_flags = 0x40; bits = 32;
                masks[0] = 0x00FF0000; masks[1] = 0x0000FF00;
                masks[2] = 0x000000FF; break;
            case D3DFMT_A4R4G4B4:
                pf_flags = 0x41; bits = 16;
                masks[0] = 0x0F00; masks[1] = 0x00F0; masks[2] = 0x000F;
                masks[3] = 0xF000; break;
            case D3DFMT_A1R5G5B5:
                pf_flags = 0x41; bits = 16;
                masks[0] = 0x7C00; masks[1] = 0x03E0; masks[2] = 0x001F;
                masks[3] = 0x8000; break;
            case D3DFMT_R5G6B5:
                pf_flags = 0x40; bits = 16;
                masks[0] = 0xF800; masks[1] = 0x07E0; masks[2] = 0x001F; break;
            case D3DFMT_A8:
                pf_flags = 0x2; bits = 8; masks[3] = 0xFF; break;
            case D3DFMT_L8:
                pf_flags = 0x20000; bits = 8; masks[0] = 0xFF; break;
            case D3DFMT_A8L8:
                pf_flags = 0x20001; bits = 16; masks[0] = 0xFF;
                masks[3] = 0xFF00; break;
            default:
                return false;
            }
            const unsigned rows = compressed
                ? (desc.Height + 3) / 4 : desc.Height;
            const unsigned row_bytes = compressed
                ? ((desc.Width + 3) / 4) * block : desc.Width * (bits / 8);
            D3DLOCKED_RECT locked{};
            if (FAILED(texture->LockRect(0, &locked, nullptr, D3DLOCK_READONLY)))
                return false;
            FILE* file = _wfopen(path, L"wb");
            if (!file)
            {
                texture->UnlockRect(0);
                return false;
            }
            DWORD header[32]{};
            header[0] = 0x20534444;  // "DDS "
            header[1] = 124;
            header[2] = 0x1 | 0x2 | 0x4 | 0x1000 |
                        (compressed ? 0x80000 : 0x8);
            header[3] = desc.Height;
            header[4] = desc.Width;
            header[5] = compressed ? row_bytes * rows : row_bytes;
            header[7] = 1;
            // Pixel format at dword 19.
            header[19] = 32;
            header[20] = compressed ? 0x4 : pf_flags;
            header[21] = fourcc;
            header[22] = bits;
            header[23] = masks[0];
            header[24] = masks[1];
            header[25] = masks[2];
            header[26] = masks[3];
            header[27] = 0x1000;     // caps: texture
            fwrite(header, 4, 32, file);
            for (unsigned y = 0; y < rows; ++y)
                fwrite(static_cast<const unsigned char*>(locked.pBits) +
                       y * locked.Pitch, 1, row_bytes, file);
            fclose(file);
            texture->UnlockRect(0);
            return true;
        }

        void finish()
        {
            wchar_t dir[MAX_PATH]{};
            swprintf_s(dir, L"%strlvr_hud_dump", exe_dir());
            CreateDirectoryW(dir, nullptr);
            ++g_capture_index;
            log("hud capture %u: %u distinct interface textures",
                g_capture_index, g_seen_count);
            for (unsigned i = 0; i < g_seen_count; ++i)
            {
                wchar_t path[MAX_PATH]{};
                swprintf_s(path, L"%s\\cap%u_%02u_%p.dds", dir,
                           g_capture_index, i, g_seen[i].texture);
                D3DSURFACE_DESC desc{};
                const bool ok = write_dds(path, g_seen[i].texture, &desc);
                log("hud capture %u: #%02u texture %p %ux%u format %d, %u "
                    "draws -> %s", g_capture_index, i, g_seen[i].texture,
                    desc.Width, desc.Height, (int)desc.Format,
                    g_seen[i].draws, ok ? "saved" : "NOT saved");
                g_seen[i].texture->Release();
            }
            g_seen_count = 0;
            for (unsigned i = 0; i < g_page_count; ++i)
                log("hud capture %u: drawable page 0x%X, %u indices, %s, "
                    "texture %p, %u draws", g_capture_index,
                    (unsigned)g_pages[i].page, g_pages[i].indices,
                    g_pages[i].kind == 1 ? "HUDDrawable" : "SimpleDrawable",
                    g_pages[i].texture, g_pages[i].draws);
            g_page_count = 0;
        }
    }

    void hud_capture_set_drawable(uintptr_t page, unsigned indices,
                                  int kind)
    {
        g_current_page = page;
        g_current_indices = indices;
        g_current_kind = kind;
    }

    void hud_capture_start()
    {
        g_requested = true;
    }

    void hud_scene_capture_start()
    {
        g_scene_requested = true;
    }

    namespace
    {
        void* g_draw_caller = nullptr;
        unsigned g_draw_primitives = 0;
    }

    void hud_capture_set_call(void* caller, unsigned primitives)
    {
        g_draw_caller = caller;
        g_draw_primitives = primitives;
    }

    void* hud_capture_current_call(unsigned* primitives)
    {
        if (primitives)
            *primitives = g_draw_primitives;
        return g_draw_caller;
    }

    bool hud_capture_note_draw(IDirect3DDevice9* device)
    {
        if (g_scene_active && device && g_scene_draws < 12000)
        {
            ++g_scene_draws;
            float c[16]{};
            device->GetVertexShaderConstantF(0, c, 4);
            DWORD zen = 0, zwrite = 0, cull = 0;
            device->GetRenderState(D3DRS_ZENABLE, &zen);
            device->GetRenderState(D3DRS_ZWRITEENABLE, &zwrite);
            device->GetRenderState(D3DRS_CULLMODE, &cull);
            // Decal diagnosis (2026-10-04): depth test, bias and blending.
            DWORD zfunc = 0, bias = 0, slope = 0, blend = 0, atest = 0;
            device->GetRenderState(D3DRS_ZFUNC, &zfunc);
            device->GetRenderState(D3DRS_DEPTHBIAS, &bias);
            device->GetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, &slope);
            device->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
            device->GetRenderState(D3DRS_ALPHATESTENABLE, &atest);
            DWORD clip = 0, scissor = 0;
            device->GetRenderState(D3DRS_CLIPPLANEENABLE, &clip);
            device->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissor);
            RECT sr{};
            device->GetScissorRect(&sr);
            float plane0[4]{};
            if (clip)
                device->GetClipPlane(0, plane0);
            IDirect3DBaseTexture9* tex1 = nullptr;
            device->GetTexture(1, &tex1);
            float fbias = 0.0f, fslope = 0.0f;
            memcpy(&fbias, &bias, sizeof(fbias));
            memcpy(&fslope, &slope, sizeof(fslope));
            IDirect3DVertexShader9* vs = nullptr;
            IDirect3DPixelShader9* ps = nullptr;
            device->GetVertexShader(&vs);
            device->GetPixelShader(&ps);
            IDirect3DBaseTexture9* tex = nullptr;
            device->GetTexture(0, &tex);
            const bool persp = is_perspective_registers(c);
            const bool ui = ui_drawing_interface();
            log("scene draw %4u: from %p prims %u %s%s z%lu/w%lu f%lu bias %g/%g blend %lu "
                "at %lu cull %lu clip %lx [%.3f %.3f %.3f %.1f] scissor %lu "
                "(%ld,%ld,%ld,%ld) vs %p ps %p tex %p/%p "
                "c0 [%+.3f %+.3f %+.3f %+.1f] c3 [%+.4f %+.4f %+.4f %+.1f]",
                g_scene_draws, g_draw_caller, g_draw_primitives,
                persp ? "persp" : "FLAT ", ui ? " UI" : "   ",
                zen, zwrite, zfunc, fbias, fslope, blend, atest, cull,
                clip, plane0[0], plane0[1], plane0[2], plane0[3], scissor,
                sr.left, sr.top, sr.right, sr.bottom, vs, ps,
                tex, tex1, c[0], c[1], c[2], c[3],
                c[12], c[13], c[14], c[15]);
            if (tex1) tex1->Release();
            if (tex && !persp && !ui && tex->GetType() == D3DRTYPE_TEXTURE)
            {
                bool known = false;
                for (unsigned i = 0; i < g_scene_dump_count && !known; ++i)
                    known = g_scene_dump[i] == tex;
                if (!known && g_scene_dump_count < 48)
                {
                    tex->AddRef();
                    g_scene_dump[g_scene_dump_count++] =
                        static_cast<IDirect3DTexture9*>(tex);
                }
            }
            if (tex) tex->Release();
            if (vs) vs->Release();
            if (ps) ps->Release();
        }
        // F9: every small drawable (sprites, <= 24 indices), interface or
        // not, with its texture page and bound texture.
        if (g_frames_left && device && g_current_page &&
            g_current_indices <= 24)
        {
            IDirect3DBaseTexture9* bound = nullptr;
            if (SUCCEEDED(device->GetTexture(0, &bound)) && bound)
            {
                bool known = false;
                for (unsigned i = 0; i < g_page_count && !known; ++i)
                    if (g_pages[i].page == g_current_page &&
                        g_pages[i].indices == g_current_indices &&
                        g_pages[i].texture == bound)
                    {
                        ++g_pages[i].draws;
                        known = true;
                    }
                if (!known && g_page_count < 256)
                    g_pages[g_page_count++] = { g_current_page,
                        g_current_indices, g_current_kind, bound, 1 };
                // Dump its texture too (the list below only holds
                // interface draws).
                bool dumped = false;
                for (unsigned i = 0; i < g_seen_count && !dumped; ++i)
                    dumped = g_seen[i].texture == bound;
                if (!dumped && g_seen_count < 256 &&
                    bound->GetType() == D3DRTYPE_TEXTURE)
                {
                    bound->AddRef();
                    g_seen[g_seen_count++] = {
                        static_cast<IDirect3DTexture9*>(bound), 0 };
                }
                bound->Release();
            }
        }
        if (!device || !ui_drawing_interface())
            return false;
        IDirect3DBaseTexture9* base = nullptr;
        if (FAILED(device->GetTexture(0, &base)) || !base)
            return false;
        if (base == g_binocular_overlay)
            g_binocular_seen_at = GetTickCount64();
        if (base->GetType() == D3DRTYPE_TEXTURE)
            check_icon(static_cast<IDirect3DTexture9*>(base));
        bool skip = false;
        if (config().immersive_controls && !camera_first_person_active())
            for (IDirect3DTexture9* gear : g_gear_hud_texture)
                if (gear && base == gear)
                    skip = true;
        if (skip && g_gear_hud_skipped++ == 0)
            log("third-person: retail gear cross draws skipped (texture "
                "%p); the 3D gear cross replaces them", base);
        if (!g_frames_left)
        {
            base->Release();
            return skip;
        }
        if (base->GetType() == D3DRTYPE_TEXTURE)
        {
            IDirect3DTexture9* texture =
                static_cast<IDirect3DTexture9*>(base);
            for (unsigned i = 0; i < g_seen_count; ++i)
                if (g_seen[i].texture == texture)
                {
                    ++g_seen[i].draws;
                    base->Release();
                    return skip;
                }
            if (g_seen_count < 256)
            {
                g_seen[g_seen_count++] = { texture, 1 };
                return skip; // keeps the reference GetTexture added
            }
        }
        base->Release();
        return skip;
    }

    void hud_capture_device_changed()
    {
        for (IDirect3DTexture9*& texture : g_icon_texture)
            if (texture)
            {
                texture->Release();
                texture = nullptr;
            }
        for (IDirect3DTexture9*& seen : g_icon_checked)
            seen = nullptr;
        g_binocular_overlay = nullptr;
        for (IDirect3DTexture9*& gear : g_gear_hud_texture)
            gear = nullptr;
    }

    IDirect3DTexture9* hud_gear_icon(IDirect3DDevice9* device, int item)
    {
        if (item < 0 || item >= 5 || !device)
            return nullptr;
        load_icon_cache();
        if (g_icon_texture[item] || !g_icon_have[item])
            return g_icon_texture[item];
        IDirect3DTexture9* texture = nullptr;
        if (FAILED(device->CreateTexture(kIconSize, kIconSize, 1, 0,
                D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr)))
            return nullptr;
        D3DLOCKED_RECT locked{};
        if (FAILED(texture->LockRect(0, &locked, nullptr, 0)))
        {
            texture->Release();
            return nullptr;
        }
        for (unsigned y = 0; y < kIconSize; ++y)
            memcpy(static_cast<unsigned char*>(locked.pBits) +
                       y * locked.Pitch,
                   g_icon_pixels[item] + y * kIconSize * 4, kIconSize * 4);
        texture->UnlockRect(0);
        g_icon_texture[item] = texture;
        return texture;
    }

    bool hud_binocular_view_active()
    {
        return g_binocular_seen_at &&
               GetTickCount64() - g_binocular_seen_at < 250;
    }

    void hud_capture_frame()
    {
        // F4: capture exactly the next whole frame (Present to Present).
        if (g_scene_active)
        {
            g_scene_active = false;
            wchar_t dir[MAX_PATH]{};
            swprintf_s(dir, L"%strlvr_hud_dump", exe_dir());
            CreateDirectoryW(dir, nullptr);
            ++g_scene_index;
            log("scene capture %u: %u draws; %u flat non-interface textures",
                g_scene_index, g_scene_draws, g_scene_dump_count);
            for (unsigned i = 0; i < g_scene_dump_count; ++i)
            {
                wchar_t path[MAX_PATH]{};
                swprintf_s(path, L"%s\\scene%u_%02u_%p.dds", dir,
                           g_scene_index, i, g_scene_dump[i]);
                D3DSURFACE_DESC desc{};
                const bool ok = write_dds(path, g_scene_dump[i], &desc);
                log("scene capture %u: flat texture %p %ux%u format %d -> %s",
                    g_scene_index, g_scene_dump[i], desc.Width, desc.Height,
                    (int)desc.Format, ok ? "saved" : "NOT saved");
                g_scene_dump[i]->Release();
            }
            g_scene_dump_count = 0;
        }
        if (g_scene_requested)
        {
            g_scene_requested = false;
            g_scene_active = true;
            g_scene_draws = 0;
            log("scene capture: F4 -- logging every draw of the next frame");
        }
        // Pointers can be reused by new textures after a level load.
        if (++g_icon_frames % 900 == 0)
        {
            for (IDirect3DTexture9*& seen : g_icon_checked)
                seen = nullptr;
            // Re-found by content on its next draw.
            g_binocular_overlay = nullptr;
            for (IDirect3DTexture9*& gear : g_gear_hud_texture)
                gear = nullptr;
        }
        if (g_requested)
        {
            g_requested = false;
            if (!g_frames_left)
            {
                g_frames_left = 270; // ~3 s at 90 Hz
                log("hud capture: F9 -- collecting interface textures for "
                    "about 3 seconds");
            }
            return;
        }
        if (g_frames_left && --g_frames_left == 0)
            finish();
    }
}
