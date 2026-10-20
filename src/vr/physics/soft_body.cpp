#include "soft_body.h"

#include <cmath>

namespace trlvr
{
namespace rear
{
    bool soft_contact(RearContact& spring, SoftContactState& st,
                      const SoftContactInput& in)
    {
        const int hand = in.hand ? 1 : 0;
        const bool first_touch = in.touching && !st.touching_last;
        if (!in.gripping)
            st.grabbing = false;
        if (!in.touching && !st.grabbing)
            st.holding = false;
        else if (in.touching && !st.holding)
        {
            st.holding = true;
            for (int k = 0; k < 3; ++k)
                st.entry[k] = in.point[k] - in.root[k];
            // Arriving: the hand's velocity relative to her goes into the
            // spring, at most 2.5 m/s worth, x 0.6, and only when it moves
            // into the surface faster than 0.2 m/s and the last point
            // sample was under 100 ms ago (not a hand lost and found again).
            const double gap = in.now_seconds - st.last_time;
            if (!st.touching_last && st.have_last && gap > 0.0 && gap < 0.1 &&
                in.velocity_valid)
            {
                float speed2 = 0.0f, into = 0.0f;
                for (int k = 0; k < 3; ++k)
                {
                    speed2 += in.hand_velocity[k] * in.hand_velocity[k];
                    into -= in.hand_velocity[k] * in.normal[k];
                }
                const float cap = 2.5f * in.metre;
                const float scale = speed2 > cap * cap ? cap / std::sqrt(speed2) : 1.0f;
                if (into > 0.2f * in.metre && std::isfinite(speed2))
                {
                    for (int k = 0; k < 3; ++k)
                        spring.kick[k] += in.hand_velocity[k] * scale * 0.6f;
                    spring.kick_pending = true;
                }
            }
        }
        if (st.holding && in.gripping)
            st.grabbing = true;
        if (st.holding && in.point_valid)
        {
            for (int k = 0; k < 3; ++k)
                spring.press[hand][k] = in.point[k] - in.root[k] - st.entry[k];
            spring.press_time[hand] = in.now_seconds;
            if (st.grabbing)
                spring.grab_time[hand] = in.now_seconds;
        }
        st.touching_last = in.touching;
        if (in.point_valid)
        {
            for (int k = 0; k < 3; ++k)
                st.last_point[k] = in.point[k];
            st.last_time = in.now_seconds;
            st.have_last = true;
        }
        return first_touch;
    }
}
}
