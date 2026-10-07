#pragma once
// The OpenVR session: headset, eye frusta, poses.
//
// openvr_api.dll is loaded by hand rather than linked. This DLL is the game's
// d3d9.dll, so a missing or broken OpenVR runtime must not stop the game from
// starting -- it logs why and the game runs flat.

#include "vr_math.h"

namespace trlvr
{
    // EyeCenter is not a real eye: it is the average of the two frusta with no
    // lateral offset, used while a single image is being sent to both eyes.
    enum Eye { EyeLeft = 0, EyeRight = 1, EyeCenter = 2 };

    // One eye's frustum, as the runtime describes it.
    struct EyeInfo
    {
        // Tangents of the half-angles from the view axis. Asymmetric on every
        // real headset, which is why the eye projection is off-centre.
        float tan_left, tan_right, tan_top, tan_bottom;

        // Offset from the head origin, in metres.
        float offset_x, offset_y, offset_z;

        // Eye-to-head rotation (OpenVR axes, row-major). Identity on most
        // headsets; canted displays (Pimax and others) turn each eye outward,
        // and the tangents above are measured about that turned axis.
        float rot[3][3];
    };

    bool vr_init();

    // The compositor, fetched through our own dynamic loader. openvr.h's
    // vr::VRCompositor() helper cannot be used: it calls the *imported*
    // VR_GetGenericInterface, and we deliberately do not link openvr_api.
    // Null unless the session came up as a Scene application.
    void* vr_compositor_raw();
    void vr_shutdown();
    bool vr_ready();

    // Any OpenVR interface by version string, from the session already up --
    // IVRInput for the controllers, say. Null until vr_ready().
    void* vr_get_interface(const char* version);

    // Empty until vr_init succeeds.
    const char* vr_status();

    const EyeInfo& vr_eye(Eye eye);
    void vr_render_target_size(unsigned* width, unsigned* height);

    // Distance between the eyes in metres, from the runtime.
    float vr_ipd();

    // Width over height of an eye's frustum. Submitting a texture of a
    // different shape makes the compositor stretch it, because Submit with no
    // bounds means "this image is the whole frustum".
    float vr_eye_aspect();

    // The widest half-angle tangent either eye needs, across both axes. The
    // engine's culling frustum has to reach at least this far or geometry the
    // headset can see gets thrown away.
    float vr_max_tangent();

    // Refresh the head pose. Cheap; safe to call once a frame. Does nothing
    // once vr_set_render_poses is supplying the compositor's poses.
    void vr_update_pose();

    // The poses returned by IVRCompositor::WaitGetPoses (seated space): the
    // predicted poses the compositor assumes the next frame is rendered
    // with. `poses` is a vr::TrackedDevicePose_t array.
    void vr_set_render_poses(const void* poses, unsigned count);

    // Head pose as a view matrix in the engine's row-vector convention,
    // in game units. Identity until a pose has been read.
    const Mat4& vr_head_view();

    // The head's heading in the horizontal plane, radians. Driving the game
    // camera with this is what makes room visibility follow the head --
    // portals are projected from the camera and cannot be aimed any other
    // way. Reported whether or not the camera is being driven.
    float vr_head_yaw();
    // F4: distinct world origins of the next left-eye scene draws (skybox hunt).
    void vr_world_capture_start();

    // Call the direction the player is currently facing 'forward'. With a
    // mouse and keyboard the headset is rarely pointing where the desk is,
    // and reaching for the keyboard turns the chair.
    void vr_recenter();
    unsigned vr_recenter_generation();

    // The head's displacement from the recentred origin, in engine units,
    // in the camera's own space.
    // The head rotation the camera should be given: recentred, turn-scaled,
    // rotation only. Composed onto the transform the engine built rather than
    // converted into its Euler convention -- see camera_head.cpp.
    Mat4 vr_head_rotation();

    // The complete view-space transform composed onto the game camera:
    // recentred/scaled rotation plus the optional scaled head displacement.
    // Keeping this construction in one place lets a paused frame advance from
    // the last camera-baked pose to the live pose without changing conventions.
    Mat4 vr_head_camera_view();

    // First-person uses Lara's live head bone as its world anchor, so the
    // headset translation must be relative to the physical position at which
    // that mode was entered rather than the SteamVR seated origin.
    void vr_set_head_position_origin(const float* xyz, bool active);

    // The main camera hook calls this immediately after baking the head pose.
    // During pause, GPU-side scene correction then starts from this exact pose
    // instead of applying the current pose twice.
    void vr_note_main_camera_head_applied();

    void vr_head_position(float* xyz);
    // How far the headset is below the position first person started at,
    // game units, positive downward (0 without an origin or positional
    // tracking). This is the physical height already in the camera.
    float vr_head_drop();
    bool vr_pose_valid();

    // Latest controller position in a yaw-only body frame, metres. This is a
    // practical torso approximation for gestures when no body tracker exists:
    // right/up/forward are positive along those player-relative directions.
    bool vr_controller_body_position(bool left, float* right, float* up,
                                     float* forward);

    // Controller velocity relative to the headset along its yaw-only forward
    // direction, metres/second. Head turning alone does not count as a throw.
    bool vr_controller_body_forward_speed(bool left, float* speed);

    // Endpoint of a tracked controller-local +X, +Y or forward (-Z) axis,
    // in the same yaw-only body frame. Used by the hand alignment gizmo.
    bool vr_controller_body_axis_endpoint(bool left, int axis, float metres,
                                          float* right, float* up,
                                          float* forward);

    // Project a point in the same yaw-only body frame used by gestures into
    // an eye's normalized texture coordinates. This keeps the debug boundary
    // aligned with the actual classifier even while the player looks down.
    bool vr_project_body_point(Eye eye, float right, float up, float forward,
                               float* u, float* v);
    // Project an engine head-space point (game units, X right/Y down/Z
    // forward), as used by the tracked-hand grab-zone renderer.
    bool vr_project_head_point(Eye eye, float x, float y, float z,
                               float* u, float* v);

    // Latest tracked controller transform relative to the HMD, in engine
    // coordinates and units. This is intentionally independent of the game
    // camera so first-person rendering can compose it with the camera's final
    // camera-to-world transform without feeding controller motion into game
    // simulation.
    bool vr_controller_head_pose(bool left, Mat4* controller_to_head);

    // A yaw-only body-frame point (metres, as vr_controller_body_position
    // reports it) in the same head space and units as the translation of
    // vr_controller_head_pose. Lets placed gear be drawn exactly where its
    // grab zone is tested.
    bool vr_body_point_head_position(float right, float up, float forward,
                                     float out[3]);

    // The per-eye projection, built to match the conventions of the projection
    // the game itself uploaded.
    Mat4 vr_eye_projection(Eye eye, const GameProjection& game);

    // The projection above with the head pose composed in front of it. This is
    // what replaces the game's c0..c3.
    Mat4 vr_eye_matrix(Eye eye, const GameProjection& game);

    // Replace a c0..c3 upload with the headset's equivalent. Handles both the
    // pure projection that view-space geometry gets and the full
    // world-view-projection that world-space geometry gets. Returns false if
    // these registers should be left exactly as they are.
    bool vr_substitute_c0(const float* in_regs16, float* out_regs16);

    // The same, for an explicit eye rather than whichever one the alternating
    // path is currently on. Same-frame stereo issues each draw twice and needs
    // to choose the matrix per pass.
    // SteamVR grip pose (palm-centred, mirrored left/right) as a fixed offset
    // from each controller's raw pose: grip-local -> raw-local, metres.
    void vr_set_controller_grip_offset(bool left, const Mat4& metres);
    bool vr_controller_grip_offset(bool left, Mat4* metres);
    // The grip pose in the head frame, like vr_controller_head_pose.
    bool vr_controller_grip_head_pose(bool left, Mat4* grip_to_head);

    // Hand calibration (pause menu "Hand Calibration", user 2026-10-06:
    // Quest testers' hands did not line up with their controllers). Lara's
    // hands are held in a fixed pose in front of the player -- guns forward,
    // thumbs up -- and the player brings each controller to where it should
    // sit in that hand and squeezes its grip. The difference becomes a rigid
    // correction in that controller's grip frame, applied inside
    // vr_controller_grip_head_pose, so hands, guns, aim and grab zones all
    // follow it. Saved to [vr] hand_calibration_left/_right.
    void vr_hand_calibration_request_from_menu();
    bool vr_hand_calibration_menu_pending();
    void vr_hand_calibration_clear_menu_request();
    // Fixes the reference pose in front of the current head. False (and
    // logged) without grip poses for both hands. with_aim: after the hands,
    // a pistol aim step (first person with the pistols drawn).
    bool vr_hand_calibration_begin(bool with_aim);
    // Steps (user 2026-10-07): 0 hand alignment, 1 pistol aim. Each can be
    // skipped (keeping what was saved before) and saves on its own.
    int vr_hand_calibration_step();
    void vr_hand_calibration_skip();
    // Pistol aim: the aim hand (left_handed picks it) points its drawn
    // pistol at the target and pulls the trigger; the direction from its
    // grip to the target becomes the grip aim (mirrored for the other hand).
    bool vr_hand_calibration_aim(bool left);
    // The aim target in head space (game units), while step 1 is up.
    bool vr_hand_calibration_target(float head_point[3]);
    void vr_hand_calibration_cancel(const char* reason);
    bool vr_hand_calibration_active();
    bool vr_hand_calibration_locked(bool left);
    // Lock one hand where its controller is now. True when it locked; the
    // second hand saves both and ends calibration.
    bool vr_hand_calibration_lock(bool left);
    // The SteamVR grip pose without any calibration (head frame, game
    // units): where the palm really is, for the calibration guide.
    bool vr_controller_grip_uncorrected_head_pose(bool left, Mat4* out);
    bool vr_substitute_c0_for_eye(const float* in_regs16, float* out_regs16, Eye eye);

    // Remember what the game asked for and what we uploaded instead, so a
    // draw can revisit the decision. The state that identifies the interface
    // -- depth testing off -- is not set until after the matrix is uploaded,
    // so upload time is too early to judge. Same-frame stereo will want the
    // same thing for the opposite reason: a matrix per draw, not per upload.
    void vr_note_c0(const float* game, const float* uploaded);

    // True if the draw about to be issued needs different registers from the
    // ones currently uploaded, and fills them in.
    bool vr_draw_c0_override(float* out_regs16);

    // What to put back afterwards.
    const float* vr_draw_c0_restore();

    // What the game last uploaded, before any substitution. Same-frame
    // stereo rebuilds a matrix per eye from this.
    const float* vr_game_c0();
    bool vr_have_game_c0();

    // Bounded ledge-hand diagnostic. Call after the per-eye c0 upload and
    // immediately before the tagged GPU draw. `actual_regs16` is the device's
    // current c0..c3, or null if GetVertexShaderConstantF failed.
    void vr_log_hand_draw_c0_sample(int hand, int marker, Eye eye,
                                    const float* actual_regs16);

    // Whether the surface currently bound is the one the scene is drawn
    // into. Shadow maps and post targets are not.
    bool vr_drawing_the_scene();

    // Size of one eye within the render target currently bound. Besides the
    // main scene, this recognises the explicitly marked half-resolution water
    // pass, whose two eyes are packed into that smaller target as well.
    bool vr_stereo_target_eye_size(unsigned* width, unsigned* height);

    // The render target currently bound. Recorded so the log can say which
    // surface each kind of projection belongs to -- the menu and the scene
    // are not necessarily drawn to the same one.
    void vr_set_render_target_size(unsigned width, unsigned height);

    // The back buffer's size, which is what the main scene is drawn at. Passes
    // targeting anything else -- shadow maps especially -- are left alone.
    void vr_set_scene_size(unsigned width, unsigned height);

    Eye vr_current_eye();
    void vr_set_current_eye(Eye eye);

}
