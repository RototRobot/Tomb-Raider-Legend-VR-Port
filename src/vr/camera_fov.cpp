// Widening the engine's own field of view, so its culling matches what the
// headset shows.
//
// The game draws a 53 degree horizontal frustum. A Reverb G2 wants about 94.
// We already replace the projection for *display*, but the engine still culls
// against its own, so anything outside 53 degrees is simply not submitted --
// look away from where the mouse points and the world is missing.
//
// Rather than find and widen the culling frustum itself, this widens the
// camera's field of view at source. Culling, streaming and anything else that
// depends on the camera then follow along for free, and our substitution keeps
// the displayed projection correct regardless.
//
//   ?CAMERA_SetProjDistance@@YAXPAUCamera@@M@Z   0x0048E540
//
// found by remapping the shipped PDB (FINDINGS.md section 2) and confirmed by
// disassembly: it stores the distance at Camera+0x2B4 and tail-calls
// CAMERA_SetProjDistance2, which fills in the aspect terms at +0x2C0..+0x2CC.
//
// Measured, after getting it wrong in both directions: multiplying the
// projection distance by 2.99 moved the uploaded x scale from 2.0000 to 5.9800,
// which is the same factor. So
//
//     x_scale = projection_distance / 256
//
// (512/256 = 2.0, and 2*atan(1/2) = 53.13 degrees, the field of view measured
// off the matrix). A **smaller** distance is a **wider** view.
//
// The amount needed is not just the headset's own field of view, either. The
// engine culls against a frustum centred on the *game camera*, which follows
// the mouse -- so turning your head away from where the mouse points looks at
// geometry that was never submitted, however wide the frustum is. Covering that
// needs a culling frustum far wider than the headset sees at any one moment,
// which costs geometry but nothing else: the projection actually displayed is
// replaced regardless, so this only ever affects what the engine bothers to
// draw.

#include "hook.h"
#include "tune.h"
#include "vr_session.h"

#include "../common/config.h"
#include "../common/log.h"

#include <cmath>

namespace trlvr
{
    namespace
    {
        typedef void(__cdecl* PFN_SetProjDistance)(void*, float);

        const uintptr_t kSetProjDistance = 0x0048E540;

        // mov eax,[esp+8] ; push esi   -- exactly five bytes, and an
        // instruction boundary, so the jmp needs no more than it overwrites.
        const unsigned char kSignature[5] = { 0x8B, 0x44, 0x24, 0x08, 0x56 };

        PFN_SetProjDistance g_original = nullptr;
        bool g_logged = false;
        const unsigned kOffCameraMode = 0x314;

        // The engine's own default: x scale 2.0 is a half-angle tangent of 0.5,
        // 53.13 degrees across.
        const float kEngineDefaultScale = 2.0f;

        // The culling frustum we want, in degrees across. Wide enough that
        // turning your head away from the mouse still finds geometry there.
        float wanted_scale()
        {
            const float deg = tune_cull_fov();
            if (deg <= 1.0f || deg >= 179.0f)
                return kEngineDefaultScale;

            const float half = deg * 0.5f * 3.14159265f / 180.0f;
            const float t = tanf(half);
            if (t <= 0.0f)
                return kEngineDefaultScale;
            return 1.0f / t;          // x scale for that field of view
        }

        // A ceiling, not a multiplier.
        //
        // Scaling every camera by the same factor widened the already-wide ones
        // into nonsense -- one came through at an x scale of 0.0048, a frustum
        // essentially 180 degrees across. What is actually wanted is "at least
        // this wide, never narrower", which is a limit on the distance:
        //
        //     x_scale = distance / 256,  so  distance <= 256 * wanted_scale
        //
        // Cameras already wider than that are left alone, and narrow ones --
        // aiming, cutscenes -- are opened up to the same floor.
        float distance_limit()
        {
            return 256.0f * wanted_scale();
        }

        void __cdecl detour(void* camera, float distance)
        {
            const float limit = distance_limit();
            short mode = -1;
            __try
            {
                mode = *reinterpret_cast<const short*>(
                    static_cast<const unsigned char*>(camera) + kOffCameraMode);
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
            }

            // The direct view-volume hook in cull.cpp already opens the real
            // object frustum. Accurate aim additionally uses projection
            // distance as part of its camera construction, so applying this
            // older source-level workaround in mode 13 can reshape the view
            // even though the final GPU projection is replaced. Leave that
            // camera value entirely to the game.
            const bool accurate_aim = mode == 13;
            const float wide = !accurate_aim && limit > 0.0f && distance > limit
                             ? limit : distance;

            if (accurate_aim)
            {
                static bool reported = false;
                if (!reported)
                {
                    reported = true;
                    log("camera: legacy widen_fov bypassed in accurate aim; "
                        "direct cull-volume widening remains active");
                }
            }

            // Log whenever the target changes, so trimming it live is visible.
            static float last = -1.0f;
            if (!g_logged || fabsf(tune_cull_fov() - last) > 0.01f)
            {
                g_logged = true;
                last = tune_cull_fov();
                log("camera: culling frustum at least %.0f degrees across "
                    "(headset sees %.0f); projection distance capped at %.1f",
                    tune_cull_fov(),
                    2.0f * atanf(vr_max_tangent()) * 57.2958f,
                    limit);
            }

            g_original(camera, wide);
        }
    }

    void camera_fov_init()
    {
        // cull.cpp now changes the view-volume planes directly.  The older
        // source hook is redundant, and projDistance turned out to participate
        // in camera construction as well as culling.  In particular it
        // contaminated mode 13; an old installed INI still had it enabled in
        // every other mode.  Accept the legacy setting but deliberately do
        // not install the hook, so the game owns its camera geometry again.
        if (config().widen_fov)
            log("camera: legacy widen_fov=1 ignored; direct cull-volume "
                "widening is active and game camera geometry is left alone");
        else
            log("camera: game projection distance left alone; direct "
                "cull-volume widening owns visibility");
    }
}
