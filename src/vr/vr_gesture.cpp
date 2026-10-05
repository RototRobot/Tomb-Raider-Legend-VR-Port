#include "vr_gesture.h"

namespace trlvr
{
    VrHolsterZone vr_default_holster_zone()
    {
        // A deliberate, grip-clicked reach beside the lower hips. The prior
        // broad zone reached the player's hands around -0.41 m and toggled
        // guns during ordinary movement.
        return { -0.24f, 0.24f, -0.63f, -0.07f,
                 0.13f, 0.15f, 0.18f, true,
                 -0.63f, -0.07f, -0.63f, -0.07f };
    }

    void vr_default_chest_zone_centre(float out[3])
    {
        // Left chest, where the medipack pouch was (user request
        // 2026-10-01); the medipack moved to the right chest.
        out[0] = -0.13f;
        out[1] = -0.25f;
        out[2] = 0.03f;
    }

    bool vr_gesture_in_chest_zone(float right, float up, float forward,
                                  const float* centre)
    {
        // Keep the light gesture near the left upper chest. The earlier
        // torso-wide ellipsoid overlapped a reach toward the binoculars on
        // the belt, so the same grip press could toggle the light instead.
        float fallback[3]{};
        vr_default_chest_zone_centre(fallback);
        const float* c = centre ? centre : fallback;
        // A pouch-sized ball (the medipack pouch's 0.09 m plus a little),
        // replacing the large chest ellipsoid (0.20 x 0.14 x 0.18).
        const float r = 0.10f;
        const float dx = right - c[0];
        const float dy = up - c[1];
        const float dz = forward - c[2];
        return dx * dx + dy * dy + dz * dz <= r * r;
    }

    bool vr_gesture_in_holster_zone(bool left, float right, float up,
                                    float forward, const VrHolsterZone* setting)
    {
        const VrHolsterZone defaults = vr_default_holster_zone();
        const VrHolsterZone& zone = setting ? *setting : defaults;
        const float nx = (right - (left ? zone.left_x : zone.right_x)) /
                         zone.radius_x;
        const float ny = (up - (left ? zone.left_y : zone.right_y)) /
                         zone.radius_y;
        const float nz = (forward - (left ? zone.left_z : zone.right_z)) /
                         zone.radius_z;
        return nx * nx + ny * ny + nz * nz <= 1.0f;
    }

    bool vr_gesture_binoculars_at_face(float right, float up, float forward)
    {
        // Centred near the left eye but broad enough for different headsets
        // and controller grip offsets. A hand resting on the chest is below it.
        const float nx = (right + 0.08f) / 0.25f;
        const float ny = (up + 0.06f) / 0.17f;
        const float nz = (forward - 0.18f) / 0.24f;
        return nx * nx + ny * ny + nz * nz <= 1.0f;
    }

    bool vr_gesture_grapple_throw(float forward_travel,
                                  float release_forward,
                                  float recent_forward_speed)
    {
        // Start is the actual item pickup at the hip. A deliberate forward
        // extension plus a recent fast segment distinguishes a throw from
        // merely moving the held item or letting go at the belt.
        return forward_travel >= 0.28f && release_forward >= 0.15f &&
               recent_forward_speed >= 1.15f;
    }

    bool vr_gesture_grapple_pull(float backward_travel,
                                 float backward_speed)
    {
        // A slow, full pull is intentional too; a shorter yank needs clear
        // backward velocity. Both distances are relative to the controller
        // position when the trigger was pressed, not to the room origin.
        return backward_travel >= 0.28f ||
               (backward_travel >= 0.14f && backward_speed >= 1.0f);
    }
}
