#include "dpad_gesture.h"

#include <cmath>

namespace trlvr
{
    namespace
    {
        unsigned short direction(float lx, float ly)
        {
            if (std::fabs(lx) >= std::fabs(ly))
                return lx >= kDpadPress ? kDpadRight : lx <= -kDpadPress ? kDpadLeft : 0;
            return ly >= kDpadPress ? kDpadUp : ly <= -kDpadPress ? kDpadDown : 0;
        }

        void disarm(DpadGesture& g, bool centred)
        {
            g.armed = false;
            g.pressed = 0;
            g.need_centre = false;
            g.idle = 0.0f;
            g.block_move = !centred;
        }
    }

    DpadOut dpad_gesture_update(DpadGesture& g, bool modifier_down, bool cancel,
                                float lx, float ly, float dt)
    {
        const bool centred = std::fabs(lx) < kDpadCentre && std::fabs(ly) < kDpadCentre;
        const bool click = modifier_down && !g.prev_modifier;
        g.prev_modifier = modifier_down;

        if (cancel)
        {
            if (g.armed)
                disarm(g, centred);
        }
        else if (click && !g.armed)
        {
            g.armed = true;
            g.pressed = 0;
            g.idle = 0.0f;
            g.need_centre = !centred;
        }
        else if (click)
            g.idle = 0.0f;              // a second click restarts the wait

        if (g.armed)
        {
            if (g.pressed)
            {
                if (centred)
                {
                    g.pressed = 0;
                    if (!modifier_down)
                        disarm(g, true);
                }
            }
            else if (g.need_centre)
            {
                if (centred)
                    g.need_centre = false;
            }
            else
                g.pressed = direction(lx, ly);

            if (g.armed && !g.pressed && !modifier_down)
            {
                g.idle += dt;
                if (g.idle >= kDpadArmSeconds)
                    disarm(g, centred);
            }
            else if (g.armed)
                g.idle = 0.0f;
        }

        if (g.block_move && centred)
            g.block_move = false;

        DpadOut out;
        out.buttons = g.armed ? g.pressed : 0;
        out.stick_to_game = !g.armed && !g.block_move;
        out.modifier = g.armed;
        return out;
    }
}
