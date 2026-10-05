#include "vr_session.h"
#include "camera_head.h"
#include "stereo.h"

#include "../common/config.h"
#include "../common/log.h"
#include "tune.h"
#include "ui_space.h"
#include "water_stereo.h"
#include "hud_capture.h"

#include <windows.h>
#include <cstring>
#include <cmath>
#include <cstdarg>

#include "openvr.h"

namespace trlvr
{
    namespace
    {
        // Only the C entry points are imported; everything else is a vtable
        // call through the interface pointer.
        typedef uint32_t(__cdecl* PFN_InitInternal2)(vr::EVRInitError*, vr::EVRApplicationType, const char*);
        typedef void(__cdecl* PFN_ShutdownInternal)();
        typedef void* (__cdecl* PFN_GetGenericInterface)(const char*, vr::EVRInitError*);
        typedef bool(__cdecl* PFN_IsInterfaceVersionValid)(const char*);
        typedef bool(__cdecl* PFN_IsHmdPresent)();
        typedef const char* (__cdecl* PFN_GetInitErrorAsEnglishDescription)(vr::EVRInitError);

        HMODULE g_dll = nullptr;
        PFN_InitInternal2 p_InitInternal2 = nullptr;
        PFN_ShutdownInternal p_ShutdownInternal = nullptr;
        PFN_GetGenericInterface p_GetGenericInterface = nullptr;
        PFN_IsInterfaceVersionValid p_IsInterfaceVersionValid = nullptr;
        PFN_IsHmdPresent p_IsHmdPresent = nullptr;
        PFN_GetInitErrorAsEnglishDescription p_ErrorText = nullptr;

        vr::IVRSystem* g_system = nullptr;
        vr::IVRCompositor* g_compositor = nullptr;
        bool g_ready = false;
        char g_status[256] = "not initialised";

        EyeInfo g_eyes[2]{};
        unsigned g_rt_width = 0, g_rt_height = 0;
        float g_ipd = 0.0f;

        Eye g_current_eye = EyeCenter;
        // The game's own pure projection, remembered from the uploads that are
        // one, so world-space draws can be decomposed against it.
        GameProjection g_game_proj{};
        Mat4 g_game_proj_inv = Mat4::identity();
        bool g_game_proj_known = false;

        unsigned g_rt_w = 0, g_rt_h = 0;
        unsigned g_scene_w = 0, g_scene_h = 0;   // the back buffer's size

        // Only the main scene gets the headset's camera. A shadow map is drawn
        // from a light's point of view, not the viewer's, and forcing the eye
        // projection onto it destroys the shadows -- the 256x256 passes were
        // being replaced before this check existed.
        bool drawing_the_scene()
        {
            if (!g_scene_w || !g_scene_h)
                return true;                     // size not known yet
            if (g_rt_w == g_scene_w && g_rt_h == g_scene_h)
                return true;

            // PCWaterEffect renders a mask/distortion image at half the
            // scene's width and height. In same-frame stereo that target also
            // contains two eyes, each at quarter scene width. Treat it as a
            // scene target only while the positively identified water pass is
            // on the stack; other half-resolution post effects stay mono.
            return water_stereo_active() &&
                   g_rt_w * 2 == g_scene_w && g_rt_h * 2 == g_scene_h;
        }

        Mat4 g_head_view = Mat4::identity();
        float g_head_yaw = 0.0f;
        float g_head_yaw_raw = 0.0f;
        float g_yaw_offset = 0.0f;
        unsigned g_recenter_generation = 0;
        float g_head_pos[3] = { 0.0f, 0.0f, 0.0f };
        float g_pos_offset[3] = { 0.0f, 0.0f, 0.0f };
        float g_head_position_origin[3] = { 0.0f, 0.0f, 0.0f };
        bool g_head_position_origin_active = false;
        bool g_head_position_recenter_pending = false;
        bool g_pose_valid = false;
        vr::TrackedDevicePose_t
            g_tracked_poses[vr::k_unMaxTrackedDeviceCount]{};
        // Set once vr_set_render_poses has supplied WaitGetPoses results.
        bool g_render_poses_active = false;

        // CAMERA_CalculateWCTransform mostly stops running while the game is
        // paused, even though OpenVR poses continue to arrive every Present.
        // Remember the pose actually baked into the last main-camera matrices
        // and advance frozen scene draws by only the remaining motion.
        Mat4 g_main_camera_head = Mat4::identity();
        bool g_main_camera_head_valid = false;
        Mat4 g_pause_head_anchor = Mat4::identity();
        bool g_pause_head_anchor_valid = false;

        Mat4 pause_head_delta()
        {
            if (!ui_menu_active() || !config().hmd_drives_camera ||
                !g_pose_valid)
            {
                g_pause_head_anchor_valid = false;
                return Mat4::identity();
            }

            const Mat4 current = vr_head_camera_view();
            if (!g_pause_head_anchor_valid)
            {
                g_pause_head_anchor = g_main_camera_head_valid
                                    ? g_main_camera_head : current;
                g_pause_head_anchor_valid = true;
                log("menu: frozen scene now follows residual HMD motion; "
                    "pause HUD head-locked, main menu world-locked");
            }
            return rigid_inverse(g_pause_head_anchor) * current;
        }

        void set_status(const char* fmt, ...)
        {
            va_list args;
            va_start(args, fmt);
            vsnprintf(g_status, sizeof(g_status), fmt, args);
            va_end(args);
        }

        bool load_dll()
        {
            if (g_dll)
                return true;

            // Beside the game first, so a version we ship wins over anything
            // else on the search path.
            wchar_t path[MAX_PATH]{};
            swprintf_s(path, L"%sopenvr_api.dll", exe_dir());
            g_dll = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (!g_dll)
                g_dll = LoadLibraryW(L"openvr_api.dll");
            if (!g_dll)
            {
                set_status("openvr_api.dll not found beside the game or on the path");
                return false;
            }

            p_InitInternal2 = (PFN_InitInternal2)GetProcAddress(g_dll, "VR_InitInternal2");
            p_ShutdownInternal = (PFN_ShutdownInternal)GetProcAddress(g_dll, "VR_ShutdownInternal");
            p_GetGenericInterface = (PFN_GetGenericInterface)GetProcAddress(g_dll, "VR_GetGenericInterface");
            p_IsInterfaceVersionValid = (PFN_IsInterfaceVersionValid)GetProcAddress(g_dll, "VR_IsInterfaceVersionValid");
            p_IsHmdPresent = (PFN_IsHmdPresent)GetProcAddress(g_dll, "VR_IsHmdPresent");
            p_ErrorText = (PFN_GetInitErrorAsEnglishDescription)GetProcAddress(g_dll, "VR_GetVRInitErrorAsEnglishDescription");

            if (!p_InitInternal2 || !p_GetGenericInterface || !p_ShutdownInternal)
            {
                set_status("openvr_api.dll is missing the entry points we need");
                return false;
            }
            return true;
        }

        void read_device_info()
        {
            g_system->GetRecommendedRenderTargetSize(&g_rt_width, &g_rt_height);

            for (int i = 0; i < 2; ++i)
            {
                const vr::EVREye e = (i == 0) ? vr::Eye_Left : vr::Eye_Right;
                EyeInfo& info = g_eyes[i];
                g_system->GetProjectionRaw(e, &info.tan_left, &info.tan_right,
                                           &info.tan_top, &info.tan_bottom);
                const vr::HmdMatrix34_t t = g_system->GetEyeToHeadTransform(e);
                info.offset_x = t.m[0][3];
                info.offset_y = t.m[1][3];
                info.offset_z = t.m[2][3];
            }
            g_ipd = g_eyes[EyeRight].offset_x - g_eyes[EyeLeft].offset_x;

            char model[128]{};
            g_system->GetStringTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd,
                                                     vr::Prop_TrackingSystemName_String,
                                                     model, sizeof(model), nullptr);
            char serialish[128]{};
            g_system->GetStringTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd,
                                                     vr::Prop_ModelNumber_String,
                                                     serialish, sizeof(serialish), nullptr);

            log("vr: headset '%s' / '%s'", model, serialish);
            log("vr: recommended render target %ux%u per eye", g_rt_width, g_rt_height);
            log("vr: ipd %.1f mm", g_ipd * 1000.0f);
            for (int i = 0; i < 2; ++i)
            {
                const EyeInfo& in = g_eyes[i];
                log("vr: %s eye tangents l=%.4f r=%.4f t=%.4f b=%.4f  offset (%.4f, %.4f, %.4f) m",
                    i == 0 ? "left " : "right",
                    in.tan_left, in.tan_right, in.tan_top, in.tan_bottom,
                    in.offset_x, in.offset_y, in.offset_z);
            }
        }
    }

    bool vr_init()
    {
        if (g_ready)
            return true;
        if (!config().vr_enabled)
        {
            set_status("disabled in trlvr.ini");
            log("vr: %s", g_status);
            return false;
        }
        if (!load_dll())
        {
            log("vr: %s", g_status);
            return false;
        }
        if (p_IsHmdPresent && !p_IsHmdPresent())
        {
            set_status("no headset detected");
            log("vr: %s", g_status);
            return false;
        }

        // Scene when we intend to submit frames -- only a Scene application
        // gets an IVRCompositor, and only Scene will start SteamVR if it is not
        // already up. Background is the read-only mode: poses and frusta, no
        // image, and it attaches to a running SteamVR without disturbing it.
        const vr::EVRApplicationType appType = config().vr_submit
            ? vr::VRApplication_Scene
            : vr::VRApplication_Background;

        vr::EVRInitError err = vr::VRInitError_None;
        p_InitInternal2(&err, appType, nullptr);
        if (err != vr::VRInitError_None)
        {
            set_status("VR_Init failed: %s",
                       p_ErrorText ? p_ErrorText(err) : "unknown error");
            log("vr: %s", g_status);
            return false;
        }

        if (p_IsInterfaceVersionValid && !p_IsInterfaceVersionValid(vr::IVRSystem_Version))
        {
            p_ShutdownInternal();
            set_status("runtime is too old for %s", vr::IVRSystem_Version);
            log("vr: %s", g_status);
            return false;
        }

        g_system = (vr::IVRSystem*)p_GetGenericInterface(vr::IVRSystem_Version, &err);
        if (!g_system || err != vr::VRInitError_None)
        {
            p_ShutdownInternal();
            set_status("could not get %s", vr::IVRSystem_Version);
            log("vr: %s", g_status);
            return false;
        }

        if (config().vr_submit)
        {
            g_compositor = (vr::IVRCompositor*)p_GetGenericInterface(
                vr::IVRCompositor_Version, &err);
            if (!g_compositor || err != vr::VRInitError_None)
            {
                log("vr: no compositor (%s) -- frames cannot be submitted",
                    vr::IVRCompositor_Version);
                g_compositor = nullptr;
            }
            else
            {
                log("vr: compositor ready (%s)", vr::IVRCompositor_Version);
            }
        }

        g_ready = true;
        set_status("ready");
        log("vr: session up as %s (%s)",
            config().vr_submit ? "Scene" : "Background", vr::IVRSystem_Version);
        read_device_info();
        return true;
    }

    void* vr_compositor_raw()
    {
        return g_compositor;
    }

    void vr_shutdown()
    {
        if (g_ready && p_ShutdownInternal)
            p_ShutdownInternal();
        g_ready = false;
        g_system = nullptr;
        g_compositor = nullptr;
        set_status("shut down");
    }

    // What the game last uploaded to c0, and what we put there instead.
    //
    // The interface is identified by depth testing being off -- but the game
    // sets that *after* uploading the matrix, so at upload time every draw
    // still looks like world geometry. Deciding there meant the interface was
    // always substituted, which is what put it in the world at some arbitrary
    // near depth, and why a live size control appeared to do nothing.
    //
    // So both are kept and the decision is made at the draw, where the state
    // that identifies the interface is finally true. Same-frame stereo needs
    // exactly this: a matrix chosen per draw rather than per upload.
    namespace
    {
        float g_c0_game[16] = { 0 };
        float g_c0_uploaded[16] = { 0 };
        bool  g_c0_have = false;
    }

    void vr_note_c0(const float* game, const float* uploaded)
    {
        for (int i = 0; i < 16; ++i)
        {
            g_c0_game[i] = game[i];
            g_c0_uploaded[i] = uploaded ? uploaded[i] : game[i];
        }
        g_c0_have = true;
    }

    bool vr_draw_c0_override(float* out)
    {
        if (!g_c0_have || !g_ready || !config().ui_passthrough)
            return false;
        if (!ui_drawing_interface() || !drawing_the_scene())
            return false;
        return vr_substitute_c0_for_eye(g_c0_game, out, g_current_eye);
    }

    const float* vr_draw_c0_restore()
    {
        return g_c0_uploaded;
    }

    const float* vr_game_c0() { return g_c0_game; }
    bool vr_have_game_c0()    { return g_c0_have; }

    void vr_log_hand_draw_c0_sample(int hand, int marker, Eye eye,
                                    const float* actual_regs16)
    {
        if (hand < 0 || hand > 1 || eye < EyeLeft || eye > EyeCenter ||
            !g_c0_have || marker < -2)
            return;

        // A hand has multiple materials and is issued once per eye. Capture
        // the first matrix for each marked state, then only path/depth changes.
        // This detects a ledge-only projection without flooding the log.
        struct Sample { int marker, path; float near_z; };
        static Sample seen[2][3]{};
        static bool initialized[2][3]{};
        static unsigned reports = 0;

        const GameProjection pure = projection_from_registers(g_c0_game);
        const bool wvp = !pure.valid && is_perspective_registers(g_c0_game);
        const int path = ui_drawing_interface() ? 3 :
                         pure.valid ? 0 : wvp ? 1 : 2;
        GameProjection depth{};
        bool recovered = false;
        if (pure.valid)
        {
            depth = pure;
            recovered = true;
        }
        else if (wvp && g_game_proj_known)
        {
            Mat4 matrix{};
            for (int column = 0; column < 4; ++column)
                for (int row = 0; row < 4; ++row)
                    matrix.m[row][column] = g_c0_game[column * 4 + row];
            recovered = projection_depth_from_wvp(
                matrix, g_game_proj, &depth);
        }
        const float near_z = recovered ? depth.near_z : -1.0f;
        Sample& previous = seen[hand][eye];
        const bool changed = !initialized[hand][eye] ||
            previous.marker != marker || previous.path != path ||
            fabsf(previous.near_z - near_z) > 0.25f;
        if (!changed || reports >= 40)
            return;
        initialized[hand][eye] = true;
        previous.marker = marker;
        previous.path = path;
        previous.near_z = near_z;
        ++reports;

        float expected[16]{};
        const bool substituted =
            vr_substitute_c0_for_eye(g_c0_game, expected, eye);
        const float* reference = substituted ? expected : g_c0_game;
        float max_expected_error = -1.0f;
        float max_game_error = -1.0f;
        if (actual_regs16)
        {
            max_expected_error = max_game_error = 0.0f;
            for (int i = 0; i < 16; ++i)
            {
                const float e = fabsf(actual_regs16[i] - reference[i]);
                const float g = fabsf(actual_regs16[i] - g_c0_game[i]);
                if (e > max_expected_error) max_expected_error = e;
                if (g > max_game_error) max_game_error = g;
            }
        }

        const char* const paths[] =
            { "pure", "WVP", "unrecognised", "interface" };
        const char* const eyes[] = { "left", "right", "centre" };
        log("render: hand c0 sample=%d hand=%s eye=%s path=%s "
            "scene=%d substitute=%d depth-recovered=%d "
            "near=%.3f far=%.1f cached-near=%.3f "
            "actual-vs-expected=%.5g actual-vs-game=%.5g",
            marker, hand == 0 ? "left" : "right", eyes[eye],
            paths[path], drawing_the_scene() ? 1 : 0,
            substituted ? 1 : 0, recovered ? 1 : 0,
            near_z, recovered ? depth.far_z : -1.0f,
            g_game_proj_known ? g_game_proj.near_z : -1.0f,
            max_expected_error, max_game_error);
        log("render: hand c0 game "
            "[%+.4f,%+.4f,%+.4f,%+.4f] "
            "[%+.4f,%+.4f,%+.4f,%+.4f] "
            "[%+.4f,%+.4f,%+.4f,%+.4f] "
            "[%+.4f,%+.4f,%+.4f,%+.4f]",
            g_c0_game[0], g_c0_game[1], g_c0_game[2], g_c0_game[3],
            g_c0_game[4], g_c0_game[5], g_c0_game[6], g_c0_game[7],
            g_c0_game[8], g_c0_game[9], g_c0_game[10], g_c0_game[11],
            g_c0_game[12], g_c0_game[13], g_c0_game[14], g_c0_game[15]);
        if (actual_regs16)
            log("render: hand c0 actual "
                "[%+.4f,%+.4f,%+.4f,%+.4f] "
                "[%+.4f,%+.4f,%+.4f,%+.4f] "
                "[%+.4f,%+.4f,%+.4f,%+.4f] "
                "[%+.4f,%+.4f,%+.4f,%+.4f]",
                actual_regs16[0], actual_regs16[1],
                actual_regs16[2], actual_regs16[3],
                actual_regs16[4], actual_regs16[5],
                actual_regs16[6], actual_regs16[7],
                actual_regs16[8], actual_regs16[9],
                actual_regs16[10], actual_regs16[11],
                actual_regs16[12], actual_regs16[13],
                actual_regs16[14], actual_regs16[15]);
    }
    bool vr_drawing_the_scene() { return drawing_the_scene(); }

    bool vr_stereo_target_eye_size(unsigned* width, unsigned* height)
    {
        if (!width || !height || !g_scene_w || !g_scene_h)
            return false;
        if (g_rt_w == g_scene_w && g_rt_h == g_scene_h)
        {
            *width = g_scene_w / 2;
            *height = g_scene_h;
            return *width != 0 && *height != 0;
        }
        if (water_stereo_active() &&
            g_rt_w * 2 == g_scene_w && g_rt_h * 2 == g_scene_h)
        {
            *width = g_rt_w / 2;
            *height = g_rt_h;
            static bool reported = false;
            if (!reported)
            {
                reported = true;
                log("water stereo: %ux%u effect target split into two %ux%u eyes",
                    g_rt_w, g_rt_h, *width, *height);
            }
            return *width != 0 && *height != 0;
        }
        return false;
    }

    // Sky dome (2026-10-03). The game places it at a fixed offset from the
    // retail camera position: stick up/down moved the camera and the sky
    // by exactly the same amount (F4 capture: -53/-117/-2209 units both),
    // while the VR eye stays put -- so the sky slid in every view.
    // Camera-pinned draws are recognised by their world origin keeping one
    // offset from the retail camera while the camera moves (checked over
    // the first draws of each frame, where the sky is drawn); such offsets
    // are remembered, and every draw at one is moved by (eye - retail
    // camera) and drawn without eye separation, at infinity.
    struct SkyCandidate
    {
        float offset[3];
        float retail[3];
    };
    SkyCandidate g_sky_last[8];
    int g_sky_last_count = 0;
    SkyCandidate g_sky_this[8];
    int g_sky_this_count = 0;
    // Learned offsets, a ring (2026-10-05: four fixed slots filled up in
    // the user's level by 285 s -- one with a 2-unit duplicate -- and a
    // later area's sky was never recentred, so it slid again).
    float g_sky_offsets[16][3];
    int g_sky_offset_count = 0;
    int g_sky_offset_next = 0;
    // Offsets of this and last frame's sky draws: a sky draw keeps being
    // recognised when its offset drifts a little between frames.
    float g_sky_hits_last[8][3];
    int g_sky_hits_last_count = 0;
    float g_sky_hits_this[8][3];
    int g_sky_hits_this_count = 0;
    int g_sky_frame_draws = 0;
    bool g_sky_no_eye_offset = false;

    void sky_frame_boundary()
    {
        memcpy(g_sky_last, g_sky_this, sizeof(g_sky_this));
        g_sky_last_count = g_sky_this_count;
        g_sky_this_count = 0;
        memcpy(g_sky_hits_last, g_sky_hits_this, sizeof(g_sky_hits_this));
        g_sky_hits_last_count = g_sky_hits_this_count;
        g_sky_hits_this_count = 0;
        g_sky_frame_draws = 0;
    }

    // Whether a draw with this world origin is pinned to the retail camera;
    // fills delta with the move that centres it on the drawn eye.
    bool sky_pinned_draw(const float origin[3], float delta[3])
    {
        float retail[3]{}, eye[3]{}, focus[3]{};
        if (!camera_frame_positions(retail, eye, focus))
            return false;
        float offset[3];
        for (int k = 0; k < 3; ++k)
            offset[k] = origin[k] - retail[k];
        auto same = [](const float* a, const float* b, float tolerance) {
            return fabsf(a[0] - b[0]) < tolerance &&
                   fabsf(a[1] - b[1]) < tolerance &&
                   fabsf(a[2] - b[2]) < tolerance;
        };
        // Learn from the first draws of the frame.
        const int draw_index = g_sky_frame_draws++;
        if (draw_index < 8)
        {
            for (int i = 0; i < g_sky_last_count; ++i)
            {
                const SkyCandidate& c = g_sky_last[i];
                const float moved =
                    fabsf(c.retail[0] - retail[0]) +
                    fabsf(c.retail[1] - retail[1]) +
                    fabsf(c.retail[2] - retail[2]);
                // 2026-10-05 (user: a black shape near the eye for half a
                // session): an offset of (-108, -297, 1252) was learned on
                // the frame gameplay began, when Lara and the camera jumped
                // together from the spawn point; from then on a world draw
                // at that offset was moved onto the eye. Every real sky so
                // far sat >= 3600 units from the camera. So: learn only
                // offsets at least 2500 units away, and never on a frame
                // where the camera jumped (more than 400 units).
                const float offset_length = sqrtf(offset[0] * offset[0] +
                                                  offset[1] * offset[1] +
                                                  offset[2] * offset[2]);
                if (moved > 10.0f && same(c.offset, offset, 2.0f) &&
                    (offset_length < 2500.0f || moved > 400.0f))
                {
                    static unsigned rejected = 0;
                    if (rejected++ < 3)
                        log("sky: camera-following draw at offset (%.0f, "
                            "%.0f, %.0f) not taken as sky (%s)", offset[0],
                            offset[1], offset[2], offset_length < 2500.0f
                                ? "too close" : "camera jumped");
                }
                else if (moved > 10.0f && same(c.offset, offset, 2.0f))
                {
                    bool known = false;
                    for (int j = 0; j < g_sky_offset_count && !known; ++j)
                        known = same(g_sky_offsets[j], offset, 4.0f);
                    if (!known)
                    {
                        memcpy(g_sky_offsets[g_sky_offset_next], offset,
                               sizeof(offset));
                        g_sky_offset_next = (g_sky_offset_next + 1) % 16;
                        if (g_sky_offset_count < 16)
                            ++g_sky_offset_count;
                        static unsigned reports = 0;
                        if (reports++ < 48)
                            log("sky: draw pinned to the retail camera at "
                                "offset (%.0f, %.0f, %.0f) -- recentred on "
                                "the VR eye and drawn at infinity (%d "
                                "remembered)", offset[0], offset[1],
                                offset[2], g_sky_offset_count);
                    }
                }
            }
            if (g_sky_this_count < 8)
            {
                SkyCandidate& c = g_sky_this[g_sky_this_count++];
                memcpy(c.offset, offset, sizeof(offset));
                memcpy(c.retail, retail, sizeof(retail));
            }
        }
        bool pinned = false;
        for (int j = 0; j < g_sky_offset_count && !pinned; ++j)
            pinned = same(g_sky_offsets[j], offset, 2.0f);
        // Last frame's sky, drifted by a few units (first draws only).
        for (int j = 0; j < g_sky_hits_last_count && !pinned &&
                        draw_index < 12; ++j)
            pinned = same(g_sky_hits_last[j], offset, 24.0f);
        if (!pinned)
            return false;
        if (g_sky_hits_this_count < 8)
            memcpy(g_sky_hits_this[g_sky_hits_this_count++], offset,
                   sizeof(offset));
        for (int k = 0; k < 3; ++k)
            delta[k] = eye[k] - retail[k];
        return true;
    }

    // F4 (skybox hunt): world origins of the next 12 left-eye WVP draws,
    // against the retail camera, the drawn eye and the focus point.
    int g_world_capture = 0;
    int g_world_logged = 0;
    float g_world_seen[40][3];

    // Per-draw matrix work, cached exactly (2026-10-05, performance): the
    // inverse of the main camera's view and of the draw's projection were
    // recomputed for every draw and every eye, though both rarely change
    // within a frame. Keyed by the full matrix / projection values, so the
    // results are identical.
    static bool main_camera_wc(float* out);

    static bool main_camera_wc_inverse(Mat4* inverse)
    {
        static Mat4 key{}, value{};
        static bool valid = false;
        Mat4 v{};
        if (!main_camera_wc(&v.m[0][0]))
            return false;
        if (valid && memcmp(&key, &v, sizeof(v)) == 0)
        {
            *inverse = value;
            return true;
        }
        if (!invert(v, &value))
        {
            valid = false;
            return false;
        }
        key = v;
        valid = true;
        *inverse = value;
        return true;
    }

    static bool projection_inverse_cached(const GameProjection& p,
                                          Mat4* inverse)
    {
        struct Entry
        {
            float key[7];
            Mat4 value;
            bool valid;
        };
        static Entry entries[4]{};
        static int next = 0;
        const float key[7] = { p.sx, p.sy, p.ox, p.oy, p.q, p.near_z,
                               p.far_z };
        for (const Entry& e : entries)
            if (e.valid && memcmp(e.key, key, sizeof(key)) == 0)
            {
                *inverse = e.value;
                return true;
            }
        Entry& e = entries[next];
        if (!invert(projection_from_game(p), &e.value))
            return false;
        memcpy(e.key, key, sizeof(key));
        e.valid = true;
        next = (next + 1) % 4;
        *inverse = e.value;
        return true;
    }

    static bool main_camera_wc(float* out)
    {
        __try
        {
            memcpy(out, reinterpret_cast<const unsigned char*>(0x010FC660) +
                   0x120, 16 * sizeof(float));
            return true;
        }
        __except(EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void vr_world_capture_start()
    {
        g_world_capture = 3000;
        g_world_logged = 0;
        float retail[3]{}, eye[3]{}, focus[3]{};
        if (camera_frame_positions(retail, eye, focus))
            log("world capture: retail camera (%.0f, %.0f, %.0f), drawn eye "
                "(%.0f, %.0f, %.0f), focus (%.0f, %.0f, %.0f)", retail[0],
                retail[1], retail[2], eye[0], eye[1], eye[2], focus[0],
                focus[1], focus[2]);
    }

    bool vr_substitute_c0(const float* in, float* out)
    {
        // The alternating path, which draws one eye per frame.
        return vr_substitute_c0_for_eye(in, out, g_current_eye);
    }

    // MOVIE_StartPlay stores its Bink handle at 0x0100257C; MOVIE_StopPlay
    // clears it.
    static bool movie_playing()
    {
        __try
        {
            return *reinterpret_cast<void* const*>(0x0100257C) != nullptr;
        }
        __except(EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool vr_substitute_c0_for_eye(const float* in, float* out, Eye eye)
    {
        if (!g_ready || !in || !out || !config().head_tracking)
            return false;
        if (!drawing_the_scene())
            return false;

        // This is identity during gameplay and the main menu. During an
        // in-game pause it advances geometry from the last head pose that the
        // now-frozen camera contained to the pose OpenVR supplied this frame.
        // Evaluate it before the interface branch so leaving pause clears the
        // anchor even when the next thing drawn is only menu UI.
        const Mat4 paused_head = pause_head_delta();

        // These primitives deliberately skip the engine's scene-matrix update.
        // They need neither the world camera nor positional eye separation,
        // but identical render-image coordinates are not identical physical
        // rays on an asymmetric headset. Reproject the game's mono NDC through
        // each eye's raw frustum so both eyes see the same head-centred ray.
        // With no eye translation the resulting surface converges at optical
        // infinity, safely away from the face. It is not yet a separate render
        // texture / OpenVR overlay.
        // A playing movie is a flat screen quad. Give it the main menu's
        // treatment: mono rays through each eye's frustum (no disparity) and
        // world-locked, level and facing forward, instead of following gaze.
        // Loading screens are the same kind of flat picture as a movie.
        // Mission Prep: only its menu draws take this path (world-locked
        // below); its 3D backdrop is handled further down. Routing every draw
        // here sent the backdrop to the mono fallback (log: "HUD matrix was
        // not a pure projection"), so its own mapping never ran.
        const bool prep_menu = ui_mission_prep_active() &&
                               ui_drawing_interface();
        const bool movie = movie_playing() || ui_loading_screen_active() ||
                           prep_menu;
        const bool screen_wipe = ui_drawing_screen_wipe();
        if (config().ui_passthrough &&
            (ui_drawing_interface() || movie || screen_wipe))
        {
            const GameProjection ui_gp = projection_from_registers(in);
            const float scale = screen_wipe ? 1.0f : tune_ui_scale();

            if (ui_gp.valid)
            {
                if (screen_wipe && camera_first_person_active())
                {
                    static DWORD last_wipe_report = 0;
                    const DWORD now = GetTickCount();
                    if (!last_wipe_report ||
                        now - last_wipe_report >= 5000)
                    {
                        last_wipe_report = now;
                        log("ui: first-person screen wipe projected as a "
                            "full-size head-locked overlay (eye %d)", int(eye));
                    }
                }
                const EyeInfo& l = g_eyes[EyeLeft];
                const EyeInfo& r = g_eyes[EyeRight];
                EyeInfo centre{};
                centre.tan_left   = 0.5f * (l.tan_left + r.tan_left);
                centre.tan_right  = 0.5f * (l.tan_right + r.tan_right);
                centre.tan_top    = 0.5f * (l.tan_top + r.tan_top);
                centre.tan_bottom = 0.5f * (l.tan_bottom + r.tan_bottom);

                const EyeInfo& target = (eye == EyeCenter) ? centre : g_eyes[eye];
                const Mat4 reference = projection_from_tangents(
                    centre.tan_left, centre.tan_right,
                    centre.tan_top, centre.tan_bottom,
                    ui_gp.near_z, ui_gp.far_z, ui_gp.sy < 0.0f);
                const Mat4 target_projection = projection_from_tangents(
                    target.tan_left, target.tan_right,
                    target.tan_top, target.tan_bottom,
                    ui_gp.near_z, ui_gp.far_z, ui_gp.sy < 0.0f);
                const bool world_locked = !screen_wipe &&
                    (ui_interface_world_locked() || movie);
                static short reported_screen = -2;
                const short screen = movie_playing() ? (short)0
                                                     : ui_root_screen_id();
                if (movie && screen != reported_screen)
                {
                    reported_screen = screen;
                    log("stereo: %s drawn as a world-locked screen, level "
                        "at the recentred forward (root screen %d)",
                        screen == 0 ? "movie" : "loading screen", screen);
                }
                static bool menu_anchor_valid = false;
                static Mat4 menu_anchor = Mat4::identity();
                Mat4 ui{};
                if (world_locked)
                {
                    // The game's HUD plane sits at (or extremely close to)
                    // its projection near plane. Rotating that plane to keep
                    // it fixed in the world immediately puts parts behind the
                    // old near plane, so D3D clips individual triangles into
                    // the diagonal wedges seen in menus. The menu is already
                    // depth-isolated at the draw; give only this overlay an
                    // equally isolated, close clip plane. X/Y rays do not
                    // change, just the otherwise irrelevant clip depth.
                    const float menu_near = ui_gp.near_z > 1.0f
                                          ? 1.0f : ui_gp.near_z;
                    const Mat4 menu_projection = projection_from_tangents(
                        target.tan_left, target.tan_right,
                        target.tan_top, target.tan_bottom,
                        menu_near, ui_gp.far_z, ui_gp.sy < 0.0f);
                    const Mat4 current = vr_head_rotation();
                    if (!menu_anchor_valid)
                    {
                        // Level and facing forward (SteamVR's forward, or the
                        // recentred one), not the head pose at the moment the
                        // menu opened -- that left menus tilted or off to the
                        // side. vr_head_rotation() is already recentred, so
                        // forward is the identity.
                        menu_anchor = Mat4::identity();
                        menu_anchor_valid = true;
                        log("ui: menu anchored level at the recentred forward; "
                            "overlay near %.2f replaces game near %.2f",
                            menu_near, ui_gp.near_z);
                    }
                    const Mat4 relative = rigid_inverse(menu_anchor) * current;
                    ui = reproject_ndc_to_world_locked(
                        projection_from_game(ui_gp), reference,
                        menu_projection, relative, scale);
                }
                else
                {
                    menu_anchor_valid = false;
                    // World markers (lock-on reticle, caution icon, grab
                    // prompt) are placed by the game projecting a world
                    // point through the camera the mod already head-posed
                    // (prescale removed). The HUD path then drew them at
                    // ui_scale with part of the head rotation mixed in, so
                    // they slid with the gaze. A first fix added the full
                    // head rotation and moved them twice as far as the
                    // world (user screenshots 2026-10-02). Correct: keep
                    // them head-locked with no extra rotation and turn the
                    // game's screen point into the game projection's own
                    // ray, at full size.
                    const bool marker = ui_drawing_world_marker() &&
                        !screen_wipe && !ui_menu_active() &&
                        g_game_proj_known;
                    const float game_sx = fabsf(g_game_proj.sx);
                    const float game_sy = fabsf(g_game_proj.sy);
                    if (marker && game_sx > 1e-3f && game_sy > 1e-3f)
                    {
                        Mat4 inv_reference;
                        const float hud_near = ui_gp.near_z > 1.0f
                                             ? 1.0f : ui_gp.near_z;
                        const Mat4 hud_projection = projection_from_tangents(
                            target.tan_left, target.tan_right,
                            target.tan_top, target.tan_bottom,
                            hud_near, ui_gp.far_z, ui_gp.sy < 0.0f);
                        if (invert(reference, &inv_reference))
                        {
                            Mat4 clip_scale = Mat4::identity();
                            clip_scale.m[0][0] =
                                fabsf(reference.m[0][0]) / game_sx;
                            clip_scale.m[1][1] =
                                fabsf(reference.m[1][1]) / game_sy;
                            // Stereo depth: the sprite has no depth of its
                            // own, so both eyes saw it at infinity while the
                            // object it marks has real parallax -- worst in
                            // board mode, where eye separation is scaled up
                            // (user screenshot 2026-10-03). Shear each eye's
                            // ray by its offset over the marker depth (camera
                            // to Lara), placing the sprite at that distance.
                            Mat4 converge = Mat4::identity();
                            // First person: the camera focus is ~0.7 m out
                            // (log 2026-10-04), not the marked object, so a
                            // tunable distance is used instead ([ and ]).
                            const bool fp_marker =
                                camera_first_person_active();
                            const float depth = fp_marker
                                ? tune_marker_distance() * tune_world_scale()
                                : camera_marker_depth();
                            if (eye != EyeCenter && depth > 1.0f)
                            {
                                const float s = tune_world_scale() *
                                    tune_ipd_scale() * camera_stereo_scale();
                                // Tuned in the headset (2026-10-03): 0.75 in
                                // board mode (the marker sits a little beyond
                                // Lara); 1 elsewhere, untuned.
                                const float k =
                                    camera_world_scale_factor() > 1.001f
                                    ? 0.75f : 1.0f;
                                converge.m[2][0] =
                                    -target.offset_x * s / depth * k;
                                converge.m[2][1] =
                                    -target.offset_y * s / depth * k;
                            }
                            ui = projection_from_game(ui_gp) * clip_scale *
                                 inv_reference * converge * hud_projection;
                            static bool marker_reported = false;
                            if (!marker_reported)
                            {
                                marker_reported = true;
                                log("ui: world markers drawn head-locked at "
                                    "the game projection's rays (ray scale "
                                    "%.3f x %.3f)", clip_scale.m[0][0],
                                    clip_scale.m[1][1]);
                            }
                            registers_from_matrix(ui, out);
                            return true;
                        }
                    }
                    // The binocular overlay is a viewfinder: it stays
                    // rigidly head-locked (user, 2026-10-02), not on the
                    // gameplay HUD's hud_follow drag.
                    const float follow = hud_binocular_view_active()
                        ? 1.0f : tune_hud_follow();
                    if (!screen_wipe && !ui_menu_active() && follow < 0.999f)
                    {
                        // Leave a small fraction of current head rotation in
                        // the gameplay HUD. This makes edge information move
                        // gently inward as the player looks toward it, while
                        // remaining stable and deterministic when the head is
                        // still. The close overlay clip plane is required for
                        // the same reason as the world-locked menu: rotating a
                        // plane that starts on the game's near depth otherwise
                        // slices its triangles.
                        const float hud_near = ui_gp.near_z > 1.0f
                                             ? 1.0f : ui_gp.near_z;
                        const Mat4 hud_projection = projection_from_tangents(
                            target.tan_left, target.tan_right,
                            target.tan_top, target.tan_bottom,
                            hud_near, ui_gp.far_z, ui_gp.sy < 0.0f);
                        const Mat4 relative = rotation_fraction(
                            vr_head_rotation(), 1.0f - follow);
                        ui = reproject_ndc_to_world_locked(
                            projection_from_game(ui_gp), reference,
                            hud_projection, relative, scale);
                    }
                    else
                    {
                        // Pause UI deliberately remains rigidly head-locked.
                        ui = reproject_ndc_to_frustum(
                            projection_from_game(ui_gp), reference,
                            target_projection, scale);
                    }
                }
                registers_from_matrix(ui, out);

                static bool reported = false;
                if (!reported)
                {
                    reported = true;
                    log("ui: HUD lens correction active; scale %.3f, "
                        "gameplay follow %.2f, "
                        "per-eye frusta on, positional eye offset off; "
                        "menus %s", scale, tune_hud_follow(),
                        config().menu_world_locked ? "world-locked"
                                                   : "head-locked");
                }
                return true;
            }

            if (screen_wipe && camera_first_person_active())
            {
                static DWORD last_invalid_report = 0;
                const DWORD now = GetTickCount();
                if (!last_invalid_report ||
                    now - last_invalid_report >= 5000)
                {
                    last_invalid_report = now;
                    log("ui: screen wipe had no pure projection; "
                        "mono overlay fallback used");
                }
            }
            // Unexpected, but safer than putting an unrecognised HUD matrix
            // through the world stereo decomposition. Retain the preceding
            // mono behaviour and size correction for that draw.
            memcpy(out, in, 16 * sizeof(float));
            for (int component = 0; component < 4; ++component)
            {
                out[component] *= scale;
                out[4 + component] *= scale;
            }
            static bool warned = false;
            if (!warned)
            {
                warned = true;
                log("ui: HUD matrix was not a pure projection; using mono fallback");
            }
            return true;
        }

        const GameProjection gp = projection_from_registers(in);

        // Mission Prep: the camera kept the game's flat shot (camera_head),
        // so every scene draw already produces the flat picture. Post-map
        // its clip coordinates exactly as the world-locked menu does
        // (source * clip scale * inv(centre frustum) * head rotation * eye
        // frustum), so background, Lara and menu stay pinned together.
        if (config().ui_passthrough && ui_mission_prep_active() &&
            (gp.valid || is_perspective_registers(in)))
        {
            Mat4 m{};
            for (int i = 0; i < 4; ++i)
                for (int k = 0; k < 4; ++k)
                    m.m[k][i] = in[i * 4 + k];
            const GameProjection depth = gp.valid ? gp
                : (g_game_proj_known ? g_game_proj : gp);
            const float near_z = depth.valid && depth.near_z > 0.0f
                ? depth.near_z : 1.0f;
            const float far_z = depth.valid && depth.far_z > near_z
                ? depth.far_z : 100000.0f;
            const bool flip = depth.valid ? depth.sy < 0.0f : false;
            const EyeInfo& l = g_eyes[EyeLeft];
            const EyeInfo& r = g_eyes[EyeRight];
            const EyeInfo& target = (eye == EyeCenter) ? l : g_eyes[eye];
            const Mat4 reference = projection_from_tangents(
                0.5f * (l.tan_left + r.tan_left),
                0.5f * (l.tan_right + r.tan_right),
                0.5f * (l.tan_top + r.tan_top),
                0.5f * (l.tan_bottom + r.tan_bottom),
                near_z, far_z, flip);
            const Mat4 panel = projection_from_tangents(
                target.tan_left, target.tan_right,
                target.tan_top, target.tan_bottom,
                near_z, far_z, flip);
            const Mat4 source = gp.valid ? projection_from_game(gp) : m;
            // The backdrop fills the view (slightly oversized) and turns
            // after the head with a lag, which reads as a distant backdrop
            // (user request): its anchor eases toward the current head
            // rotation, and the mapping counter-rotates by the remaining
            // difference.
            static Mat4 anchor = Mat4::identity();
            static LARGE_INTEGER last{};
            LARGE_INTEGER now{}, frequency{};
            QueryPerformanceCounter(&now);
            QueryPerformanceFrequency(&frequency);
            const Mat4 current = vr_head_rotation();
            const double since = last.QuadPart && frequency.QuadPart
                ? (double)(now.QuadPart - last.QuadPart) /
                  (double)frequency.QuadPart : 1.0;
            if (since > 0.5)
                anchor = current;  // screen (re)opened: start centred
            else if (since > 0.0)
            {
                const float lag = config().mission_prep_backdrop_lag;
                const float follow = lag > 0.001f
                    ? 1.0f - expf(-(float)since / lag) : 1.0f;
                anchor = anchor * rotation_fraction(
                    rigid_inverse(anchor) * current, follow);
            }
            last = now;
            const float backdrop = config().mission_prep_backdrop_scale;
            registers_from_matrix(reproject_ndc_to_world_locked(
                source, reference, panel,
                rigid_inverse(anchor) * current, backdrop), out);
            static bool reported = false;
            if (!reported)
            {
                reported = true;
                log("stereo: Mission Prep backdrop fills the view (scale "
                    "%.2f) and follows the head with a %.2f s lag",
                    backdrop, config().mission_prep_backdrop_lag);
            }
            return true;
        }

        if (gp.valid)
        {
            // Lara's skinned vertices arrive in view space and use this pure
            // projection directly. During a ledge hang the retail game varies
            // its depth term with the third-person camera. The recovered
            // near value can become negative; then clip.z exceeds clip.w for
            // every nearby hand vertex, so the entire hand vanishes even
            // against the sky. Use one valid close plane for tagged VR hands
            // while retaining the game's far distance and the HMD frustum.
            // Do not cache this per-hand depth as the scene projection.
            const int hand = camera_first_person_current_gpu_hand();
            // Third-person controller hands in board mode: drawn at their
            // own size at 1/F of the controller distance (camera_head);
            // scaling view space by F about the head puts them on the
            // controllers at the player's size. The eye offset follows the
            // scale, so stereo stays right.
            if (!camera_first_person_active() && (hand == 2 || hand == 3))
            {
                const float grow = camera_world_scale_factor();
                Mat4 scale = Mat4::identity();
                if (grow > 1.001f)
                    scale.m[0][0] = scale.m[1][1] = scale.m[2][2] = grow;
                // The scene's own projection (same near/far), so the hands'
                // depth values are comparable with the world's. A near of 4
                // (copied from first person, where the whole scene uses 4)
                // bent the non-linear depth curve: hands read as farther
                // than they were and nearer-looking rocks hid them (user
                // screenshots 2026-10-02). They are an arm's length away,
                // far beyond any game near plane, so nothing clips.
                registers_from_matrix(scale * paused_head *
                    vr_eye_matrix(eye, gp), out);
                static bool reported = false;
                if (!reported && grow > 1.001f)
                {
                    reported = true;
                    log("render: third-person hands drawn x%.1f about the "
                        "head (board scale)", grow);
                }
                return true;
            }
            if (camera_first_person_active() && hand >= 0 && hand < 2)
            {
                GameProjection hand_projection = gp;
                hand_projection.near_z = 4.0f;
                if (eye == EyeLeft)
                {
                    static unsigned reported_mark[2] = { ~0u, ~0u };
                    const unsigned mark = tune_ledge_visibility_marker();
                    if (reported_mark[hand] != mark)
                    {
                        reported_mark[hand] = mark;
                        log("render: tracked %s hand projection F8=%u "
                            "game near=%+.3f -> VR near=4.000 far=%.1f",
                            hand == 0 ? "left" : "right", mark,
                            gp.near_z, gp.far_z);
                    }
                }
                registers_from_matrix(
                    paused_head * vr_eye_matrix(eye, hand_projection), out);
                return true;
            }

            // View-space geometry: the registers are the projection itself, so
            // ours replaces it outright. Remember it -- world-space draws are
            // decomposed against this.
            if (!g_game_proj_known ||
                gp.sx != g_game_proj.sx || gp.sy != g_game_proj.sy ||
                gp.near_z != g_game_proj.near_z)
            {
                Mat4 inv;
                if (invert(projection_from_game(gp), &inv))
                {
                    g_game_proj = gp;
                    g_game_proj_inv = inv;
                    g_game_proj_known = true;
                }
            }

            registers_from_matrix(paused_head * vr_eye_matrix(eye, gp), out);
            return true;
        }

        // Not a pure projection. If w still varies with position it is a
        // world-view-projection: X * P, with P the projection we cached above.
        // Recover X and re-compose it with the headset's projection instead.
        //
        //     new = M * inverse(P) * head_view * P_eye
        //
        // Anything else -- a 2D or orthographic pass, where w is constant --
        // is left alone.
        if (!g_game_proj_known || !is_perspective_registers(in))
        {
            return false;
        }

        Mat4 m{};
        for (int i = 0; i < 4; ++i)          // registers are columns
            for (int k = 0; k < 4; ++k)
                m.m[k][i] = in[i * 4 + k];

        // The accurate-aim camera can change the depth range without a
        // matching pure-projection upload. Recover q/near/far from this WVP
        // itself before removing P; using the stale cached depth terms leaves
        // a projective shear that differs across the two eye projections.
        GameProjection draw_projection = g_game_proj;
        Mat4 draw_projection_inv = g_game_proj_inv;
        GameProjection recovered{};
        if (projection_depth_from_wvp(m, g_game_proj, &recovered))
        {
            Mat4 recovered_inv{};
            if (projection_inverse_cached(recovered, &recovered_inv))
            {
                draw_projection = recovered;
                draw_projection_inv = recovered_inv;

                if (camera_accurate_aim_active() &&
                    fabsf(recovered.near_z - g_game_proj.near_z) > 0.25f)
                {
                    static DWORD last_depth_report = 0;
                    const DWORD now = GetTickCount();
                    if (!last_depth_report || now - last_depth_report >= 5000)
                    {
                        last_depth_report = now;
                        log("stereo: accurate-aim WVP near %.2f (cached pure "
                            "projection %.2f); using per-draw depth terms",
                            recovered.near_z, g_game_proj.near_z);
                    }
                }
            }
        }

        if (g_world_capture > 0 && eye == EyeLeft)
        {
            --g_world_capture;
            Mat4 v{}, v_inv{};
            const Mat4 x = m * draw_projection_inv;
            if (main_camera_wc(&v.m[0][0]) && invert(v, &v_inv))
            {
                const Mat4 w = x * v_inv;
                float retail[3]{}, at[3]{}, focus[3]{};
                camera_frame_positions(retail, at, focus);
                auto dist = [&](const float* a) {
                    float d = 0.0f;
                    for (int k = 0; k < 3; ++k)
                        d += (w.m[3][k] - a[k]) * (w.m[3][k] - a[k]);
                    return sqrtf(d);
                };
                bool known = false;
                for (int i = 0; i < g_world_logged && !known; ++i)
                    known = fabsf(g_world_seen[i][0] - w.m[3][0]) < 1.0f &&
                            fabsf(g_world_seen[i][1] - w.m[3][1]) < 1.0f &&
                            fabsf(g_world_seen[i][2] - w.m[3][2]) < 1.0f;
                if (!known && g_world_logged < 40)
                {
                    for (int k = 0; k < 3; ++k)
                        g_world_seen[g_world_logged][k] = w.m[3][k];
                    ++g_world_logged;
                    log("world capture %2d: origin (%.0f, %.0f, %.0f); %.0f "
                        "from the retail camera, %.0f from the eye, %.0f from "
                        "focus", g_world_logged, w.m[3][0], w.m[3][1],
                        w.m[3][2], dist(retail), dist(at), dist(focus));
                }
            }
        }
        // The sky dome: recentred on the VR eye, no eye separation.
        {
            Mat4 v_inv{};
            if (main_camera_wc_inverse(&v_inv))
            {
                const Mat4 w = m * draw_projection_inv * v_inv;
                const float origin[3] = { w.m[3][0], w.m[3][1], w.m[3][2] };
                float delta[3]{};
                if (std::isfinite(origin[0]) && sky_pinned_draw(origin, delta))
                {
                    g_sky_no_eye_offset = true;
                    const Mat4 sky = translation(delta[0], delta[1], delta[2]) *
                        m * draw_projection_inv * paused_head *
                        vr_eye_matrix(eye, draw_projection);
                    g_sky_no_eye_offset = false;
                    registers_from_matrix(sky, out);
                    return true;
                }
            }
        }
        const Mat4 replaced = m * draw_projection_inv * paused_head *
                              vr_eye_matrix(eye, draw_projection);
        registers_from_matrix(replaced, out);

        // Log one world-space draw per eye, exactly as uploaded. If the two
        // differ only trivially then the eye offset is not reaching world
        // geometry, whatever the maths says it should do -- which is what the
        // headset reports: the offset moves the UI and leaves the world alone.
        {
            static bool logged[3] = { false, false, false };
            const int e = (int)eye;
            if (e >= 0 && e < 3 && !logged[e])
            {
                logged[e] = true;
                const char* names[3] = { "left", "right", "centre" };
                log("world-space draw, %s eye -- incoming c0..c3:", names[e]);
                for (int i = 0; i < 4; ++i)
                    log("    in  c%d [ %12.5f %12.5f %12.5f %12.5f ]",
                        i, in[i * 4 + 0], in[i * 4 + 1], in[i * 4 + 2], in[i * 4 + 3]);
                log("  uploaded instead:");
                for (int i = 0; i < 4; ++i)
                    log("    out c%d [ %12.5f %12.5f %12.5f %12.5f ]",
                        i, out[i * 4 + 0], out[i * 4 + 1], out[i * 4 + 2], out[i * 4 + 3]);
            }
        }
        return true;
    }

    void vr_set_render_target_size(unsigned w, unsigned h)
    {
        g_rt_w = w;
        g_rt_h = h;

    }

    void vr_set_scene_size(unsigned w, unsigned h)
    {
        g_scene_w = w;
        g_scene_h = h;
        g_rt_w = w;
        g_rt_h = h;
    }

    Eye vr_current_eye()
    {
        return g_current_eye;
    }

    void vr_set_current_eye(Eye eye)
    {
        g_current_eye = eye;
    }

    void* vr_get_interface(const char* version)
    {
        if (!g_ready || !p_GetGenericInterface || !version)
            return nullptr;
        vr::EVRInitError err = vr::VRInitError_None;
        return p_GetGenericInterface(version, &err);
    }

    bool vr_ready() { return g_ready; }
    const char* vr_status() { return g_status; }
    const EyeInfo& vr_eye(Eye eye) { return g_eyes[eye]; }
    float vr_ipd() { return g_ipd; }

    float vr_max_tangent()
    {
        if (!g_ready)
            return 0.0f;
        float m = 0.0f;
        for (int i = 0; i < 2; ++i)
        {
            const EyeInfo& e = g_eyes[i];
            const float v[4] = { fabsf(e.tan_left), fabsf(e.tan_right),
                                 fabsf(e.tan_top), fabsf(e.tan_bottom) };
            for (int k = 0; k < 4; ++k)
                if (v[k] > m)
                    m = v[k];
        }
        return m;
    }

    float vr_eye_aspect()
    {
        if (!g_ready)
            return 0.0f;
        const EyeInfo& l = g_eyes[EyeLeft];
        const float w = l.tan_right - l.tan_left;
        const float h = l.tan_bottom - l.tan_top;
        if (w <= 0.0f || h == 0.0f)
            return 0.0f;
        return w / fabsf(h);
    }
    bool vr_pose_valid() { return g_pose_valid; }
    const Mat4& vr_head_view() { return g_head_view; }
    float vr_head_yaw() { return g_head_yaw; }

    Mat4 vr_head_rotation()
    {
        // Rotation only, recentred, with the turn scale applied.
        //
        // set_yaw does both at once and is already pinned by tests: it puts a
        // chosen heading on the matrix and leaves pitch and roll exactly as
        // they are. Recentring is a change of heading and so is turn scale,
        // so both are that one operation rather than two more conventions to
        // get wrong.
        Mat4 r = g_head_view;
        r.m[3][0] = r.m[3][1] = r.m[3][2] = 0.0f;

        const float heading = wrap_pi(g_head_yaw_raw - g_yaw_offset);
        return set_yaw(r, heading * tune_turn_scale());
    }

    Mat4 vr_head_camera_view()
    {
        const Mat4 head = vr_head_rotation();
        Mat4 move = Mat4::identity();
        if (config().hmd_drives_position)
        {
            float pos[3] = { 0.0f, 0.0f, 0.0f };
            vr_head_position(pos);
            if (g_head_position_origin_active)
            {
                pos[0] -= g_head_position_origin[0];
                pos[1] -= g_head_position_origin[1];
                pos[2] -= g_head_position_origin[2];
            }
            const float scale = tune_move_scale();
            move = translation(-pos[0] * scale,
                               -pos[1] * scale,
                               -pos[2] * scale);
        }
        return move * head;
    }

    float vr_head_drop()
    {
        if (!config().hmd_drives_position || !g_head_position_origin_active)
            return 0.0f;
        // Engine view convention: +Y is down.
        const float drop = (g_head_pos[1] - g_head_position_origin[1]) *
                           tune_move_scale();
        return std::isfinite(drop) ? drop : 0.0f;
    }

    void vr_set_head_position_origin(const float* xyz, bool active)
    {
        g_head_position_origin_active = active;
        g_head_position_recenter_pending = false;
        if (active && xyz)
        {
            g_head_position_origin[0] = xyz[0];
            g_head_position_origin[1] = xyz[1];
            g_head_position_origin[2] = xyz[2];
        }
        else
        {
            g_head_position_origin[0] = 0.0f;
            g_head_position_origin[1] = 0.0f;
            g_head_position_origin[2] = 0.0f;
        }
    }

    void vr_note_main_camera_head_applied()
    {
        g_main_camera_head = vr_head_camera_view();
        g_main_camera_head_valid = true;

        // A rare camera rebuild while paused already incorporates the current
        // pose, so make it the new residual origin immediately.
        if (ui_menu_active())
        {
            g_pause_head_anchor = g_main_camera_head;
            g_pause_head_anchor_valid = true;
        }
    }

    void vr_head_position(float* xyz)
    {
        xyz[0] = g_head_pos[0];
        xyz[1] = g_head_pos[1];
        xyz[2] = g_head_pos[2];
    }

    bool vr_controller_body_position(bool left, float* right, float* up,
                                     float* forward)
    {
        if (!g_ready || !g_system || !g_pose_valid ||
            !right || !up || !forward)
            return false;

        const vr::ETrackedControllerRole role = left
            ? vr::TrackedControllerRole_LeftHand
            : vr::TrackedControllerRole_RightHand;
        const vr::TrackedDeviceIndex_t index =
            g_system->GetTrackedDeviceIndexForControllerRole(role);
        if (index == vr::k_unTrackedDeviceIndexInvalid ||
            index >= vr::k_unMaxTrackedDeviceCount)
            return false;

        const vr::TrackedDevicePose_t& controller = g_tracked_poses[index];
        const vr::TrackedDevicePose_t& hmd =
            g_tracked_poses[vr::k_unTrackedDeviceIndex_Hmd];
        if (!controller.bPoseIsValid || !controller.bDeviceIsConnected ||
            !hmd.bPoseIsValid || !hmd.bDeviceIsConnected)
            return false;

        const vr::HmdMatrix34_t& hm = hmd.mDeviceToAbsoluteTracking;
        const vr::HmdMatrix34_t& cm = controller.mDeviceToAbsoluteTracking;
        const float dx = cm.m[0][3] - hm.m[0][3];
        const float dy = cm.m[1][3] - hm.m[1][3];
        const float dz = cm.m[2][3] - hm.m[2][3];

        // OpenVR's local -Z is forward. Use headset yaw but deliberately
        // discard pitch/roll: looking down at the gesture must not rotate the
        // virtual chest up toward the face. With no body tracker, headset yaw
        // is the least surprising approximation of torso heading.
        float fx = -hm.m[0][2];
        float fz = -hm.m[2][2];
        const float length = sqrtf(fx * fx + fz * fz);
        if (length < 1.0e-4f)
            return false;
        fx /= length;
        fz /= length;
        const float rx = -fz;
        const float rz = fx;

        *right = dx * rx + dz * rz;
        *up = dy;
        *forward = dx * fx + dz * fz;
        return true;
    }

    bool vr_controller_body_forward_speed(bool left, float* speed)
    {
        if (!speed || !g_ready || !g_system || !g_pose_valid)
            return false;
        const vr::ETrackedControllerRole role = left
            ? vr::TrackedControllerRole_LeftHand
            : vr::TrackedControllerRole_RightHand;
        const vr::TrackedDeviceIndex_t index =
            g_system->GetTrackedDeviceIndexForControllerRole(role);
        if (index == vr::k_unTrackedDeviceIndexInvalid ||
            index >= vr::k_unMaxTrackedDeviceCount)
            return false;
        const vr::TrackedDevicePose_t& controller = g_tracked_poses[index];
        const vr::TrackedDevicePose_t& hmd =
            g_tracked_poses[vr::k_unTrackedDeviceIndex_Hmd];
        if (!controller.bPoseIsValid || !controller.bDeviceIsConnected ||
            !hmd.bPoseIsValid || !hmd.bDeviceIsConnected)
            return false;
        const vr::HmdMatrix34_t& hm = hmd.mDeviceToAbsoluteTracking;
        float fx = -hm.m[0][2], fz = -hm.m[2][2];
        const float length = sqrtf(fx * fx + fz * fz);
        if (length < 1.0e-4f)
            return false;
        fx /= length;
        fz /= length;
        const float vx = controller.vVelocity.v[0] - hmd.vVelocity.v[0];
        const float vz = controller.vVelocity.v[2] - hmd.vVelocity.v[2];
        *speed = vx * fx + vz * fz;
        return std::isfinite(*speed);
    }

    bool vr_controller_body_axis_endpoint(bool left, int axis, float metres,
                                          float* right, float* up,
                                          float* forward)
    {
        if (!right || !up || !forward || axis < 0 || axis > 2 ||
            !std::isfinite(metres) || !vr_controller_body_position(
                left, right, up, forward))
            return false;
        const vr::ETrackedControllerRole role = left
            ? vr::TrackedControllerRole_LeftHand
            : vr::TrackedControllerRole_RightHand;
        const vr::TrackedDeviceIndex_t index =
            g_system->GetTrackedDeviceIndexForControllerRole(role);
        if (index == vr::k_unTrackedDeviceIndexInvalid ||
            index >= vr::k_unMaxTrackedDeviceCount)
            return false;
        const vr::HmdMatrix34_t& cm =
            g_tracked_poses[index].mDeviceToAbsoluteTracking;
        const vr::HmdMatrix34_t& hm =
            g_tracked_poses[vr::k_unTrackedDeviceIndex_Hmd]
                .mDeviceToAbsoluteTracking;
        float fx = -hm.m[0][2], fz = -hm.m[2][2];
        const float length = sqrtf(fx * fx + fz * fz);
        if (length < 1.0e-4f)
            return false;
        fx /= length;
        fz /= length;
        const float rx = -fz, rz = fx;
        const float sign = axis == 2 ? -1.0f : 1.0f;
        const float dx = sign * metres * cm.m[0][axis];
        const float dy = sign * metres * cm.m[1][axis];
        const float dz = sign * metres * cm.m[2][axis];
        *right += dx * rx + dz * rz;
        *up += dy;
        *forward += dx * fx + dz * fz;
        return true;
    }

    bool vr_project_body_point(Eye eye, float right, float up, float forward,
                               float* u, float* v)
    {
        if (!u || !v || eye == EyeCenter || !g_ready || !g_system ||
            !g_pose_valid)
            return false;
        const vr::TrackedDevicePose_t& hmd =
            g_tracked_poses[vr::k_unTrackedDeviceIndex_Hmd];
        if (!hmd.bPoseIsValid || !hmd.bDeviceIsConnected)
            return false;
        const vr::HmdMatrix34_t& hm = hmd.mDeviceToAbsoluteTracking;
        float fx = -hm.m[0][2], fz = -hm.m[2][2];
        const float length = sqrtf(fx * fx + fz * fz);
        if (length < 1.0e-4f)
            return false;
        fx /= length;
        fz /= length;
        // Convert the yaw-only body vector back to tracking coordinates,
        // then into the fully pitched/rolled HMD coordinate system.
        const float tracking[3] = {
            right * -fz + forward * fx,
            up,
            right * fx + forward * fz
        };
        float head[3]{};
        for (int axis = 0; axis < 3; ++axis)
            for (int row = 0; row < 3; ++row)
                head[axis] += hm.m[row][axis] * tracking[row];
        const vr::EVREye e = eye == EyeLeft ? vr::Eye_Left : vr::Eye_Right;
        const vr::HmdMatrix34_t eye_to_head = g_system->GetEyeToHeadTransform(e);
        const float delta[3] = {
            head[0] - eye_to_head.m[0][3],
            head[1] - eye_to_head.m[1][3],
            head[2] - eye_to_head.m[2][3]
        };
        float point[3]{};
        for (int axis = 0; axis < 3; ++axis)
            for (int row = 0; row < 3; ++row)
                point[axis] += eye_to_head.m[row][axis] * delta[row];
        const float depth = -point[2];
        if (depth < 0.08f)
            return false;
        const EyeInfo& projection = vr_eye(eye);
        const float dx = projection.tan_right - projection.tan_left;
        const float dy = projection.tan_bottom - projection.tan_top;
        if (dx < 1.0e-4f || dy < 1.0e-4f)
            return false;
        *u = (point[0] / depth - projection.tan_left) / dx;
        *v = (-point[1] / depth - projection.tan_top) / dy;
        return std::isfinite(*u) && std::isfinite(*v);
    }

    bool vr_project_head_point(Eye eye, float x, float y, float z,
                               float* u, float* v)
    {
        if (!u || !v || eye == EyeCenter || !g_ready || !g_system ||
            !g_pose_valid)
            return false;
        // Controller-to-head translations use world_scale * move_scale.
        // view_from_pose flips engine Y and Z against OpenVR, so reverse
        // that flip before applying the runtime's per-eye transform.
        const float scale = tune_world_scale() * tune_move_scale();
        if (!(scale > 0.0f) || !std::isfinite(scale))
            return false;
        const float head[3] = { x / scale, -y / scale, -z / scale };
        const vr::EVREye e = eye == EyeLeft ? vr::Eye_Left : vr::Eye_Right;
        const vr::HmdMatrix34_t eye_to_head =
            g_system->GetEyeToHeadTransform(e);
        const float delta[3] = {
            head[0] - eye_to_head.m[0][3],
            head[1] - eye_to_head.m[1][3],
            head[2] - eye_to_head.m[2][3]
        };
        float point[3]{};
        for (int axis = 0; axis < 3; ++axis)
            for (int row = 0; row < 3; ++row)
                point[axis] += eye_to_head.m[row][axis] * delta[row];
        const float depth = -point[2];
        if (depth < 0.08f)
            return false;
        const EyeInfo& projection = vr_eye(eye);
        const float dx = projection.tan_right - projection.tan_left;
        const float dy = projection.tan_bottom - projection.tan_top;
        if (dx < 1.0e-4f || dy < 1.0e-4f)
            return false;
        *u = (point[0] / depth - projection.tan_left) / dx;
        *v = (-point[1] / depth - projection.tan_top) / dy;
        return std::isfinite(*u) && std::isfinite(*v);
    }

    namespace
    {
        // grip-local -> raw-device-local, in metres (engine convention),
        // measured by vr_input from the SteamVR /pose/grip actions.
        Mat4 g_grip_offset[2] = { Mat4::identity(), Mat4::identity() };
        bool g_grip_offset_valid[2] = { false, false };
    }

    void vr_set_controller_grip_offset(bool left, const Mat4& metres)
    {
        const int hand = left ? 0 : 1;
        g_grip_offset[hand] = metres;
        g_grip_offset_valid[hand] = true;
    }

    bool vr_controller_grip_offset(bool left, Mat4* metres)
    {
        const int hand = left ? 0 : 1;
        if (!g_grip_offset_valid[hand])
            return false;
        if (metres)
            *metres = g_grip_offset[hand];
        return true;
    }

    bool vr_controller_grip_head_pose(bool left, Mat4* grip_to_head)
    {
        const int hand = left ? 0 : 1;
        if (!grip_to_head || !g_grip_offset_valid[hand])
            return false;
        Mat4 raw;
        if (!vr_controller_head_pose(left, &raw))
            return false;
        Mat4 offset = g_grip_offset[hand];
        const float scale = tune_world_scale();
        for (int axis = 0; axis < 3; ++axis)
            offset.m[3][axis] *= scale;
        *grip_to_head = offset * raw;
        return true;
    }

    bool vr_controller_head_pose(bool left, Mat4* controller_to_head)
    {
        if (!controller_to_head || !g_ready || !g_system || !g_pose_valid)
            return false;

        const vr::ETrackedControllerRole role = left
            ? vr::TrackedControllerRole_LeftHand
            : vr::TrackedControllerRole_RightHand;
        const vr::TrackedDeviceIndex_t index =
            g_system->GetTrackedDeviceIndexForControllerRole(role);
        if (index == vr::k_unTrackedDeviceIndexInvalid ||
            index >= vr::k_unMaxTrackedDeviceCount)
            return false;

        const vr::TrackedDevicePose_t& controller = g_tracked_poses[index];
        const vr::TrackedDevicePose_t& hmd =
            g_tracked_poses[vr::k_unTrackedDeviceIndex_Hmd];
        if (!controller.bPoseIsValid || !controller.bDeviceIsConnected ||
            !hmd.bPoseIsValid || !hmd.bDeviceIsConnected)
            return false;

        const float scale = tune_world_scale();
        const Mat4 controller_view = view_from_pose(
            &controller.mDeviceToAbsoluteTracking.m[0][0], scale);
        const Mat4 hmd_view = view_from_pose(
            &hmd.mDeviceToAbsoluteTracking.m[0][0], scale);

        // controller->tracking * tracking->head = controller->head.
        *controller_to_head = rigid_inverse(controller_view) * hmd_view;
        const float move_scale = tune_move_scale();
        controller_to_head->m[3][0] *= move_scale;
        controller_to_head->m[3][1] *= move_scale;
        controller_to_head->m[3][2] *= move_scale;
        return true;
    }

    bool vr_body_point_head_position(float right, float up, float forward,
                                     float out[3])
    {
        if (!out || !g_ready || !g_system || !g_pose_valid)
            return false;
        const vr::TrackedDevicePose_t& hmd =
            g_tracked_poses[vr::k_unTrackedDeviceIndex_Hmd];
        if (!hmd.bPoseIsValid || !hmd.bDeviceIsConnected)
            return false;
        const vr::HmdMatrix34_t& hm = hmd.mDeviceToAbsoluteTracking;
        float fx = -hm.m[0][2], fz = -hm.m[2][2];
        const float length = sqrtf(fx * fx + fz * fz);
        if (length < 1.0e-4f)
            return false;
        fx /= length;
        fz /= length;

        // The inverse of vr_controller_body_position, as a tracking-space
        // pose with no rotation, then the same composition as
        // vr_controller_head_pose so conventions cannot drift apart.
        vr::HmdMatrix34_t point{};
        point.m[0][0] = point.m[1][1] = point.m[2][2] = 1.0f;
        point.m[0][3] = hm.m[0][3] + right * -fz + forward * fx;
        point.m[1][3] = hm.m[1][3] + up;
        point.m[2][3] = hm.m[2][3] + right * fx + forward * fz;

        const float scale = tune_world_scale();
        const Mat4 point_view = view_from_pose(&point.m[0][0], scale);
        const Mat4 hmd_view = view_from_pose(&hm.m[0][0], scale);
        const Mat4 point_to_head = rigid_inverse(point_view) * hmd_view;
        const float move_scale = tune_move_scale();
        for (int axis = 0; axis < 3; ++axis)
        {
            out[axis] = point_to_head.m[3][axis] * move_scale;
            if (!std::isfinite(out[axis]))
                return false;
        }
        return true;
    }

    void vr_recenter()
    {
        for (int hand = 0; hand < 2; ++hand)
        {
            float right = 0.0f, up = 0.0f, forward = 0.0f;
            if (vr_controller_body_position(hand == 0, &right,
                                            &up, &forward))
                log("vr: recenter calibration %s hand right %+.3f, "
                    "up %+.3f, forward %+.3f metres",
                    hand == 0 ? "left" : "right", right, up, forward);
        }
        g_yaw_offset = g_head_yaw_raw;
        g_head_yaw = 0.0f;
        ++g_recenter_generation;

        // Position too, or recentring leaves you standing off to one side of
        // wherever the runtime thinks its origin is.
        const float sc = tune_world_scale();
        if (sc > 0.0f)
        {
            g_pos_offset[0] += g_head_pos[0] / sc;
            g_pos_offset[1] -= g_head_pos[1] / sc;
            g_pos_offset[2] -= g_head_pos[2] / sc;
        }

        if (g_head_position_origin_active)
        {
            // Zero first-person displacement immediately. The global offset
            // above makes the next sampled g_head_pos zero; until that sample
            // arrives, using the current point as the private origin avoids a
            // one-frame jump. The pending step below then rebases the private
            // origin to the newly sampled zero.
            g_head_position_origin[0] = g_head_pos[0];
            g_head_position_origin[1] = g_head_pos[1];
            g_head_position_origin[2] = g_head_pos[2];
            g_head_position_recenter_pending = true;
        }
        log("vr: recentred -- facing %+.1f deg is now forward",
            g_yaw_offset * 57.29578f);
    }

    unsigned vr_recenter_generation()
    {
        return g_recenter_generation;
    }

    void vr_render_target_size(unsigned* w, unsigned* h)
    {
        // Asked afresh: it carries SteamVR's resolution setting, which the
        // user may have changed since start-up (applied on a device reset).
        if (g_ready && g_system)
        {
            uint32_t rw = 0, rh = 0;
            g_system->GetRecommendedRenderTargetSize(&rw, &rh);
            if (rw && rh)
            {
                g_rt_width = rw;
                g_rt_height = rh;
            }
        }
        if (w) *w = g_rt_width;
        if (h) *h = g_rt_height;
    }

    static void ingest_poses();

    void vr_update_pose()
    {
        sky_frame_boundary();
        if (!g_ready)
            return;

        // With eyes drawn on alternate frames, refreshing the pose every frame
        // means the two halves of a stereo pair are taken from different head
        // positions. The pair then disagrees about where the world is, which
        // reads as jitter while turning -- and it is not a performance problem:
        // the game holds a locked 90 fps, it is just that each eye only gets 45
        // of them.
        //
        // Holding the pose across a pair makes the two images at least
        // consistent with each other. The cost is a frame of extra latency on
        // one eye, which the compositor's reprojection already handles.
        // Not when both eyes are drawn in the same frame: they already share an
        // instant, so there is no pair to hold a pose across. Worse, this decides
        // when a pair *starts* by watching the eye flip, and same-frame stereo
        // never flips -- so the condition below was never satisfied and the pose
        // was never read at all. Identity head view, zero yaw, no tracking, with
        // nothing in the log to say why. A mitigation outliving the problem it
        // was built for.
        if (config().stereo && config().stereo_lock_pose && !stereo_same_frame_active())
        {
            // Called before the eye is flipped, so the *next* frame draws the
            // other eye. Only refresh when that next frame starts a new pair.
            const bool next_starts_a_pair = (g_current_eye == EyeRight);
            if (!next_starts_a_pair)
                return;
        }

        // Once the compositor loop is running, the render poses come from
        // WaitGetPoses (vr_set_render_poses) -- the poses the compositor
        // assumes each submitted frame was rendered with. Reading our own
        // un-predicted pose here instead rendered every frame from an older
        // head position than the compositor expected, so its reprojection
        // made world-fixed content swim behind head motion.
        if (g_render_poses_active)
            return;
        g_system->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseSeated, 0.0f,
                                                  g_tracked_poses,
                                                  vr::k_unMaxTrackedDeviceCount);
        ingest_poses();
    }

    void vr_set_render_poses(const void* poses, unsigned count)
    {
        if (!g_ready || !poses || count < vr::k_unMaxTrackedDeviceCount)
            return;
        memcpy(g_tracked_poses, poses, sizeof(g_tracked_poses));
        if (!g_render_poses_active)
        {
            g_render_poses_active = true;
            log("vr: rendering with the compositor's predicted poses "
                "(WaitGetPoses, seated space)");
        }
        ingest_poses();
    }

    static void ingest_poses()
    {
        const vr::TrackedDevicePose_t& hmd =
            g_tracked_poses[vr::k_unTrackedDeviceIndex_Hmd];
        g_pose_valid = hmd.bPoseIsValid && hmd.bDeviceIsConnected;
        if (!g_pose_valid)
            return;

        g_head_view = view_from_pose(&hmd.mDeviceToAbsoluteTracking.m[0][0], tune_world_scale());

        // The heading, in the same terms the camera hook uses.
        //
        // Taken from the head's world-space forward direction, which for a
        // *view* matrix -- the inverse of the pose -- is column 2, not row 2.
        // Row 2 differs only by a sign for pure yaw, so it looks right up
        // until the pose has pitch in it; then the error grows with the turn
        // and peaks facing backwards, as an inverting heading and a sloping
        // horizon.
        g_head_yaw_raw = yaw_of(g_head_view);

        // The head's displacement, in engine units, in the camera's own space.
        //
        // Taken from the pose rather than read back out of the view matrix: the
        // view's translation row is -t*R, so recovering t from it means undoing
        // a rotation that the yaw surgery below is about to change. The same
        // basis change as the rotation -- diag(1,-1,-1) -- and the same units.
        {
            const float* m = &hmd.mDeviceToAbsoluteTracking.m[0][0];
            const float sc = tune_world_scale();
            g_head_pos[0] = (m[3] - g_pos_offset[0]) * sc;
            g_head_pos[1] = -(m[7] - g_pos_offset[1]) * sc;
            g_head_pos[2] = -(m[11] - g_pos_offset[2]) * sc;
        }

        if (g_head_position_origin_active &&
            g_head_position_recenter_pending)
        {
            g_head_position_origin[0] = g_head_pos[0];
            g_head_position_origin[1] = g_head_pos[1];
            g_head_position_origin[2] = g_head_pos[2];
            g_head_position_recenter_pending = false;
        }

        // What the camera is told is measured from wherever the player last
        // called forward, not from wherever the headset happened to be facing
        // when the runtime started. Sitting at a desk those are rarely the
        // same, and reaching for the keyboard makes it worse.
        g_head_yaw = wrap_pi(g_head_yaw_raw - g_yaw_offset);

        // The camera owns the head pose now -- all of it, rotation and
        // position -- so nothing is taken back out here and nothing is left
        // for the projection to make up.
        //
        // What was here before split the pose in two: yaw on the camera, pitch
        // and roll in the projection, with a residual to cover the gap when the
        // camera had not been rebuilt. Each half was right on its own and the
        // composition was not: applying pitch in view space after the camera had
        // already turned is not the same rotation as turning and pitching
        // together, and the error grows exactly as both angles grow. That is why
        // it read as fine looking straight ahead and wrong the moment you looked
        // up while turned.
        // Rotation and position can be taken separately. A third-person camera
        // that lurches with every head movement is worse than one that only
        // turns, so position can be scaled down or switched off on its own.
        if (!config().head_rotation)
        {
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    g_head_view.m[i][j] = (i == j) ? 1.0f : 0.0f;
        }
        // When the camera is being moved by the head, the displacement is
        // already in the world and must not be applied a second time here.
        const bool camera_has_it = config().hmd_drives_camera
                                && config().hmd_drives_position;
        const float ps = (config().head_position && !camera_has_it)
                       ? config().head_position_scale : 0.0f;
        g_head_view.m[3][0] *= ps;
        g_head_view.m[3][1] *= ps;
        g_head_view.m[3][2] *= ps;
    }

    Mat4 vr_eye_matrix(Eye eye, const GameProjection& game)
    {
        // With the camera carrying the head pose, the projection is the eye
        // offset and the headset frustum and nothing else. One owner for the
        // head, which is the whole point of the rewrite.
        if (!config().head_tracking || !g_pose_valid ||
            config().hmd_drives_camera)
            return vr_eye_projection(eye, game);

        return g_head_view * vr_eye_projection(eye, game);
    }

    // Binocular magnification. The VR frustum is the headset's own, so the
    // game's binocular zoom (a narrower game projection: larger x scale)
    // never reached the player. While the binocular overlay is drawn
    // (hud_binocular_view_active), the zoom is the game's x scale over its
    // normal value, learned slowly while the binoculars are down.
    float binocular_zoom(const GameProjection& game)
    {
        static float normal = 0.0f;
        static float reported = 1.0f;
        const float sx = fabsf(game.sx);
        if (!game.valid || !(sx > 0.01f) || !std::isfinite(sx))
            return 1.0f;
        if (!hud_binocular_view_active())
        {
            normal = normal > 0.0f ? normal + (sx - normal) * 0.02f : sx;
            if (reported != 1.0f)
            {
                reported = 1.0f;
                log("binoculars: view back to normal (game x scale %.3f)",
                    normal);
            }
            return 1.0f;
        }
        if (!(normal > 0.0f))
            return 1.0f;
        float zoom = sx / normal;
        zoom = zoom < 1.0f ? 1.0f : zoom > 20.0f ? 20.0f : zoom;
        if (fabsf(zoom - reported) > 0.25f)
        {
            reported = zoom;
            log("binoculars: VR view magnified x%.2f (game x scale %.3f, "
                "normal %.3f)", zoom, sx, normal);
        }
        return zoom;
    }

    Mat4 vr_eye_projection(Eye eye, const GameProjection& game)
    {
        const float zoom = binocular_zoom(game);
        // Mode 13 moves the camera into geometry that the original mono view
        // never exposes to two laterally separated eyes. Its normal near plane
        // can therefore cut a surface in just one eye. Four game units are
        // about 14 mm at the calibrated scale: close enough for the shoulder
        // camera without changing the depth range of normal gameplay.
        float near_z = game.near_z;
        // First person: always 4 units. Values above were already clamped;
        // the retail near recovered from Lara-space projections also swings
        // with head pitch and can go negative, which clips every close
        // vertex (the ledge-hand fault, fixed earlier for tagged hand draws
        // only). Drawn pistols and held gear are separate instances without
        // that tag, and kept being drawn while vanishing near the view edge.
        if (camera_first_person_active())
        {
            static bool invalid_reported = false;
            if (near_z < 1.0f && !invalid_reported)
            {
                invalid_reported = true;
                log("stereo: first-person projection near %.2f replaced by "
                    "4.0 (retail near can go negative with head pitch)",
                    near_z);
            }
            near_z = 4.0f;
        }
        else if (camera_accurate_aim_active() && near_z > 4.0f)
        {
            static bool announced = false;
            if (!announced)
            {
                announced = true;
                log("stereo: close-camera VR near plane %.1f -> 4.0 game "
                    "units", near_z);
            }
            near_z = 4.0f;
        }

        // (A board-view far-plane extension made no difference in the
        // headset, 2026-10-03: the board camera is only ~700 units from the
        // retail one. Removed.)
        const float far_z = game.far_z;

        // A centre eye for the mono stage: the average of the two frusta and no
        // lateral offset, so the single image is not shot from one eyeball.
        if (eye == EyeCenter)
        {
            const EyeInfo& l = g_eyes[EyeLeft];
            const EyeInfo& r = g_eyes[EyeRight];
            EyeInfo c{};
            c.tan_left   = 0.5f * (l.tan_left + r.tan_left) / zoom;
            c.tan_right  = 0.5f * (l.tan_right + r.tan_right) / zoom;
            c.tan_top    = 0.5f * (l.tan_top + r.tan_top) / zoom;
            c.tan_bottom = 0.5f * (l.tan_bottom + r.tan_bottom) / zoom;

            // No prescale correction here -- vr_eye_matrix undoes it in front
            // of the whole chain, so this stays a plain headset projection.
            return projection_from_tangents(c.tan_left, c.tan_right,
                                            c.tan_top, c.tan_bottom,
                                            near_z, far_z,
                                            game.sy < 0.0f);
        }

        const EyeInfo& in = g_eyes[eye];

        // The game's own near and far, so depth precision, fog and any clip
        // planes behave exactly as they always did. Only the frustum shape
        // changes.
        Mat4 proj = projection_from_tangents(in.tan_left / zoom,
                                             in.tan_right / zoom,
                                             in.tan_top / zoom,
                                             in.tan_bottom / zoom,
                                             near_z, far_z,
                                             game.sy < 0.0f);


        // Moving the camera to the eye is moving the world the other way.
        // Except during Bink playback (MOVIE_StartPlay stores its handle at
        // 0x0100257C): the movie is a flat quad a few units from the camera,
        // so a real eye separation gave each eye a widely shifted copy. With
        // no offset both eyes see it through their own frusta, converged at
        // infinity like a distant screen.
        // Mission Prep too: its close, staged shot of Lara behind the menu
        // had full eye separation, so each eye saw her widely shifted and
        // the screen could not be fused (user screenshot 2026-10-01).
        const bool prep = ui_mission_prep_active();
        const bool movie = movie_playing() || ui_loading_screen_active() ||
                           prep;
        static bool movie_reported = false;
        if (movie && !movie_reported)
        {
            movie_reported = true;
            log("stereo: movie playback drawn without eye separation");
        }
        static bool prep_reported = false;
        if (prep && !prep_reported)
        {
            prep_reported = true;
            log("stereo: Mission Prep scene drawn without eye separation "
                "(root screen %d)", ui_root_screen_id());
        }
        // Magnified: the eye separation shrinks with the zoom, as through
        // real binoculars, so the enlarged image still fuses.
        const float s = movie ? 0.0f
                      : tune_world_scale() * tune_ipd_scale()
                      * camera_stereo_scale() / zoom;
        Mat4 offset = g_sky_no_eye_offset ? Mat4::identity()
            : translation(-in.offset_x * s, -in.offset_y * s, -in.offset_z * s);
        return offset * proj;
    }
}


