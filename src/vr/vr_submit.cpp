#include "vr_submit.h"
#include "perf_cpu.h"
#include "vr_session.h"
#include "stereo.h"
#include "tune.h"
#include "camera_head.h"
#include "vr_input.h"
#include "hud_capture.h"

#include "../common/config.h"
#include "../common/log.h"

#include <windows.h>
#include <cmath>
#include <d2d1.h>
#include <dwrite.h>
#include <cwchar>

#include "openvr.h"
#include "d3d9_vr.h"

namespace trlvr
{
    namespace
    {
        typedef HRESULT(__stdcall* PFN_Direct3DCreateVR9)(IDirect3DDevice9*, IDirect3DVR9**);

        IDirect3DVR9* g_vr = nullptr;
        vr::IVRCompositor* g_comp = nullptr;
        IDirect3DDevice9* g_device = nullptr;

        // One image per eye. In alternating mode only one is redrawn each
        // frame; the other is last frame's, which is what makes this cheap and
        // also what makes the eyes a frame out of step.
        IDirect3DSurface9* g_eye[2] = { nullptr, nullptr };
        unsigned g_eye_w = 0, g_eye_h = 0;
        bool g_eye_ready[2] = { false, false };
        unsigned g_drawn_count[2] = { 0, 0 };

        struct DebugLineVertex
        {
            float x, y, z, rhw;
            DWORD colour;
        };

        void draw_hand_lines(IDirect3DSurface9* back,
                             const D3DSURFACE_DESC& desc,
                             const DebugLineVertex* lines, unsigned used)
        {
            if (!used || !g_device)
                return;
            IDirect3DStateBlock9* state = nullptr;
            IDirect3DSurface9* old_target = nullptr;
            D3DVIEWPORT9 old_viewport{};
            if (FAILED(g_device->CreateStateBlock(D3DSBT_ALL, &state)) ||
                !state)
                return;
            state->Capture();
            g_device->GetRenderTarget(0, &old_target);
            g_device->GetViewport(&old_viewport);
            if (SUCCEEDED(g_device->SetRenderTarget(0, back)))
            {
                const D3DVIEWPORT9 viewport = { 0, 0, desc.Width,
                                                desc.Height, 0.0f, 1.0f };
                g_device->SetViewport(&viewport);
                g_device->SetVertexShader(nullptr);
                g_device->SetPixelShader(nullptr);
                g_device->SetTexture(0, nullptr);
                g_device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
                g_device->SetRenderState(D3DRS_ZENABLE, FALSE);
                g_device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
                g_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
                g_device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
                g_device->SetRenderState(D3DRS_LIGHTING, FALSE);
                g_device->SetRenderState(D3DRS_FOGENABLE, FALSE);
                g_device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
                g_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
                g_device->SetTextureStageState(0, D3DTSS_COLOROP,
                                               D3DTOP_SELECTARG2);
                g_device->SetTextureStageState(0, D3DTSS_COLORARG2,
                                               D3DTA_DIFFUSE);
                g_device->SetTextureStageState(0, D3DTSS_ALPHAOP,
                                               D3DTOP_SELECTARG2);
                g_device->SetTextureStageState(0, D3DTSS_ALPHAARG2,
                                               D3DTA_DIFFUSE);
                g_device->SetTextureStageState(1, D3DTSS_COLOROP,
                                               D3DTOP_DISABLE);
                g_device->SetTextureStageState(1, D3DTSS_ALPHAOP,
                                               D3DTOP_DISABLE);
                if (SUCCEEDED(g_device->BeginScene()))
                {
                    g_device->DrawPrimitiveUP(D3DPT_LINELIST, used / 2,
                                             lines, sizeof(DebugLineVertex));
                    g_device->EndScene();
                }
            }
            if (old_target)
            {
                g_device->SetRenderTarget(0, old_target);
                old_target->Release();
            }
            state->Apply();
            g_device->SetViewport(&old_viewport);
            state->Release();
        }

        void draw_holster_debug(IDirect3DSurface9* back)
        {
            const VrHolsterZone& zone = tune_holster_zone();
            if (!zone.debug_draw || !config().immersive_controls ||
                !camera_first_person_active() || !stereo_same_frame_active())
                return;
            D3DSURFACE_DESC desc{};
            if (FAILED(back->GetDesc(&desc)) || desc.Width < 2 ||
                !desc.Height)
                return;
            const float eye_width = desc.Width * 0.5f;
            DebugLineVertex lines[2 * 2 * 3 * 32 * 2]{};
            unsigned used = 0;
            const DWORD colours[4] = {
                0xFF40E8FFu, // ready: cyan
                0xFFFFD54Au, // hand inside: yellow
                0xFF5AFF66u, // reserved
                0xFFFF50E0u  // gun drawn: magenta
            };
            for (int eye_index = 0; eye_index < 2; ++eye_index)
            {
                const Eye eye = eye_index ? EyeRight : EyeLeft;
                for (int hand = 0; hand < 2; ++hand)
                {
                    const int state = vr_input_holster_visual_state(hand == 0);
                    const DWORD colour = colours[state >= 0 && state < 4
                        ? state : 0];
                    const float centre_x = hand == 0 ? zone.left_x :
                                                     zone.right_x;
                    const float centre_y = hand == 0 ? zone.left_y :
                                                     zone.right_y;
                    const float centre_z = hand == 0 ? zone.left_z :
                                                     zone.right_z;
                    // Three great-circle outlines show the exact ellipsoid
                    // tested by the gesture classifier, in metres.
                    for (int plane = 0; plane < 3; ++plane)
                        for (int step = 0; step < 32; ++step)
                        {
                            float points[2][3]{};
                            for (int end = 0; end < 2; ++end)
                            {
                                const float angle = 6.28318530718f *
                                    static_cast<float>(step + end) / 32.0f;
                                const float cs = cosf(angle), sn = sinf(angle);
                                points[end][0] = centre_x +
                                    (plane == 2 ? 0.0f : zone.radius_x * cs);
                                points[end][1] = centre_y +
                                    (plane == 0 ? 0.0f : zone.radius_y *
                                     (plane == 1 ? sn : cs));
                                points[end][2] = centre_z +
                                    (plane == 1 ? 0.0f : zone.radius_z * sn);
                            }
                            float uv[2][2]{};
                            if (!vr_project_body_point(eye, points[0][0],
                                    points[0][1], points[0][2], &uv[0][0],
                                    &uv[0][1]) ||
                                !vr_project_body_point(eye, points[1][0],
                                    points[1][1], points[1][2], &uv[1][0],
                                    &uv[1][1]))
                                continue;
                            for (int end = 0; end < 2; ++end)
                                lines[used++] = {
                                    eye_width * (eye_index + uv[end][0]),
                                    desc.Height * uv[end][1], 0.0f, 1.0f,
                                    colour
                                };
                        }
                }
            }
            if (!used)
                return;

            IDirect3DStateBlock9* state = nullptr;
            IDirect3DSurface9* old_target = nullptr;
            D3DVIEWPORT9 old_viewport{};
            if (FAILED(g_device->CreateStateBlock(D3DSBT_ALL, &state)) ||
                !state)
                return;
            state->Capture();
            g_device->GetRenderTarget(0, &old_target);
            g_device->GetViewport(&old_viewport);
            if (SUCCEEDED(g_device->SetRenderTarget(0, back)))
            {
                const D3DVIEWPORT9 viewport = { 0, 0, desc.Width,
                                                desc.Height, 0.0f, 1.0f };
                g_device->SetViewport(&viewport);
                g_device->SetVertexShader(nullptr);
                g_device->SetPixelShader(nullptr);
                g_device->SetTexture(0, nullptr);
                g_device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
                g_device->SetRenderState(D3DRS_ZENABLE, FALSE);
                g_device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
                g_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
                g_device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
                g_device->SetRenderState(D3DRS_LIGHTING, FALSE);
                g_device->SetRenderState(D3DRS_FOGENABLE, FALSE);
                g_device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
                g_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
                g_device->SetTextureStageState(0, D3DTSS_COLOROP,
                                               D3DTOP_SELECTARG2);
                g_device->SetTextureStageState(0, D3DTSS_COLORARG2,
                                               D3DTA_DIFFUSE);
                g_device->SetTextureStageState(0, D3DTSS_ALPHAOP,
                                               D3DTOP_SELECTARG2);
                g_device->SetTextureStageState(0, D3DTSS_ALPHAARG2,
                                               D3DTA_DIFFUSE);
                g_device->SetTextureStageState(1, D3DTSS_COLOROP,
                                               D3DTOP_DISABLE);
                g_device->SetTextureStageState(1, D3DTSS_ALPHAOP,
                                               D3DTOP_DISABLE);
                if (SUCCEEDED(g_device->BeginScene()))
                {
                    g_device->DrawPrimitiveUP(D3DPT_LINELIST, used / 2,
                                             lines, sizeof(DebugLineVertex));
                    g_device->EndScene();
                }
            }
            if (old_target)
            {
                g_device->SetRenderTarget(0, old_target);
                old_target->Release();
            }
            state->Apply();
            g_device->SetViewport(&old_viewport);
            state->Release();
        }

        void draw_hand_tuning_debug(IDirect3DSurface9* back)
        {
            if (!tune_hand_debug_enabled() ||
                !config().first_person_tracked_hands ||
                !camera_first_person_active() || !stereo_same_frame_active())
                return;
            D3DSURFACE_DESC desc{};
            if (FAILED(back->GetDesc(&desc)) || desc.Width < 2 ||
                !desc.Height)
                return;
            const float eye_width = desc.Width * 0.5f;
            DebugLineVertex lines[128]{};
            unsigned used = 0;
            const auto line = [&](float x0, float y0, float x1, float y1,
                                  DWORD colour)
            {
                if (used + 2 > _countof(lines))
                    return;
                lines[used++] = { x0, y0, 0.0f, 1.0f, colour };
                lines[used++] = { x1, y1, 0.0f, 1.0f, colour };
            };
            for (int eye_index = 0; eye_index < 2; ++eye_index)
            {
                const Eye eye = eye_index ? EyeRight : EyeLeft;
                for (int hand = 0; hand < 2; ++hand)
                {
                    float right = 0.0f, up = 0.0f, forward = 0.0f;
                    if (!vr_controller_body_position(hand == 0,
                            &right, &up, &forward))
                        continue;
                    float u = 0.0f, v = 0.0f;
                    if (!vr_project_body_point(eye, right, up, forward,
                                               &u, &v) ||
                        u < 0.04f || u > 0.96f || v < 0.04f || v > 0.96f)
                        continue;
                    const float x = eye_width * (eye_index + u);
                    const float y = desc.Height * v;
                    const bool selected = (hand == 0) ==
                        tune_hand_debug_selected_left();
                    const DWORD colour = hand == 0 ? 0xFF55DFFFu : 0xFFFFB25Au;
                    const float arm = selected ? 15.0f : 10.0f;
                    line(x - arm, y, x + arm, y, colour);
                    line(x, y - arm, x, y + arm, colour);
                    if (!selected)
                        continue;
                    const float box = 22.0f;
                    const DWORD highlight = tune_hand_debug_rotation_mode()
                        ? 0xFFFF50E0u : 0xFFFFFF55u;
                    line(x - box, y - box, x + box, y - box, highlight);
                    line(x + box, y - box, x + box, y + box, highlight);
                    line(x + box, y + box, x - box, y + box, highlight);
                    line(x - box, y + box, x - box, y - box, highlight);
                    if (!tune_hand_debug_rotation_mode())
                        continue;
                    const DWORD axes[3] = {
                        0xFFFF5555u, 0xFF66FF66u, 0xFF6688FFu
                    };
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        float endpoint[3]{};
                        if (!vr_controller_body_axis_endpoint(hand == 0,
                                axis, 0.06f, &endpoint[0], &endpoint[1],
                                &endpoint[2]))
                            continue;
                        float au = 0.0f, av = 0.0f;
                        if (!vr_project_body_point(eye, endpoint[0],
                                endpoint[1], endpoint[2], &au, &av) ||
                            au < 0.0f || au > 1.0f ||
                            av < 0.0f || av > 1.0f)
                            continue;
                        line(x, y, eye_width * (eye_index + au),
                             desc.Height * av, axes[axis]);
                    }
                }
            }
            draw_hand_lines(back, desc, lines, used);
        }

        void draw_handhold_debug(IDirect3DSurface9* back)
        {
            if (!tune_ledge_grab_debug_draw() ||
                !config().immersive_controls ||
                !camera_first_person_active() || !stereo_same_frame_active())
                return;
            D3DSURFACE_DESC desc{};
            if (FAILED(back->GetDesc(&desc)) || desc.Width < 2 ||
                !desc.Height)
                return;
            const float eye_width = desc.Width * 0.5f;
            DebugLineVertex lines[2 * 12 * 2]{};
            unsigned used = 0;
            float corners[8][3]{};
            int state = 0;
            if (!camera_first_person_grab_box(corners, &state))
                return;
            const DWORD colour = state == 2 ? 0xFFFF50E0u :
                state == 1 ? 0xFFFFFF55u : 0xFF40E8FFu;
            for (int eye_index = 0; eye_index < 2; ++eye_index)
            {
                const Eye eye = eye_index ? EyeRight : EyeLeft;
                float uv[8][2]{};
                bool visible[8]{};
                for (int corner = 0; corner < 8; ++corner)
                {
                    visible[corner] = vr_project_head_point(eye,
                        corners[corner][0], corners[corner][1],
                        corners[corner][2], &uv[corner][0],
                        &uv[corner][1]) && uv[corner][0] >= 0.0f &&
                        uv[corner][0] <= 1.0f &&
                        uv[corner][1] >= 0.0f &&
                        uv[corner][1] <= 1.0f;
                }
                // Corners differ by one bit along each box axis. Emit each
                // of the twelve edges once, in both stereo eye halves.
                for (int corner = 0; corner < 8; ++corner)
                    for (int bit = 1; bit <= 4; bit *= 2)
                    {
                        if (corner & bit)
                            continue;
                        const int other = corner | bit;
                        if (!visible[corner] || !visible[other])
                            continue;
                        lines[used++] = {
                            eye_width * (eye_index + uv[corner][0]),
                            desc.Height * uv[corner][1], 0.0f, 1.0f,
                            colour
                        };
                        lines[used++] = {
                            eye_width * (eye_index + uv[other][0]),
                            desc.Height * uv[other][1], 0.0f, 1.0f,
                            colour
                        };
                    }
            }
            draw_hand_lines(back, desc, lines, used);
        }

        struct VignetteVertex
        {
            float x, y, z, rhw;
            DWORD colour;
        };

        // Comfort vignette: per eye, a soft black ring around the lens
        // centre (the projection of a far point straight ahead), clear
        // inside, fading over a band, opaque beyond. Strength 0..1 narrows
        // the clear area from beyond the view edge to ~45% of it.
        void draw_comfort_vignette(IDirect3DSurface9* back, float strength)
        {
            static unsigned reports = 0;
            static float peak = 0.0f;
            if (strength > peak + 0.25f && reports < 6)
            {
                ++reports;
                log("vignette: strength %.2f", strength);
            }
            peak = strength > peak ? strength : peak * 0.999f;
            if (strength < 0.01f || !g_device)
                return;
            D3DSURFACE_DESC desc{};
            if (FAILED(back->GetDesc(&desc)) || desc.Width < 2 ||
                !desc.Height)
                return;
            const int kSteps = 48;
            static VignetteVertex tris[2 * kSteps * 12];
            unsigned used = 0;
            const float eye_w = desc.Width * 0.5f;
            const float eye_h = (float)desc.Height;
            const float s = fminf(1.0f, strength);
            for (int eye_index = 0; eye_index < 2; ++eye_index)
            {
                float uv[2] = { 0.5f, 0.5f };
                if (!vr_project_head_point(eye_index ? EyeRight : EyeLeft,
                                           0.0f, 0.0f, 100000.0f,
                                           &uv[0], &uv[1]))
                    uv[0] = uv[1] = 0.5f;
                const float cx = eye_w * (eye_index + uv[0]);
                const float cy = eye_h * uv[1];
                // Radii in eye-image heights. The lens shows roughly a disc
                // of radius 0.5 around the centre; the first build's radii
                // (0.95 - 0.5s) sat almost wholly outside it and were not
                // visible (headset test). Clear radius 0.50 -> 0.17 at full
                // strength, fading to black over 0.14.
                const float inner = (0.50f - 0.33f * s) * eye_h;
                const float outer = inner + 0.14f * eye_h;
                const float far_r = 2.0f * (eye_w > eye_h ? eye_w : eye_h);
                const DWORD clear = 0x00000000u;
                const DWORD black = (DWORD)(255.0f * fminf(1.0f, 0.4f + s))
                                    << 24;
                for (int i = 0; i < kSteps; ++i)
                {
                    const float a0 = 6.28318531f * i / kSteps;
                    const float a1 = 6.28318531f * (i + 1) / kSteps;
                    const float c0 = cosf(a0), s0 = sinf(a0);
                    const float c1 = cosf(a1), s1 = sinf(a1);
                    auto v = [&](float r, float c, float sn, DWORD col) {
                        return VignetteVertex{ cx + r * c, cy + r * sn, 0.0f,
                                               1.0f, col };
                    };
                    // Fade band.
                    tris[used++] = v(inner, c0, s0, clear);
                    tris[used++] = v(outer, c0, s0, black);
                    tris[used++] = v(outer, c1, s1, black);
                    tris[used++] = v(inner, c0, s0, clear);
                    tris[used++] = v(outer, c1, s1, black);
                    tris[used++] = v(inner, c1, s1, clear);
                    // Opaque outside, clipped to this eye by the scissor.
                    tris[used++] = v(outer, c0, s0, black);
                    tris[used++] = v(far_r, c0, s0, black);
                    tris[used++] = v(far_r, c1, s1, black);
                    tris[used++] = v(outer, c0, s0, black);
                    tris[used++] = v(far_r, c1, s1, black);
                    tris[used++] = v(outer, c1, s1, black);
                }
            }
            IDirect3DStateBlock9* state = nullptr;
            IDirect3DSurface9* old_target = nullptr;
            D3DVIEWPORT9 old_viewport{};
            if (FAILED(g_device->CreateStateBlock(D3DSBT_ALL, &state)) ||
                !state)
                return;
            state->Capture();
            g_device->GetRenderTarget(0, &old_target);
            g_device->GetViewport(&old_viewport);
            if (SUCCEEDED(g_device->SetRenderTarget(0, back)))
            {
                const D3DVIEWPORT9 viewport = { 0, 0, desc.Width,
                                                desc.Height, 0.0f, 1.0f };
                g_device->SetViewport(&viewport);
                g_device->SetVertexShader(nullptr);
                g_device->SetPixelShader(nullptr);
                g_device->SetTexture(0, nullptr);
                g_device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
                g_device->SetRenderState(D3DRS_ZENABLE, FALSE);
                g_device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
                g_device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
                g_device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
                g_device->SetRenderState(D3DRS_DESTBLEND,
                                         D3DBLEND_INVSRCALPHA);
                g_device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
                g_device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
                g_device->SetRenderState(D3DRS_LIGHTING, FALSE);
                g_device->SetRenderState(D3DRS_FOGENABLE, FALSE);
                g_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
                g_device->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
                g_device->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
                g_device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
                g_device->SetTextureStageState(0, D3DTSS_COLOROP,
                                               D3DTOP_SELECTARG2);
                g_device->SetTextureStageState(0, D3DTSS_COLORARG2,
                                               D3DTA_DIFFUSE);
                g_device->SetTextureStageState(0, D3DTSS_ALPHAOP,
                                               D3DTOP_SELECTARG2);
                g_device->SetTextureStageState(0, D3DTSS_ALPHAARG2,
                                               D3DTA_DIFFUSE);
                g_device->SetTextureStageState(1, D3DTSS_COLOROP,
                                               D3DTOP_DISABLE);
                g_device->SetTextureStageState(1, D3DTSS_ALPHAOP,
                                               D3DTOP_DISABLE);
                if (SUCCEEDED(g_device->BeginScene()))
                {
                    const unsigned per_eye = used / 2;
                    for (int eye_index = 0; eye_index < 2; ++eye_index)
                    {
                        const RECT scissor = {
                            (LONG)(eye_w * eye_index), 0,
                            (LONG)(eye_w * (eye_index + 1)),
                            (LONG)desc.Height };
                        g_device->SetScissorRect(&scissor);
                        g_device->DrawPrimitiveUP(
                            D3DPT_TRIANGLELIST, per_eye / 3,
                            tris + per_eye * eye_index,
                            sizeof(VignetteVertex));
                    }
                    g_device->EndScene();
                }
            }
            if (old_target)
            {
                g_device->SetRenderTarget(0, old_target);
                old_target->Release();
            }
            state->Apply();
            g_device->SetViewport(&old_viewport);
            state->Release();
        }

        // Secure-grip badge: a solid yellow disc with a fist emoji, drawn
        // once into a texture with Direct2D/DirectWrite (colour glyphs; both
        // DLLs loaded at runtime, nothing linked).
        IDirect3DTexture9* g_badge = nullptr;
        bool g_badge_tried = false;
        const UINT kBadgeSize = 128;

        // A round icon: a filled disc with a rim and a glyph (colour emoji
        // where the font has one, else drawn in the ink colour).
        IDirect3DTexture9* make_icon_texture(const wchar_t* glyph,
                                             D2D1_COLOR_F fill_colour,
                                             D2D1_COLOR_F rim_colour,
                                             D2D1_COLOR_F ink_colour,
                                             const char* what)
        {
            if (!g_device || !glyph)
                return nullptr;
            typedef HRESULT(WINAPI* PFN_D2D1CreateFactory)(
                D2D1_FACTORY_TYPE, REFIID, const D2D1_FACTORY_OPTIONS*,
                void**);
            typedef HRESULT(WINAPI* PFN_DWriteCreateFactory)(
                DWRITE_FACTORY_TYPE, REFIID, IUnknown**);
            HMODULE d2d = LoadLibraryW(L"d2d1.dll");
            HMODULE dw = LoadLibraryW(L"dwrite.dll");
            HMODULE gdi = LoadLibraryW(L"gdi32.dll");
            auto create_d2d = d2d ? reinterpret_cast<PFN_D2D1CreateFactory>(
                GetProcAddress(d2d, "D2D1CreateFactory")) : nullptr;
            auto create_dw = dw ? reinterpret_cast<PFN_DWriteCreateFactory>(
                GetProcAddress(dw, "DWriteCreateFactory")) : nullptr;
            typedef HBITMAP(WINAPI* PFN_CreateDIBSection)(HDC,
                const BITMAPINFO*, UINT, void**, HANDLE, DWORD);
            typedef HDC(WINAPI* PFN_CreateCompatibleDC)(HDC);
            typedef HGDIOBJ(WINAPI* PFN_SelectObject)(HDC, HGDIOBJ);
            typedef BOOL(WINAPI* PFN_DeleteObject)(HGDIOBJ);
            typedef BOOL(WINAPI* PFN_DeleteDC)(HDC);
            auto create_dib = gdi ? reinterpret_cast<PFN_CreateDIBSection>(
                GetProcAddress(gdi, "CreateDIBSection")) : nullptr;
            auto create_dc = gdi ? reinterpret_cast<PFN_CreateCompatibleDC>(
                GetProcAddress(gdi, "CreateCompatibleDC")) : nullptr;
            auto select = gdi ? reinterpret_cast<PFN_SelectObject>(
                GetProcAddress(gdi, "SelectObject")) : nullptr;
            auto delete_object = gdi ? reinterpret_cast<PFN_DeleteObject>(
                GetProcAddress(gdi, "DeleteObject")) : nullptr;
            auto delete_dc = gdi ? reinterpret_cast<PFN_DeleteDC>(
                GetProcAddress(gdi, "DeleteDC")) : nullptr;
            if (!create_d2d || !create_dw || !create_dib || !create_dc ||
                !select || !delete_object || !delete_dc)
            {
                log("icons: Direct2D/DirectWrite unavailable (%s)", what);
                return nullptr;
            }
            const UINT n = kBadgeSize;
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(info.bmiHeader);
            info.bmiHeader.biWidth = (LONG)n;
            info.bmiHeader.biHeight = -(LONG)n; // top-down
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            void* bits = nullptr;
            HDC dc = create_dc(nullptr);
            HBITMAP dib = dc ? create_dib(dc, &info, DIB_RGB_COLORS, &bits,
                                          nullptr, 0) : nullptr;
            HGDIOBJ old = dib ? select(dc, dib) : nullptr;
            ID2D1Factory* factory = nullptr;
            IDWriteFactory* write = nullptr;
            ID2D1DCRenderTarget* target = nullptr;
            IDWriteTextFormat* format = nullptr;
            ID2D1SolidColorBrush* yellow = nullptr;
            ID2D1SolidColorBrush* rim = nullptr;
            ID2D1SolidColorBrush* ink = nullptr;
            bool drawn = false;
            if (bits &&
                SUCCEEDED(create_d2d(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                    __uuidof(ID2D1Factory), nullptr,
                    reinterpret_cast<void**>(&factory))) &&
                SUCCEEDED(create_dw(DWRITE_FACTORY_TYPE_SHARED,
                    __uuidof(IDWriteFactory),
                    reinterpret_cast<IUnknown**>(&write))))
            {
                const D2D1_RENDER_TARGET_PROPERTIES props =
                    D2D1::RenderTargetProperties(
                        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                          D2D1_ALPHA_MODE_PREMULTIPLIED));
                const RECT rect = { 0, 0, (LONG)n, (LONG)n };
                if (SUCCEEDED(factory->CreateDCRenderTarget(&props,
                                                            &target)) &&
                    SUCCEEDED(target->BindDC(dc, &rect)) &&
                    SUCCEEDED(write->CreateTextFormat(L"Segoe UI Emoji",
                        nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                        n * 0.50f, L"", &format)) &&
                    SUCCEEDED(target->CreateSolidColorBrush(
                        fill_colour, &yellow)) &&
                    SUCCEEDED(target->CreateSolidColorBrush(
                        rim_colour, &rim)) &&
                    SUCCEEDED(target->CreateSolidColorBrush(
                        ink_colour, &ink)))
                {
                    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                    format->SetParagraphAlignment(
                        DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    const float c = n * 0.5f;
                    target->BeginDraw();
                    target->Clear(D2D1::ColorF(0, 0, 0, 0));
                    if (glyph[0] == L' ' && !glyph[1])
                    {
                        const D2D1_ROUNDED_RECT tile = D2D1::RoundedRect(
                            D2D1::RectF(6.0f, 6.0f, n - 6.0f, n - 6.0f),
                            n * 0.16f, n * 0.16f);
                        target->FillRoundedRectangle(tile, yellow);
                        target->DrawRoundedRectangle(tile, rim, 5.0f);
                    }
                    else if (!glyph[0])
                    {
                        // A handle: three rounded horizontal bars, outlined.
                        for (int bar = -1; bar <= 1; ++bar)
                        {
                            const float y = c + bar * n * 0.24f;
                            const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(
                                D2D1::RectF(n * 0.14f, y - n * 0.07f,
                                            n * 0.86f, y + n * 0.07f),
                                n * 0.06f, n * 0.06f);
                            target->FillRoundedRectangle(rr, ink);
                            target->DrawRoundedRectangle(rr, rim, 4.0f);
                        }
                    }
                    else
                    {
                        const D2D1_ELLIPSE disc = D2D1::Ellipse(
                            D2D1::Point2F(c, c), c - 4.0f, c - 4.0f);
                        target->FillEllipse(disc, yellow);
                        target->DrawEllipse(disc, rim, 5.0f);
                        // Colour glyph where supported (Windows 8.1+);
                        // the ink brush is the monochrome fallback.
                        target->DrawText(glyph, (UINT32)wcslen(glyph),
                            format, D2D1::RectF(0.0f, n * 0.04f, (float)n,
                                                (float)n),
                            ink, static_cast<D2D1_DRAW_TEXT_OPTIONS>(4));
                    }
                    drawn = SUCCEEDED(target->EndDraw());
                }
            }
            if (ink) ink->Release();
            if (rim) rim->Release();
            if (yellow) yellow->Release();
            if (format) format->Release();
            if (target) target->Release();
            if (write) write->Release();
            if (factory) factory->Release();
            IDirect3DTexture9* texture = nullptr;
            if (drawn && SUCCEEDED(g_device->CreateTexture(n, n, 1, 0,
                    D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr)))
            {
                D3DLOCKED_RECT locked{};
                if (SUCCEEDED(texture->LockRect(0, &locked, nullptr, 0)))
                {
                    for (UINT y = 0; y < n; ++y)
                        memcpy(static_cast<unsigned char*>(locked.pBits) +
                                   y * locked.Pitch,
                               static_cast<unsigned char*>(bits) + y * n * 4,
                               n * 4);
                    texture->UnlockRect(0);
                }
                else
                {
                    texture->Release();
                    texture = nullptr;
                }
            }
            if (old)
                select(dc, old);
            if (dib)
                delete_object(dib);
            if (dc)
                delete_dc(dc);
            log("icons: %s texture %s", what, texture ? "ready" : "failed");
            return texture;
        }

        IDirect3DTexture9* grab_badge_texture()
        {
            if (g_badge || g_badge_tried || !g_device)
                return g_badge;
            g_badge_tried = true;
            g_badge = make_icon_texture(L"\u270A", // raised fist
                D2D1::ColorF(1.0f, 0.82f, 0.10f),
                D2D1::ColorF(0.55f, 0.36f, 0.0f),
                D2D1::ColorF(0.25f, 0.15f, 0.0f), "grab badge");
            return g_badge;
        }

        inline float eye_width_px(float eye_w, int eye_index, float u)
        {
            return eye_w * (eye_index + u);
        }

        struct BadgeVertex
        {
            float x, y, z, rhw;
            DWORD colour;
            float u, v;
        };

        // While a precarious one-hand catch waits for a grab: a pulsing
        // yellow fist badge on the centre of the grab zone, replacing the
        // retail prompt (hidden in camera_head).
        void draw_secure_grab_badge(IDirect3DSurface9* back)
        {
            float centre[3]{};
            if (!g_device || !stereo_same_frame_active() ||
                !camera_first_person_secure_grab_point(centre))
                return;
            D3DSURFACE_DESC desc{};
            if (FAILED(back->GetDesc(&desc)) || desc.Width < 2 ||
                !desc.Height)
                return;
            const float metre = tune_world_scale();
            if (!(metre > 0.0f))
                return;
            static LARGE_INTEGER frequency{};
            if (!frequency.QuadPart)
                QueryPerformanceFrequency(&frequency);
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            const double t = frequency.QuadPart
                ? (double)now.QuadPart / (double)frequency.QuadPart : 0.0;
            // 1.6 pulses a second: size 0.85..1.15 x 14.4 cm (8 cm x 1.8, user
            // request), alpha 0.65..1.
            const float wave = 0.5f + 0.5f * (float)sin(t * 6.2831853 * 1.6);
            const float radius = 0.072f * metre * (0.85f + 0.30f * wave);
            const DWORD alpha = (DWORD)(255.0f * (0.65f + 0.35f * wave));
            IDirect3DTexture9* texture = grab_badge_texture();
            // Premultiplied: the diffuse scales colour and alpha together.
            const DWORD shade = texture
                ? (alpha << 24) | (alpha << 16) | (alpha << 8) | alpha
                : (alpha << 24) | ((DWORD)(alpha * 1.0f) << 16) |
                  ((DWORD)(alpha * 0.82f) << 8) | (DWORD)(alpha * 0.10f);
            const float eye_w = desc.Width * 0.5f;
            BadgeVertex quads[2][4]{};
            bool visible[2]{};
            for (int eye_index = 0; eye_index < 2; ++eye_index)
            {
                const Eye eye = eye_index ? EyeRight : EyeLeft;
                // Billboard in the head frame: corners offset along X
                // (right) and Y (down) at the zone's depth.
                const float corner[4][2] = {
                    { -1.0f, -1.0f }, { 1.0f, -1.0f },
                    { -1.0f, 1.0f }, { 1.0f, 1.0f } };
                bool ok = true;
                for (int k = 0; k < 4 && ok; ++k)
                {
                    float u = 0.0f, v = 0.0f;
                    ok = vr_project_head_point(eye,
                        centre[0] + corner[k][0] * radius,
                        centre[1] + corner[k][1] * radius, centre[2],
                        &u, &v) && u > -1.0f && u < 2.0f && v > -1.0f &&
                        v < 2.0f;
                    quads[eye_index][k] = { eye_w * (eye_index + u),
                        desc.Height * v, 0.0f, 1.0f, shade,
                        corner[k][0] > 0.0f ? 1.0f : 0.0f,
                        corner[k][1] > 0.0f ? 1.0f : 0.0f };
                }
                visible[eye_index] = ok;
            }
            if (!visible[0] && !visible[1])
                return;
            static bool reported = false;
            if (!reported)
            {
                reported = true;
                log("grab badge: shown on the grab zone (head %.0f, %.0f, "
                    "%.0f)", centre[0], centre[1], centre[2]);
            }
            IDirect3DStateBlock9* state = nullptr;
            IDirect3DSurface9* old_target = nullptr;
            D3DVIEWPORT9 old_viewport{};
            if (FAILED(g_device->CreateStateBlock(D3DSBT_ALL, &state)) ||
                !state)
                return;
            state->Capture();
            g_device->GetRenderTarget(0, &old_target);
            g_device->GetViewport(&old_viewport);
            if (SUCCEEDED(g_device->SetRenderTarget(0, back)))
            {
                const D3DVIEWPORT9 viewport = { 0, 0, desc.Width,
                                                desc.Height, 0.0f, 1.0f };
                g_device->SetViewport(&viewport);
                g_device->SetVertexShader(nullptr);
                g_device->SetPixelShader(nullptr);
                g_device->SetTexture(0, texture);
                g_device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE |
                                 D3DFVF_TEX1);
                g_device->SetRenderState(D3DRS_ZENABLE, FALSE);
                g_device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
                g_device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
                g_device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
                g_device->SetRenderState(D3DRS_DESTBLEND,
                                         D3DBLEND_INVSRCALPHA);
                g_device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
                g_device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
                g_device->SetRenderState(D3DRS_LIGHTING, FALSE);
                g_device->SetRenderState(D3DRS_FOGENABLE, FALSE);
                g_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
                g_device->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
                g_device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
                g_device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
                g_device->SetSamplerState(0, D3DSAMP_MINFILTER,
                                          D3DTEXF_LINEAR);
                g_device->SetSamplerState(0, D3DSAMP_MAGFILTER,
                                          D3DTEXF_LINEAR);
                g_device->SetSamplerState(0, D3DSAMP_MIPFILTER,
                                          D3DTEXF_NONE);
                g_device->SetSamplerState(0, D3DSAMP_ADDRESSU,
                                          D3DTADDRESS_CLAMP);
                g_device->SetSamplerState(0, D3DSAMP_ADDRESSV,
                                          D3DTADDRESS_CLAMP);
                g_device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
                const DWORD op = texture ? D3DTOP_MODULATE
                                         : D3DTOP_SELECTARG2;
                g_device->SetTextureStageState(0, D3DTSS_COLOROP, op);
                g_device->SetTextureStageState(0, D3DTSS_COLORARG1,
                                               D3DTA_TEXTURE);
                g_device->SetTextureStageState(0, D3DTSS_COLORARG2,
                                               D3DTA_DIFFUSE);
                g_device->SetTextureStageState(0, D3DTSS_ALPHAOP, op);
                g_device->SetTextureStageState(0, D3DTSS_ALPHAARG1,
                                               D3DTA_TEXTURE);
                g_device->SetTextureStageState(0, D3DTSS_ALPHAARG2,
                                               D3DTA_DIFFUSE);
                g_device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
                g_device->SetTextureStageState(0,
                    D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
                g_device->SetTextureStageState(1, D3DTSS_COLOROP,
                                               D3DTOP_DISABLE);
                g_device->SetTextureStageState(1, D3DTSS_ALPHAOP,
                                               D3DTOP_DISABLE);
                if (SUCCEEDED(g_device->BeginScene()))
                {
                    for (int eye_index = 0; eye_index < 2; ++eye_index)
                    {
                        if (!visible[eye_index])
                            continue;
                        const RECT scissor = {
                            (LONG)(eye_w * eye_index), 0,
                            (LONG)(eye_w * (eye_index + 1)),
                            (LONG)desc.Height };
                        g_device->SetScissorRect(&scissor);
                        g_device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2,
                            quads[eye_index], sizeof(BadgeVertex));
                    }
                    g_device->EndScene();
                }
            }
            if (old_target)
            {
                g_device->SetRenderTarget(0, old_target);
                old_target->Release();
            }
            state->Apply();
            g_device->SetViewport(&old_viewport);
            state->Release();
        }

        // Third-person gear cross: four round icons in a d-pad layout,
        // fixed to the body in front of the waist (vr_input owns the layout
        // and the grabbing). Head-facing billboards in each eye, joined by
        // thin spokes to the centre.
        IDirect3DTexture9* g_gear_icons[4] = {};
        IDirect3DTexture9* g_gear_handle = nullptr;
        // Rounded tile behind a game icon (the retail cross shows white
        // icons on rounded squares).
        IDirect3DTexture9* g_gear_tile = nullptr;
        // Cross item -> hud_gear_icon index (up medipack, right binoculars,
        // down weapon switch, left flashlight).
        const int kGearGameIcon[4] = { 0, 1, 2, 3 };

        // Personal light charge 0..1 from the retail HUD's game value 0x44
        // (Hud::draw 0x00454559: GEAR_Query(PlayerData+0x428, 0x12B); the
        // game warns at <= 10, so 100 is taken as full). -1 when Lara has
        // no light.
        float personal_light_charge()
        {
            __try
            {
                const unsigned char* player =
                    *reinterpret_cast<unsigned char* const*>(0x0111713C);
                if (!player ||
                    !*reinterpret_cast<void* const*>(player + 0x428))
                    return -1.0f;
                const float v = *reinterpret_cast<const float*>(
                    0x00F147D0 + 0x44 * 8);
                if (!std::isfinite(v))
                    return -1.0f;
                static float reported = -1000.0f;
                if (fabsf(v - reported) >= 10.0f)
                {
                    reported = v;
                    log("gear cross: personal light charge value %.1f", v);
                }
                return v <= 0.0f ? 0.0f : v >= 100.0f ? 1.0f : v / 100.0f;
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                return -1.0f;
            }
        }
        bool g_gear_icons_tried = false;

        void draw_gear_cross(IDirect3DSurface9* back)
        {
            VrGearCross cross{};
            if (!g_device || !stereo_same_frame_active() ||
                camera_first_person_active() ||
                !vr_input_gear_cross(&cross) || !cross.visible)
                return;
            D3DSURFACE_DESC desc{};
            if (FAILED(back->GetDesc(&desc)) || desc.Width < 2 ||
                !desc.Height)
                return;
            const float metre = tune_world_scale();
            if (!(metre > 0.0f))
                return;
            if (!g_gear_icons_tried)
            {
                g_gear_icons_tried = true;
                const D2D1_COLOR_F slate = D2D1::ColorF(0.16f, 0.18f, 0.22f);
                const D2D1_COLOR_F rim = D2D1::ColorF(0.85f, 0.85f, 0.85f);
                // Order matches vr_input: up, right, down, left. Emoji
                // stand-ins until the game's own icons are captured (F9).
                g_gear_icons[0] = make_icon_texture(L"\u271A",
                    D2D1::ColorF(0.95f, 0.95f, 0.95f),
                    D2D1::ColorF(0.75f, 0.1f, 0.1f),
                    D2D1::ColorF(0.85f, 0.08f, 0.08f), "gear medipack");
                g_gear_icons[1] = make_icon_texture(L"\U0001F52D", slate, rim,
                    D2D1::ColorF(0.8f, 0.8f, 0.9f), "gear binoculars");
                g_gear_icons[2] = make_icon_texture(L"\U0001F52B", slate, rim,
                    D2D1::ColorF(0.8f, 0.8f, 0.8f), "gear weapon switch");
                g_gear_icons[3] = make_icon_texture(L"\U0001F526", slate, rim,
                    D2D1::ColorF(1.0f, 0.9f, 0.4f), "gear flashlight");
                // A rounded navy tile with a white rim for the game icons.
                g_gear_tile = make_icon_texture(L" ",
                    D2D1::ColorF(0.10f, 0.16f, 0.27f, 0.80f),
                    D2D1::ColorF(0.92f, 0.92f, 0.92f),
                    D2D1::ColorF(0, 0, 0, 0), "gear tile");
                // Handle: three horizontal bars, no disc.
                g_gear_handle = make_icon_texture(L"",
                    D2D1::ColorF(0, 0, 0, 0), D2D1::ColorF(0.1f, 0.1f, 0.1f),
                    D2D1::ColorF(0.92f, 0.92f, 0.92f), "gear handle");
            }
            const float eye_w = desc.Width * 0.5f;
            const float charge = personal_light_charge();
            // Opacity (user, 2026-10-02): 20% at rest, up to 60% as the
            // gaze centres on the cross (sliding between 35 and 8 degrees
            // off the view axis), and the remaining 40% from a hand's
            // nearness. Spokes, icons, handle and charge bar all use it.
            float opacity = 0.2f + 0.4f * cross.nearness;
            {
                float hub_head[3]{};
                if (vr_body_point_head_position(cross.centre[0],
                        cross.centre[1], cross.centre[2], hub_head))
                {
                    const float length = sqrtf(hub_head[0] * hub_head[0] +
                        hub_head[1] * hub_head[1] + hub_head[2] * hub_head[2]);
                    const float cosine = length > 1e-4f
                        ? hub_head[2] / length : 0.0f;
                    const float wide = 0.81915204f;   // cos 35 deg
                    const float narrow = 0.99026807f; // cos 8 deg
                    float gaze = (cosine - wide) / (narrow - wide);
                    gaze = gaze < 0.0f ? 0.0f : gaze > 1.0f ? 1.0f : gaze;
                    gaze = gaze * gaze * (3.0f - 2.0f * gaze);
                    opacity += 0.4f * gaze;
                }
                opacity = opacity > 1.0f ? 1.0f : opacity;
            }
            // Spokes from the centre to each icon (behind the icons).
            {
                static DebugLineVertex lines[2 * 4 * 2];
                unsigned used = 0;
                float hub[3]{};
                if (vr_body_point_head_position(cross.centre[0],
                        cross.centre[1], cross.centre[2], hub))
                {
                    // Lines draw opaque: dim the colour instead.
                    const DWORD grey = 0xFF000000u |
                        ((DWORD)(0x9A * opacity) << 16) |
                        ((DWORD)(0xA0 * opacity) << 8) |
                        (DWORD)(0xA8 * opacity);
                    for (int item = 0; item < 4; ++item)
                    {
                        float tip[3]{};
                        if (!vr_body_point_head_position(cross.items[item][0],
                                cross.items[item][1], cross.items[item][2],
                                tip))
                            continue;
                        for (int eye_index = 0; eye_index < 2; ++eye_index)
                        {
                            const Eye eye = eye_index ? EyeRight : EyeLeft;
                            float a[2], b[2];
                            if (!vr_project_head_point(eye, hub[0], hub[1],
                                    hub[2], &a[0], &a[1]) ||
                                !vr_project_head_point(eye, tip[0], tip[1],
                                    tip[2], &b[0], &b[1]))
                                continue;
                            lines[used++] = { eye_width_px(eye_w, eye_index,
                                a[0]), desc.Height * a[1], 0.0f, 1.0f, grey };
                            lines[used++] = { eye_width_px(eye_w, eye_index,
                                b[0]), desc.Height * b[1], 0.0f, 1.0f, grey };
                        }
                    }
                }
                draw_hand_lines(back, desc, lines, used);
            }
            IDirect3DStateBlock9* state = nullptr;
            IDirect3DSurface9* old_target = nullptr;
            D3DVIEWPORT9 old_viewport{};
            if (FAILED(g_device->CreateStateBlock(D3DSBT_ALL, &state)) ||
                !state)
                return;
            state->Capture();
            g_device->GetRenderTarget(0, &old_target);
            g_device->GetViewport(&old_viewport);
            if (SUCCEEDED(g_device->SetRenderTarget(0, back)))
            {
                const D3DVIEWPORT9 viewport = { 0, 0, desc.Width,
                                                desc.Height, 0.0f, 1.0f };
                g_device->SetViewport(&viewport);
                g_device->SetVertexShader(nullptr);
                g_device->SetPixelShader(nullptr);
                g_device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE |
                                 D3DFVF_TEX1);
                g_device->SetRenderState(D3DRS_ZENABLE, FALSE);
                g_device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
                g_device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
                g_device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
                g_device->SetRenderState(D3DRS_DESTBLEND,
                                         D3DBLEND_INVSRCALPHA);
                g_device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
                g_device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
                g_device->SetRenderState(D3DRS_LIGHTING, FALSE);
                g_device->SetRenderState(D3DRS_FOGENABLE, FALSE);
                g_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
                g_device->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
                g_device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
                g_device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
                g_device->SetSamplerState(0, D3DSAMP_MINFILTER,
                                          D3DTEXF_LINEAR);
                g_device->SetSamplerState(0, D3DSAMP_MAGFILTER,
                                          D3DTEXF_LINEAR);
                g_device->SetSamplerState(0, D3DSAMP_MIPFILTER,
                                          D3DTEXF_NONE);
                g_device->SetSamplerState(0, D3DSAMP_ADDRESSU,
                                          D3DTADDRESS_CLAMP);
                g_device->SetSamplerState(0, D3DSAMP_ADDRESSV,
                                          D3DTADDRESS_CLAMP);
                g_device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
                g_device->SetTextureStageState(0, D3DTSS_COLORARG1,
                                               D3DTA_TEXTURE);
                g_device->SetTextureStageState(0, D3DTSS_COLORARG2,
                                               D3DTA_DIFFUSE);
                g_device->SetTextureStageState(0, D3DTSS_ALPHAARG1,
                                               D3DTA_TEXTURE);
                g_device->SetTextureStageState(0, D3DTSS_ALPHAARG2,
                                               D3DTA_DIFFUSE);
                g_device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
                g_device->SetTextureStageState(0,
                    D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
                g_device->SetTextureStageState(1, D3DTSS_COLOROP,
                                               D3DTOP_DISABLE);
                g_device->SetTextureStageState(1, D3DTSS_ALPHAOP,
                                               D3DTOP_DISABLE);
                if (SUCCEEDED(g_device->BeginScene()))
                {
                    for (int item = 0; item < 5; ++item)
                    {
                        // 0..3 the gear icons, 4 the middle handle.
                        const float* body = item < 4 ? cross.items[item]
                                                     : cross.centre;
                        float centre[3]{};
                        if (!vr_body_point_head_position(body[0], body[1],
                                                         body[2], centre))
                            continue;
                        const bool hovered = item < 4
                            ? cross.hover[0] == item || cross.hover[1] == item
                            : cross.handle_hover || cross.dragging;
                        const bool flash = cross.flash_item == item &&
                                           cross.flash > 0.0f;
                        float radius = (item < 4 ? 0.04f : 0.03f) * metre *
                            (hovered ? 1.3f : 1.0f) *
                            (1.0f + 0.25f * (flash ? cross.flash : 0.0f));
                        // The cross is drawn over the finished frame, so a
                        // hand in front of it did not hide it (user,
                        // 2026-10-04). Fade an icon behind a hand: the hand
                        // as a 9 cm disc about its grip, nearer the eye
                        // by 2 cm or more; by how much the discs overlap.
                        float covered = 1.0f;
                        {
                            const float ic = sqrtf(centre[0] * centre[0] +
                                centre[1] * centre[1] + centre[2] * centre[2]);
                            for (int h = 0; h < 2 && ic > 1e-3f; ++h)
                            {
                                Mat4 pose;
                                if (!vr_controller_head_pose(h == 0, &pose))
                                    continue;
                                const float* p = pose.m[3];
                                const float hc = sqrtf(p[0] * p[0] +
                                    p[1] * p[1] + p[2] * p[2]);
                                if (hc < 1e-3f || hc > ic - 0.02f * metre)
                                    continue;
                                float cosine = (p[0] * centre[0] +
                                    p[1] * centre[1] + p[2] * centre[2]) /
                                    (hc * ic);
                                cosine = cosine > 1.0f ? 1.0f
                                       : cosine < -1.0f ? -1.0f : cosine;
                                const float apart = acosf(cosine);
                                const float icon_r = atanf(radius / ic);
                                const float hand_r =
                                    atanf(0.09f * metre / hc);
                                const float outer = icon_r + hand_r;
                                const float inner =
                                    fmaxf(hand_r - icon_r, 0.0f);
                                float t = outer > inner
                                    ? (apart - inner) / (outer - inner)
                                    : 1.0f;
                                t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
                                covered = fminf(covered, 0.08f + 0.92f * t);
                            }
                            if (hovered)
                                covered = fmaxf(covered, 0.35f);
                        }
                        const float a = (flash ? 1.0f : opacity) * covered;
                        const DWORD alpha = (DWORD)(255.0f * a);
                        IDirect3DTexture9* game_icon = item < 4
                            ? hud_gear_icon(g_device, kGearGameIcon[item])
                            : nullptr;
                        IDirect3DTexture9* texture = item < 4
                            ? (game_icon && g_gear_tile ? g_gear_tile
                                                        : g_gear_icons[item])
                            : g_gear_handle;
                        // Premultiplied texture: scale all four channels;
                        // without a texture, a plain grey disc quad.
                        const DWORD shade = texture
                            ? (alpha << 24) | (alpha << 16) | (alpha << 8) |
                              alpha
                            : (alpha << 24) | ((alpha * 6 / 10) << 16) |
                              ((alpha * 6 / 10) << 8) | (alpha * 6 / 10);
                        const DWORD op = texture ? D3DTOP_MODULATE
                                                 : D3DTOP_SELECTARG2;
                        g_device->SetTexture(0, texture);
                        g_device->SetTextureStageState(0, D3DTSS_COLOROP, op);
                        g_device->SetTextureStageState(0, D3DTSS_ALPHAOP, op);
                        const float corner[4][2] = {
                            { -1.0f, -1.0f }, { 1.0f, -1.0f },
                            { -1.0f, 1.0f }, { 1.0f, 1.0f } };
                        for (int eye_index = 0; eye_index < 2; ++eye_index)
                        {
                            const Eye eye = eye_index ? EyeRight : EyeLeft;
                            BadgeVertex quad[4]{};
                            bool ok = true;
                            for (int k = 0; k < 4 && ok; ++k)
                            {
                                float u = 0.0f, v = 0.0f;
                                ok = vr_project_head_point(eye,
                                    centre[0] + corner[k][0] * radius,
                                    centre[1] + corner[k][1] * radius,
                                    centre[2], &u, &v) && u > -1.0f &&
                                    u < 2.0f && v > -1.0f && v < 2.0f;
                                quad[k] = { eye_width_px(eye_w, eye_index, u),
                                    desc.Height * v, 0.0f, 1.0f, shade,
                                    corner[k][0] > 0.0f ? 1.0f : 0.0f,
                                    corner[k][1] > 0.0f ? 1.0f : 0.0f };
                            }
                            if (!ok)
                                continue;
                            const RECT scissor = {
                                (LONG)(eye_w * eye_index), 0,
                                (LONG)(eye_w * (eye_index + 1)),
                                (LONG)desc.Height };
                            g_device->SetScissorRect(&scissor);
                            g_device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2,
                                quad, sizeof(BadgeVertex));
                            if (game_icon && g_gear_tile)
                            {
                                // The game's white icon on the tile, inset.
                                g_device->SetTexture(0, game_icon);
                                BadgeVertex inner[4];
                                for (int k = 0; k < 4; ++k)
                                {
                                    inner[k] = quad[k];
                                    const float cx = 0.25f * (quad[0].x +
                                        quad[1].x + quad[2].x + quad[3].x);
                                    const float cy = 0.25f * (quad[0].y +
                                        quad[1].y + quad[2].y + quad[3].y);
                                    inner[k].x = cx + (quad[k].x - cx) * 0.72f;
                                    inner[k].y = cy + (quad[k].y - cy) * 0.72f;
                                }
                                g_device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,
                                    2, inner, sizeof(BadgeVertex));
                                g_device->SetTexture(0, texture);
                            }
                            // Flashlight: the retail yellow charge bar
                            // under the icon.
                            if (item == 3 && charge >= 0.0f)
                            {
                                const float left_x = quad[2].x;
                                const float right_x = quad[3].x;
                                const float top = quad[2].y +
                                    (quad[2].y - quad[0].y) * 0.08f;
                                const float height =
                                    (quad[2].y - quad[0].y) * 0.14f;
                                const float split = left_x +
                                    (right_x - left_x) * charge;
                                const DWORD back_colour =
                                    ((alpha * 7 / 10) << 24) |
                                    ((alpha * 2 / 10) << 16) |
                                    ((alpha * 2 / 10) << 8) |
                                    (alpha * 2 / 10);
                                const DWORD yellow_colour = (alpha << 24) |
                                    (alpha << 16) | ((alpha * 82 / 100) << 8) |
                                    (alpha * 10 / 100);
                                auto bar = [&](float x0, float x1, DWORD c) {
                                    if (x1 <= x0)
                                        return;
                                    BadgeVertex b[4] = {
                                        { x0, top, 0, 1, c, 0, 0 },
                                        { x1, top, 0, 1, c, 0, 0 },
                                        { x0, top + height, 0, 1, c, 0, 0 },
                                        { x1, top + height, 0, 1, c, 0, 0 } };
                                    g_device->DrawPrimitiveUP(
                                        D3DPT_TRIANGLESTRIP, 2, b,
                                        sizeof(BadgeVertex));
                                };
                                g_device->SetTexture(0, nullptr);
                                g_device->SetTextureStageState(0,
                                    D3DTSS_COLOROP, D3DTOP_SELECTARG2);
                                g_device->SetTextureStageState(0,
                                    D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
                                bar(left_x, right_x, back_colour);
                                bar(left_x, split, yellow_colour);
                                g_device->SetTexture(0, texture);
                                g_device->SetTextureStageState(0,
                                    D3DTSS_COLOROP, op);
                                g_device->SetTextureStageState(0,
                                    D3DTSS_ALPHAOP, op);
                            }
                        }
                    }
                    g_device->EndScene();
                }
            }
            if (old_target)
            {
                g_device->SetRenderTarget(0, old_target);
                old_target->Release();
            }
            state->Apply();
            g_device->SetViewport(&old_viewport);
            state->Release();
        }

        // Board-mode pick-up ring: yellow around Lara when a hand can take
        // her, green on the floor where she will land, red when there is
        // no floor below.
        void draw_pluck_ring(IDirect3DSurface9* back)
        {
            float points[24][3]{};
            const int state = stereo_same_frame_active()
                ? camera_board_pluck_ring(points) : 0;
            if (!state)
                return;
            D3DSURFACE_DESC desc{};
            if (FAILED(back->GetDesc(&desc)) || desc.Width < 2 ||
                !desc.Height)
                return;
            const DWORD colour = state == 1 ? 0xFFFFD54Au
                               : state == 2 ? 0xFF5AFF66u : 0xFFFF4040u;
            const float eye_w = desc.Width * 0.5f;
            static DebugLineVertex lines[2 * 24 * 2];
            unsigned used = 0;
            for (int eye_index = 0; eye_index < 2; ++eye_index)
            {
                const Eye eye = eye_index ? EyeRight : EyeLeft;
                float uv[24][2]{};
                bool ok[24]{};
                for (int i = 0; i < 24; ++i)
                    ok[i] = vr_project_head_point(eye, points[i][0],
                        points[i][1], points[i][2], &uv[i][0], &uv[i][1]) &&
                        uv[i][0] > -0.5f && uv[i][0] < 1.5f &&
                        uv[i][1] > -0.5f && uv[i][1] < 1.5f;
                for (int i = 0; i < 24; ++i)
                {
                    const int j = (i + 1) % 24;
                    if (!ok[i] || !ok[j])
                        continue;
                    lines[used++] = { eye_w * (eye_index + uv[i][0]),
                        desc.Height * uv[i][1], 0.0f, 1.0f, colour };
                    lines[used++] = { eye_w * (eye_index + uv[j][0]),
                        desc.Height * uv[j][1], 0.0f, 1.0f, colour };
                }
            }
            draw_hand_lines(back, desc, lines, used);
        }

        // Pouch items as outlines: the medipack (white box, red cross) on the
        // chest and the grenade/flare (olive canister) on the belt, or in
        // the hand that holds one. Axis-aligned to the head frame (X right,
        // Y down, Z forward, game units), projected into each eye.
        // A text panel texture (white text on a dark rounded panel), drawn
        // with Direct2D like the icons. w x h pixels; the first line is a
        // larger title.
        IDirect3DTexture9* make_text_texture(const wchar_t* title,
                                             const wchar_t* body, UINT w,
                                             UINT h)
        {
            if (!g_device || !title || !body)
                return nullptr;
            typedef HRESULT(WINAPI* PFN_D2D1CreateFactory)(
                D2D1_FACTORY_TYPE, REFIID, const D2D1_FACTORY_OPTIONS*,
                void**);
            typedef HRESULT(WINAPI* PFN_DWriteCreateFactory)(
                DWRITE_FACTORY_TYPE, REFIID, IUnknown**);
            HMODULE d2d = LoadLibraryW(L"d2d1.dll");
            HMODULE dw = LoadLibraryW(L"dwrite.dll");
            auto create_d2d = d2d ? reinterpret_cast<PFN_D2D1CreateFactory>(
                GetProcAddress(d2d, "D2D1CreateFactory")) : nullptr;
            auto create_dw = dw ? reinterpret_cast<PFN_DWriteCreateFactory>(
                GetProcAddress(dw, "DWriteCreateFactory")) : nullptr;
            // gdi32 is not linked; resolve it as make_icon_texture does.
            HMODULE gdi = LoadLibraryW(L"gdi32.dll");
            typedef HBITMAP(WINAPI* PFN_CreateDIBSection)(HDC,
                const BITMAPINFO*, UINT, void**, HANDLE, DWORD);
            typedef HDC(WINAPI* PFN_CreateCompatibleDC)(HDC);
            typedef HGDIOBJ(WINAPI* PFN_SelectObject)(HDC, HGDIOBJ);
            typedef BOOL(WINAPI* PFN_DeleteObject)(HGDIOBJ);
            typedef BOOL(WINAPI* PFN_DeleteDC)(HDC);
            auto create_dib = gdi ? reinterpret_cast<PFN_CreateDIBSection>(
                GetProcAddress(gdi, "CreateDIBSection")) : nullptr;
            auto create_dc = gdi ? reinterpret_cast<PFN_CreateCompatibleDC>(
                GetProcAddress(gdi, "CreateCompatibleDC")) : nullptr;
            auto select = gdi ? reinterpret_cast<PFN_SelectObject>(
                GetProcAddress(gdi, "SelectObject")) : nullptr;
            auto delete_object = gdi ? reinterpret_cast<PFN_DeleteObject>(
                GetProcAddress(gdi, "DeleteObject")) : nullptr;
            auto delete_dc = gdi ? reinterpret_cast<PFN_DeleteDC>(
                GetProcAddress(gdi, "DeleteDC")) : nullptr;
            if (!create_d2d || !create_dw || !create_dib || !create_dc ||
                !select || !delete_object || !delete_dc)
                return nullptr;
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(info.bmiHeader);
            info.bmiHeader.biWidth = (LONG)w;
            info.bmiHeader.biHeight = -(LONG)h;
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            void* bits = nullptr;
            HDC dc = create_dc(nullptr);
            HBITMAP dib = dc ? create_dib(dc, &info, DIB_RGB_COLORS, &bits,
                                          nullptr, 0) : nullptr;
            HGDIOBJ old = dib ? select(dc, dib) : nullptr;
            ID2D1Factory* factory = nullptr;
            IDWriteFactory* write = nullptr;
            ID2D1DCRenderTarget* target = nullptr;
            IDWriteTextFormat* head = nullptr;
            IDWriteTextFormat* text = nullptr;
            ID2D1SolidColorBrush* panel = nullptr;
            ID2D1SolidColorBrush* rim = nullptr;
            ID2D1SolidColorBrush* ink = nullptr;
            ID2D1SolidColorBrush* gold = nullptr;
            bool drawn = false;
            if (bits &&
                SUCCEEDED(create_d2d(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                    __uuidof(ID2D1Factory), nullptr,
                    reinterpret_cast<void**>(&factory))) &&
                SUCCEEDED(create_dw(DWRITE_FACTORY_TYPE_SHARED,
                    __uuidof(IDWriteFactory),
                    reinterpret_cast<IUnknown**>(&write))))
            {
                const D2D1_RENDER_TARGET_PROPERTIES props =
                    D2D1::RenderTargetProperties(
                        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                          D2D1_ALPHA_MODE_PREMULTIPLIED));
                const RECT rect = { 0, 0, (LONG)w, (LONG)h };
                if (SUCCEEDED(factory->CreateDCRenderTarget(&props,
                                                            &target)) &&
                    SUCCEEDED(target->BindDC(dc, &rect)) &&
                    SUCCEEDED(write->CreateTextFormat(L"Segoe UI", nullptr,
                        DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                        DWRITE_FONT_STRETCH_NORMAL, h * 0.11f, L"",
                        &head)) &&
                    SUCCEEDED(write->CreateTextFormat(L"Segoe UI", nullptr,
                        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                        DWRITE_FONT_STRETCH_NORMAL, h * 0.072f, L"",
                        &text)) &&
                    SUCCEEDED(target->CreateSolidColorBrush(
                        D2D1::ColorF(0.06f, 0.09f, 0.15f, 0.88f), &panel)) &&
                    SUCCEEDED(target->CreateSolidColorBrush(
                        D2D1::ColorF(0.92f, 0.92f, 0.92f), &rim)) &&
                    SUCCEEDED(target->CreateSolidColorBrush(
                        D2D1::ColorF(0.95f, 0.95f, 0.95f), &ink)) &&
                    SUCCEEDED(target->CreateSolidColorBrush(
                        D2D1::ColorF(1.0f, 0.82f, 0.25f), &gold)))
                {
                    head->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                    text->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                    text->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
                    target->BeginDraw();
                    target->Clear(D2D1::ColorF(0, 0, 0, 0));
                    const D2D1_ROUNDED_RECT tile = D2D1::RoundedRect(
                        D2D1::RectF(6.0f, 6.0f, w - 6.0f, h - 6.0f),
                        h * 0.06f, h * 0.06f);
                    target->FillRoundedRectangle(tile, panel);
                    target->DrawRoundedRectangle(tile, rim, 4.0f);
                    target->DrawText(title, (UINT32)wcslen(title), head,
                        D2D1::RectF(20.0f, h * 0.05f, w - 20.0f, h * 0.20f),
                        gold);
                    target->DrawText(body, (UINT32)wcslen(body), text,
                        D2D1::RectF(30.0f, h * 0.22f, w - 30.0f, h - 20.0f),
                        ink);
                    drawn = SUCCEEDED(target->EndDraw());
                }
            }
            if (gold) gold->Release();
            if (ink) ink->Release();
            if (rim) rim->Release();
            if (panel) panel->Release();
            if (text) text->Release();
            if (head) head->Release();
            if (target) target->Release();
            if (write) write->Release();
            if (factory) factory->Release();
            IDirect3DTexture9* texture = nullptr;
            if (drawn && SUCCEEDED(g_device->CreateTexture(w, h, 1, 0,
                    D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr)))
            {
                D3DLOCKED_RECT locked{};
                if (SUCCEEDED(texture->LockRect(0, &locked, nullptr, 0)))
                {
                    for (UINT y = 0; y < h; ++y)
                        memcpy(static_cast<unsigned char*>(locked.pBits) +
                                   y * locked.Pitch,
                               static_cast<unsigned char*>(bits) + y * w * 4,
                               w * 4);
                    texture->UnlockRect(0);
                }
                else
                {
                    texture->Release();
                    texture = nullptr;
                }
            }
            if (old)
                select(dc, old);
            if (dib)
                delete_object(dib);
            if (dc)
                delete_dc(dc);
            return texture;
        }

        // Holster setup instructions (user, 2026-10-04): a panel floating
        // 0.6 m in front (yaw-only body frame), saying what each hand holds
        // and how to place it. Rebuilt when the text changes.
        IDirect3DTexture9* g_tuning_panel = nullptr;
        wchar_t g_tuning_panel_text[512] = {};

        void draw_gear_tuning_panel(IDirect3DSurface9* back)
        {
            if (!g_device || !stereo_same_frame_active() ||
                !tune_gear_tuning_active())
                return;
            D3DSURFACE_DESC desc{};
            if (FAILED(back->GetDesc(&desc)) || desc.Width < 2 ||
                !desc.Height)
                return;
            const float metre = tune_world_scale();
            if (!(metre > 0.0f))
                return;
            wchar_t hands[2][96]{};
            for (int hand = 0; hand < 2; ++hand)
            {
                const int gear = tune_gear_tuning_slot(hand);
                if (gear < 0)
                    swprintf_s(hands[hand], L"%s hand: nothing this step",
                               hand ? L"Right" : L"Left");
                else
                    swprintf_s(hands[hand], L"%s hand: %S%s",
                               hand ? L"Right" : L"Left",
                               tune_gear_name(gear),
                               tune_gear_tuning_placed(gear)
                                   ? L"  (placed)" : L"");
            }
            wchar_t body[512]{};
            swprintf_s(body,
                L"Step %d of %d\n%s\n%s\n\nMove each item to where it feels "
                L"comfortable on your body and pull that hand's trigger to "
                L"place it. Pull again to pick it back up.\nThe last item "
                L"saves. Double-click the right stick to cancel.",
                tune_gear_tuning_step() + 1, tune_gear_tuning_step_count(),
                hands[0], hands[1]);
            if (!g_tuning_panel || wcscmp(body, g_tuning_panel_text) != 0)
            {
                if (g_tuning_panel)
                {
                    g_tuning_panel->Release();
                    g_tuning_panel = nullptr;
                }
                wcscpy_s(g_tuning_panel_text, body);
                g_tuning_panel = make_text_texture(L"VR Holster Setup", body,
                                                   1024, 640);
                if (!g_tuning_panel)
                    return;
            }
            float centre[3]{};
            if (!vr_body_point_head_position(0.0f, -0.05f, 0.60f, centre))
                return;
            const float half_w = 0.25f * metre, half_h = 0.156f * metre;
            const float eye_w = desc.Width * 0.5f;
            IDirect3DStateBlock9* state = nullptr;
            if (FAILED(g_device->CreateStateBlock(D3DSBT_ALL, &state)) ||
                !state)
                return;
            state->Capture();
            IDirect3DSurface9* old_target = nullptr;
            g_device->GetRenderTarget(0, &old_target);
            if (SUCCEEDED(g_device->SetRenderTarget(0, back)))
            {
                const D3DVIEWPORT9 viewport = { 0, 0, desc.Width,
                                                desc.Height, 0.0f, 1.0f };
                g_device->SetViewport(&viewport);
                g_device->SetVertexShader(nullptr);
                g_device->SetPixelShader(nullptr);
                g_device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE |
                                 D3DFVF_TEX1);
                g_device->SetRenderState(D3DRS_ZENABLE, FALSE);
                g_device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
                g_device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
                g_device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
                g_device->SetRenderState(D3DRS_DESTBLEND,
                                         D3DBLEND_INVSRCALPHA);
                g_device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
                g_device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
                g_device->SetRenderState(D3DRS_LIGHTING, FALSE);
                g_device->SetRenderState(D3DRS_FOGENABLE, FALSE);
                g_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
                g_device->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
                g_device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
                g_device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
                g_device->SetSamplerState(0, D3DSAMP_MINFILTER,
                                          D3DTEXF_LINEAR);
                g_device->SetSamplerState(0, D3DSAMP_MAGFILTER,
                                          D3DTEXF_LINEAR);
                g_device->SetSamplerState(0, D3DSAMP_MIPFILTER,
                                          D3DTEXF_NONE);
                g_device->SetSamplerState(0, D3DSAMP_ADDRESSU,
                                          D3DTADDRESS_CLAMP);
                g_device->SetSamplerState(0, D3DSAMP_ADDRESSV,
                                          D3DTADDRESS_CLAMP);
                g_device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
                g_device->SetTextureStageState(0, D3DTSS_COLOROP,
                                               D3DTOP_MODULATE);
                g_device->SetTextureStageState(0, D3DTSS_COLORARG1,
                                               D3DTA_TEXTURE);
                g_device->SetTextureStageState(0, D3DTSS_COLORARG2,
                                               D3DTA_DIFFUSE);
                g_device->SetTextureStageState(0, D3DTSS_ALPHAOP,
                                               D3DTOP_MODULATE);
                g_device->SetTextureStageState(0, D3DTSS_ALPHAARG1,
                                               D3DTA_TEXTURE);
                g_device->SetTextureStageState(0, D3DTSS_ALPHAARG2,
                                               D3DTA_DIFFUSE);
                g_device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
                g_device->SetTextureStageState(0,
                    D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
                g_device->SetTextureStageState(1, D3DTSS_COLOROP,
                                               D3DTOP_DISABLE);
                g_device->SetTextureStageState(1, D3DTSS_ALPHAOP,
                                               D3DTOP_DISABLE);
                g_device->SetTexture(0, g_tuning_panel);
                if (SUCCEEDED(g_device->BeginScene()))
                {
                    const float corner[4][2] = {
                        { -1.0f, -1.0f }, { 1.0f, -1.0f },
                        { -1.0f, 1.0f }, { 1.0f, 1.0f } };
                    for (int eye_index = 0; eye_index < 2; ++eye_index)
                    {
                        const Eye eye = eye_index ? EyeRight : EyeLeft;
                        BadgeVertex quad[4]{};
                        bool ok = true;
                        for (int k = 0; k < 4 && ok; ++k)
                        {
                            float u = 0.0f, v = 0.0f;
                            ok = vr_project_head_point(eye,
                                centre[0] + corner[k][0] * half_w,
                                centre[1] + corner[k][1] * half_h,
                                centre[2], &u, &v) && u > -1.0f &&
                                u < 2.0f && v > -1.0f && v < 2.0f;
                            quad[k] = { eye_width_px(eye_w, eye_index, u),
                                desc.Height * v, 0.0f, 1.0f, 0xFFFFFFFFu,
                                corner[k][0] > 0.0f ? 1.0f : 0.0f,
                                corner[k][1] > 0.0f ? 1.0f : 0.0f };
                        }
                        if (!ok)
                            continue;
                        const RECT scissor = {
                            (LONG)(eye_w * eye_index), 0,
                            (LONG)(eye_w * (eye_index + 1)),
                            (LONG)desc.Height };
                        g_device->SetScissorRect(&scissor);
                        g_device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2,
                            quad, sizeof(BadgeVertex));
                    }
                    g_device->EndScene();
                }
            }
            if (old_target)
            {
                g_device->SetRenderTarget(0, old_target);
                old_target->Release();
            }
            state->Apply();
            state->Release();
        }

        void draw_pouch_items(IDirect3DSurface9* back)
        {
            if (!stereo_same_frame_active() || !camera_first_person_active() ||
                !config().immersive_controls)
                return;
            D3DSURFACE_DESC desc{};
            if (FAILED(back->GetDesc(&desc)) || desc.Width < 2 ||
                !desc.Height)
                return;
            const float metre = tune_world_scale();
            if (!(metre > 0.0f))
                return;
            static DebugLineVertex lines[2 * 2 * 160];
            unsigned used = 0;
            const float eye_width = desc.Width * 0.5f;
            for (int item = 0; item < 2; ++item)
            {
                float body[3]{};
                int held = -1;
                if (!vr_input_pouch_item(item, body, &held))
                    continue;
                float centre[3]{};
                if (held >= 0)
                {
                    Mat4 pose;
                    if (!vr_controller_head_pose(held == 0, &pose))
                        continue;
                    for (int k = 0; k < 3; ++k)
                        centre[k] = pose.m[3][k];
                }
                else if (!vr_body_point_head_position(body[0], body[1],
                                                      body[2], centre))
                    continue;
                // Segments in metres relative to the centre.
                float seg[64][2][3];
                DWORD colour[64];
                int n = 0;
                auto add = [&](float x0, float y0, float z0, float x1,
                               float y1, float z1, DWORD c) {
                    if (n >= 64)
                        return;
                    seg[n][0][0] = x0; seg[n][0][1] = y0; seg[n][0][2] = z0;
                    seg[n][1][0] = x1; seg[n][1][1] = y1; seg[n][1][2] = z1;
                    colour[n++] = c;
                };
                if (item == 1)
                {
                    // Medipack 8 x 5 x 3 cm, red cross on the front face.
                    const float hx = 0.04f, hy = 0.025f, hz = 0.015f;
                    const DWORD white = 0xFFF0F0F0u, red = 0xFFFF2020u;
                    for (int a = 0; a < 2; ++a)
                    {
                        const float z = a ? hz : -hz;
                        add(-hx, -hy, z, hx, -hy, z, white);
                        add(hx, -hy, z, hx, hy, z, white);
                        add(hx, hy, z, -hx, hy, z, white);
                        add(-hx, hy, z, -hx, -hy, z, white);
                    }
                    add(-hx, -hy, -hz, -hx, -hy, hz, white);
                    add(hx, -hy, -hz, hx, -hy, hz, white);
                    add(hx, hy, -hz, hx, hy, hz, white);
                    add(-hx, hy, -hz, -hx, hy, hz, white);
                    for (int t = -1; t <= 1; ++t)
                    {
                        const float o = 0.002f * t;
                        add(-0.018f, o, -hz - 0.001f, 0.018f, o,
                            -hz - 0.001f, red);
                        add(o, -0.018f, -hz - 0.001f, o, 0.018f,
                            -hz - 0.001f, red);
                    }
                }
                else
                {
                    // Grenade: octagonal canister 5 cm across, 7 cm tall,
                    // with a lever on top.
                    const DWORD olive = 0xFF7FA040u, grey = 0xFFC0C0C0u;
                    const float r = 0.025f, hy = 0.035f;
                    for (int i = 0; i < 8; ++i)
                    {
                        const float a0 = 0.785398f * i;
                        const float a1 = 0.785398f * (i + 1);
                        const float x0 = r * cosf(a0), z0 = r * sinf(a0);
                        const float x1 = r * cosf(a1), z1 = r * sinf(a1);
                        add(x0, -hy, z0, x1, -hy, z1, olive);
                        add(x0, hy, z0, x1, hy, z1, olive);
                        if (i % 2 == 0)
                            add(x0, -hy, z0, x0, hy, z0, olive);
                    }
                    add(0.0f, -hy, 0.0f, 0.0f, -hy - 0.015f, 0.0f, grey);
                    add(0.0f, -hy - 0.015f, 0.0f, 0.02f, -hy - 0.01f,
                        -0.01f, grey);
                }
                for (int eye_index = 0; eye_index < 2; ++eye_index)
                {
                    const Eye eye = eye_index ? EyeRight : EyeLeft;
                    for (int s = 0; s < n; ++s)
                    {
                        float uv[2][2];
                        bool ok = true;
                        for (int e = 0; e < 2 && ok; ++e)
                        {
                            const float p[3] = {
                                centre[0] + seg[s][e][0] * metre,
                                centre[1] + seg[s][e][1] * metre,
                                centre[2] + seg[s][e][2] * metre
                            };
                            ok = vr_project_head_point(eye, p[0], p[1], p[2],
                                                       &uv[e][0], &uv[e][1]) &&
                                 uv[e][0] > -0.5f && uv[e][0] < 1.5f &&
                                 uv[e][1] > -0.5f && uv[e][1] < 1.5f;
                        }
                        if (!ok || used + 2 > sizeof(lines) / sizeof(lines[0]))
                            continue;
                        lines[used++] = { eye_width * (eye_index + uv[0][0]),
                                          desc.Height * uv[0][1], 0.0f, 1.0f,
                                          colour[s] };
                        lines[used++] = { eye_width * (eye_index + uv[1][0]),
                                          desc.Height * uv[1][1], 0.0f, 1.0f,
                                          colour[s] };
                    }
                }
            }
            draw_hand_lines(back, desc, lines, used);
        }

        // The VR crosshair: a ring and centre cross at the actual impact
        // point, projected separately into each eye so it converges at the
        // surface the shot will hit. Fixed angular size keeps it readable at
        // any range; it is drawn over the scene like the other overlays.
        void draw_aim_crosshair(IDirect3DSurface9* back)
        {
            float point[3]{};
            int style = 0;
            if (!stereo_same_frame_active() ||
                !camera_aim_crosshair(point, &style))
                return;
            D3DSURFACE_DESC desc{};
            if (FAILED(back->GetDesc(&desc)) || desc.Width < 2 ||
                !desc.Height)
                return;

            const float distance = sqrtf(point[0] * point[0] +
                                         point[1] * point[1] +
                                         point[2] * point[2]);
            if (!std::isfinite(distance) || distance < 1.0f)
                return;
            // Two unit vectors perpendicular to the view ray to the point.
            const float view[3] = { point[0] / distance, point[1] / distance,
                                    point[2] / distance };
            float a[3] = { view[2], 0.0f, -view[0] }; // view x (0,1,0)
            float length = sqrtf(a[0] * a[0] + a[2] * a[2]);
            if (length < 1.0e-4f)
                return;
            a[0] /= length;
            a[2] /= length;
            const float b[3] = {
                view[1] * a[2] - view[2] * a[1],
                view[2] * a[0] - view[0] * a[2],
                view[0] * a[1] - view[1] * a[0]
            };

            // White, red when the ray meets an enemy (the retail reticle's own
            // target classification); accurate aim is slightly larger.
            const DWORD colour = (style & 4) ? 0xFFFF3030u : 0xFFF0F0F0u;
            const float ring = (style & 1) ? 0.0192f : 0.0157f;  // ~1.1/0.9 deg
            const float tick = 0.0045f;
            const float eye_width = desc.Width * 0.5f;
            DebugLineVertex lines[2 * (3 * 24 + 2) * 2]{};
            unsigned used = 0;
            for (int eye_index = 0; eye_index < 2; ++eye_index)
            {
                const Eye eye = eye_index ? EyeRight : EyeLeft;
                auto project = [&](float u_offset, float v_offset,
                                   float* uv) -> bool
                {
                    float p[3]{};
                    for (int i = 0; i < 3; ++i)
                        p[i] = point[i] + distance *
                               (u_offset * a[i] + v_offset * b[i]);
                    return vr_project_head_point(eye, p[0], p[1], p[2],
                                                 &uv[0], &uv[1]) &&
                           uv[0] > -0.5f && uv[0] < 1.5f &&
                           uv[1] > -0.5f && uv[1] < 1.5f;
                };
                auto emit = [&](const float* from, const float* to)
                {
                    lines[used++] = { eye_width * (eye_index + from[0]),
                                      desc.Height * from[1], 0.0f, 1.0f,
                                      colour };
                    lines[used++] = { eye_width * (eye_index + to[0]),
                                      desc.Height * to[1], 0.0f, 1.0f,
                                      colour };
                };
                // Three concentric rings give the line some weight at the
                // headset's resolution.
                for (int band = 0; band < 3; ++band)
                {
                    const float radius = ring * (1.0f + 0.04f * band);
                    for (int step = 0; step < 24; ++step)
                    {
                        const float t0 = 6.28318531f * step / 24.0f;
                        const float t1 = 6.28318531f * (step + 1) / 24.0f;
                        float p0[2]{}, p1[2]{};
                        if (project(radius * cosf(t0), radius * sinf(t0),
                                    p0) &&
                            project(radius * cosf(t1), radius * sinf(t1),
                                    p1))
                            emit(p0, p1);
                    }
                }
                float p0[2]{}, p1[2]{};
                if (project(-tick, 0.0f, p0) && project(tick, 0.0f, p1))
                    emit(p0, p1);
                if (project(0.0f, -tick, p0) && project(0.0f, tick, p1))
                    emit(p0, p1);
            }
            draw_hand_lines(back, desc, lines, used);
        }

        // Aim tuning: a laser along the actual controller aim ray, so it can
        // be rotated onto the gun barrel. Samples are packed near the hand,
        // where the barrel comparison is made, and segments whose ends do not
        // both project (behind the eye or far outside view) are skipped.
        void draw_aim_debug_laser(IDirect3DSurface9* back)
        {
            float origin[3]{}, end[3]{};
            bool hit = false;
            if (!stereo_same_frame_active() ||
                !camera_aim_debug_ray(origin, end, &hit))
                return;
            D3DSURFACE_DESC desc{};
            if (FAILED(back->GetDesc(&desc)) || desc.Width < 2 ||
                !desc.Height)
                return;
            const float eye_width = desc.Width * 0.5f;
            const DWORD colour = hit ? 0xFFFF9A30u : 0xFFFFE040u;
            const int kSegments = 48;
            DebugLineVertex lines[2 * kSegments * 2 * 2]{};
            unsigned used = 0;
            for (int eye_index = 0; eye_index < 2; ++eye_index)
            {
                const Eye eye = eye_index ? EyeRight : EyeLeft;
                float previous[2]{};
                bool have_previous = false;
                for (int step = 0; step <= kSegments; ++step)
                {
                    const float s = static_cast<float>(step) / kSegments;
                    const float t = s * s;
                    float uv[2]{};
                    const bool ok = vr_project_head_point(eye,
                        origin[0] + (end[0] - origin[0]) * t,
                        origin[1] + (end[1] - origin[1]) * t,
                        origin[2] + (end[2] - origin[2]) * t,
                        &uv[0], &uv[1]) &&
                        uv[0] > -0.5f && uv[0] < 1.5f &&
                        uv[1] > -0.5f && uv[1] < 1.5f;
                    if (ok && have_previous)
                        for (int offset = 0; offset < 2; ++offset)
                        {
                            lines[used++] = {
                                eye_width * (eye_index + previous[0]) +
                                    offset, desc.Height * previous[1],
                                0.0f, 1.0f, colour };
                            lines[used++] = {
                                eye_width * (eye_index + uv[0]) + offset,
                                desc.Height * uv[1], 0.0f, 1.0f, colour };
                        }
                    have_previous = ok;
                    previous[0] = uv[0];
                    previous[1] = uv[1];
                }
            }
            draw_hand_lines(back, desc, lines, used);
        }

        bool ensure_eye_targets(IDirect3DSurface9* like)
        {
            D3DSURFACE_DESC d{};
            if (!like || FAILED(like->GetDesc(&d)))
                return false;

            if (g_eye[0] && g_eye_w == d.Width && g_eye_h == d.Height)
                return true;

            for (int i = 0; i < 2; ++i)
            {
                if (g_eye[i])
                {
                    g_eye[i]->Release();
                    g_eye[i] = nullptr;
                }
                g_eye_ready[i] = false;
            }

            for (int i = 0; i < 2; ++i)
            {
                // Lockable = FALSE: these never go back to the CPU, they go
                // straight to the compositor as VkImages.
                const HRESULT hr = g_device->CreateRenderTarget(
                    d.Width, d.Height, d.Format, D3DMULTISAMPLE_NONE, 0,
                    FALSE, &g_eye[i], nullptr);
                if (FAILED(hr) || !g_eye[i])
                {
                    log("submit: could not create the %s eye target %ux%u (0x%08lX)",
                        i == 0 ? "left" : "right", d.Width, d.Height, hr);
                    return false;
                }
            }

            g_eye_w = d.Width;
            g_eye_h = d.Height;
            log("submit: eye targets %ux%u created", d.Width, d.Height);
            return true;
        }

        // Split deliberately into two halves.
        //
        // prepare_surface flushes DXVK's command stream and waits for the GPU.
        // It must NOT be called while the submission queue is locked: DXVK's
        // own submit thread needs that lock to push work to the queue, so
        // waiting for GPU work while holding it deadlocks the process. That is
        // not hypothetical -- it hung on the first stereo frame, every thread
        // waiting, when these two were briefly a single function.
        //
        // So: prepare both eyes first, then take the lock only around the
        // Submit calls, which are quick and do not wait on anything.
        struct EyeSubmission
        {
            vr::VRVulkanTextureData_t vk;
            bool ok;
        };

        EyeSubmission prepare_surface(IDirect3DSurface9* surface)
        {
            EyeSubmission out{};
            out.ok = false;
            if (!surface)
                return out;

            log_set_activity("DXVK TransferSurface");
            const bool transferred =
                SUCCEEDED(g_vr->TransferSurface(surface, TRUE));
            log_set_activity(nullptr);
            if (!transferred)
                return out;

            D3D9_TEXTURE_VR_DESC desc{};
            log_set_activity("DXVK GetVRDesc");
            const bool described = SUCCEEDED(g_vr->GetVRDesc(surface, &desc));
            log_set_activity(nullptr);
            if (!described)
                return out;
            // Never hand the compositor an incomplete description (SiN VR
            // 1.0.1 lesson): every handle set, a real size, one sample.
            if (!desc.Image || !desc.Device || !desc.PhysicalDevice ||
                !desc.Instance || !desc.Queue || !desc.Width ||
                !desc.Height || !desc.Format || desc.SampleCount != 1)
            {
                static bool reported = false;
                if (!reported)
                {
                    reported = true;
                    log("submit: incomplete surface description (image %llX, "
                        "%ux%u, format %u, samples %u) -- frame skipped",
                        (unsigned long long)desc.Image, desc.Width,
                        desc.Height, (unsigned)desc.Format,
                        desc.SampleCount);
                }
                return out;
            }

            out.vk.m_nImage            = desc.Image;
            out.vk.m_pDevice           = (VkDevice_T*)desc.Device;
            out.vk.m_pPhysicalDevice   = (VkPhysicalDevice_T*)desc.PhysicalDevice;
            out.vk.m_pInstance         = (VkInstance_T*)desc.Instance;
            out.vk.m_pQueue            = (VkQueue_T*)desc.Queue;
            out.vk.m_nQueueFamilyIndex = desc.QueueFamilyIndex;
            out.vk.m_nWidth            = desc.Width;
            out.vk.m_nHeight           = desc.Height;
            out.vk.m_nFormat           = (uint32_t)desc.Format;
            out.vk.m_nSampleCount      = desc.SampleCount;
            out.ok = true;
            return out;
        }

        bool submit_prepared(vr::EVREye eye, EyeSubmission& s,
                             const vr::VRTextureBounds_t* bounds = nullptr)
        {
            if (!s.ok)
                return false;

            vr::Texture_t tex{};
            tex.handle      = &s.vk;
            tex.eType       = vr::TextureType_Vulkan;
            tex.eColorSpace = vr::ColorSpace_Auto;

            // Bounds are how a double-wide surface becomes two eyes without
            // a copy: the same image is submitted twice, each time naming
            // the half that belongs to that eye.
            log_set_activity(eye == vr::Eye_Left
                ? "IVRCompositor::Submit (left eye)"
                : "IVRCompositor::Submit (right eye)");
            const bool ok = g_comp->Submit(eye, &tex, bounds,
                                           vr::Submit_Default) ==
                            vr::VRCompositorError_None;
            log_set_activity(nullptr);
            return ok;
        }

        // Both eyes from one double-wide surface.
        bool submit_side_by_side(IDirect3DSurface9* surface)
        {
            EyeSubmission s = prepare_surface(surface);
            if (!s.ok)
                return false;

            vr::VRTextureBounds_t left{ 0.0f, 0.0f, 0.5f, 1.0f };
            vr::VRTextureBounds_t right{ 0.5f, 0.0f, 1.0f, 1.0f };
            if (tune_swap_eyes())
            {
                const vr::VRTextureBounds_t t = left;
                left = right;
                right = t;
            }

            g_vr->LockSubmissionQueue();
            const bool ok = submit_prepared(vr::Eye_Left, s, &left)
                         && submit_prepared(vr::Eye_Right, s, &right);
            g_vr->UnlockSubmissionQueue();
            return ok;
        }

        // Prepare both, then lock only for the submits.
        bool submit_pair(IDirect3DSurface9* left, IDirect3DSurface9* right)
        {
            EyeSubmission l = prepare_surface(left);
            EyeSubmission r = (right == left) ? l : prepare_surface(right);
            if (!l.ok || !r.ok)
                return false;

            g_vr->LockSubmissionQueue();
            const bool ok = submit_prepared(vr::Eye_Left, l)
                         && submit_prepared(vr::Eye_Right, r);
            g_vr->UnlockSubmissionQueue();
            return ok;
        }
        // The game's FSAA (launcher "EnableFSAA", 2026-10-04) makes the back
        // buffer multisampled, which the compositor cannot take. It is
        // resolved into this single-sample copy first (StretchRect).
        IDirect3DSurface9* g_resolve = nullptr;
        UINT g_resolve_w = 0, g_resolve_h = 0;

        IDirect3DSurface9* resolved_back_buffer(IDirect3DSurface9* back)
        {
            D3DSURFACE_DESC d{};
            if (!back || FAILED(back->GetDesc(&d)) ||
                d.MultiSampleType == D3DMULTISAMPLE_NONE)
                return back;
            if (g_resolve && (g_resolve_w != d.Width || g_resolve_h != d.Height))
            {
                g_resolve->Release();
                g_resolve = nullptr;
            }
            if (!g_resolve)
            {
                const HRESULT hr = g_device->CreateRenderTarget(d.Width,
                    d.Height, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE,
                    &g_resolve, nullptr);
                if (FAILED(hr) || !g_resolve)
                {
                    g_resolve = nullptr;
                    static bool reported = false;
                    if (!reported)
                    {
                        reported = true;
                        log("submit: could not make the FSAA resolve target "
                            "%ux%u (0x%08lX)", d.Width, d.Height, hr);
                    }
                    return nullptr;
                }
                g_resolve_w = d.Width;
                g_resolve_h = d.Height;
                log("submit: back buffer is multisampled (type %d, quality "
                    "%lu); resolving into a %ux%u copy for the compositor",
                    (int)d.MultiSampleType, d.MultiSampleQuality, d.Width,
                    d.Height);
            }
            if (FAILED(g_device->StretchRect(back, nullptr, g_resolve,
                                             nullptr, D3DTEXF_NONE)))
                return nullptr;
            return g_resolve;
        }

        bool g_ready = false;
        unsigned g_frames = 0;
        unsigned g_failures = 0;

        // Reported once rather than every frame; a per-frame failure would
        // otherwise fill the log faster than anything useful could be read.
        void report_once(const char* what, HRESULT hr)
        {
            if (g_failures++ == 0)
                log("submit: %s failed (0x%08lX) -- reporting once", what, hr);
        }
    }

    bool vr_submit_ready()
    {
        return g_ready;
    }

    void vr_submit_release_targets()
    {
        if (g_resolve)
        {
            g_resolve->Release();
            g_resolve = nullptr;
        }
    }

    bool vr_submit_init(IDirect3DDevice9* real_device)
    {
        // The in-game anti-aliasing toggle makes the game release its
        // device and create a new one (log 2026-10-04: a second CreateDevice,
        // then a crash). Everything made on the old device -- the interop,
        // the resolve target, the icon and panel textures -- goes, and the
        // interop is rebuilt on the new device.
        if (g_ready && real_device && real_device != g_device)
        {
            log("submit: the game created a new device; rebinding the VR "
                "submit path to it");
            vr_submit_release_targets();
            IDirect3DTexture9** owned[] = { &g_badge, &g_gear_icons[0],
                &g_gear_icons[1], &g_gear_icons[2], &g_gear_icons[3],
                &g_gear_handle, &g_gear_tile, &g_tuning_panel };
            for (IDirect3DTexture9** texture : owned)
                if (*texture)
                {
                    (*texture)->Release();
                    *texture = nullptr;
                }
            g_badge_tried = false;
            g_gear_icons_tried = false;
            g_tuning_panel_text[0] = 0;
            hud_capture_device_changed();
            vr_submit_shutdown();
        }
        if (g_ready)
            return true;
        if (!real_device || !vr_ready())
            return false;

        g_comp = (vr::IVRCompositor*)vr_compositor_raw();
        if (!g_comp)
        {
            log("submit: no IVRCompositor -- the session is not a Scene application");
            return false;
        }
        // The mod's pose maths, recentring and holster/gesture frames are all
        // in seated space. WaitGetPoses returns poses in the compositor's
        // space (standing by default), and those poses now drive rendering.
        g_comp->SetTrackingSpace(vr::TrackingUniverseSeated);

        // Direct3DCreateVR9 only exists in our DXVK fork. On stock DXVK or on
        // Windows' d3d9 it is simply absent, and we stay mono-on-screen.
        HMODULE backend = GetModuleHandleW(L"d3d9_dxvk.dll");
        if (!backend)
        {
            log("submit: d3d9_dxvk.dll is not loaded -- set backend = dxvk");
            return false;
        }

        PFN_Direct3DCreateVR9 create =
            (PFN_Direct3DCreateVR9)GetProcAddress(backend, "Direct3DCreateVR9");
        if (!create)
        {
            log("submit: this DXVK has no Direct3DCreateVR9 -- it is not our fork. "
                "Run build_dxvk.bat and install.bat dxvk.");
            return false;
        }

        g_device = real_device;
        const HRESULT hr = create(real_device, &g_vr);
        if (FAILED(hr) || !g_vr)
        {
            log("submit: Direct3DCreateVR9 failed (0x%08lX)", hr);
            return false;
        }

        g_ready = true;
        log("submit: Vulkan interop up; submitting mono to both eyes");
        return true;
    }

    void vr_submit_shutdown()
    {
        for (int i = 0; i < 2; ++i)
        {
            if (g_eye[i])
            {
                g_eye[i]->Release();
                g_eye[i] = nullptr;
            }
        }
        if (g_vr)
        {
            g_vr->Release();
            g_vr = nullptr;
        }
        g_device = nullptr;
        g_ready = false;
        g_comp = nullptr;
    }

    // Performance line every 5 s (user, 2026-10-05: the water area ran at
    // about 30 against 55 elsewhere). From SteamVR's own frame timing:
    // the GPU time of our scene, the compositor's, how often a frame was
    // reprojected or dropped; plus the time between frames, how long
    // WaitGetPoses blocked (long = waiting for the headset, so the game
    // was fast enough) and the game's draw calls per frame.
    namespace
    {
        volatile LONG g_perf_draws = 0;
        LARGE_INTEGER g_perf_qpf{}, g_perf_last_frame{}, g_perf_window{};
        unsigned g_perf_frames = 0, g_perf_reprojected = 0;
        unsigned g_perf_cpu_late = 0, g_perf_gpu_late = 0;
        unsigned g_perf_throttled = 0;
        unsigned g_perf_dropped = 0, g_perf_timed = 0;
        double g_perf_wait_ms = 0.0, g_perf_scene_gpu_ms = 0.0;
        double g_perf_compositor_gpu_ms = 0.0, g_perf_frame_ms_max = 0.0;
        LONG g_perf_draw_total = 0;

        double perf_ms(const LARGE_INTEGER& a, const LARGE_INTEGER& b)
        {
            return g_perf_qpf.QuadPart
                ? double(b.QuadPart - a.QuadPart) * 1000.0 /
                  double(g_perf_qpf.QuadPart) : 0.0;
        }

        void perf_frame(const LARGE_INTEGER& before,
                        const LARGE_INTEGER& after)
        {
            if (!g_perf_qpf.QuadPart)
            {
                QueryPerformanceFrequency(&g_perf_qpf);
                g_perf_window = after;
                g_perf_last_frame = after;
                return;
            }
            const double frame = perf_ms(g_perf_last_frame, after);
            g_perf_last_frame = after;
            if (frame > g_perf_frame_ms_max && frame < 1000.0)
                g_perf_frame_ms_max = frame;
            ++g_perf_frames;
            g_perf_wait_ms += perf_ms(before, after);
            g_perf_draw_total += InterlockedExchange(&g_perf_draws, 0);

            vr::Compositor_FrameTiming timing{};
            timing.m_nSize = sizeof(timing);
            if (g_comp->GetFrameTiming(&timing, 1))
            {
                ++g_perf_timed;
                g_perf_scene_gpu_ms += timing.m_flPreSubmitGpuMs;
                g_perf_compositor_gpu_ms += timing.m_flCompositorRenderGpuMs;
                const uint32_t flags = timing.m_nReprojectionFlags;
                if (flags & (vr::VRCompositor_ReprojectionReason_Cpu |
                             vr::VRCompositor_ReprojectionReason_Gpu))
                    ++g_perf_reprojected;
                if (flags & vr::VRCompositor_ReprojectionReason_Cpu)
                    ++g_perf_cpu_late;
                if (flags & vr::VRCompositor_ReprojectionReason_Gpu)
                    ++g_perf_gpu_late;
                if (flags & vr::VRCompositor_ThrottleMask)
                    ++g_perf_throttled;
                g_perf_dropped += timing.m_nNumDroppedFrames;
            }

            const double window = perf_ms(g_perf_window, after);
            if (window >= 5000.0 && g_perf_frames)
            {
                const double n = g_perf_frames;
                const double timed = g_perf_timed ? g_perf_timed : 1.0;
                log("perf: %.0f fps (frame %.1f ms avg, %.0f worst; "
                    "WaitGetPoses %.1f ms), GPU scene %.1f ms, compositor "
                    "%.1f ms, reprojected %.0f%% (CPU late %.0f%%, GPU late "
                    "%.0f%%, throttled %.0f%%), dropped %u, %.0f draws/frame",
                    n * 1000.0 / window, window / n, g_perf_frame_ms_max,
                    g_perf_wait_ms / n, g_perf_scene_gpu_ms / timed,
                    g_perf_compositor_gpu_ms / timed,
                    100.0 * g_perf_reprojected / timed,
                    100.0 * g_perf_cpu_late / timed,
                    100.0 * g_perf_gpu_late / timed,
                    100.0 * g_perf_throttled / timed, g_perf_dropped,
                    g_perf_draw_total / n);
                perf_cpu_report(g_perf_frames);
                camera_perf_instance_report(g_perf_frames);
                g_perf_window = after;
                g_perf_frames = g_perf_reprojected = g_perf_dropped = 0;
                g_perf_cpu_late = g_perf_gpu_late = g_perf_throttled = 0;
                g_perf_timed = 0;
                g_perf_wait_ms = g_perf_scene_gpu_ms = 0.0;
                g_perf_compositor_gpu_ms = g_perf_frame_ms_max = 0.0;
                g_perf_draw_total = 0;
            }
        }
    }

    void vr_submit_note_draw()
    {
        InterlockedIncrement(&g_perf_draws);
    }

    void vr_submit_wait()
    {
        if (!g_ready)
            return;

        log_install_crash_handler();
        vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount]{};
        LARGE_INTEGER before{}, after{};
        QueryPerformanceCounter(&before);
        log_set_activity("IVRCompositor::WaitGetPoses");
        const bool posed = g_comp->WaitGetPoses(poses,
            vr::k_unMaxTrackedDeviceCount, nullptr, 0) ==
            vr::VRCompositorError_None;
        log_set_activity(nullptr);
        QueryPerformanceCounter(&after);
        if (posed)
        {
            vr_set_render_poses(poses, vr::k_unMaxTrackedDeviceCount);
            log_set_activity("IVRCompositor::GetFrameTiming");
            perf_frame(before, after);
            log_set_activity(nullptr);
        }
    }

    void vr_submit_frame(IDirect3DSurface9* back_buffer)
    {
        PerfCpuScope perf_scope(PerfSubmit);
        hud_capture_frame();
        camera_core_position_restore();
        if (!g_ready || !back_buffer)
            return;

        // Mono: one image, both eyes, no copy.
        if (!config().stereo)
        {
            const bool ok = submit_pair(back_buffer, back_buffer);

            if (!ok)
            {
                if (g_failures++ == 0)
                    log("submit: the compositor would not take the frame -- reporting once");
                return;
            }
            if (++g_frames == 1)
                log("submit: first frame accepted (mono)");
            return;
        }

        // Same-frame stereo has already drawn both eyes into this surface,
        // side by side. No eye targets, no StretchRect, and both halves are
        // from the same instant.
        if (stereo_same_frame_active())
        {
            draw_holster_debug(back_buffer);
            draw_hand_tuning_debug(back_buffer);
            draw_handhold_debug(back_buffer);
            draw_aim_debug_laser(back_buffer);
            draw_pouch_items(back_buffer);
            draw_gear_tuning_panel(back_buffer);
            draw_secure_grab_badge(back_buffer);
            draw_gear_cross(back_buffer);
            draw_pluck_ring(back_buffer);
            draw_comfort_vignette(back_buffer, camera_comfort_vignette());
            draw_aim_crosshair(back_buffer);
            // Nothing flips the eye on this path, so anything that misses the
            // per-pass substitution would otherwise keep whichever eye happened
            // to be current when the mode started. Centre is the honest answer
            // for a draw with no eye of its own.
            vr_set_current_eye(EyeCenter);
        
            IDirect3DSurface9* submitted = resolved_back_buffer(back_buffer);
            if (!submitted || !submit_side_by_side(submitted))
            {
                if (g_failures++ == 0)
                    log("submit: the compositor would not take the side-by-side "
                        "frame -- reporting once");
                return;
            }
            if (++g_frames == 1)
                log("submit: first side-by-side frame accepted");
            return;
        }

        if (!ensure_eye_targets(back_buffer))
            return;

        // The frame just drawn belongs to whichever eye was current while it
        // was being drawn. Keep it, so the other eye still has something to
        // show while it waits its turn.
        const int drawn = (vr_current_eye() == EyeRight) ? 1 : 0;
        HRESULT hr = g_device->StretchRect(back_buffer, nullptr,
                                           g_eye[drawn], nullptr, D3DTEXF_NONE);
        if (FAILED(hr))
        {
            report_once("StretchRect into the eye target", hr);
            return;
        }
        g_eye_ready[drawn] = true;

        // Until both eyes have been drawn once, show the fresh one in both --
        // otherwise the first frames have one blank eye, which is unpleasant.
        IDirect3DSurface9* left  = g_eye_ready[0] ? g_eye[0] : g_eye[drawn];
        IDirect3DSurface9* right = g_eye_ready[1] ? g_eye[1] : g_eye[drawn];

        // Inverted stereo is hard to put into words and instant to recognise,
        // so it is a key rather than a rebuild.
        if (tune_swap_eyes())
        {
            IDirect3DSurface9* t = left;
            left = right;
            right = t;
        }

        const bool ok = submit_pair(left, right);

        // Flip first, and unconditionally. Returning early on a failed submit
        // would leave the eye stuck on whichever one it was, so a single bad
        // frame would wedge it into drawing the same eye for ever -- which
        // looks exactly like stereo quietly not working.
        vr_set_current_eye(drawn == 0 ? EyeRight : EyeLeft);
        g_drawn_count[drawn]++;

        if (!ok)
        {
            if (g_failures++ == 0)
                log("submit: the compositor would not take the frame -- reporting once");
            return;
        }

        if (++g_frames == 1)
            log("submit: first stereo pair accepted (%ux%u per eye, alternating)",
                g_eye_w, g_eye_h);

        // Proof that the eyes really are alternating, rather than one of them
        // silently getting every frame.
        if ((g_frames % 900u) == 0u)
            log("submit: %u frames drawn for the left eye, %u for the right",
                g_drawn_count[0], g_drawn_count[1]);
    }
}
