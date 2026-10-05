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
    bool tune_hand_debug_enabled();
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
