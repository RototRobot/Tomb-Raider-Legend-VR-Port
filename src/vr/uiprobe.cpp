// Working out how the interface actually reaches the GPU.
//
// What is already known, from section 25: the interface never uploads a
// projection to c0 -- zero two-dimensional passes across gameplay and menus --
// and SetTransform is called zero times in the whole run. So its vertices are
// already past the transform stage by the time the device sees them. It is
// glued to the rendered image, which is why it cannot drift on its own, and
// why it ends up stretched across the entire eye frustum: roughly 105 degrees
// of health bar.
//
// There are two ways that can happen, and they need opposite fixes:
//
//   pre-transformed, fixed function (an FVF carrying D3DFVF_XYZRHW)
//       The vertices are in screen pixels and Direct3D uses them as they are.
//       The viewport clips them but does not scale them, so shrinking the
//       interface means rewriting vertex data -- intercepting the draws and
//       scaling x and y towards the centre.
//
//   a vertex shader that passes position straight through
//       Then the vertices are in *clip* space and the viewport transform does
//       still apply. Setting a smaller viewport around the interface draws
//       scales it, with no vertex data touched at all -- a much smaller and
//       safer change.
//
// Guessing between those would mean writing the harder one and hoping, so this
// counts instead. It also counts draws per frame in each class, which is what
// says whether the interface is a handful of quads (easy to bracket) or
// thousands of draws mixed in with everything else.

#include "uiprobe.h"

#include "ui_space.h"

#include "../common/config.h"
#include "../common/log.h"

namespace trlvr
{
    namespace
    {
        enum Class
        {
            ClassShader = 0,       // a vertex shader is bound
            ClassPreTransformed,   // fixed function, FVF has XYZRHW
            ClassFixedFunction,    // fixed function, untransformed
            ClassDeclaration,      // a vertex declaration, no FVF
            ClassCount
        };

        const char* kNames[ClassCount] = {
            "vertex shader",
            "pre-transformed (XYZRHW)",
            "fixed function",
            "vertex declaration",
        };

        // Depth state at the moment of each draw. Interface and menus are
        // drawn with Z-testing off almost universally, so this is the usual
        // way to tell them from world geometry -- but post-processing does
        // the same, so the counts have to be read together with the vertex
        // path rather than on their own.
        bool g_ztest = true;
        bool g_zwrite = true;
        unsigned g_draws_znone = 0, g_prims_znone = 0;
        unsigned g_draws_ztest = 0, g_prims_ztest = 0;

        DWORD g_fvf = 0;
        const void* g_shader = nullptr;

        // A one-frame census, grouped by vertex shader. Small and bounded --
        // a frame uses a few dozen shaders, not thousands.
        struct ShaderTally
        {
            const void* shader;
            unsigned draws;
            unsigned prims;
            unsigned depth_off;
        };
        ShaderTally g_tally[96];
        int  g_tally_count = 0;
        bool g_capturing = false;
        bool g_capture_armed = false;
        bool  g_decl = false;

        unsigned g_draws[ClassCount] = { 0, 0, 0, 0 };
        unsigned g_prims[ClassCount] = { 0, 0, 0, 0 };
        unsigned g_frames = 0;
        unsigned g_last_ms = 0;

        Class classify()
        {
            if (g_shader)
                return ClassShader;
            if (g_fvf & D3DFVF_XYZRHW)
                return ClassPreTransformed;
            if (g_fvf)
                return ClassFixedFunction;
            return g_decl ? ClassDeclaration : ClassFixedFunction;
        }
    }

    void ui_note_fvf(DWORD fvf)
    {
        g_fvf = fvf;
        if (fvf)
            g_decl = false;
    }

    void ui_note_vertex_shader(const void* shader)
    {
        g_shader = shader;
    }

    void ui_capture_frame()
    {
        g_capture_armed = true;
        log("ui: census armed -- the next frame's draws will be listed by shader");
    }

    void ui_note_declaration(bool bound)
    {
        g_decl = bound;
        if (bound)
            g_fvf = 0;
    }

    void ui_note_render_state(D3DRENDERSTATETYPE state, DWORD value)
    {
        if (state == D3DRS_ZENABLE)
            g_ztest = (value != D3DZB_FALSE);
        else if (state == D3DRS_ZWRITEENABLE)
            g_zwrite = (value != 0);
    }

    bool ui_depth_disabled()
    {
        return !g_ztest && !g_zwrite;
    }

    // Pass-count buckets, keyed by everything that decides where a draw lands.
    namespace
    {
        struct PassBucket
        {
            int      passes;
            DWORD    vp_x;
            DWORD    vp_w;
            DWORD    vp_h;
            bool     shader;
            bool     xyzrhw;
            bool     ui;        // inside the engine's interface bracket
            bool     depth_off;
            unsigned draws;
        };

        PassBucket g_pass[24]{};
        int g_pass_count = 0;
    }

    void ui_report_passes();

    void ui_note_pass(int passes, IDirect3DDevice9* device)
    {
        if (!config().probe_constants || !device)
            return;

        D3DVIEWPORT9 vp{};
        if (FAILED(device->GetViewport(&vp)))
            return;

        const bool shader = (g_shader != nullptr);
        const bool xyzrhw = (g_fvf & D3DFVF_XYZRHW) != 0;
        const bool ui = ui_drawing_interface();
        const bool zoff = !g_ztest && !g_zwrite;

        for (int i = 0; i < g_pass_count; ++i)
        {
            PassBucket& b = g_pass[i];
            if (b.passes == passes && b.vp_x == vp.X && b.vp_w == vp.Width &&
                b.vp_h == vp.Height && b.shader == shader &&
                b.xyzrhw == xyzrhw && b.ui == ui && b.depth_off == zoff)
            {
                ++b.draws;
                return;
            }
        }
        if (g_pass_count < 24)
        {
            PassBucket& b = g_pass[g_pass_count++];
            b.passes = passes;
            b.vp_x = vp.X;
            b.vp_w = vp.Width;
            b.vp_h = vp.Height;
            b.shader = shader;
            b.xyzrhw = xyzrhw;
            b.ui = ui;
            b.depth_off = zoff;
            b.draws = 1;
        }
    }

    void ui_report_passes()
    {
        if (!g_pass_count)
            return;
        log("ui: draws by stereo pass count --");
        log("      passes  viewport x/ w x h        interface  depth-off  draws");
        for (int i = 0; i < g_pass_count; ++i)
        {
            const PassBucket& b = g_pass[i];
            log("      %6d  %6lu /%5lu x%5lu   %-9s  %-9s  %6u",
                b.passes, (unsigned long)b.vp_x, (unsigned long)b.vp_w,
                (unsigned long)b.vp_h,
                b.ui ? "YES" : "no", b.depth_off ? "yes" : "no", b.draws);
        }
        g_pass_count = 0;
    }

    void ui_note_draw(UINT primitives)
    {
        if (g_capturing)
        {
            int i = 0;
            for (; i < g_tally_count; ++i)
                if (g_tally[i].shader == g_shader)
                    break;
            if (i == g_tally_count && g_tally_count < 96)
            {
                g_tally[g_tally_count].shader = g_shader;
                g_tally[g_tally_count].draws = 0;
                g_tally[g_tally_count].prims = 0;
                g_tally[g_tally_count].depth_off = 0;
                ++g_tally_count;
            }
            if (i < g_tally_count)
            {
                ++g_tally[i].draws;
                g_tally[i].prims += primitives;
                if (!g_ztest && !g_zwrite)
                    ++g_tally[i].depth_off;
            }
        }

        const Class c = classify();
        ++g_draws[c];
        g_prims[c] += primitives;

        if (g_ztest || g_zwrite)
        {
            ++g_draws_ztest;
            g_prims_ztest += primitives;
        }
        else
        {
            ++g_draws_znone;
            g_prims_znone += primitives;
        }
    }

    void ui_report()
    {
        // Behind the probe flag rather than plain logging: the interface is
        // not identified and the work is parked, so this is investigation
        // noise during ordinary play. The census is still here for whenever
        // that is picked up again.
        if (!config().logging || !config().probe_constants)
            return;



        if (g_capturing)
        {
            g_capturing = false;
            log("ui: one frame, %d distinct vertex shaders --", g_tally_count);
            for (int i = 0; i < g_tally_count; ++i)
                log("      shader %p  %4u draws  %7u primitives  %4u with depth off",
                    g_tally[i].shader, g_tally[i].draws, g_tally[i].prims,
                    g_tally[i].depth_off);
            log("ui: census done");
        }
        if (g_capture_armed)
        {
            g_capture_armed = false;
            g_capturing = true;
            g_tally_count = 0;
        }

        ++g_frames;

        const unsigned now = GetTickCount();
        if (g_last_ms == 0)
            g_last_ms = now;
        if (now - g_last_ms < 5000 || g_frames == 0)
            return;

        ui_report_passes();
        log("ui: draws per frame by vertex path --");
        for (int i = 0; i < ClassCount; ++i)
        {
            if (!g_draws[i])
                continue;
            log("      %-26s %7.1f draws, %8.1f primitives",
                kNames[i],
                g_draws[i] / (float)g_frames,
                g_prims[i] / (float)g_frames);
            g_draws[i] = 0;
            g_prims[i] = 0;
        }

        // The interesting number. If this stays small and steady while the
        // interface is up, these are the draws to head-lock; if it swells
        // with scene complexity, depth state is not the discriminator and
        // something else has to be.
        log("      %-26s %7.1f draws, %8.1f primitives",
            "depth off (interface?)",
            g_draws_znone / (float)g_frames,
            g_prims_znone / (float)g_frames);
        log("      %-26s %7.1f draws, %8.1f primitives",
            "depth on (world)",
            g_draws_ztest / (float)g_frames,
            g_prims_ztest / (float)g_frames);

        g_draws_znone = g_prims_znone = 0;
        g_draws_ztest = g_prims_ztest = 0;
        g_frames = 0;
        g_last_ms = now;
    }
}

