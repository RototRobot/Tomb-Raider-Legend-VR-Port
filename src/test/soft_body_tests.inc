// Tests for src/vr/soft_body, the contact part (included by mathtest.cpp).
#include "../vr/physics/soft_body.h"

static void test_soft_body()
{
    using namespace trlvr::rear;
    const float metre = 291.0f;
    const double dt = 1.0 / 60;

    // A press records how far the hand has moved since it took hold, and
    // stops being updated once the hand is out.
    {
        RearContact sp;
        SoftContactState hand;
        SoftContactInput in;
        in.touching = true;
        in.hand = 1;
        in.metre = metre;
        in.normal[1] = -1.0f;
        in.point[1] = -12.0f;
        in.point[2] = 150.0f;
        double now = 1.0;
        bool first = false;
        for (int i = 0; i < 60; ++i)
        {
            now += dt;
            in.point[1] = -12.0f + 0.03f * metre * std::fmin(1.0f, i / 30.0f);
            in.now_seconds = now;
            if (soft_contact(sp, hand, in))
                first = true;
        }
        CHECK(first && hand.holding && !hand.grabbing);
        CHECK(near(sp.press[1][1], 0.03f * metre, 1e-3f) && sp.press[0][1] == 0.0f);
        CHECK(sp.press_time[1] == now && sp.grab_time[1] == 0.0);
        in.touching = false;
        in.point[1] += 5.0f;
        now += dt;
        in.now_seconds = now;
        soft_contact(sp, hand, in);
        CHECK(!hand.holding);
        CHECK(near(sp.press[1][1], 0.03f * metre, 1e-3f));
    }

    // A fast arrival kicks once; a slow, outgoing or stale one does not.
    {
        const struct { float speed_m; double gap; float dir; bool kicks; } cases[] = {
            { 1.0f, 0.016, +1.0f, true },       // into the surface at 1 m/s
            { 0.1f, 0.016, +1.0f, false },      // too slow (< 0.2 m/s)
            { 1.0f, 0.016, -1.0f, false },      // moving away from it
            { 1.0f, 0.15, +1.0f, false },       // the last sample is too old
            { 10.0f, 0.016, +1.0f, true },      // capped at 2.5 m/s
        };
        for (const auto& c : cases)
        {
            RearContact sp;
            SoftContactState hand;
            double now = 5.0;
            SoftContactInput in;
            in.hand = 0;
            in.metre = metre;
            in.normal[1] = -1.0f;
            in.point[1] = -0.1f * metre;
            in.now_seconds = now;
            soft_contact(sp, hand, in);             // outside, seen
            in.touching = true;
            in.now_seconds = now + c.gap;
            in.point[1] = 0.0f;
            in.hand_velocity[1] = c.dir * c.speed_m * metre;
            soft_contact(sp, hand, in);
            CHECK(sp.kick_pending == c.kicks);
            if (c.kicks)
            {
                const float v = c.speed_m > 2.5f ? 2.5f : c.speed_m;
                CHECK(near(sp.kick[1], 0.6f * v * metre, 1e-3f));
            }
            else
                CHECK(sp.kick[1] == 0.0f);
        }
        // Staying in does not kick again.
        RearContact sp;
        SoftContactState hand;
        SoftContactInput in;
        in.metre = metre;
        in.normal[1] = -1.0f;
        in.point[1] = -0.1f * metre;
        in.now_seconds = 2.0;
        soft_contact(sp, hand, in);
        in.touching = true;
        in.now_seconds = 2.016;
        in.hand_velocity[1] = 1.0f * metre;
        soft_contact(sp, hand, in);
        const float once = sp.kick[1];
        in.now_seconds = 2.032;
        soft_contact(sp, hand, in);
        CHECK(once > 0.0f && sp.kick[1] == once);
        // No valid velocity: no kick.
        RearContact blind;
        SoftContactState h2;
        SoftContactInput bi;
        bi.metre = metre;
        bi.normal[1] = -1.0f;
        bi.now_seconds = 3.0;
        soft_contact(blind, h2, bi);
        bi.touching = true;
        bi.velocity_valid = false;
        bi.hand_velocity[1] = metre;
        bi.now_seconds = 3.016;
        soft_contact(blind, h2, bi);
        CHECK(!blind.kick_pending);
    }

    // A grip holds the part even with the hand out of her body, until it opens.
    {
        RearContact sp;
        SoftContactState hand;
        double now = 1.0;
        SoftContactInput in;
        in.hand = 0;
        in.metre = metre;
        in.normal[1] = -1.0f;
        in.touching = true;
        in.gripping = true;
        for (int i = 0; i < 150; ++i)
        {
            now += dt;
            in.now_seconds = now;
            in.point[0] = 0.03f * metre * std::fmin(1.0f, i / 60.0f);
            if (i > 60)
                in.touching = false;            // beyond her surface, grip still shut
            soft_contact(sp, hand, in);
        }
        CHECK(hand.holding && hand.grabbing);
        CHECK(near(sp.press[0][0], 0.03f * metre, 1e-3f));
        CHECK(sp.grab_time[0] == now);
        in.gripping = false;
        now += dt;
        in.now_seconds = now;
        soft_contact(sp, hand, in);
        CHECK(!hand.holding && !hand.grabbing);
    }
}
