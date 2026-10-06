// VR-safe production culling.
//
// Object bounds use camera frustum planes and are retained after widening.
// Rooms use portals clipped against a flat 512x448 virtual screen; that screen
// cannot cover the headset view and caused eye-dependent holes, so room culling
// remains bypassed. Object planes are rebuilt from the final HMD camera pose;
// first person also covers both eye origins at its four-unit VR near plane.

#include "cull.h"
#include "perf_cpu.h"
#include "hook.h"
#include "tune.h"
#include "camera_head.h"
#include "vr_session.h"
#include "../common/config.h"
#include "../common/log.h"

#include <cmath>
#include <cstring>

namespace trlvr
{
    namespace
    {
        const uintptr_t kCalcVVClipInfo  = 0x0048DEA0;
        const uintptr_t kTerrainClipRect = 0x0046D0A0;
        const uintptr_t kCheckInstanceVisibility = 0x00462FB0;
        const void* const kMainCamera = reinterpret_cast<const void*>(0x010FC660);
        const unsigned char kPrologue[6] =
            { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0 };
        const unsigned kOffVVNormals = 0x220;

        // Terrain strips (2026-10-04): DRAW_DrawTerrainGroup (0x0040C650)
        // copies the camera's sphere-test frustum matrices (+0x20/+0x60, or
        // +0xA0/+0xE0 for groups with flag 0x400000) to 0x00F48A70/0x00F48AB0
        // and walks a tree of bounding spheres (0x0040C430) against them.
        // Small detail strips -- the wet-rock decal, the river-bank edge --
        // showed only when looked at almost directly, in first and third
        // person. While a group is drawn the main camera's four matrices are
        // zeroed (every sphere passes, as for PIPE3D instances), then
        // restored; rooms stay portal-culled. (The wet-rock sheen itself
        // turned out to be vanilla behaviour, GOG copy; this stays because
        // retail threw the strips away entirely in VR.)
        const uintptr_t kDrawTerrainGroup = 0x0040C650;
        typedef void(__cdecl* PFN_DrawTerrainGroup)(void*, void*);
        PFN_DrawTerrainGroup g_draw_terrain_group = nullptr;

        bool g_terrain_culling_open = true;

        void __cdecl detour_draw_terrain_group(void* terrain, void* group)
        {
            PerfCpuScope perf_scope(PerfTerrain);
            if (!g_terrain_culling_open)
            {
                // [vr] terrain_culling = frustum: retail strip culling against
                // the camera's (headset-widened) sphere-test matrices.
                g_draw_terrain_group(terrain, group);
                return;
            }
            unsigned char* planes = (unsigned char*)0x010FC660 + 0x20;
            alignas(16) unsigned char saved[0x100];
            memcpy(saved, planes, sizeof(saved));
            memset(planes, 0, sizeof(saved));
            g_draw_terrain_group(terrain, group);
            memcpy(planes, saved, sizeof(saved));
            static bool reported = false;
            if (!reported)
            {
                reported = true;
                log("cull: terrain strip sphere culling opened (all strips "
                    "of portal-visible terrain are drawn)");
            }
        }

        typedef void(__cdecl* PFN_CalcVV)(void*);
        PFN_CalcVV g_calc_vv = nullptr;
        // [vr] first_person_object_culling; F2 toggles (2026-10-05: world
        // objects took ~15 ms of CPU per frame in the water area with the
        // object frustum fully open in first person).
        bool g_object_culling_open = true;
        typedef void(__cdecl* PFN_CheckInstanceVisibility)(
            void*, void*, void*);
        PFN_CheckInstanceVisibility g_check_instance_visibility = nullptr;

        void __cdecl detour_check_instance_visibility(
            void* camera, void* instance, void* level)
        {
            PerfCpuScope perf_scope(PerfVisibility);
            if (!g_check_instance_visibility)
                return;

            // In first person the player can look or lean far away from the
            // retail camera's centre ray. Even a widened sphere frustum can
            // then reject a terrain instance before the stereo draw sees it.
            // Keep third-person object culling intact, but make the main
            // first-person scene camera permissive. Room traversal is already
            // using the full portal rectangle; this closes the remaining
            // object-level hole without touching shadow/reflection cameras.
            if (camera_first_person_active() && camera == kMainCamera &&
                g_object_culling_open)
            {
                alignas(16) unsigned char camera_copy[0x300];
                memcpy(camera_copy, camera, sizeof(camera_copy));
                memset(camera_copy + 0x20, 0, 0x80);
                g_check_instance_visibility(camera_copy, instance, level);

                static bool reported = false;
                if (!reported)
                {
                    reported = true;
                    log("cull: first-person main-scene object frustum "
                        "bypassed for all world instances");
                }
                return;
            }

            const int owned = camera_first_person_visibility_class(instance);
            if (!owned || !camera)
            {
                g_check_instance_visibility(camera, instance, level);
                return;
            }

            // PIPE3D's sphere checks use the two 4x4 frustum matrices at
            // CameraCore+0x20 and +0x60. A private, aligned camera copy keeps
            // those tests open for Lara and carried items only. The real
            // camera and all unrelated objects retain normal culling.
            alignas(16) unsigned char camera_copy[0x300];
            memcpy(camera_copy, camera, sizeof(camera_copy));
            memset(camera_copy + 0x20, 0, 0x80);
            g_check_instance_visibility(camera_copy, instance, level);

            static bool reported[3]{};
            if (!reported[owned])
            {
                reported[owned] = true;
                log("cull: first-person %s bypasses object frustum; "
                    "game visibility flags preserved",
                    owned == 1 ? "Lara hands" : "Lara-linked equipment");
            }
        }

        enum PlaneKind { PlaneDepth, PlaneHorizontal, PlaneVertical };

        PlaneKind classify(const float* n)
        {
            const float lateral = sqrtf(n[0] * n[0] + n[1] * n[1]);
            const float length = sqrtf(lateral * lateral + n[2] * n[2]);
            if (lateral <= 1e-4f * (length > 0.0f ? length : 1.0f))
                return PlaneDepth;
            return fabsf(n[0]) >= fabsf(n[1])
                 ? PlaneHorizontal : PlaneVertical;
        }

        void widen_plane(float* n, float wanted_half_angle_degrees)
        {
            const float length = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            const float lateral = sqrtf(n[0] * n[0] + n[1] * n[1]);
            if (length <= 1e-6f || lateral <= 1e-6f)
                return;

            float sine = n[2] / length;
            if (sine > 1.0f) sine = 1.0f;
            if (sine < -1.0f) sine = -1.0f;
            const float have = asinf(sine);
            const float want = wanted_half_angle_degrees / 57.29578f;
            if (want <= have)
                return;

            const float lateral_scale = cosf(want) * length / lateral;
            n[0] *= lateral_scale;
            n[1] *= lateral_scale;
            n[2] = sinf(want) * length;
        }

        void volume_half_angles(bool first_person, float* horizontal,
                                float* vertical)
        {
            *horizontal = tune_cull_fov() * 0.5f;
            *vertical = atanf(tanf(*horizontal / 57.29578f) *
                              384.0f / 512.0f) * 57.29578f;
            if (!first_person || !vr_ready())
                return;

            // At the four-unit first-person VR near plane, either eye can
            // see beyond a centre-eye frustum even when the camera heading
            // matches the HMD. Cover the union of both eye projections and
            // their physical offsets, with five degrees for pose latency.
            float lateral_tangent = 0.0f;
            float vertical_tangent = 0.0f;
            float lateral_offset = 0.0f;
            float vertical_offset = 0.0f;
            for (int eye = 0; eye < 2; ++eye)
            {
                const EyeInfo& e = vr_eye(eye == 0 ? EyeLeft : EyeRight);
                const float h = fmaxf(fabsf(e.tan_left), fabsf(e.tan_right));
                const float v = fmaxf(fabsf(e.tan_top), fabsf(e.tan_bottom));
                lateral_tangent = fmaxf(lateral_tangent, h);
                vertical_tangent = fmaxf(vertical_tangent, v);
                lateral_offset = fmaxf(lateral_offset, fabsf(e.offset_x));
                vertical_offset = fmaxf(vertical_offset, fabsf(e.offset_y));
            }
            const float scale = tune_world_scale() * tune_ipd_scale() *
                                camera_stereo_scale();
            const float near_units = 4.0f;
            *horizontal = fmaxf(*horizontal,
                atanf(lateral_tangent + lateral_offset * scale / near_units) *
                    57.29578f + 5.0f);
            *vertical = fmaxf(*vertical,
                atanf(vertical_tangent + vertical_offset * scale / near_units) *
                    57.29578f + 5.0f);
            *horizontal = fminf(*horizontal, 85.0f);
            *vertical = fminf(*vertical, 85.0f);
        }

        void build_volume(void* camera, bool first_person)
        {
            if (!camera || !g_calc_vv)
                return;
            float* normals = (float*)((unsigned char*)camera + kOffVVNormals);
            float saved[20];
            memcpy(saved, normals, sizeof(saved));
            float horizontal = 0.0f, vertical = 0.0f;
            volume_half_angles(first_person, &horizontal, &vertical);
            for (int i = 0; i < 5; ++i)
            {
                float* plane = normals + i * 4;
                const PlaneKind kind = classify(plane);
                if (kind != PlaneDepth)
                    widen_plane(plane,
                        kind == PlaneHorizontal ? horizontal : vertical);
            }

            g_calc_vv(camera);
            memcpy(normals, saved, sizeof(saved));
        }

        void __cdecl detour_calc_vv(void* camera)
        {
            build_volume(camera, camera_first_person_active() &&
                                 camera == (void*)0x010FC660);
        }

        int __cdecl detour_clip_rect(void* portal, void* rect)
        {
            (void)portal;
            short* output = (short*)rect;
            output[0] = 0;
            output[1] = 0;
            output[2] = 0x200;
            output[3] = 0x1C0;
            return 1;
        }
    }

    void cull_rebuild_headset_volume(void* camera, bool first_person)
    {
        build_volume(camera, first_person);
        static bool reported[2]{};
        const int mode = first_person ? 1 : 0;
        if (g_calc_vv && !reported[mode])
        {
            reported[mode] = true;
            float horizontal = 0.0f, vertical = 0.0f;
            volume_half_angles(first_person, &horizontal, &vertical);
            log("cull: %s object planes rebuilt from final HMD pose "
                "(horizontal %.1f, vertical %.1f degrees full)",
                first_person ? "first-person" : "third-person",
                horizontal * 2.0f, vertical * 2.0f);
        }
    }

    void cull_toggle_object_culling()
    {
        g_object_culling_open = !g_object_culling_open;
        log("cull: F2 -- first-person object culling %s", g_object_culling_open
            ? "OPEN (every object in a visible room is drawn)"
            : "FRUSTUM (objects culled against the wide headset view)");
    }

    void cull_toggle_terrain_culling()
    {
        g_terrain_culling_open = !g_terrain_culling_open;
        log("cull: Shift+F2 -- terrain culling %s", g_terrain_culling_open
            ? "OPEN (every strip of a visible room is drawn)"
            : "FRUSTUM (strips culled against the wide headset view)");
    }

    void cull_init()
    {
        g_object_culling_open = config().first_person_object_culling_open;
        g_terrain_culling_open = config().terrain_culling_open;
        log("cull: terrain culling %s at start (Shift+F2 switches)",
            g_terrain_culling_open ? "open" : "frustum");
        log("cull: first-person object culling %s at start (F2 switches)",
            g_object_culling_open ? "open" : "frustum");
        void* trampoline = nullptr;
        // Either starting view: the view can switch live, and the detour
        // only acts while first person is active.
        if (hook_install((void*)kCheckInstanceVisibility,
                         (void*)&detour_check_instance_visibility,
                         &trampoline, kPrologue, sizeof(kPrologue),
                         "PIPE3D_CheckInstanceVisibility (first-person gear)"))
            g_check_instance_visibility =
                (PFN_CheckInstanceVisibility)trampoline;

        trampoline = nullptr;
        if (hook_install((void*)kCalcVVClipInfo, (void*)&detour_calc_vv,
                         &trampoline, kPrologue, sizeof(kPrologue),
                         "CAMERA_CalcVVClipInfo"))
            g_calc_vv = (PFN_CalcVV)trampoline;

        trampoline = nullptr;
        hook_install((void*)kTerrainClipRect, (void*)&detour_clip_rect,
                     &trampoline, kPrologue, sizeof(kPrologue),
                     "TERRAIN_GetClipRect");

        trampoline = nullptr;
        if (hook_install((void*)kDrawTerrainGroup,
                         (void*)&detour_draw_terrain_group, &trampoline,
                         kPrologue, sizeof(kPrologue),
                         "DRAW_DrawTerrainGroup (terrain strip culling)"))
            g_draw_terrain_group = (PFN_DrawTerrainGroup)trampoline;
    }


}
