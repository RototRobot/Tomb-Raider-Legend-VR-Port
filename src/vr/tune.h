#pragma once
// Runtime values. F1 recentres; F5 reloads developer alignment and zones.

#include "vr_gesture.h"

namespace trlvr
{
    void tune_update();
    float tune_ipd_scale();
    // First person: the distance (metres) world-marker sprites such as the
    // caution icon are converged at. [ and ] adjust it in game and save it
    // to trlvr.ini ([vr] first_person_marker_distance).
    float tune_marker_distance();
    float tune_world_scale();
    float tune_look_speed();
    bool tune_swap_eyes();
    float tune_cull_fov();
    bool tune_level_horizon();
    bool tune_camera_auto_center();
    bool tune_camera_shake();
    float tune_move_scale();
    float tune_turn_scale();
    float tune_ui_scale();
    float tune_hud_follow();
    void tune_hand_offset(bool left, float out[3]);
    void tune_hand_rotation(bool left, float out[3]);
    // Saved wrist/controller basis captured when the user presses Numpad 7.
    // The nine values are the row-major 3x3 rotation portion of the rigid
    // calibration matrix. A missing or invalid set returns false.
    bool tune_load_hand_calibration(bool left, float out[9]);
    // hand_model 2 (grip pose): the shared hand pose, its converted base
    // rotation, and the converted aim ray. Setters persist to trlvr.ini.
    void tune_aim_offset_hand(bool left, float* pitch_degrees,
                              float* yaw_degrees);
    void tune_grip_hand(float position_metres[3], float rotation_degrees[3]);
    bool tune_grip_hand_calibration(float out[9]);
    bool tune_set_grip_hand_calibration(const float in[9]);
    bool tune_grip_aim(float* pitch_degrees, float* yaw_degrees);
    bool tune_set_grip_aim(float pitch_degrees, float yaw_degrees);
    bool tune_save_hand_calibration(bool left, const float in[9]);
    // Pause-menu hand calibration (2026-10-06): a rigid correction in each
    // controller's grip frame, row-major 3x3 rotation then translation in
    // metres (12 values), saved as [vr] hand_calibration_left/_right.
    // False when none is saved. F5 reloads. Setting null clears it.
    bool tune_hand_correction(bool left, float out[12]);
    bool tune_set_hand_correction(bool left, const float in[12]);
    bool tune_hand_debug_enabled();
    // [developer] hand_ruler (2026-10-07): the controllers' true grip poses
    // and a ruler from the left palm to the right controller's tip, in real
    // centimetres, to measure the drawn hand against the controller in each
    // view. F5 reloads.
    bool tune_hand_ruler_enabled();
    // Theatre window placement: its offset (and the eye anchored to it) from
    // where it first appears, metres (right, up, forward). [vr]
    // theatre_window_right/up/forward; the handle saves them.
    void tune_theatre_offset(float out[3]);
    void tune_set_theatre_offset(const float in[3]);
    // Board placement moved by the diorama's handle, metres (right, up,
    // forward) in the recentred frame. [vr] board_offset_right/up/forward.
    void tune_board_offset(float out[3]);
    void tune_set_board_offset(const float in[3]);
    // [developer] gear_cross_debug: draw each gear cross grab sphere and the
    // point of each controller that is tested against them. F5 reloads.
    bool tune_gear_cross_debug();
    // [developer] finger_debug: SteamVR's hand skeleton drawn at each
    // controller with five finger-curl bars, and the finger numbers logged
    // once a second. F5 reloads.
    bool tune_finger_debug();
    // [developer] finger_record: numpad / starts and stops recording both
    // hands' finger poses to trlvr_finger_poses.csv. F5 reloads.
    bool tune_finger_record();
    // Built-in tuning profiles (2026-10-08): hand model, aim, holsters, gear
    // and gear cross defaults per controller type, used for any key the INI
    // does not set. 0 = the developer's Reverb G2 tuning (also WMR, Index
    // and anything unknown), 1 = Oculus Touch (a Quest tester's tuning).
    // vr_input picks it from SteamVR's controller type; changing it reloads.
    void tune_set_controller_profile(int profile, const char* type);
    // [controls] reset_controller_tuning_on_load = 1: at startup, before
    // anything reads them, remove the saved hand tuning (Hand Calibration,
    // the numpad hand / grip alignment and the aim-ray offsets) from
    // trlvr.ini, so the controller profile's defaults apply again (Oculus
    // Touch for Quest, the default for every other controller). Returns
    // how many saved values were removed, or -1 with the option off.
    int tune_reset_controller_tuning_on_load();
    int tune_controller_profile();
    // An INI value with the profile's default when the INI has none (the
    // old auto-written holster values count as none). False if neither.
    bool tune_profile_read(const wchar_t* section, const wchar_t* key,
                           wchar_t* out, size_t out_count);
    // The profile's own default, ignoring the INI.
    bool tune_profile_default(const wchar_t* section, const wchar_t* key,
                              float* out);
    bool tune_hand_debug_selected_left();
    bool tune_hand_debug_rotation_mode();
    float tune_first_person_belt_height();
    float tune_ledge_grab_height();
    bool tune_ledge_grab_debug_draw();
    // [developer] grip_prompt_scale: size of the secure-grip prompt on the
    // left hand, as a fraction of its retail size. F5 reloads.
    float tune_grip_prompt_scale();
    // Strip the engine's flat-screen prescale from the rendered view. INI
    // default, F7 toggles for this session.
    bool tune_view_prescale_fix();
    // [developer] aim_pitch / aim_yaw, degrees; F5 reloads.
    void tune_aim_offset(float* pitch_degrees, float* yaw_degrees);
    // [developer] aim_tuning_debug: draw the aim laser and let the numpad
    // rotate it (8/2 pitch, 4/6 yaw, 5 reset, 7 save). F5 reloads.
    bool tune_aim_debug_enabled();
    // F8 records a visibility sample without changing rendering.
    unsigned tune_ledge_visibility_marker();
    const VrHolsterZone& tune_holster_zone();

    // In-game gear placement. Gear indices match the first-person belt
    // renderer: pistols left/right, grapple, binoculars, personal light.
    // Positions are metres in the yaw-only body frame (right, up, forward)
    // used by every immersive grab zone, so a placed item and its zone are
    // the same point.
    enum TuneGear
    {
        TuneGearLeftPistol, TuneGearRightPistol, TuneGearGrapple,
        TuneGearBinoculars, TuneGearLight,
        // Pouches (2026-10-04): grenade/flare on the belt, medipack on the
        // chest. Saved like the others; unsaved they keep the defaults.
        TuneGearGrenade, TuneGearMedipack, TuneGearCount
    };
    // For the in-headset instructions: the current step (0-based), the
    // number of steps, the item a hand places in this step (-1 none), and
    // an item's name.
    int tune_gear_tuning_step();
    int tune_gear_tuning_step_count();
    int tune_gear_tuning_slot(int hand);
    bool tune_gear_tuning_placed(int gear);
    const char* tune_gear_name(int gear);
    // [developer] gear_tuning = 1 allows entering the tuning mode.
    bool tune_gear_tuning_enabled();
    bool tune_gear_tuning_active();
    // Start tuning, or cancel it without saving.
    void tune_gear_tuning_begin();
    // Pause menu "VR Holster Setup" (the Widescreen entry, 2026-10-04):
    // asks for gear placement once back in gameplay, for any player.
    void tune_gear_tuning_request_from_menu();
    bool tune_gear_tuning_menu_pending();
    void tune_gear_tuning_clear_menu_request();
    void tune_gear_tuning_cancel(const char* reason);
    // F6 is the keyboard equivalent of the double-click; vr_input owns the
    // entry checks, so the key only queues a request.
    bool tune_gear_tuning_take_request();
    // The hand (0 left, 1 right) whose controller the item follows right
    // now, or -1.
    int tune_gear_tuning_hand(int gear);
    // A trigger press by that hand in tuning mode: place its current item,
    // or pick a placed one back up. Returns +1 placed, -1 picked up, 0 none.
    // Placing the last item saves everything to trlvr.ini and exits.
    int tune_gear_tuning_trigger(bool left, const float body[3]);
    // Saved, or placed during this tuning pass. False keeps the item on its
    // original belt marker.
    bool tune_gear_position(int gear, float body[3]);
    // Personal-light chest zone centre, or null for the default zone.
    const float* tune_light_zone_centre();
}
