#pragma once
#include <cstdint>
// The VR controllers, as the keyboard and mouse the game is played with.
//
// Actions are named for what they do in the game and each presses a fixed
// key; which button produces which action lives in input/bindings_*.json and
// SteamVR's own bindings UI. See vr_input.cpp for why this is keyboard and
// mouse rather than a gamepad.

namespace trlvr
{
    enum VrControllerStyle
    {
        VrControllerTouch,       // HP Reverb G2 and Oculus Touch
        VrControllerIndex,
        VrControllerWmr,
    };

    // Once a frame, from Present. Connects on first use; does nothing at all
    // if the runtime, the manifest or the controllers are missing, and never
    // sends anything while the game is not the foreground window.
    void vr_input_update();

    // Used by the tutorial-label hook to choose the face-button names from
    // the supplied binding for the controller SteamVR actually found.
    VrControllerStyle vr_input_controller_style();
    bool vr_input_holster_hand_drawn(bool left);
    bool vr_input_holster_combat_held();
    // The UI screen on top of the menu stack as of the last input update,
    // -1 in gameplay (pause root 1, its settings page 2, load game 3).
    short vr_input_top_screen_id();
    // 0 ready, 1 hand inside, 3 both pistols drawn.
    int vr_input_holster_visual_state(bool left);

    // Called after the retail game really ends combat (including scripted
    // transitions), so VR pistol placement cannot outlive equipped guns.
    void vr_input_pistol_combat_ended(uintptr_t caller);

    // Cumulative right-stick yaw for first-person. The camera owns this angle
    // directly so Lara's movement and jump steering cannot turn the view.
    float vr_input_first_person_turn();

    // Latched gameplay movement intent in headset-local coordinates. Matches
    // the W/A/S/D keys sent to retail movement, including diagonals.
    bool vr_input_first_person_move(float* right, float* forward);
    // The raw analog left stick (x right, y forward), before any key mapping.
    bool vr_input_first_person_stick(float* right, float* forward);
    // Alternating-hand strokes on a swing bar request retail fast traverse.
    bool vr_input_bar_fast_traverse();

    // Cached controller jump state for first-person traversal steering.
    bool vr_input_first_person_jump_held();
    // The physical crouch is holding the game's crouch key.
    bool vr_input_physical_crouch_held();

    // A brief pulse on the matching controller when a free hand first
    // enters an immersive ledge or vine grab zone.
    void vr_input_handhold_haptic(bool left);
    // One vibration pulse for a shot from the gun in that hand (0..1).
    void vr_input_weapon_haptic(bool left, float strength);
    // The game's own pad motors this frame (0..1): low = the big, heavy
    // motor, high = the small, buzzing one. Played on both controllers.
    void vr_input_game_motors(float low, float high);
    // The grip button of one controller, as last sampled.
    bool vr_input_grip_held(bool left);
    // SteamVR skeletal input (finger tracking): each finger's curl, 0
    // straight .. 1 curled (thumb, index, middle, ring, pinky), and the
    // splay between neighbours (0 touching .. 1 apart). False when that
    // hand has no skeleton this frame or [controls] finger_tracking = 0.
    bool vr_input_finger_pose(bool left, float curl[5], float splay[4]);
    // Each finger's measured sideways angle from the middle finger (index,
    // middle = 0, ring, pinky; radians, pinky-ward positive), from
    // SteamVR's bones. False without bone data.
    bool vr_input_finger_spread(bool left, float angle[4]);
    // The thumb tip in the hand's frame, from SteamVR's bones: out[0..2] =
    // forward, sideways (towards the pinky) and palm-ward of the index
    // knuckle, in index-to-pinky knuckle spans; out[3] = 0..1 as the thumb
    // tip meets the index tip. False without bone data.
    bool vr_input_thumb_target(bool left, float out[4]);
    // SteamVR's 31 hand bone positions (model space, metres, OpenVR axes,
    // relative to the controller's raw pose), while finger data is live.
    bool vr_input_finger_bones(bool left, float out[31][3]);
    // The move stick is walking Lara this frame (any of W/A/S/D).
    bool vr_input_stick_moving();
    // The physical trigger of one controller, as last sampled in gameplay.
    bool vr_input_trigger_held(bool left);
    // Pouch item 0 = grenade/flare (belt), 1 = medipack (chest): its body
    // position (metres: right, up, forward) and the hand holding it (-1).
    bool vr_input_pouch_item(int item, float body[3], int* held_hand);

    // Third-person immersive gear cross (d-pad layout in front of the
    // waist). Body-frame metres (right, up, forward). Items: 0 up =
    // medipack, 1 right = binoculars, 2 down = weapon switch, 3 left =
    // flashlight. The centre is a handle that moves the whole cross.
    // Third-person scale / zoom sliders on the off hand's upturned palm
    // (2026-10-10). Body-frame metres (right, up, forward), as
    // vr_body_point_head_position takes them. Slider 0 is scale, 1 zoom.
    struct VrTpSliders
    {
        bool visible = false;
        float opacity = 0.0f;           // 0..1, fading in
        float start[2][3]{};            // each bar's ends
        float end[2][3]{};
        float knob[2][3]{};             // each knob's centre
        bool hover[2] = { false, false };
        bool held[2] = { false, false };
        float icon[2][3]{};             // where each slider's icon sits
    };
    bool vr_input_tp_sliders(VrTpSliders* out);

    struct VrGearCross
    {
        bool visible = false;
        float centre[3]{};
        float items[4][3]{};
        int hover[2] = { -1, -1 };  // item under each hand (left, right)
        int flash_item = -1;        // last used item
        float flash = 0.0f;         // 1 -> 0 over 0.4 s after use
        float nearness = 0.0f;      // 0 far .. 1 a hand at the cross
        bool handle_hover = false;  // a hand on the middle handle
        bool dragging = false;      // the cross is being moved
        float item_radius = 0.0f;   // grab sphere round each icon, metres
        float handle_radius = 0.0f; // grab sphere round the middle handle
        // The point of each hand tested against the spheres (body frame,
        // metres), and whether it is Lara's palm (else the controller).
        float hand_point[2][3]{};
        bool hand_point_valid[2] = { false, false };
        bool hand_point_palm[2] = { false, false };
    };
    bool vr_input_gear_cross(VrGearCross* out);
}
