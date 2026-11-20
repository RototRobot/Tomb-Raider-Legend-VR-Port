// Same-frame stereo: both eyes drawn in the same frame, in step.
//
// What alternating eyes cost, measured (FINDINGS section 20): the game holds a
// locked 90 fps with GPU time to spare, but each eye only refreshes at 45 Hz
// and the two halves of a stereo pair are taken a frame apart. Section 22
// showed that temporal disparity is far larger than the 6.4 cm of real
// parallax, which is why the eyes were so reluctant to fuse.
//
// Drawing the scene twice trades that frame rate for correctness: near 45 fps
// with both eyes **in step**, which is the better half of the bargain.
//
// The shape of it:
//
//   * The render surfaces the scene is drawn into become twice as wide, and
//     each eye gets half. That is cheaper than a render-target switch, which
//     would otherwise happen twice per draw across a few thousand draws.
//
//   * Every draw issued while the scene target is bound goes out twice, once
//     per viewport, with the matrix for that eye. This is why the c0 decision
//     had to move from constant-upload time to the draw: one upload can serve
//     many draws, but each pass needs its own matrix.
//
//   * At the end, the double-wide surface is handed to the compositor twice
//     with texture bounds selecting each half -- no copy, and no separate eye
//     targets at all.
//
// The known limitation is post-processing. Those passes read the scene target
// as a texture and would treat the two halves as one image, so the first
// version asks for Depth of Field and Fullscreen Effects to be off in the
// game's own Setup dialog. That is a real restriction, not a temporary one:
// lifting it means understanding each post pass well enough to run it per eye.

#include "stereo.h"
#include "perf_cpu.h"

#include "tune.h"
#include "vr_math.h"
#include "vr_session.h"

#include "../common/config.h"
#include "../common/log.h"

namespace trlvr
{
    namespace
    {
        unsigned g_eye_w = 0;
        unsigned g_eye_h = 0;

        // The device begin_pass last drew on. Only used while a pass has left
        // state on it, and the device wrapper flushes that before it closes.
        IDirect3DDevice9* g_last_device = nullptr;

        // The device holds a viewport that begin_pass set, not the game's.
        // g_viewport is then the game's, saved so it can be put back.
        bool g_saved_viewport = false;
        D3DVIEWPORT9 g_viewport{};
        // The viewport begin_pass last set, while nothing else has touched
        // it, so an identical request can be skipped.
        bool g_last_vp_valid = false;
        D3DVIEWPORT9 g_last_vp{};

        // The device holds c0..c3 that begin_pass set, not the game's.
        bool g_patched_c0 = false;
        bool g_announced = false;
    }

    void stereo_set_eye_size(unsigned width, unsigned height)
    {
        g_eye_w = width;
        g_eye_h = height;
    }

    unsigned stereo_eye_width()  { return g_eye_w; }
    unsigned stereo_eye_height() { return g_eye_h; }

    bool stereo_same_frame_active()
    {
        return config().stereo_same_frame && vr_ready() && g_eye_w != 0;
    }

    void stereo_widen(UINT* width, UINT height)
    {
        // Only surfaces exactly the size of one eye. Half-resolution post
        // targets and shadow maps are left alone -- they are not what the
        // scene is drawn into, and doubling them would break the passes that
        // sample them without helping anything.
        if (!width || !stereo_same_frame_active())
            return;
        if (*width == g_eye_w && height == g_eye_h)
            *width = g_eye_w * 2;
    }

    int stereo_passes()
    {
        if (!stereo_same_frame_active() || !vr_drawing_the_scene())
            return 1;
        if (!vr_have_game_c0())
            return 1;

        if (!g_announced)
        {
            g_announced = true;
            log("stereo: drawing both eyes in the same frame, %ux%u an eye",
                g_eye_w, g_eye_h);
            log("        Depth of Field and Fullscreen Effects need to be off "
                "in the game's Setup dialog -- the post passes read the scene "
                "target as one image and would treat both eyes as one.");
        }
        return 2;
    }

    void stereo_begin_pass(IDirect3DDevice9* device, int pass)
    {
        if (!device)
            return;

        g_last_device = device;
        if (!g_saved_viewport)
        {
            device->GetViewport(&g_viewport);
            g_saved_viewport = true;
        }

        // Each eye gets its own half, keeping whatever depth range the game
        // had set -- that belongs to the pass, not to us.
        unsigned eye_w = g_eye_w;
        unsigned eye_h = g_eye_h;
        vr_stereo_target_eye_size(&eye_w, &eye_h);

        D3DVIEWPORT9 vp = g_viewport;
        vp.X = (pass == 0) ? 0 : eye_w;
        vp.Y = 0;
        vp.Width = eye_w;
        vp.Height = eye_h;
        if (!g_last_vp_valid || memcmp(&g_last_vp, &vp, sizeof(vp)) != 0)
        {
            device->SetViewport(&vp);
            g_last_vp = vp;
            g_last_vp_valid = true;
        }

        const Eye eye = (pass == 0) ? EyeLeft : EyeRight;
        float regs[16];
        perf_cpu_begin(PerfDrawMath);
        const bool substituted =
            vr_substitute_c0_for_eye(vr_game_c0(), regs, eye);
        perf_cpu_end(PerfDrawMath);
        if (substituted)
        {
            device->SetVertexShaderConstantF(0, regs, 4);
            g_patched_c0 = true;
        }
        else if (g_patched_c0)
        {
            // This draw wants the game's own matrix, and the previous pass
            // left ours on the device.
            device->SetVertexShaderConstantF(0, vr_draw_c0_restore(), 4);
            g_patched_c0 = false;
        }
    }

    void stereo_restore_game_state(IDirect3DDevice9* device)
    {
        // The swap chain's Present has no device of its own to pass.
        if (!device)
            device = g_last_device;
        if (!device)
            return;

        if (g_patched_c0)
        {
            // Back to whatever the single-eye path had uploaded, so anything
            // that does not go through here still sees what it expects.
            device->SetVertexShaderConstantF(0, vr_draw_c0_restore(), 4);
            g_patched_c0 = false;
        }

        if (g_saved_viewport)
        {
            device->SetViewport(&g_viewport);
            g_saved_viewport = false;
        }
        g_last_vp_valid = false;
    }

    void stereo_note_game_viewport()
    {
        // The game's viewport is on the device now; the next pass saves it
        // afresh.
        g_saved_viewport = false;
        g_last_vp_valid = false;
    }

    void stereo_note_game_constants(UINT start, UINT count)
    {
        // An upload that covers any of c0..c3 replaces ours. (The game's
        // value is tracked separately, by vr_note_c0.)
        if (start < 4 && start + count > 0)
            g_patched_c0 = false;
    }

    void stereo_note_viewport_modified()
    {
        g_last_vp_valid = false;
    }
}
