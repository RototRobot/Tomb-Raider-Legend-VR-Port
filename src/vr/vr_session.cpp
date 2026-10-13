#include "vr_session.h"
#include "camera_head.h"
#include "stereo.h"

#include "reflection_stereo.h"
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
#include <cstdio>

#include "xr_runtime.h"

namespace trlvr
{
    namespace
    {
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
            // The water reflection, drawn per eye into its own targets.
            if (reflection_stereo_pass_active())
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
        // The headset's flat forward (tracking x, z) at the last recentre.
        // Head rotation is measured from it (g_yaw_offset), so head
        // position must be too: it was left in SteamVR's raw axes, which put
        // every lean -- and, with roomscale, every step -- off by the
        // recentre's yaw (user 2026-10-10: walking only went the right way
        // toward one direction; that log recentred at -57 degrees).
        float g_recentre_forward[2] = { 0.0f, -1.0f };

        // Raw head displacement (engine axes: x right, y down, z forward
        // of SteamVR's own frame) -> the recentred frame, and back.
        void head_raw_to_recentred(const float d[3], float out[3])
        {
            const float fx = g_recentre_forward[0], fz = g_recentre_forward[1];
            const float right = -d[0] * fz - d[2] * fx;
            const float forward = d[0] * fx - d[2] * fz;
            out[0] = right;
            out[1] = d[1];
            out[2] = forward;
        }

        void head_recentred_to_raw(const float d[3], float out[3])
        {
            const float fx = g_recentre_forward[0], fz = g_recentre_forward[1];
            out[0] = -d[0] * fz + d[2] * fx;
            out[1] = d[1];
            out[2] = -d[0] * fx - d[2] * fz;
        }
        bool g_head_position_origin_active = false;
        bool g_head_position_recenter_pending = false;
        bool g_pose_valid = false;
        TrackedPose
            g_tracked_poses[kPoseCount]{};
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
        // Theatre diagnostics, per 5 s: camera builds against frames, and
        // the largest correction the residual below made.
        unsigned g_theatre_builds = 0, g_theatre_frames = 0;
        float g_theatre_max_turn = 0.0f, g_theatre_max_move = 0.0f;

        // Bumped whenever anything pause_head_delta reads can change: new
        // poses (once a frame), a camera build, a recentre.
        unsigned g_head_state_generation = 1;

        Mat4 pause_head_delta_compute()
        {
            // Theatre (user 2026-10-09: the window "moves some" with the
            // head): the room-fixed surround is drawn with this frame's
            // pose, but the scene with the pose baked into the cutscene
            // camera at its last build -- and a cutscene camera is not
            // necessarily rebuilt every frame. Carry the scene from that
            // pose to this one, as a paused scene is carried.
            if (vr_theatre_active() && !ui_menu_active() &&
                config().hmd_drives_camera && g_pose_valid &&
                g_main_camera_head_valid)
            {
                g_pause_head_anchor_valid = false;
                const Mat4 delta = rigid_inverse(g_main_camera_head) *
                                   vr_head_camera_view();
                const float c = 0.5f * (delta.m[0][0] + delta.m[1][1] +
                                        delta.m[2][2] - 1.0f);
                const float turn = acosf(c > 1.0f ? 1.0f : c < -1.0f ? -1.0f
                                                                     : c);
                const float move = sqrtf(delta.m[3][0] * delta.m[3][0] +
                                         delta.m[3][1] * delta.m[3][1] +
                                         delta.m[3][2] * delta.m[3][2]);
                if (std::isfinite(turn) && turn > g_theatre_max_turn)
                    g_theatre_max_turn = turn;
                if (std::isfinite(move) && move > g_theatre_max_move)
                    g_theatre_max_move = move;
                return delta;
            }
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

        // Once per change, not once per draw (2026-10-11 performance pass:
        // paused, ~4000 draws x 2 eyes each redid the menu checks, the head
        // pose and an inverse -- 8-18 ms of CPU a frame, the pause menu at
        // 33 fps against 2-3 ms for the same scene in gameplay).
        Mat4 pause_head_delta()
        {
            static unsigned cached_generation = 0;
            static unsigned cached_recenter = 0;
            static Mat4 cached = Mat4::identity();
            if (cached_generation != g_head_state_generation ||
                cached_recenter != g_recenter_generation)
            {
                cached = pause_head_delta_compute();
                cached_generation = g_head_state_generation;
                cached_recenter = g_recenter_generation;
            }
            return cached;
        }

        void set_status(const char* fmt, ...)
        {
            va_list args;
            va_start(args, fmt);
            vsnprintf(g_status, sizeof(g_status), fmt, args);
            va_end(args);
        }

        // Each eye relative to the head, as a pose matrix.
        Mat34 eye_to_head_matrix(Eye eye)
        {
            const EyeInfo& info = g_eyes[eye == EyeRight ? EyeRight : EyeLeft];
            Mat34 out{};
            for (int row = 0; row < 3; ++row)
                for (int col = 0; col < 3; ++col)
                    out.m[row][col] = info.rot[row][col];
            out.m[0][3] = info.offset_x;
            out.m[1][3] = info.offset_y;
            out.m[2][3] = info.offset_z;
            return out;
        }

        void log_eyes(const char* source)
        {
            log("vr: ipd %.1f mm (%s)", g_ipd * 1000.0f, source);
            for (int i = 0; i < 2; ++i)
            {
                const EyeInfo& in = g_eyes[i];
                log("vr: %s eye tangents l=%.4f r=%.4f t=%.4f b=%.4f  offset (%.4f, %.4f, %.4f) m",
                    i == 0 ? "left " : "right",
                    in.tan_left, in.tan_right, in.tan_top, in.tan_bottom,
                    in.offset_x, in.offset_y, in.offset_z);
                // Cant: the angle between this eye's forward axis and the head's.
                const float cant = acosf(fminf(1.0f, fmaxf(-1.0f, in.rot[2][2])))
                                 * 57.29578f;
                if (cant > 0.05f)
                    log("vr: %s eye is canted %.2f degrees from the head axis",
                        i == 0 ? "left " : "right", cant);
            }
        }

        // OpenXR only reports the frusta and the eye offsets once a session
        // is running, and the session needs the device -- but the back
        // buffer is sized before the device exists. So start from a
        // symmetric frustum with the shape of the recommended image, and let
        // vr_set_eye_views replace it with the real one on the first frame.
        bool g_eyes_from_runtime = false;

        void read_device_info()
        {
            xr_recommended_size(&g_rt_width, &g_rt_height);

            const float aspect = g_rt_height
                ? (float)g_rt_width / (float)g_rt_height : 1.0f;
            const float half_v = 1.0f;
            const float half_h = half_v * aspect;
            for (int i = 0; i < 2; ++i)
            {
                EyeInfo& info = g_eyes[i];
                info = EyeInfo{};
                info.tan_left = -half_h;
                info.tan_right = half_h;
                info.tan_top = -half_v;
                info.tan_bottom = half_v;
                info.offset_x = i == 0 ? -0.032f : 0.032f;
                info.rot[0][0] = info.rot[1][1] = info.rot[2][2] = 1.0f;
            }
            g_ipd = g_eyes[EyeRight].offset_x - g_eyes[EyeLeft].offset_x;
            g_eyes_from_runtime = false;

            log("vr: headset '%s'", xr_system_name());
            log("vr: recommended render target %ux%u per eye", g_rt_width, g_rt_height);
            log_eyes("provisional, until the session reports the real frusta");
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
        if (!xr_instance_init())
        {
            set_status("%s", xr_last_error());
            log("vr: %s", g_status);
            return false;
        }

        g_ready = true;
        set_status("ready");
        log("vr: OpenXR instance up");
        if (!config().vr_submit)
            log("vr: [vr] submit = 0 -- no session is created, so there is "
                "no image in the headset and no tracking");
        read_device_info();
        return true;
    }

    void vr_set_eye_views(const XrEyeView eyes[2])
    {
        if (!g_ready || !eyes)
            return;
        float change = 0.0f;
        for (int i = 0; i < 2; ++i)
        {
            EyeInfo next{};
            next.tan_left = eyes[i].tan_left;
            next.tan_right = eyes[i].tan_right;
            next.tan_top = eyes[i].tan_top;
            next.tan_bottom = eyes[i].tan_bottom;
            next.offset_x = eyes[i].eye_to_head.m[0][3];
            next.offset_y = eyes[i].eye_to_head.m[1][3];
            next.offset_z = eyes[i].eye_to_head.m[2][3];
            for (int row = 0; row < 3; ++row)
                for (int col = 0; col < 3; ++col)
                    next.rot[row][col] = eyes[i].eye_to_head.m[row][col];
            const float* a = &next.tan_left;
            const float* b = &g_eyes[i].tan_left;
            // The four tangents and three offsets lead the struct.
            for (int k = 0; k < 7; ++k)
            {
                if (!std::isfinite(a[k]))
                    return;
                change = fmaxf(change, fabsf(a[k] - b[k]));
            }
            if (!(next.tan_right > next.tan_left) ||
                !(next.tan_bottom > next.tan_top))
                return;
            g_eyes[i] = next;
        }
        g_ipd = g_eyes[EyeRight].offset_x - g_eyes[EyeLeft].offset_x;
        // The IPD wheel moves these a fraction of a millimetre at a time.
        if (!g_eyes_from_runtime || change > 0.0005f)
        {
            const float provisional_aspect = g_rt_height
                ? (float)g_rt_width / (float)g_rt_height : 0.0f;
            log_eyes(g_eyes_from_runtime ? "changed" : "from the runtime");
            if (!g_eyes_from_runtime)
                log("vr: eye frustum aspect %.3f (the back buffer was sized "
                    "for %.3f)", vr_eye_aspect(), provisional_aspect);
            g_eyes_from_runtime = true;
        }
    }

    void vr_shutdown()
    {
        if (g_ready)
            xr_instance_shutdown();
        g_ready = false;
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
        float focus[3];
    };
    // 2026-10-11 (user: the second level's sky slid with the look stick and
    // precision aim again): the window was the first 8 calls of a frame --
    // one per eye per upload, so only ~4 draws; a level that draws other
    // things before its sky never had it learned. Now 32 calls.
    const int kSkyLearnCalls = 32;
    // Nearest a sky may be (2026-10-11: level 2's dome sits 2393 units from
    // the camera, and was refused at 2500; the black-slab draw it guards
    // against was 1302 units).
    const float kSkyMinDistance = 2000.0f;
    SkyCandidate g_sky_last[32];
    int g_sky_last_count = 0;
    SkyCandidate g_sky_this[32];
    int g_sky_this_count = 0;
    // Learned offsets, a ring (2026-10-05: four fixed slots filled up in
    // the user's level by 285 s -- one with a 2-unit duplicate -- and a
    // later area's sky was never recentred, so it slid again).
    float g_sky_offsets[16][3];
    int g_sky_offset_count = 0;
    int g_sky_offset_next = 0;
    // Offsets of this and last frame's sky draws: a sky draw keeps being
    // recognised when its offset drifts a little between frames.
    float g_sky_hits_last[16][3];
    int g_sky_hits_last_count = 0;
    float g_sky_hits_this[16][3];
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
        if (draw_index < kSkyLearnCalls)
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
                // 2026-10-06 (black slab again, shoulder camera walking):
                // (-130, -118, 3629) passed the distance test. While the
                // game camera follows Lara rigidly, anything attached to
                // her also keeps one offset from the camera. Real sky stays
                // pinned when the camera moves relative to Lara too (orbit,
                // lag, zoom), so learn only on a frame where the camera-to-
                // focus vector changed by more than 20 units.
                const float offset_length = sqrtf(offset[0] * offset[0] +
                                                  offset[1] * offset[1] +
                                                  offset[2] * offset[2]);
                float orbit = 0.0f;
                for (int k = 0; k < 3; ++k)
                    orbit += fabsf((retail[k] - focus[k]) -
                                   (c.retail[k] - c.focus[k]));
                const bool follows_lara = orbit < 20.0f;
                if (moved > 10.0f && same(c.offset, offset, 2.0f) &&
                    (offset_length < kSkyMinDistance || moved > 400.0f ||
                     follows_lara))
                {
                    static unsigned rejected = 0;
                    if (rejected++ < 6 && !follows_lara)
                        log("sky: camera-following draw at offset (%.0f, "
                            "%.0f, %.0f) not taken as sky (%s)", offset[0],
                            offset[1], offset[2], offset_length < kSkyMinDistance
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
            if (g_sky_this_count < kSkyLearnCalls)
            {
                SkyCandidate& c = g_sky_this[g_sky_this_count++];
                memcpy(c.offset, offset, sizeof(offset));
                memcpy(c.retail, retail, sizeof(retail));
                memcpy(c.focus, focus, sizeof(focus));
            }
        }
        bool pinned = false;
        for (int j = 0; j < g_sky_offset_count && !pinned; ++j)
            pinned = same(g_sky_offsets[j], offset, 2.0f);
        // Last frame's sky, drifted by a few units (first draws only).
        for (int j = 0; j < g_sky_hits_last_count && !pinned &&
                        draw_index < kSkyLearnCalls + 8; ++j)
            pinned = same(g_sky_hits_last[j], offset, 24.0f);
        if (!pinned)
            return false;
        if (g_sky_hits_this_count < 16)
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

        // The PDA's backdrop (the device screen and map behind its menu)
        // is not interface geometry, so it went through the head-tracked
        // eye projection: stuck to the view and the wrong size (user
        // screenshot 2026-10-06). With the PDA open nothing else is drawn,
        // so map its flat picture exactly as the world-locked menu panels
        // are mapped -- same anchor (level, recentred forward), same UI
        // scale -- so it stays pinned behind the menu items.
        if (config().ui_passthrough && ui_pda_active() &&
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
            // The menu's own close clip plane, so the rotated plane is not
            // cut into wedges (see the world-locked menu above).
            const Mat4 panel = projection_from_tangents(
                target.tan_left, target.tan_right,
                target.tan_top, target.tan_bottom,
                near_z > 1.0f ? 1.0f : near_z, far_z, flip);
            const Mat4 source = gp.valid ? projection_from_game(gp) : m;
            registers_from_matrix(reproject_ndc_to_world_locked(
                source, reference, panel, vr_head_rotation(),
                tune_ui_scale()), out);
            static bool reported = false;
            if (!reported)
            {
                reported = true;
                log("stereo: PDA backdrop pinned to the world-locked menu "
                    "(scale %.2f, %s matrix)", tune_ui_scale(),
                    gp.valid ? "projection" : "full");
            }
            return true;
        }

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
                // Theatre: the eye separation carries the shot's scale, so
                // the hands are scaled about the head by it too, or they
                // read that much nearer or farther than the real hands
                // (user 2026-10-10).
                const float grow = camera_world_scale_factor() *
                    (vr_theatre_active() ? camera_theatre_scale() : 1.0f);
                Mat4 scale = Mat4::identity();
                if (fabsf(grow - 1.0f) > 0.001f)
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

    // Cutscene theatre (user 2026-10-09, after Vertigo 2): the cutscene
    // camera stays where the game puts it -- unlevelled, the flat game's
    // own framing -- and the head moves the eye from it as through a
    // window: anchored where the head was when the cutscene began (or at a
    // recentre), at 1:1, so leaning looks round into the scene. Outside
    // the window everything is black (vr_submit draws the surround before
    // the HUD, so subtitles stay on top). Bink movies keep their own flat
    // screen.
    namespace
    {
        bool g_theatre_active = false;
        float g_theatre_yaw = 0.0f;               // raw head yaw, anchor
        float g_theatre_origin[3] = { 0.0f, 0.0f, 0.0f };   // tracking, m
        float g_theatre_forward[2] = { 0.0f, -1.0f };       // tracking x, z
        ULONGLONG g_theatre_since = 0;
        unsigned g_theatre_recentre_seen = 0;
        bool g_theatre_gameplay = false;    // gameplay (not a cutscene)
        // The window's offset from where it first appears (anchor frame,
        // metres: right, up, forward); the eye anchor moves with it, so the
        // world behind the glass goes along. Saved by the handle.
        // (A turn with the handle was tried 2026-10-10 and taken out: it
        // swung the view with it. The window moves, always facing the
        // anchor's forward.)
        float g_theatre_offset[3]{};
        bool g_theatre_offset_loaded = false;
        int g_theatre_drag = -1;
        float g_theatre_drag_hand[3]{};     // tracking metres at the grab
        float g_theatre_drag_offset[3]{};
        bool g_theatre_handle_hover[2] = { false, false };
        // The corner size handle (a backward L at the theatre window's
        // bottom-right, a mirrored one at the diorama opening's top-right;
        // user 2026-10-12): width and height separately, any aspect.
        int g_resize_drag = -1;
        bool g_resize_board = false;
        float g_resize_hand[3]{};
        float g_resize_start[3]{};          // half width, height, below
        int g_resize_corner = 0;            // 0 lower, 1 upper
        bool g_resize_hover[2] = { false, false };
        // Nearest hand to each handle (metres), for the fade: the handles
        // show only as a hand comes near (user 2026-10-12).
        float g_move_handle_near = 9.0f;
        float g_resize_handle_near = 9.0f;

        float theatre_hw() { return 0.5f * config().cutscene_screen_width; }
        float theatre_hh()
        {
            return config().cutscene_screen_height > 0.0f
                ? 0.5f * config().cutscene_screen_height
                : theatre_hw() * 9.0f / 16.0f;
        }

        // Handle colour faded toward the dark room by the nearest hand:
        // unseen beyond 0.45 m, full within 0.2 m, lit while held.
        unsigned handle_colour(float near_m, bool hot)
        {
            const unsigned lit = hot ? 0xFFE25Au : 0xC8C8C8u;
            const unsigned dark = config().theatre_backdrop & 0xFFFFFFu;
            float a = (0.45f - near_m) / 0.25f;
            a = hot ? 1.0f : a < 0.0f ? 0.0f : a > 1.0f ? 1.0f : a;
            unsigned out = 0;
            for (int shift = 0; shift <= 16; shift += 8)
            {
                const float l = (float)((lit >> shift) & 0xFF);
                const float d = (float)((dark >> shift) & 0xFF);
                out |= ((unsigned)(d + (l - d) * a + 0.5f) & 0xFF) << shift;
            }
            return out;
        }
        // A portal, not a room (user 2026-10-10: leaning through the window
        // and looking back showed the black box all round). Once the head
        // goes through the window rectangle it is in the world, and the
        // room is only the window seen from behind -- a floating black
        // rectangle. Crossing the plane outside the rectangle changes
        // nothing; coming back through the rectangle returns to the room.
        bool g_theatre_in_world = false;
        bool g_theatre_side_valid = false;
        float g_theatre_last_forward = 0.0f;

        void theatre_offset_load()
        {
            if (!g_theatre_offset_loaded)
            {
                g_theatre_offset_loaded = true;
                tune_theatre_offset(g_theatre_offset);
            }
        }

        // The window's flat forward in tracking space (the anchor's).
        void theatre_window_forward(float out[2])
        {
            out[0] = g_theatre_forward[0];
            out[1] = g_theatre_forward[1];
        }

        // The eye anchor in tracking space: the anchor moved by the saved
        // offset (in the anchor's own frame).
        void theatre_eye_anchor(float out[3])
        {
            theatre_offset_load();
            const float fx = g_theatre_forward[0], fz = g_theatre_forward[1];
            const float r = g_theatre_offset[0], u = g_theatre_offset[1],
                        f = g_theatre_offset[2];
            out[0] = g_theatre_origin[0] + r * -fz + f * fx;
            out[1] = g_theatre_origin[1] + u;
            out[2] = g_theatre_origin[2] + r * fx + f * fz;
        }

        // Window frame (right, up, forward metres from the eye anchor, the
        // window's turn applied) -> tracking space.
        void theatre_point(float r, float u, float f, float out[3])
        {
            float eye[3], w[2];
            theatre_eye_anchor(eye);
            theatre_window_forward(w);
            out[0] = eye[0] + r * -w[1] + f * w[0];
            out[1] = eye[1] + u;
            out[2] = eye[2] + r * w[0] + f * w[1];
        }

        // SteamVR's raw yaw (g_head_yaw_raw's convention) of a level head
        // facing a flat tracking direction.
        float raw_yaw_of_forward(float fx, float fz)
        {
            const float pose[12] = { -fz, 0.0f, -fx, 0.0f,
                                     0.0f, 1.0f, 0.0f, 0.0f,
                                     fx, 0.0f, -fz, 0.0f };
            return yaw_of(view_from_pose(pose, 1.0f));
        }

        void theatre_anchor()
        {
            const float* m = &g_tracked_poses[kPoseHmd]
                                  .mDeviceToAbsoluteTracking.m[0][0];
            g_theatre_origin[0] = m[3];
            g_theatre_origin[1] = m[7];
            g_theatre_origin[2] = m[11];
            float fx = -m[2], fz = -m[10];
            const float length = sqrtf(fx * fx + fz * fz);
            if (length > 1.0e-4f)
            {
                g_theatre_forward[0] = fx / length;
                g_theatre_forward[1] = fz / length;
            }
            g_theatre_yaw = g_head_yaw_raw;
            g_theatre_recentre_seen = g_recenter_generation;
        }

        void theatre_track_side();

        void theatre_update()
        {
            // Cutscenes, and with gameplay_theatre third-person play (not
            // board mode, not first person, not the main menu or loads; a
            // pause keeps it).
            const bool cinematic = camera_cinematic_playing();
            const bool cutscene = config().cutscene_theatre && cinematic;
            const bool gameplay = config().gameplay_theatre && !cinematic &&
                !camera_view_first_person() &&
                camera_third_person_mode() != 2 &&
                !(ui_menu_active() && !ui_pause_menu_active()) &&
                !ui_loading_screen_active();
            const bool want = (cutscene || gameplay) &&
                config().hmd_drives_camera && g_pose_valid &&
                !movie_playing();
            if (want && (!g_theatre_active || gameplay != g_theatre_gameplay))
                log("theatre: %s shown through a %.1f m window %.1f m ahead",
                    gameplay ? "third-person play" : "cutscene",
                    config().cutscene_screen_width,
                    config().cutscene_screen_distance);
            g_theatre_gameplay = gameplay;
            if (want && !g_theatre_active)
            {
                theatre_anchor();
                g_theatre_since = GetTickCount64();
                g_theatre_in_world = false;
                g_theatre_side_valid = false;
            }
            else if (want && g_recenter_generation != g_theatre_recentre_seen)
            {
                theatre_anchor();
                g_theatre_in_world = false;
                g_theatre_side_valid = false;
                log("theatre: window moved in front of you (recentre)");
            }
            else if (!want && g_theatre_active)
                log("theatre: window closed, back to the normal view");
            g_theatre_active = want;
            if (want)
                theatre_track_side();
            static ULONGLONG report_at = 0;
            const ULONGLONG now = GetTickCount64();
            if (!want)
            {
                report_at = 0;
                return;
            }
            if (!report_at)
            {
                report_at = now + 5000;
                g_theatre_builds = g_theatre_frames = 0;
                g_theatre_max_turn = g_theatre_max_move = 0.0f;
            }
            ++g_theatre_frames;
            if (now >= report_at)
            {
                const float s = tune_world_scale() * tune_move_scale();
                log("theatre: %u camera builds in %u frames; largest "
                    "catch-up %.2f deg, %.1f mm", g_theatre_builds,
                    g_theatre_frames, g_theatre_max_turn * 57.29578f,
                    s > 0.0f ? g_theatre_max_move / s * 1000.0f : 0.0f);
                report_at = now + 5000;
                g_theatre_builds = g_theatre_frames = 0;
                g_theatre_max_turn = g_theatre_max_move = 0.0f;
            }
        }

        // The headset's place in the window's frame (metres from the eye
        // anchor: right, up, forward).
        bool theatre_head_in_frame(float out[3])
        {
            const float* m = &g_tracked_poses[kPoseHmd]
                                  .mDeviceToAbsoluteTracking.m[0][0];
            float eye_anchor[3];
            theatre_point(0.0f, 0.0f, 0.0f, eye_anchor);
            const float d[3] = { m[3] - eye_anchor[0], m[7] - eye_anchor[1],
                                 m[11] - eye_anchor[2] };
            float w[2];
            theatre_window_forward(w);
            const float fx = w[0], fz = w[1];
            out[0] = d[0] * -fz + d[2] * fx;
            out[1] = d[1];
            out[2] = d[0] * fx + d[2] * fz;
            return std::isfinite(out[0]) && std::isfinite(out[1]) &&
                   std::isfinite(out[2]);
        }

        void theatre_track_side()
        {
            float h[3];
            if (!theatre_head_in_frame(h))
                return;
            const float plane = config().cutscene_screen_distance;
            const float hw = theatre_hw();
            const float hh = theatre_hh();
            if (g_theatre_side_valid)
            {
                const bool was_in = g_theatre_last_forward > plane;
                const bool now_in = h[2] > plane;
                const bool through = fabsf(h[0]) <= hw && fabsf(h[1]) <= hh;
                if (was_in != now_in && through)
                {
                    g_theatre_in_world = now_in;
                    log("theatre: head %s through the window",
                        now_in ? "went" : "came back");
                }
            }
            g_theatre_last_forward = h[2];
            g_theatre_side_valid = true;
        }

        Mat4 theatre_head_camera_view()
        {
            Mat4 r = g_head_view;
            r.m[3][0] = r.m[3][1] = r.m[3][2] = 0.0f;
            float w[2];
            theatre_window_forward(w);
            const Mat4 head = set_yaw(r, wrap_pi(g_head_yaw_raw -
                raw_yaw_of_forward(w[0], w[1])));
            const float* m = &g_tracked_poses[kPoseHmd]
                                  .mDeviceToAbsoluteTracking.m[0][0];
            float eye_anchor[3];
            theatre_point(0.0f, 0.0f, 0.0f, eye_anchor);
            const float d[3] = { m[3] - eye_anchor[0],
                                 m[7] - eye_anchor[1],
                                 m[11] - eye_anchor[2] };
            const float fx = w[0], fz = w[1];
            const float right = d[0] * -fz + d[2] * fx;
            const float up = d[1];
            const float forward = d[0] * fx + d[2] * fz;
            const float s = tune_world_scale() * tune_move_scale() *
                            camera_theatre_scale();
            // Engine view axes: x right, y down, z forward.
            return translation(-right * s, up * s, -forward * s) * head;
        }
    }

    bool vr_theatre_active() { return g_theatre_active; }

    // Board-mode diorama (user 2026-10-10). Board mode already shows the
    // world as a model fixed in the room (the head is the camera at table
    // scale), so nothing about the camera changes: only what is dark. The
    // box sits round Lara's spot -- board_distance ahead along the recentred
    // forward and board_height below the head at the recentre, as the board
    // seat is placed (camera_head) -- open at the front (toward you) and
    // the top. Its other faces are one-way: dark from outside, the world
    // from inside.
    namespace
    {
        bool g_diorama_active = false;
        float g_board_offset[3]{};
        bool g_board_offset_loaded = false;
        bool g_handle_board = false;     // the drag is the diorama's

        const float* board_offset()
        {
            if (!g_board_offset_loaded)
            {
                g_board_offset_loaded = true;
                tune_board_offset(g_board_offset);
            }
            return g_board_offset;
        }

        void diorama_update()
        {
            const bool want = config().board_diorama &&
                config().hmd_drives_camera && g_pose_valid &&
                !camera_cinematic_playing() && !camera_view_first_person() &&
                camera_third_person_mode() == 2 &&
                !(ui_menu_active() && !ui_pause_menu_active()) &&
                !ui_loading_screen_active() && !movie_playing();
            if (want != g_diorama_active)
                log("diorama: %s", want
                    ? "board mode seen through the box (open front and top)"
                    : "off");
            g_diorama_active = want;
        }

        // Box frame -> tracking: right, up (from Lara's floor), forward
        // (from the box centre), metres.
        void diorama_point(float r, float u, float f, float out[3])
        {
            const float* o = board_offset();
            const float fx = g_recentre_forward[0], fz = g_recentre_forward[1];
            r += o[0];
            u += o[1];
            const float ahead = config().board_distance + f + o[2];
            out[0] = g_pos_offset[0] + r * -fz + ahead * fx;
            out[1] = g_pos_offset[1] - config().board_height + u;
            out[2] = g_pos_offset[2] + r * fx + ahead * fz;
        }

        // The handle: a bar just under the box's front opening, in the dark
        // like the theatre window's (on the opening itself the game's later
        // draws covered it -- user 2026-10-10).
        void diorama_handle_point(float r, float u, float out[3])
        {
            diorama_point(r, -config().board_diorama_below - 0.05f + u,
                          -0.5f * config().board_diorama_depth - 0.01f, out);
        }
    }

    bool vr_diorama_active() { return g_diorama_active; }

    void vr_board_offset(float out[3])
    {
        const float* o = board_offset();
        for (int k = 0; k < 3; ++k)
            out[k] = o[k];
    }

    bool vr_dark_room_active()
    {
        return g_theatre_active || g_diorama_active;
    }

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
        if (g_theatre_active)
            return theatre_head_camera_view();
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
            {
                const float raw[3] = { pos[0], pos[1], pos[2] };
                head_raw_to_recentred(raw, pos);
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
        ++g_theatre_builds;
        ++g_head_state_generation;

        // A rare camera rebuild while paused already incorporates the current
        // pose, so make it the new residual origin immediately.
        if (ui_menu_active())
        {
            g_pause_head_anchor = g_main_camera_head;
            g_pause_head_anchor_valid = true;
        }
    }

    bool vr_head_position_offset(float out[3])
    {
        if (!out || !config().hmd_drives_position ||
            !g_head_position_origin_active || !g_pose_valid)
            return false;
        const float scale = tune_move_scale();
        float raw[3];
        for (int k = 0; k < 3; ++k)
            raw[k] = (g_head_pos[k] - g_head_position_origin[k]) * scale;
        head_raw_to_recentred(raw, out);
        return true;
    }

    void vr_shift_head_position_origin(const float d[3])
    {
        const float scale = tune_move_scale();
        if (!d || !g_head_position_origin_active || !(scale > 0.0f))
            return;
        float raw[3];
        head_recentred_to_raw(d, raw);
        for (int k = 0; k < 3; ++k)
            if (std::isfinite(raw[k]))
                g_head_position_origin[k] += raw[k] / scale;
    }

    void vr_head_position(float* xyz)
    {
        xyz[0] = g_head_pos[0];
        xyz[1] = g_head_pos[1];
        xyz[2] = g_head_pos[2];
    }

    // The raw controller point; vr_controller_body_position below prefers
    // Lara's palm.
    bool vr_controller_raw_body_position(bool left, float* right,
                                         float* up, float* forward)
    {
        if (!g_ready || !g_pose_valid ||
            !right || !up || !forward)
            return false;

        const unsigned index = left ? kPoseLeft : kPoseRight;
        const TrackedPose& controller = g_tracked_poses[index];
        const TrackedPose& hmd =
            g_tracked_poses[kPoseHmd];
        if (!controller.bPoseIsValid || !controller.bDeviceIsConnected ||
            !hmd.bPoseIsValid || !hmd.bDeviceIsConnected)
            return false;

        const Mat34& hm = hmd.mDeviceToAbsoluteTracking;
        const Mat34& cm = controller.mDeviceToAbsoluteTracking;
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

    bool vr_controller_tracking_pose(bool left, float pos[3], float rot[9])
    {
        if (!g_ready || !g_pose_valid || !pos || !rot)
            return false;
        const unsigned index = left ? kPoseLeft : kPoseRight;
        const TrackedPose& controller = g_tracked_poses[index];
        if (!controller.bPoseIsValid || !controller.bDeviceIsConnected)
            return false;
        const Mat34& cm = controller.mDeviceToAbsoluteTracking;
        for (int r = 0; r < 3; ++r)
        {
            pos[r] = cm.m[r][3];
            for (int c = 0; c < 3; ++c)
                rot[r * 3 + c] = cm.m[r][c];
        }
        return true;
    }

    bool vr_hmd_tracking_forward(float* fx, float* fz)
    {
        if (!g_ready || !g_pose_valid || !fx || !fz)
            return false;
        const TrackedPose& hmd =
            g_tracked_poses[kPoseHmd];
        if (!hmd.bPoseIsValid)
            return false;
        float x = -hmd.mDeviceToAbsoluteTracking.m[0][2];
        float z = -hmd.mDeviceToAbsoluteTracking.m[2][2];
        const float length = sqrtf(x * x + z * z);
        if (length < 1.0e-4f)
            return false;
        *fx = x / length;
        *fz = z / length;
        return true;
    }

    bool vr_controller_body_forward_speed(bool left, float* speed)
    {
        if (!speed || !g_ready || !g_pose_valid)
            return false;
        const unsigned index = left ? kPoseLeft : kPoseRight;
        const TrackedPose& controller = g_tracked_poses[index];
        const TrackedPose& hmd =
            g_tracked_poses[kPoseHmd];
        if (!controller.bPoseIsValid || !controller.bDeviceIsConnected ||
            !hmd.bPoseIsValid || !hmd.bDeviceIsConnected)
            return false;
        const Mat34& hm = hmd.mDeviceToAbsoluteTracking;
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

    // Every body-zone test -- holsters, belt items, the chest light, the
    // grapple, holster setup -- uses Lara's palm where the hand model gives
    // one (user 2026-10-10, Quest: the raw controller point sat far from
    // the drawn hand, so holster setup placed items well off it), and the
    // raw controller point otherwise.
    bool vr_controller_body_position(bool left, float* right, float* up,
                                     float* forward)
    {
        return vr_hand_palm_body_position(left, right, up, forward) ||
               vr_controller_raw_body_position(left, right, up, forward);
    }

    bool vr_controller_body_axis_endpoint(bool left, int axis, float metres,
                                          float* right, float* up,
                                          float* forward)
    {
        if (!right || !up || !forward || axis < 0 || axis > 2 ||
            !std::isfinite(metres) || !vr_controller_body_position(
                left, right, up, forward))
            return false;
        const unsigned index = left ? kPoseLeft : kPoseRight;
        const Mat34& cm =
            g_tracked_poses[index].mDeviceToAbsoluteTracking;
        const Mat34& hm =
            g_tracked_poses[kPoseHmd]
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
        if (!u || !v || eye == EyeCenter || !g_ready ||
            !g_pose_valid)
            return false;
        const TrackedPose& hmd =
            g_tracked_poses[kPoseHmd];
        if (!hmd.bPoseIsValid || !hmd.bDeviceIsConnected)
            return false;
        const Mat34& hm = hmd.mDeviceToAbsoluteTracking;
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
        const Mat34 eye_to_head = eye_to_head_matrix(eye);
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
        // Texture v runs down from the up edge, whose tangent OpenVR calls
        // "bottom" (its top/bottom are backwards; see projection_from_tangents).
        *v = (projection.tan_bottom - point[1] / depth) / dy;
        return std::isfinite(*u) && std::isfinite(*v);
    }

    bool vr_project_head_point(Eye eye, float x, float y, float z,
                               float* u, float* v)
    {
        if (!u || !v || eye == EyeCenter || !g_ready ||
            !g_pose_valid)
            return false;
        // Controller-to-head translations use world_scale * move_scale.
        // view_from_pose flips engine Y and Z against OpenVR, so reverse
        // that flip before applying the runtime's per-eye transform.
        const float scale = tune_world_scale() * tune_move_scale();
        if (!(scale > 0.0f) || !std::isfinite(scale))
            return false;
        const float head[3] = { x / scale, -y / scale, -z / scale };
        const Mat34 eye_to_head = eye_to_head_matrix(eye);
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
        // Texture v runs down from the up edge, whose tangent OpenVR calls
        // "bottom" (its top/bottom are backwards; see projection_from_tangents).
        *v = (projection.tan_bottom - point[1] / depth) / dy;
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

    namespace
    {
        // The SteamVR grip pose in the head frame, before any calibration.
        bool uncorrected_grip_head_pose(int hand, Mat4* grip_to_head)
        {
            if (!g_grip_offset_valid[hand])
                return false;
            Mat4 raw;
            if (!vr_controller_head_pose(hand == 0, &raw))
                return false;
            Mat4 offset = g_grip_offset[hand];
            const float scale = tune_world_scale();
            for (int axis = 0; axis < 3; ++axis)
                offset.m[3][axis] *= scale;
            *grip_to_head = offset * raw;
            return true;
        }

        bool g_cal_menu_request = false;
        bool g_cal_active = false;
        bool g_cal_locked[2] = { false, false };
        // The pose each hand is held at, fixed in tracking space when
        // calibration starts so it does not follow the head.
        Mat34 g_cal_reference[2]{};
        // Locked this pass, not yet saved: rotation then metres.
        float g_cal_pending[2][12]{};
        int g_cal_step = 0;            // 0 hands, 1 pistol aim
        bool g_cal_with_aim = false;
        // The aim target, fixed in tracking space (metres) at step 1.
        float g_cal_target[3]{};
        // Metres ahead of the head the target is placed: far enough that
        // the palm-to-barrel offset is a small angle (8 cm at 4 m: ~1 deg).
        const float kCalTargetDistance = 4.0f;

        // Where the reference hands sit, metres from the head in its
        // yaw-only frame: arms out in front, guns level.
        const float kCalSide = 0.18f, kCalUp = -0.28f, kCalForward = 0.42f;
        // Degrees the shown hands' barrels point below level.
        const float kCalPitchDown = 10.0f;

        // A tracking-space pose in the head frame, composed exactly as
        // vr_controller_head_pose composes a controller.
        bool tracking_pose_to_head(const Mat34& pose, Mat4* out)
        {
            if (!g_ready || !g_pose_valid)
                return false;
            const TrackedPose& hmd =
                g_tracked_poses[kPoseHmd];
            if (!hmd.bPoseIsValid || !hmd.bDeviceIsConnected)
                return false;
            const float scale = tune_world_scale();
            *out = rigid_inverse(view_from_pose(&pose.m[0][0], scale)) *
                   view_from_pose(&hmd.mDeviceToAbsoluteTracking.m[0][0],
                                  scale);
            const float move_scale = tune_move_scale();
            for (int axis = 0; axis < 3; ++axis)
                out->m[3][axis] *= move_scale;
            return true;
        }

        // 12 stored values (rotation, metres) as a grip-frame matrix.
        Mat4 correction_matrix(const float c[12])
        {
            Mat4 k = Mat4::identity();
            for (int r = 0; r < 3; ++r)
                for (int col = 0; col < 3; ++col)
                    k.m[r][col] = c[r * 3 + col];
            const float scale = tune_world_scale();
            for (int axis = 0; axis < 3; ++axis)
                k.m[3][axis] = c[9 + axis] * scale;
            return k;
        }
    }

    bool vr_controller_grip_head_pose(bool left, Mat4* grip_to_head)
    {
        const int hand = left ? 0 : 1;
        if (!grip_to_head || !g_grip_offset_valid[hand])
            return false;
        // Calibrating: an unlocked hand is held at the reference.
        if (g_cal_active && g_cal_step == 0 && !g_cal_locked[hand])
            return tracking_pose_to_head(g_cal_reference[hand], grip_to_head);
        Mat4 grip;
        if (!uncorrected_grip_head_pose(hand, &grip))
            return false;
        float c[12]{};
        if (g_cal_active && g_cal_step == 0 && g_cal_locked[hand])
            memcpy(c, g_cal_pending[hand], sizeof(c));
        else if (!tune_hand_correction(left, c))
        {
            *grip_to_head = grip;
            return true;
        }
        *grip_to_head = correction_matrix(c) * grip;
        return true;
    }

    bool vr_head_forward_xz(float* x, float* z)
    {
        if (!x || !z || !g_ready || !g_pose_valid)
            return false;
        const Mat34& hm =
            g_tracked_poses[kPoseHmd]
                .mDeviceToAbsoluteTracking;
        const float fx = -hm.m[0][2], fz = -hm.m[2][2];
        const float length = sqrtf(fx * fx + fz * fz);
        if (length < 1.0e-4f)
            return false;
        *x = fx / length;
        *z = fz / length;
        return true;
    }

    bool vr_controller_grip_uncorrected_head_pose(bool left, Mat4* out)
    {
        return out && uncorrected_grip_head_pose(left ? 0 : 1, out);
    }

    bool vr_hand_palm_head_position(bool left, float out[3])
    {
        Mat4 grip;
        if (!out || config().hand_model != 2 ||
            !vr_controller_grip_head_pose(left, &grip))
            return false;
        // The hand model puts its palm at the tuned grip position in the
        // grip frame (first_person_grip_hand_transform); the left hand is
        // the right mirrored across grip x.
        float position[3]{};
        tune_grip_hand(position, nullptr);
        const float scale = tune_world_scale();
        const float palm[3] = { (left ? -position[0] : position[0]) * scale,
                                position[1] * scale, position[2] * scale };
        for (int axis = 0; axis < 3; ++axis)
        {
            out[axis] = palm[0] * grip.m[0][axis] +
                        palm[1] * grip.m[1][axis] +
                        palm[2] * grip.m[2][axis] + grip.m[3][axis];
            if (!std::isfinite(out[axis]))
                return false;
        }
        return true;
    }

    bool vr_hand_fingertip_head_position(bool left, float out[3])
    {
        Mat4 grip;
        float palm[3];
        if (!out || !vr_hand_palm_head_position(left, palm) ||
            !vr_controller_grip_head_pose(left, &grip))
            return false;
        float f[3] = { grip.m[2][0], grip.m[2][1], grip.m[2][2] };
        const float len = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
        if (!(len > 1.0e-4f))
            return false;
        const float reach = 0.09f * tune_world_scale() / len;
        for (int k = 0; k < 3; ++k)
        {
            out[k] = palm[k] + f[k] * reach;
            if (!std::isfinite(out[k]))
                return false;
        }
        return true;
    }

    bool vr_hand_fingertip_body_position(bool left, float* right, float* up,
                                         float* forward)
    {
        float head[3];
        return vr_hand_fingertip_head_position(left, head) &&
               vr_head_point_body_position(head, right, up, forward);
    }

    bool vr_head_point_body_position(const float head[3], float* right,
                                     float* up, float* forward)
    {
        if (!head || !right || !up || !forward || !g_ready ||
            !g_pose_valid)
            return false;
        const TrackedPose& hmd =
            g_tracked_poses[kPoseHmd];
        if (!hmd.bPoseIsValid || !hmd.bDeviceIsConnected)
            return false;
        const float scale = tune_world_scale() * tune_move_scale();
        if (!(scale > 0.0f) || !std::isfinite(scale))
            return false;
        // The head frame flips OpenVR's Y and Z (as vr_project_head_point
        // undoes); back to metres in the headset's own axes, then into
        // tracking space.
        const float local[3] = { head[0] / scale, -head[1] / scale,
                                 -head[2] / scale };
        const Mat34& hm = hmd.mDeviceToAbsoluteTracking;
        float d[3];
        for (int row = 0; row < 3; ++row)
            d[row] = hm.m[row][0] * local[0] + hm.m[row][1] * local[1] +
                     hm.m[row][2] * local[2];
        float fx = -hm.m[0][2], fz = -hm.m[2][2];
        const float length = sqrtf(fx * fx + fz * fz);
        if (length < 1.0e-4f)
            return false;
        fx /= length;
        fz /= length;
        *right = d[0] * -fz + d[2] * fx;
        *up = d[1];
        *forward = d[0] * fx + d[2] * fz;
        return true;
    }

    bool vr_hand_palm_body_position(bool left, float* right, float* up,
                                    float* forward)
    {
        float palm[3];
        return vr_hand_palm_head_position(left, palm) &&
               vr_head_point_body_position(palm, right, up, forward);
    }

    unsigned vr_theatre_surround(float (*out)[3], unsigned* colours,
                                 unsigned max, float* alpha)
    {
        if (!out || !colours || !g_theatre_active || !g_pose_valid ||
            max < 60)
            return 0;
        // In the anchor's frame (metres): right, up, forward. The window
        // is 16:9, centred at eye height; the box is 30 m each way and
        // its front wall is the window's plane.
        const float hw = theatre_hw();
        const float hh = theatre_hh();
        const float d = config().cutscene_screen_distance;
        const float L = 30.0f;
        const DWORD backdrop = config().theatre_backdrop & 0xFFFFFFu;
        DWORD colour = backdrop;
        unsigned used = 0;
        const auto corner = [&](float r, float u, float f)
        {
            float at[3];
            theatre_point(r, u, f, at);
            Mat34 pose{};
            pose.m[0][0] = pose.m[1][1] = pose.m[2][2] = 1.0f;
            pose.m[0][3] = at[0];
            pose.m[1][3] = at[1];
            pose.m[2][3] = at[2];
            Mat4 head;
            if (!tracking_pose_to_head(pose, &head))
                return false;
            for (int k = 0; k < 3; ++k)
                out[used][k] = head.m[3][k];
            colours[used] = colour;
            ++used;
            return true;
        };
        // A quad from four (r, u, f) corners, as two triangles.
        const auto quad = [&](const float a[3], const float b[3],
                              const float c[3], const float e[3])
        {
            const float* order[6] = { a, b, c, a, c, e };
            for (const float* p : order)
                if (!corner(p[0], p[1], p[2]))
                    return false;
            return true;
        };
        const float q[9][4][3] = {
            // Front wall round the window.
            { { -L, hh, d }, { L, hh, d }, { L, L, d }, { -L, L, d } },
            { { -L, -L, d }, { L, -L, d }, { L, -hh, d }, { -L, -hh, d } },
            { { -L, -hh, d }, { -hw, -hh, d }, { -hw, hh, d }, { -L, hh, d } },
            { { hw, -hh, d }, { L, -hh, d }, { L, hh, d }, { hw, hh, d } },
            // Back, left, right, top, bottom.
            { { -L, -L, -L }, { L, -L, -L }, { L, L, -L }, { -L, L, -L } },
            { { -L, -L, -L }, { -L, -L, d }, { -L, L, d }, { -L, L, -L } },
            { { L, -L, -L }, { L, -L, d }, { L, L, d }, { L, L, -L } },
            { { -L, L, -L }, { L, L, -L }, { L, L, d }, { -L, L, d } },
            { { -L, -L, -L }, { L, -L, -L }, { L, -L, d }, { -L, -L, d } } };
        if (g_theatre_in_world)
        {
            const float w[4][3] = { { -hw, -hh, d }, { hw, -hh, d },
                                    { hw, hh, d }, { -hw, hh, d } };
            if (!quad(w[0], w[1], w[2], w[3]))
                return 0;
        }
        else
            for (const auto& face : q)
                if (!quad(face[0], face[1], face[2], face[3]))
                    return 0;
        // The move handle: a bar under the window, just in front of it;
        // yellow while a hand is on it, faded out with no hand near.
        {
            colour = handle_colour(g_move_handle_near,
                g_theatre_drag >= 0 || g_theatre_handle_hover[0] ||
                g_theatre_handle_hover[1]);
            const float y = -hh - 0.07f, f = d - 0.01f;
            const float h[4][3] = {
                { -0.09f, y - 0.018f, f }, { 0.09f, y - 0.018f, f },
                { 0.09f, y + 0.018f, f }, { -0.09f, y + 0.018f, f } };
            if (!quad(h[0], h[1], h[2], h[3]))
                return 0;
        }
        // The size handle: a backward L round the bottom-right corner.
        {
            colour = handle_colour(g_resize_handle_near,
                g_resize_drag >= 0 || g_resize_hover[0] ||
                g_resize_hover[1]);
            const float cx = hw + 0.03f, cy = -hh - 0.03f, f = d - 0.01f;
            const float t = 0.011f, len = 0.14f;
            const float a[4][3] = {
                { cx - len, cy - t, f }, { cx + t, cy - t, f },
                { cx + t, cy + t, f }, { cx - len, cy + t, f } };
            const float b[4][3] = {
                { cx - t, cy + t, f }, { cx + t, cy + t, f },
                { cx + t, cy + len, f }, { cx - t, cy + len, f } };
            if (!quad(a[0], a[1], a[2], a[3]) ||
                !quad(b[0], b[1], b[2], b[3]))
                return 0;
            // And round the top-right corner (user 2026-10-12).
            const float ty = hh + 0.03f;
            const float c[4][3] = {
                { cx - len, ty - t, f }, { cx + t, ty - t, f },
                { cx + t, ty + t, f }, { cx - len, ty + t, f } };
            const float e[4][3] = {
                { cx - t, ty - len, f }, { cx + t, ty - len, f },
                { cx + t, ty - t, f }, { cx - t, ty - t, f } };
            if (!quad(c[0], c[1], c[2], c[3]) ||
                !quad(e[0], e[1], e[2], e[3]))
                return 0;
        }
        if (alpha)
        {
            const float t = (GetTickCount64() - g_theatre_since) / 400.0f;
            *alpha = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
        }
        return used;
    }

    unsigned vr_diorama_openings(float (*out)[3], unsigned max,
                                 bool* eye_inside)
    {
        if (!out || !eye_inside || !g_diorama_active || !g_pose_valid ||
            max < 12)
            return 0;
        const float hw = 0.5f * config().board_diorama_width;
        const float hd = 0.5f * config().board_diorama_depth;
        const float bottom = -config().board_diorama_below;
        const float top = bottom + config().board_diorama_height;
        // The eye in the box frame.
        const float* m = &g_tracked_poses[kPoseHmd]
                              .mDeviceToAbsoluteTracking.m[0][0];
        float centre[3];
        diorama_point(0.0f, 0.0f, 0.0f, centre);
        const float d[3] = { m[3] - centre[0], m[7] - centre[1],
                             m[11] - centre[2] };
        const float fx = g_recentre_forward[0], fz = g_recentre_forward[1];
        const float er = d[0] * -fz + d[2] * fx;
        const float eu = d[1];
        const float ef = d[0] * fx + d[2] * fz;
        *eye_inside = fabsf(er) <= hw && ef >= -hd && ef <= hd &&
                      eu >= bottom && eu <= top;
        // Openings to mark: from outside, those whose outer side the eye is
        // on (the faces it looks in through); from inside, only the front
        // -- the way back out to the room. The top shows the world from
        // inside (user 2026-10-10).
        const bool front = *eye_inside || ef < -hd;
        const bool roof = !*eye_inside && eu > top;
        unsigned used = 0;
        const auto corner = [&](float r, float u, float f)
        {
            float at[3];
            diorama_point(r, u, f, at);
            Mat34 pose{};
            pose.m[0][0] = pose.m[1][1] = pose.m[2][2] = 1.0f;
            pose.m[0][3] = at[0];
            pose.m[1][3] = at[1];
            pose.m[2][3] = at[2];
            Mat4 head;
            if (!tracking_pose_to_head(pose, &head))
                return false;
            for (int k = 0; k < 3; ++k)
                out[used][k] = head.m[3][k];
            ++used;
            return true;
        };
        const auto quad = [&](const float a[3], const float b[3],
                              const float c[3], const float e[3])
        {
            const float* order[6] = { a, b, c, a, c, e };
            for (const float* p : order)
                if (!corner(p[0], p[1], p[2]))
                    return false;
            return true;
        };
        if (front)
        {
            const float q[4][3] = { { -hw, bottom, -hd }, { hw, bottom, -hd },
                                    { hw, top, -hd }, { -hw, top, -hd } };
            if (!quad(q[0], q[1], q[2], q[3]))
                return 0;
        }
        if (roof)
        {
            const float q[4][3] = { { -hw, top, -hd }, { hw, top, -hd },
                                    { hw, top, hd }, { -hw, top, hd } };
            if (!quad(q[0], q[1], q[2], q[3]))
                return 0;
        }
        return used;
    }

    unsigned vr_diorama_handle(float (*out)[3], unsigned max,
                               unsigned* colours)
    {
        if (!out || !colours || !g_diorama_active || max < 30)
            return 0;
        const unsigned bar = handle_colour(g_move_handle_near,
            g_theatre_drag >= 0 || g_theatre_handle_hover[0] ||
            g_theatre_handle_hover[1]);
        const unsigned size = handle_colour(g_resize_handle_near,
            g_resize_drag >= 0 || g_resize_hover[0] || g_resize_hover[1]);
        // Bar (handle frame: right, up), then the size handle as a
        // mirrored L round the opening's top-right corner (box frame).
        const float corners[6][2] = { { -0.12f, -0.02f }, { 0.12f, -0.02f },
                                      { 0.12f, 0.02f }, { -0.12f, -0.02f },
                                      { 0.12f, 0.02f }, { -0.12f, 0.02f } };
        const float hw = 0.5f * config().board_diorama_width;
        const float hd = 0.5f * config().board_diorama_depth;
        const float top = -config().board_diorama_below +
                          config().board_diorama_height;
        const float cx = hw + 0.03f, cy = top + 0.03f, t = 0.011f,
                    len = 0.14f;
        // Lower corner too (user 2026-10-12): a backward L.
        const float by = -config().board_diorama_below - 0.03f;
        const float l[24][2] = {
            { cx - len, cy - t }, { cx + t, cy - t }, { cx + t, cy + t },
            { cx - len, cy - t }, { cx + t, cy + t }, { cx - len, cy + t },
            { cx - t, cy - len }, { cx + t, cy - len }, { cx + t, cy + t },
            { cx - t, cy - len }, { cx + t, cy + t }, { cx - t, cy + t },
            { cx - len, by - t }, { cx + t, by - t }, { cx + t, by + t },
            { cx - len, by - t }, { cx + t, by + t }, { cx - len, by + t },
            { cx - t, by - t }, { cx + t, by - t }, { cx + t, by + len },
            { cx - t, by - t }, { cx + t, by + len }, { cx - t, by + len } };
        // Faded all the way: not drawn at all (user 2026-10-12: a faded
        // handle showed as a black shape over the world).
        const unsigned dark = config().theatre_backdrop & 0xFFFFFFu;
        const bool show_bar = bar != dark;
        const bool show_size = size != dark;
        if (!show_bar && !show_size)
            return 0;
        for (unsigned i = 0; i < 30; ++i)
        {
            float at[3];
            if (i < 6)
                diorama_handle_point(corners[i][0], corners[i][1], at);
            else
                diorama_point(l[i - 6][0], l[i - 6][1], -hd - 0.01f, at);
            colours[i] = i < 6 ? bar : size;
            Mat34 pose{};
            pose.m[0][0] = pose.m[1][1] = pose.m[2][2] = 1.0f;
            pose.m[0][3] = at[0];
            pose.m[1][3] = at[1];
            pose.m[2][3] = at[2];
            Mat4 head;
            if (!tracking_pose_to_head(pose, &head))
                return 0;
            for (int k = 0; k < 3; ++k)
                out[i][k] = head.m[3][k];
        }
        if (!show_bar)
        {
            // Only the size handles: move them to the front.
            for (unsigned i = 0; i < 24; ++i)
            {
                for (int k = 0; k < 3; ++k)
                    out[i][k] = out[i + 6][k];
                colours[i] = colours[i + 6];
            }
            return 24;
        }
        return show_size ? 30 : 6;
    }

    void vr_theatre_handle_update(bool left_grip, bool right_grip,
                                  bool claim[2], bool entered[2])
    {
        claim[0] = claim[1] = false;
        entered[0] = entered[1] = false;
        const bool grips[2] = { left_grip, right_grip };
        static bool grip_was[2] = { false, false };
        // The theatre window's handle, or in board mode the diorama's (it
        // moves the box and the board world with it).
        const bool board = g_diorama_active && !g_theatre_active;
        float* offset = board ? g_board_offset : g_theatre_offset;
        const float* frame = board ? g_recentre_forward : g_theatre_forward;
        if (board)
            board_offset();
        const auto end_drag = [&]()
        {
            if (g_theatre_drag >= 0)
            {
                if (g_handle_board)
                    tune_set_board_offset(g_board_offset);
                else
                    tune_set_theatre_offset(g_theatre_offset);
            }
            g_theatre_drag = -1;
        };
        if (g_theatre_drag >= 0 && g_handle_board != board)
            end_drag();
        if (!(g_theatre_active || g_diorama_active) || !g_pose_valid)
        {
            end_drag();
            if (g_resize_drag >= 0)
            {
                if (g_resize_board)
                    config_set_diorama_size(config().board_diorama_width,
                                            config().board_diorama_height,
                                            config().board_diorama_below,
                                            true);
                else
                    config_set_theatre_size(config().cutscene_screen_width,
                                            2.0f * theatre_hh(), true);
                g_resize_drag = -1;
            }
            g_resize_hover[0] = g_resize_hover[1] = false;
            g_theatre_handle_hover[0] = g_theatre_handle_hover[1] = false;
            grip_was[0] = grips[0];
            grip_was[1] = grips[1];
            return;
        }
        const float s = tune_world_scale() * tune_move_scale();
        const float hh = theatre_hh();
        // The size handles' corners (0 lower, 1 upper), tracking space.
        Mat4 resize_heads[2];
        bool have_resizes[2] = { false, false };
        for (int c = 0; c < 2; ++c)
        {
            float resize_tracking[3];
            if (board)
                diorama_point(0.5f * config().board_diorama_width + 0.03f,
                              c ? -config().board_diorama_below +
                                      config().board_diorama_height + 0.03f
                                : -config().board_diorama_below - 0.03f,
                              -0.5f * config().board_diorama_depth - 0.01f,
                              resize_tracking);
            else
                theatre_point(theatre_hw() + 0.03f,
                              c ? hh + 0.03f : -hh - 0.03f,
                              config().cutscene_screen_distance,
                              resize_tracking);
            Mat34 resize_pose{};
            resize_pose.m[0][0] = resize_pose.m[1][1] =
                resize_pose.m[2][2] = 1.0f;
            resize_pose.m[0][3] = resize_tracking[0];
            resize_pose.m[1][3] = resize_tracking[1];
            resize_pose.m[2][3] = resize_tracking[2];
            have_resizes[c] = s > 0.0f &&
                tracking_pose_to_head(resize_pose, &resize_heads[c]);
        }
        g_move_handle_near = 9.0f;
        g_resize_handle_near = 9.0f;
        const auto end_resize = [&]()
        {
            if (g_resize_drag >= 0)
            {
                if (g_resize_board)
                {
                    config_set_diorama_size(config().board_diorama_width,
                                            config().board_diorama_height,
                                            config().board_diorama_below,
                                            true);
                    log("diorama: size %.2f x %.2f m, %.2f m below (saved)",
                        config().board_diorama_width,
                        config().board_diorama_height,
                        config().board_diorama_below);
                }
                else
                {
                    config_set_theatre_size(config().cutscene_screen_width,
                        2.0f * theatre_hh(), true);
                    log("theatre: window %.2f x %.2f m (saved)",
                        config().cutscene_screen_width, 2.0f * theatre_hh());
                }
            }
            g_resize_drag = -1;
        };
        if (g_resize_drag >= 0 && g_resize_board != board)
            end_resize();
        float handle_tracking[3];
        if (board)
            diorama_handle_point(0.0f, 0.0f, handle_tracking);
        else
            theatre_point(0.0f, -hh - 0.07f,
                          config().cutscene_screen_distance, handle_tracking);
        Mat34 pose{};
        pose.m[0][0] = pose.m[1][1] = pose.m[2][2] = 1.0f;
        pose.m[0][3] = handle_tracking[0];
        pose.m[1][3] = handle_tracking[1];
        pose.m[2][3] = handle_tracking[2];
        Mat4 handle_head;
        const bool have_handle = s > 0.0f &&
            tracking_pose_to_head(pose, &handle_head);
        for (int hand = 0; hand < 2; ++hand)
        {
            const bool left = hand == 0;
            const bool pressed = grips[hand] && !grip_was[hand];
            grip_was[hand] = grips[hand];
            // The hand: Lara's palm if the hand model is on, else the
            // controller.
            float palm[3];
            bool have_palm = vr_hand_palm_head_position(left, palm);
            if (!have_palm)
            {
                Mat4 device;
                if (vr_controller_head_pose(left, &device))
                {
                    for (int k = 0; k < 3; ++k)
                        palm[k] = device.m[3][k];
                    have_palm = true;
                }
            }
            // The index fingertip reaches a handle as well as the palm.
            float tip[3];
            const bool have_tip = vr_hand_fingertip_head_position(left, tip);
            const auto tip_d2 = [&](const Mat4& target) {
                float d2 = 0.0f;
                for (int k = 0; k < 3; ++k)
                {
                    const float d = (tip[k] - target.m[3][k]) / s;
                    d2 += d * d;
                }
                return d2;
            };
            bool near_handle = false;
            if (have_handle && have_tip)
            {
                const float d2 = tip_d2(handle_head);
                near_handle = d2 < 0.10f * 0.10f;
                g_move_handle_near = fminf(g_move_handle_near, sqrtf(d2));
            }
            if (have_handle && have_palm)
            {
                float d2 = 0.0f;
                for (int k = 0; k < 3; ++k)
                {
                    const float d = (palm[k] - handle_head.m[3][k]) / s;
                    d2 += d * d;
                }
                near_handle = near_handle || d2 < 0.10f * 0.10f;
                g_move_handle_near = fminf(g_move_handle_near, sqrtf(d2));
            }
            bool near_resize = false;
            int near_corner = 0;
            float corner_best = 0.10f * 0.10f;
            for (int c = 0; c < 2; ++c)
            {
                if (!have_resizes[c])
                    continue;
                float best = 1.0e9f;
                if (have_tip)
                    best = tip_d2(resize_heads[c]);
                if (have_palm)
                {
                    float d2 = 0.0f;
                    for (int k = 0; k < 3; ++k)
                    {
                        const float d =
                            (palm[k] - resize_heads[c].m[3][k]) / s;
                        d2 += d * d;
                    }
                    best = fminf(best, d2);
                }
                g_resize_handle_near = fminf(g_resize_handle_near,
                                             sqrtf(best));
                if (best < corner_best)
                {
                    corner_best = best;
                    near_resize = true;
                    near_corner = c;
                }
            }
            if (near_resize && !g_resize_hover[hand])
                entered[hand] = true;
            g_resize_hover[hand] = near_resize;
            if (near_handle && !g_theatre_handle_hover[hand])
                entered[hand] = true;
            g_theatre_handle_hover[hand] = near_handle;
            // The controller in tracking space, for the drag.
            const unsigned index = left ? kPoseLeft : kPoseRight;
            if (!g_tracked_poses[index].bPoseIsValid)
            {
                if (g_theatre_drag == hand)
                    end_drag();
                continue;
            }
            const Mat34& cm =
                g_tracked_poses[index].mDeviceToAbsoluteTracking;
            const float at[3] = { cm.m[0][3], cm.m[1][3], cm.m[2][3] };
            // The size handle (before the move bar: the nearer one wins
            // only if both are in reach, which they are not).
            if (pressed && near_resize && g_resize_drag < 0 &&
                g_theatre_drag < 0)
            {
                g_resize_drag = hand;
                g_resize_board = board;
                g_resize_corner = near_corner;
                memcpy(g_resize_hand, at, sizeof(at));
                if (board)
                {
                    g_resize_start[0] = 0.5f * config().board_diorama_width;
                    g_resize_start[1] = config().board_diorama_height;
                    g_resize_start[2] = config().board_diorama_below;
                }
                else
                {
                    g_resize_start[0] = theatre_hw();
                    g_resize_start[1] = hh;
                }
                log("%s: %s hand took the size handle", board ? "diorama"
                    : "theatre", left ? "left" : "right");
            }
            if (g_resize_drag == hand)
            {
                if (!grips[hand])
                {
                    end_resize();
                    continue;
                }
                claim[hand] = true;
                const float dx = at[0] - g_resize_hand[0];
                const float dy = at[1] - g_resize_hand[1];
                const float dz = at[2] - g_resize_hand[2];
                const float fx = frame[0], fz = frame[1];
                const float dr = dx * -fz + dz * fx;
                const auto clampf = [](float v, float lo, float hi) {
                    return v < lo ? lo : v > hi ? hi : v;
                };
                if (board)
                {
                    // Width both ways from the middle; the grabbed corner's
                    // edge follows the hand (top: the floor stays; bottom:
                    // the top stays and the box reaches further down).
                    const float hw = clampf(g_resize_start[0] + dr,
                                            0.1f, 1.0f);
                    if (g_resize_corner == 1)
                    {
                        const float height = clampf(g_resize_start[1] + dy,
                                                    0.15f, 1.5f);
                        config_set_diorama_size(2.0f * hw, height,
                            config().board_diorama_below, false);
                    }
                    else
                    {
                        const float grow = clampf(-dy,
                            0.15f - g_resize_start[1],
                            1.5f - g_resize_start[1]);
                        config_set_diorama_size(2.0f * hw,
                            g_resize_start[1] + grow,
                            g_resize_start[2] + grow, false);
                    }
                }
                else
                {
                    // Centred: the grabbed corner follows the hand.
                    const float hw = clampf(g_resize_start[0] + dr,
                                            0.25f, 5.0f);
                    const float h2 = clampf(g_resize_start[1] +
                                            (g_resize_corner ? dy : -dy),
                                            0.15f, 4.0f);
                    config_set_theatre_size(2.0f * hw, 2.0f * h2, false);
                }
                continue;
            }
            if (pressed && near_handle && g_theatre_drag < 0)
            {
                g_theatre_drag = hand;
                g_handle_board = board;
                memcpy(g_theatre_drag_hand, at, sizeof(at));
                memcpy(g_theatre_drag_offset, offset,
                       sizeof(g_theatre_drag_offset));
                log("%s: %s hand took the handle", board ? "diorama"
                    : "theatre", left ? "left" : "right");
            }
            if (g_theatre_drag != hand)
                continue;
            if (!grips[hand])
            {
                end_drag();
                continue;
            }
            claim[hand] = true;
            const float dx = at[0] - g_theatre_drag_hand[0];
            const float dy = at[1] - g_theatre_drag_hand[1];
            const float dz = at[2] - g_theatre_drag_hand[2];
            const float fx = frame[0], fz = frame[1];
            offset[0] = g_theatre_drag_offset[0] + dx * -fz + dz * fx;
            offset[1] = g_theatre_drag_offset[1] + dy;
            offset[2] = g_theatre_drag_offset[2] + dx * fx + dz * fz;
        }
        // A hand on a handle keeps its grip from the game.
        for (int hand = 0; hand < 2; ++hand)
            if ((g_theatre_handle_hover[hand] || g_resize_hover[hand]) &&
                grips[hand])
                claim[hand] = true;
    }

    bool vr_head_clip_matrix(Eye eye, float out[4][4])
    {
        if (!out || eye == EyeCenter || !g_ready || !g_pose_valid)
            return false;
        // The same mapping as vr_project_head_point, as one matrix: head
        // game units -> OpenVR head metres -> eye metres -> clip.
        const float s = tune_world_scale() * tune_move_scale();
        if (!(s > 0.0f) || !std::isfinite(s))
            return false;
        Mat4 a = Mat4::identity();
        a.m[0][0] = 1.0f / s;
        a.m[1][1] = -1.0f / s;
        a.m[2][2] = -1.0f / s;
        const Mat34 eh = eye_to_head_matrix(eye);
        Mat4 b = Mat4::identity();
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                b.m[r][c] = eh.m[r][c];
        for (int c = 0; c < 3; ++c)
            b.m[3][c] = -(eh.m[0][3] * eh.m[0][c] + eh.m[1][3] * eh.m[1][c] +
                          eh.m[2][3] * eh.m[2][c]);
        const EyeInfo& in = vr_eye(eye);
        const float dx = in.tan_right - in.tan_left;
        const float dy = in.tan_bottom - in.tan_top;
        if (dx < 1.0e-4f || dy < 1.0e-4f)
            return false;
        const float n = 0.05f, f = 500.0f;
        Mat4 c{};
        c.m[0][0] = 2.0f / dx;
        c.m[1][1] = 2.0f / dy;
        c.m[2][0] = 2.0f * in.tan_left / dx + 1.0f;
        c.m[2][1] = -(1.0f - 2.0f * in.tan_bottom / dy);
        c.m[2][2] = -f / (f - n);
        c.m[2][3] = -1.0f;
        c.m[3][2] = -n * f / (f - n);
        const Mat4 m = a * b * c;
        memcpy(out, m.m, sizeof(m.m));
        return true;
    }

    void vr_hand_calibration_request_from_menu()
    {
        g_cal_menu_request = true;
        log("vr: hand calibration requested from the pause menu");
    }

    bool vr_hand_calibration_menu_pending() { return g_cal_menu_request; }

    void vr_hand_calibration_clear_menu_request()
    {
        g_cal_menu_request = false;
    }

    bool vr_hand_calibration_active() { return g_cal_active; }

    bool vr_hand_calibration_locked(bool left)
    {
        return g_cal_active && g_cal_locked[left ? 0 : 1];
    }

    int vr_hand_calibration_step()
    {
        return g_cal_active ? g_cal_step : -1;
    }

    namespace
    {
        void calibration_finish(const char* how)
        {
            g_cal_active = false;
            g_cal_step = 0;
            log("vr: hand calibration finished (%s)", how);
        }

        // Step 1: a target kCalTargetDistance ahead of the head (level,
        // 10 cm below the eyes), fixed in tracking space.
        void calibration_start_aim()
        {
            const Mat34& head =
                g_tracked_poses[kPoseHmd]
                    .mDeviceToAbsoluteTracking;
            float fx = -head.m[0][2], fz = -head.m[2][2];
            const float length = sqrtf(fx * fx + fz * fz);
            if (length < 1.0e-3f)
            {
                calibration_finish("no forward for the aim target");
                return;
            }
            fx /= length;
            fz /= length;
            g_cal_target[0] = head.m[0][3] + fx * kCalTargetDistance;
            g_cal_target[1] = head.m[1][3] - 0.10f;
            g_cal_target[2] = head.m[2][3] + fz * kCalTargetDistance;
            g_cal_step = 1;
            log("vr: hand calibration step 2: pistol aim (target %.1f m "
                "ahead)", kCalTargetDistance);
        }
    }

    void vr_hand_calibration_skip()
    {
        if (!g_cal_active)
            return;
        if (g_cal_step == 0)
        {
            g_cal_locked[0] = g_cal_locked[1] = false;
            log("vr: hand alignment skipped; the saved alignment is kept");
            if (g_cal_with_aim)
                calibration_start_aim();
            else
                calibration_finish("hand alignment skipped");
            return;
        }
        log("vr: pistol aim skipped; the saved aim is kept");
        calibration_finish("pistol aim skipped");
    }

    bool vr_hand_calibration_target(float head_point[3])
    {
        if (!g_cal_active || g_cal_step != 1 || !head_point)
            return false;
        Mat34 pose{};
        pose.m[0][0] = pose.m[1][1] = pose.m[2][2] = 1.0f;
        for (int i = 0; i < 3; ++i)
            pose.m[i][3] = g_cal_target[i];
        Mat4 m;
        if (!tracking_pose_to_head(pose, &m))
            return false;
        for (int i = 0; i < 3; ++i)
            head_point[i] = m.m[3][i];
        return true;
    }

    bool vr_hand_calibration_aim(bool left)
    {
        if (!g_cal_active || g_cal_step != 1)
            return false;
        Mat4 grip;
        float target[3];
        if (!vr_controller_grip_head_pose(left, &grip) ||
            !vr_hand_calibration_target(target))
            return false;
        // The direction from the corrected grip (where the aim ray starts)
        // to the target, in that grip's own axes (engine: x right, y down,
        // z forward) -- the same frame the aim ray's pitch/yaw live in.
        float d[3];
        for (int k = 0; k < 3; ++k)
            d[k] = target[k] - grip.m[3][k];
        float local[3]{};
        for (int row = 0; row < 3; ++row)
        {
            float len = 0.0f, dot = 0.0f;
            for (int k = 0; k < 3; ++k)
            {
                len += grip.m[row][k] * grip.m[row][k];
                dot += grip.m[row][k] * d[k];
            }
            if (len < 1.0e-8f)
                return false;
            local[row] = dot / sqrtf(len);
        }
        const float n = sqrtf(local[0] * local[0] + local[1] * local[1] +
                              local[2] * local[2]);
        if (!(n > 1.0e-4f))
            return false;
        for (float& v : local)
            v /= n;
        // local = (sin y cos p, -sin p, cos y cos p).
        const float pitch = asinf(fminf(1.0f, fmaxf(-1.0f, -local[1]))) *
                            57.29578f;
        float yaw = atan2f(local[0], local[2]) * 57.29578f;
        // Stored as the right hand's; the left grip is its mirror image.
        if (left)
            yaw = -yaw;
        float old_pitch = 0.0f, old_yaw = 0.0f;
        const bool had = tune_grip_aim(&old_pitch, &old_yaw);
        const bool saved = tune_set_grip_aim(pitch, yaw);
        log("vr: pistol aim set from the %s hand: pitch %+.1f, yaw %+.1f "
            "(was %s%+.1f, %+.1f) -- %s", left ? "left" : "right", pitch,
            yaw, had ? "" : "unset ", old_pitch, old_yaw,
            saved ? "saved" : "NOT saved");
        calibration_finish("pistol aim set");
        return saved;
    }

    bool vr_hand_calibration_begin(bool with_aim)
    {
        if (!g_ready || !g_pose_valid)
            return false;
        for (int hand = 0; hand < 2; ++hand)
            if (!g_grip_offset_valid[hand])
            {
                log("vr: hand calibration needs SteamVR grip poses; none yet "
                    "for the %s hand (hand_model 2 and the shipped "
                    "bindings provide them)", hand ? "right" : "left");
                return false;
            }
        const Mat34& head =
            g_tracked_poses[kPoseHmd]
                .mDeviceToAbsoluteTracking;
        float fx = -head.m[0][2], fz = -head.m[2][2];
        const float length = sqrtf(fx * fx + fz * fz);
        if (length < 1.0e-3f)
            return false;
        fx /= length;
        fz /= length;
        // Target frame in tracking space: right, up, forward.
        const float target[3][3] = {
            { -fz, 0.0f, fx }, { 0.0f, 1.0f, 0.0f }, { fx, 0.0f, fz } };
        // The hands' orientation: the same frame pitched kCalPitchDown
        // nose-down (user 2026-10-11: level barrels made the wrist bend up
        // awkwardly to match). Matching any shown pose gives the same
        // correction, so this changes comfort only. Positions stay on the
        // level frame.
        const float pitch_down = kCalPitchDown * 0.0174532925f;
        const float cp = cosf(pitch_down), sp = sinf(pitch_down);
        const float tilted[3][3] = {
            { target[0][0], target[0][1], target[0][2] },
            { target[1][0] * cp + target[2][0] * sp,
              target[1][1] * cp + target[2][1] * sp,
              target[1][2] * cp + target[2][2] * sp },
            { target[2][0] * cp - target[1][0] * sp,
              target[2][1] * cp - target[1][1] * sp,
              target[2][2] * cp - target[1][2] * sp } };

        // The gun's barrel in grip-local terms, as the aim ray uses it
        // (camera_head barrel_world): the grip aim, its yaw mirrored for
        // the left hand, else the shared aim offset.
        float pitch = 0.0f, yaw = 0.0f;
        const bool grip_aim = config().hand_model == 2 &&
                              tune_grip_aim(&pitch, &yaw);
        if (!grip_aim)
            tune_aim_offset(&pitch, &yaw);
        for (int hand = 0; hand < 2; ++hand)
        {
            const float p = pitch * 0.0174532925f;
            const float y = (grip_aim && hand == 0 ? -yaw : yaw) *
                            0.0174532925f;
            // Engine local (x right, y down, z forward) to OpenVR local
            // (y up, z back).
            float fwd[3] = { sinf(y) * cosf(p), sinf(p),
                             -cosf(y) * cosf(p) };
            // Up: the grip's own up, square to the barrel -- thumbs up.
            float up[3] = { -fwd[1] * fwd[0], 1.0f - fwd[1] * fwd[1],
                            -fwd[1] * fwd[2] };
            const float ul = sqrtf(up[0] * up[0] + up[1] * up[1] +
                                   up[2] * up[2]);
            if (ul < 1.0e-3f)
                return false;
            for (float& v : up)
                v /= ul;
            const float right[3] = {
                fwd[1] * up[2] - fwd[2] * up[1],
                fwd[2] * up[0] - fwd[0] * up[2],
                fwd[0] * up[1] - fwd[1] * up[0] };
            const float* local[3] = { right, up, fwd };
            // Turn the grip's right/up/barrel onto body right/up/forward.
            Mat34& pose = g_cal_reference[hand];
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    pose.m[i][j] = tilted[0][i] * local[0][j] +
                                   tilted[1][i] * local[1][j] +
                                   tilted[2][i] * local[2][j];
            const float side = hand == 0 ? -kCalSide : kCalSide;
            for (int i = 0; i < 3; ++i)
                pose.m[i][3] = head.m[i][3] + target[0][i] * side +
                               target[1][i] * kCalUp +
                               target[2][i] * kCalForward;
        }
        g_cal_locked[0] = g_cal_locked[1] = false;
        g_cal_active = true;
        g_cal_step = 0;
        g_cal_with_aim = with_aim;
        log("vr: hand calibration started (hands held %.2f m forward, "
            "%.2f m down, %.2f m apart, %.0f deg nose-down; barrel pitch "
            "%+.1f, yaw %+.1f)", kCalForward, -kCalUp, 2.0f * kCalSide,
            kCalPitchDown, pitch, yaw);
        return true;
    }

    void vr_hand_calibration_cancel(const char* reason)
    {
        if (!g_cal_active)
            return;
        g_cal_active = false;
        log("vr: hand calibration cancelled (%s); nothing saved",
            reason ? reason : "?");
    }

    bool vr_hand_calibration_lock(bool left)
    {
        const int hand = left ? 0 : 1;
        if (!g_cal_active || g_cal_locked[hand])
            return false;
        Mat4 reference, grip;
        if (!tracking_pose_to_head(g_cal_reference[hand], &reference) ||
            !uncorrected_grip_head_pose(hand, &grip))
            return false;
        // K * grip = reference, so the hand shows exactly here from now on.
        const Mat4 k = reference * rigid_inverse(grip);
        const float scale = tune_world_scale();
        if (!(scale > 0.0f))
            return false;
        float c[12]{};
        for (int r = 0; r < 3; ++r)
            for (int col = 0; col < 3; ++col)
                c[r * 3 + col] = k.m[r][col];
        float distance = 0.0f;
        for (int axis = 0; axis < 3; ++axis)
        {
            c[9 + axis] = k.m[3][axis] / scale;
            distance += c[9 + axis] * c[9 + axis];
        }
        distance = sqrtf(distance);
        bool finite = true;
        for (float v : c)
            finite = finite && std::isfinite(v);
        if (!finite || distance > 0.5f)
        {
            log("vr: %s hand not locked -- the controller is %.2f m from "
                "the hand (0.5 m at most)", left ? "left" : "right",
                distance);
            return false;
        }
        const float angle = acosf(fminf(1.0f, fmaxf(-1.0f,
            0.5f * (c[0] + c[4] + c[8] - 1.0f)))) * 57.29578f;
        memcpy(g_cal_pending[hand], c, sizeof(c));
        g_cal_locked[hand] = true;
        log("vr: %s hand locked: %.1f degrees and %.3f m from the SteamVR "
            "grip pose", left ? "left" : "right", angle, distance);
        if (!g_cal_locked[0] || !g_cal_locked[1])
            return true;
        const bool saved = tune_set_hand_correction(true, g_cal_pending[0]) &&
                           tune_set_hand_correction(false, g_cal_pending[1]);
        log("vr: hand alignment %s", saved
            ? "saved to trlvr.ini" : "could NOT be saved to trlvr.ini");
        if (g_cal_with_aim)
            calibration_start_aim();
        else
            calibration_finish("hand alignment saved");
        return true;
    }

    bool vr_controller_head_pose(bool left, Mat4* controller_to_head)
    {
        if (!controller_to_head || !g_ready || !g_pose_valid)
            return false;

        const unsigned index = left ? kPoseLeft : kPoseRight;
        const TrackedPose& controller = g_tracked_poses[index];
        const TrackedPose& hmd =
            g_tracked_poses[kPoseHmd];
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
        if (!out || !g_ready || !g_pose_valid)
            return false;
        const TrackedPose& hmd =
            g_tracked_poses[kPoseHmd];
        if (!hmd.bPoseIsValid || !hmd.bDeviceIsConnected)
            return false;
        const Mat34& hm = hmd.mDeviceToAbsoluteTracking;
        float fx = -hm.m[0][2], fz = -hm.m[2][2];
        const float length = sqrtf(fx * fx + fz * fz);
        if (length < 1.0e-4f)
            return false;
        fx /= length;
        fz /= length;

        // The inverse of vr_controller_body_position, as a tracking-space
        // pose with no rotation, then the same composition as
        // vr_controller_head_pose so conventions cannot drift apart.
        Mat34 point{};
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
        {
            const Mat34& hm =
                g_tracked_poses[kPoseHmd]
                    .mDeviceToAbsoluteTracking;
            const float fx = -hm.m[0][2], fz = -hm.m[2][2];
            const float length = sqrtf(fx * fx + fz * fz);
            if (g_pose_valid && length > 1.0e-4f)
            {
                g_recentre_forward[0] = fx / length;
                g_recentre_forward[1] = fz / length;
            }
        }

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
        // Asked afresh: it carries the runtime's resolution setting, which
        // the user may have changed since start-up (applied on a device
        // reset).
        if (g_ready)
        {
            unsigned rw = 0, rh = 0;
            if (xr_recommended_size(&rw, &rh) && rw && rh)
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

        // The poses come from xrWaitFrame's predicted display time
        // (vr_set_render_poses), which is the only time OpenXR will locate
        // anything for. There is nothing to read here.
    }

    void vr_set_render_poses(const TrackedPose* poses, unsigned count)
    {
        if (!g_ready || !poses || count < kPoseCount)
            return;
        memcpy(g_tracked_poses, poses, sizeof(g_tracked_poses));
        if (!g_render_poses_active)
        {
            g_render_poses_active = true;
            log("vr: rendering with the runtime's predicted poses "
                "(xrWaitFrame, LOCAL space)");
        }
        ingest_poses();
    }

    static void ingest_poses()
    {
        ++g_head_state_generation;
        const TrackedPose& hmd =
            g_tracked_poses[kPoseHmd];
        g_pose_valid = hmd.bPoseIsValid && hmd.bDeviceIsConnected;
        if (!g_pose_valid)
            return;

        g_head_view = view_from_pose(&hmd.mDeviceToAbsoluteTracking.m[0][0], tune_world_scale());
        theatre_update();
        diorama_update();

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

    // Head view space to this eye's view space, the whole rigid transform the
    // runtime reports, in the game's axes (x right, y down, z forward; OpenVR
    // is y up, z back, so S = diag(1,-1,-1) on both sides). The offset alone
    // used to be applied, with y and z in OpenVR's signs: harmless while both
    // are a fraction of a millimetre, wrong on a headset that reports depth or
    // cant. Row vectors: v_eye = (v_head - t) * R, with t scaled to game units.
    static Mat4 eye_from_head(const EyeInfo& in, float scale)
    {
        const float sign[3] = { 1.0f, -1.0f, -1.0f };
        const float t[3] = { in.offset_x * scale, -in.offset_y * scale,
                             -in.offset_z * scale };
        Mat4 m = Mat4::identity();
        // An unread (all-zero) rotation keeps identity.
        const bool have_rot = in.rot[0][0] != 0.0f || in.rot[1][1] != 0.0f ||
                              in.rot[2][2] != 0.0f;
        for (int i = 0; have_rot && i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                m.m[i][j] = sign[i] * in.rot[i][j] * sign[j];
        for (int j = 0; j < 3; ++j)
        {
            float acc = 0.0f;
            for (int i = 0; i < 3; ++i)
                acc += t[i] * m.m[i][j];
            m.m[3][j] = -acc;
        }
        return m;
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
        return (g_sky_no_eye_offset ? Mat4::identity()
                                    : eye_from_head(in, s)) * proj;
    }
}


