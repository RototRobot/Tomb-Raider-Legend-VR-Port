#include "menu_panel.h"

#include <cmath>

namespace trlvr
{
    bool menu_panel_place(const float head[3], float forward_x, float forward_z,
                          float distance, float yaw_left_degrees,
                          float pitch_up_degrees, MenuPanelPose* out)
    {
        const float length = sqrtf(forward_x * forward_x + forward_z * forward_z);
        if (!out || !head || !(length > 1.0e-4f))
            return false;
        const float fx = forward_x / length, fz = forward_z / length;
        // Turned about +y (up): in these right-handed axes a positive turn
        // takes straight ahead (-z) to the left (-x).
        const float yaw = yaw_left_degrees * 0.01745329f;
        const float pitch = pitch_up_degrees * 0.01745329f;
        const float sy = sinf(yaw), cy = cosf(yaw);
        const float gx = fx * cy + fz * sy;
        const float gz = -fx * sy + fz * cy;
        const float sp = sinf(pitch), cp = cosf(pitch);
        const float d[3] = { gx * cp, sp, gz * cp };
        for (int i = 0; i < 3; ++i)
            out->centre[i] = head[i] + d[i] * distance;
        // Width level, to the right of the turned direction; height the
        // third axis (right x direction), leaning back toward the head when
        // the panel is above it.
        out->right[0] = -gz;
        out->right[1] = 0.0f;
        out->right[2] = gx;
        out->up[0] = -gx * sp;
        out->up[1] = cp;
        out->up[2] = -gz * sp;
        return true;
    }

    void menu_panel_corners(const MenuPanelPose& pose, float half_size,
                            float out[4][3])
    {
        const float sx[4] = { -1.0f, 1.0f, -1.0f, 1.0f };
        const float sy[4] = { 1.0f, 1.0f, -1.0f, -1.0f };
        for (int k = 0; k < 4; ++k)
            for (int i = 0; i < 3; ++i)
                out[k][i] = pose.centre[i] +
                            (sx[k] * pose.right[i] + sy[k] * pose.up[i]) * half_size;
    }

    float menu_panel_half_size(float distance, float width_degrees)
    {
        return distance * tanf(width_degrees * 0.5f * 0.01745329f);
    }

    namespace
    {
        enum { HoldIdle = 0, HoldPending = 1, HoldConsumed = 2 };
    }

    MenuHoldEvent menu_hold_update(MenuHold& hold, bool down,
                                   unsigned long long now_ms,
                                   float hold_seconds, bool overlay_busy,
                                   bool* to_game)
    {
        bool pass = false;
        MenuHoldEvent event = MenuHoldNone;
        if (overlay_busy)
        {
            // The overlay's button now; a press still down when it closes
            // is not a new one for the game.
            hold.state = down ? HoldConsumed : HoldIdle;
        }
        else if (!(hold_seconds > 0.0f))
        {
            // Off: the button goes straight to the game, except the rest of
            // a press that opened the overlay.
            if (hold.state == HoldConsumed && down)
                pass = false;
            else
            {
                hold.state = HoldIdle;
                pass = down;
            }
        }
        else
        {
            switch (hold.state)
            {
            case HoldIdle:
                if (down)
                {
                    hold.state = HoldPending;
                    hold.down_ms = now_ms;
                }
                break;
            case HoldPending:
                if (!down)
                {
                    hold.state = HoldIdle;
                    event = MenuHoldTap;
                }
                else if (now_ms - hold.down_ms >=
                         (unsigned long long)(hold_seconds * 1000.0f + 0.5f))
                {
                    hold.state = HoldConsumed;
                    event = MenuHoldOpen;
                }
                break;
            default:
                if (!down)
                    hold.state = HoldIdle;
                break;
            }
        }
        if (to_game)
            *to_game = pass;
        return event;
    }
}
