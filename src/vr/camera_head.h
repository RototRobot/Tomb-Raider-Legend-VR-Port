#pragma once
// Driving the game camera's orientation from the headset.
//
// The camera owns the whole head pose -- rotation and position -- so that
// culling, room visibility and what is drawn all agree about where the
// player is looking from. See camera_head.cpp.

namespace trlvr
{
    void camera_head_init();

    // Optional positional stereo multiplier for accurate aim. It defaults to
    // one, matching every other camera mode and preserving physical IPD.
    float camera_stereo_scale();

    // The accurate-aim camera uses a much more aggressive close camera and a
    // different depth range. Projection replacement needs to know the mode at
    // draw time so it can use a VR-safe near plane.
    bool camera_accurate_aim_active();

    // First-person VR crosshair: where the next shot will land, in engine
    // head space (game units, X right, Y down, Z forward). Only while the
    // game would show a combat reticle. style bit 0: accurate aim; bit 1:
    // the aim ray hit a surface (clear rays end at weapon range); bit 2: the
    // hit is an enemy by the retail reticle's target test.
    bool camera_aim_crosshair(float head_point[3], int* style);
    // Aim tuning debug: the controller aim ray from its origin to the probed
    // impact (or 20 m out of combat), in the same head space.
    bool camera_aim_debug_ray(float head_origin[3], float head_end[3],
                              bool* hit);

    // True only while the opt-in first-person path has a validated gameplay
    // Lara/head anchor. Menus and cinematics deliberately fall back safely.
    bool camera_first_person_active();
    // 1 normally; the tabletop factor (Lara's real height / board height)
    // while the board-game third-person camera is in use.
    float camera_world_scale_factor();
    // Main camera to Lara after the head pose, world units (world-marker
    // stereo depth).
    float camera_marker_depth();
    // Puts the retail camera position back after a third-person frame drew
    // from its own (called at Present).
    void camera_core_position_restore();
    // Retail has a target for a thrown item (an enemy it has sensed).
    bool camera_grapple_has_target();

    // This frame's retail camera position (the game's own), the drawn eye
    // and the camera focus point, world units.
    bool camera_frame_positions(float retail[3], float eye[3],
                                float focus[3]);
    // The flat-screen view prescale removed from the main camera (x, y);
    // 1, 1 until seen. The game's own screen projections include it.
    void camera_view_prescale(float* x, float* y);

    // Board-mode "god hand" cheat (third-person immersive, board camera
    // only): a grip with the hand around the tiny Lara picks her up; she is
    // drawn following the hand; releasing puts her on the floor below
    // (retail INSTANCE_SetPositionPlayerFindGround). Called once a frame
    // from vr_input with each grip (already masked of gear-cross claims);
    // claimed[] says which grips the pick-up owns (no grapple/lock-on key).
    void camera_board_pluck_update(bool active, const bool grip[2],
                                   bool claimed[2]);
    bool camera_board_pluck_held();
    // Board mode finger flick: left hand, held = loaded, release = snap.
    void camera_board_flick_update(bool active, bool held);
    // Held by a pinch (not standing on a palm, where she may walk).
    bool camera_board_pluck_carried();
    // Developer pose tools ([developer] board_pose_debug = 1; keys read in
    // tune.cpp): 0 next finger chain, 1 finger curl on/off, 2 next curl
    // axis, 3 next / 4 previous dangle animation (board_dangle_anim);
    // 10..15 hang offset -x +x -y +y -z +z, 16 reset (board_hang_x/y/z).
    void camera_pose_debug_key(int key);
    // The marker ring in the head frame (game units): 0 none, 1 a hand can
    // pick Lara up, 2 held with floor below (landing spot), 3 held with no
    // floor found (release does nothing).
    int camera_board_pluck_ring(float points[24][3]);
    // A cutscene is playing (retail cinematic or resident cinematic).
    bool camera_cinematic_playing();

    // Immersive ledge hold. A grip near the upper ledge anchors the rendered
    // hand. Pull displacement is fed to the game's existing shimmy input;
    // collision and climb transitions remain authoritative.
    bool camera_first_person_ledge_hanging();
    // First person on foot (not hanging, climbing or swimming).
    bool camera_first_person_grounded();
    // Lara's animation is crouched (her eye well below standing).
    bool camera_first_person_crouched();
    // Comfort vignette strength 0..1 for this frame (call once per frame).
    float camera_comfort_vignette();
    bool camera_first_person_bar_hanging();
    // True for 150 ms after a grab that secures a precarious one-hand catch;
    // vr_input sends the retail Action key (E) for it.
    bool camera_first_person_secure_grip_pulse();
    // True while that controller is the free hand holding (or positioned to
    // hold) the fore-end of Lara's drawn long gun; its grip button then
    // belongs to the two-handed hold.
    bool camera_two_hand_grip_claims(bool left);
    bool camera_two_hand_engaged();
    // True only during the retail LedgeClimb pull-up animation, after the
    // hanging state has released the tracked hand anchors.
    bool camera_first_person_ledge_climbing();
    bool camera_first_person_vine_climbing();
    // A free-standing vertical pole or chain (VertPoleAttached /
    // VertPoleAirAttach): Lara can turn around it, unlike a wall vine.
    bool camera_first_person_free_pole();
    // The costliest DRAW_DrawInstance instances since the last call (perf
    // line); resets.
    void camera_perf_instance_report(unsigned frames);
    // Signed view yaw relative to Lara's vine-facing direction, in radians.
    // Positive means the player looks toward the vine's right.
    bool camera_first_person_vine_look_angle(float* radians);
    // Retail grapple hook state: 0 stowed/invalid, 1 deployed, 2 latched
    // onto an instance the player can pull.
    int camera_first_person_grapple_state();
    // One shared ledge/vine/bar reach box. Corners are in engine head-space
    // units (X right, Y down, Z forward). State is ready/inside/held.
    bool camera_first_person_grab_box(float corners[8][3], int* state);
    // While a precarious one-hand catch waits for a grab: the centre of the
    // grab zone in the head frame (game units), for the secure-grip badge.
    bool camera_first_person_secure_grab_point(float centre[3]);
    // The game queues BasicDrawable objects while building Lara's hand
    // geometry, then submits their GPU draws later in the frame.
    void camera_first_person_note_gpu_draw();
    int camera_first_person_drawable_hand(void* drawable);
    int camera_first_person_gpu_hand_draw(int hand);
    void camera_first_person_gpu_hand_draw_end(int previous);
    int camera_first_person_current_gpu_hand();
    int camera_first_person_hand_render_state_mark();
    void camera_first_person_gpu_frame_end();
    void camera_first_person_ledge_grip_input(bool left, bool down,
                                               bool enabled);
    struct HandholdPullSample
    {
        float right_metres;
        unsigned grip_serial;
        unsigned grabbed_at_ms;
        // Body pulled up (the hand moved down) since the grip, metres.
        float up_metres;
    };
    // True while a hand is held, including a brief tracking loss. Positive
    // output means the player has pulled their body to the ledge's right.
    // Both anchored hands must pull down on a horizontal ledge for positive
    // up displacement; on vertical holds the active anchored hand supplies
    // signed up/down travel. One hand still supplies ledge shimmy. Individual
    // samples identify successive grabs for alternating-hand fast shimmy;
    // a zero serial means that hand has no tracked pull this frame.
    bool camera_first_person_ledge_pull(float* right_metres,
                                        float* up_metres,
                                        HandholdPullSample hands[2]);

    // Copy the live wrist/controller calibration for persistence when the
    // developer hand-tuning controls save their values.
    bool camera_first_person_hand_calibration(int hand, float out[9]);

    // Immersive belt items: 0 binoculars, 1 grapple. Either hand may reach.
    // Reach uses the rendered player-facing belt pose.
    bool camera_first_person_gear_belt_distance(int item, bool left,
                                                 float* distance_metres);
    bool camera_first_person_gear_grab(int item, bool left,
                                      float radius_metres,
                                      float* distance_metres);
    void camera_first_person_gear_release(int item);

    // 0: unrelated, 1: Lara, 2: an instance linked to Lara. Used only to
    // bypass frustum culling for first-person hands and carried equipment.
    int camera_first_person_visibility_class(void* instance);

}
