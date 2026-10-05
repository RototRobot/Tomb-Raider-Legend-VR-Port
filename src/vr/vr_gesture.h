#pragma once
// Small, headset-independent gesture classifiers. Keeping the geometry here
// makes the thresholds testable without OpenVR or a running game.

namespace trlvr
{
    struct VrHolsterZone
    {
        // left_x/right_x with the shared y/z are the original INI keys.
        float left_x, right_x, y, z;
        float radius_x, radius_y, radius_z;
        bool debug_draw;
        // Per-side centre height and depth. They default to the shared y/z;
        // in-game gear tuning places each holster independently.
        float left_y, left_z, right_y, right_z;
    };

    VrHolsterZone vr_default_holster_zone();
    // Default centre of the chest personal-light zone, metres.
    void vr_default_chest_zone_centre(float out[3]);
    // Controller position relative to the player's head/body frame, metres:
    // right is positive toward the right shoulder, up is positive, and
    // forward is positive in the direction the headset faces (yaw only).
    // `centre` moves the zone (right, up, forward); null uses the default.
    bool vr_gesture_in_chest_zone(float right, float up, float forward,
                                  const float* centre = nullptr);
    bool vr_gesture_in_holster_zone(bool left, float right, float up,
                                    float forward,
                                    const VrHolsterZone* zone = nullptr);
    bool vr_gesture_binoculars_at_face(float right, float up, float forward);
    bool vr_gesture_grapple_throw(float forward_travel,
                                  float release_forward,
                                  float recent_forward_speed);
    // A deliberate backward pull, or a shorter fast yank, while a trigger
    // is held and the retail hook has a pullable target.
    bool vr_gesture_grapple_pull(float backward_travel,
                                 float backward_speed);
}
