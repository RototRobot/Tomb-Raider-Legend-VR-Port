// Tests for src/vr/god_pluck (included by mathtest.cpp).
#include "../vr/godmode/god_pluck.h"

static void test_god_pluck()
{
    using namespace trlvr;
    PluckParams params;
    params.metre = 100.0f;
    params.table = 400.0f;      // radius 30 + 16 = 46, z from -12 to 196

    auto frame = [](float x, float y, float z, bool grip, int hand) {
        PluckInput in;
        in.dt = 1.0f / 60.0f;
        in.root[0] = 0.0f; in.root[1] = 0.0f; in.root[2] = 0.0f;
        for (int h = 0; h < 2; ++h)
        {
            in.point_valid[h] = true;
            in.point[h][0] = 1000.0f;       // far away unless it is `hand`
            in.grip[h] = false;
        }
        in.point[hand][0] = x; in.point[hand][1] = y; in.point[hand][2] = z;
        in.grip[hand] = grip;
        return in;
    };

    // Outside the zone: no pick-up, however it is gripped (sides, above, below).
    {
        PluckState s; PluckEvents ev;
        const float out[4][3] = { { 50, 0, 50 }, { 0, 0, 200 }, { 0, 0, -20 },
                                  { 0, 47, 10 } };
        for (const auto& o : out)
        {
            pluck_step(s, params, frame(o[0], o[1], o[2], false, 1), &ev);
            pluck_step(s, params, frame(o[0], o[1], o[2], true, 1), &ev);
            CHECK(!ev.picked_up);
            CHECK(s.hand == -1);
            pluck_step(s, params, frame(o[0], o[1], o[2], false, 1), &ev);
        }
        // The edges of the zone are inside.
        pluck_step(s, params, frame(46, 0, -12, false, 1), &ev);
        pluck_step(s, params, frame(46, 0, 196, true, 1), &ev);
        CHECK(ev.picked_up);
    }

    // Head only: a grip at her middle does nothing; at her head (radius
    // 0.10 m x 100 + 0.01 x 400 = 14) it picks her up; no head, no pick-up.
    {
        PluckParams hp = params;
        hp.head_only = true;
        CHECK(near(pluck_head_radius(hp), 14.0f));
        auto at = [&](float x, float z, bool grip, bool head_valid) {
            PluckInput in = frame(x, 0, z, grip, 1);
            in.head_valid = head_valid;
            in.head[2] = 170.0f;
            return in;
        };
        PluckState s; PluckEvents ev;
        pluck_step(s, hp, at(0, 100, false, true), &ev);
        pluck_step(s, hp, at(0, 100, true, true), &ev);
        CHECK(!ev.picked_up && s.hand == -1);
        pluck_step(s, hp, at(0, 100, false, true), &ev);
        pluck_step(s, hp, at(10, 165, false, false), &ev);
        pluck_step(s, hp, at(10, 165, true, false), &ev);
        CHECK(!ev.picked_up && s.hand == -1);
        pluck_step(s, hp, at(10, 165, false, true), &ev);
        CHECK(s.inside[1]);
        pluck_step(s, hp, at(10, 165, true, true), &ev);
        CHECK(ev.picked_up && s.hand == 1);
    }

    // Near her: a capsule from feet (0,0,0) to head top (0,0,180), reach
    // 0.6 m x 100 + 0.03 x 400 = 72.
    {
        const float feet[3] = { 0, 0, 0 }, top[3] = { 0, 0, 180 };
        const float in1[3] = { 70, 0, 90 }, out1[3] = { 74, 0, 90 };
        const float above[3] = { 0, 0, 250 }, far_above[3] = { 0, 0, 260 };
        CHECK(pluck_near(params, feet, top, in1, 0.6f));
        CHECK(!pluck_near(params, feet, top, out1, 0.6f));
        CHECK(pluck_near(params, feet, top, above, 0.6f));
        CHECK(!pluck_near(params, feet, top, far_above, 0.6f));
        PluckParams wide = params;
        wide.head_radius = 0.3f;
        CHECK(near(pluck_head_radius(wide), 34.0f));
    }

    // A grip held before entering does not pick up; a hover event fires.
    {
        PluckState s; PluckEvents ev;
        pluck_step(s, params, frame(200, 0, 50, true, 1), &ev);
        pluck_step(s, params, frame(10, 0, 50, true, 1), &ev);
        CHECK(ev.hover_enter[1]);
        CHECK(!ev.picked_up && s.hand == -1);
        pluck_step(s, params, frame(10, 0, 50, true, 1), &ev);
        CHECK(!ev.picked_up && s.hand == -1);
        CHECK(s.hover);
    }

    // Press inside picks up with that hand; grab offset = root - point.
    {
        PluckState s; PluckEvents ev;
        pluck_step(s, params, frame(10, -5, 50, false, 0), &ev);
        pluck_step(s, params, frame(10, -5, 50, true, 0), &ev);
        CHECK(ev.picked_up && ev.hand == 0);
        CHECK(s.hand == 0);
        CHECK(near(s.grab_offset[0], -10.0f) && near(s.grab_offset[1], 5.0f) &&
              near(s.grab_offset[2], -50.0f));

        // The other hand cannot take her while held.
        PluckInput in = frame(10, -5, 50, true, 0);
        in.point[1][0] = 5; in.point[1][1] = 0; in.point[1][2] = 40;
        in.grip[1] = false;
        pluck_step(s, params, in, &ev);
        in.grip[1] = true;
        pluck_step(s, params, in, &ev);
        CHECK(!ev.picked_up && s.hand == 0);

        // Still hand: tilts ~0 and target = point + grab offset.
        for (int i = 0; i < 120; ++i)
            pluck_step(s, params, frame(10, -5, 50, true, 0), &ev);
        CHECK(near(s.tilt[0], 0.0f, 1e-3f) && near(s.tilt[1], 0.0f, 1e-3f));
        CHECK(near(s.target[0], 0.0f) && near(s.target[1], 0.0f) &&
              near(s.target[2], 0.0f));
        CHECK(near(s.carry[0], 0.0f) && near(s.carry[2], 0.0f));
    }

    // part_held blocks the pick-up.
    {
        PluckState s; PluckEvents ev;
        pluck_step(s, params, frame(10, 0, 50, false, 1), &ev);
        PluckInput in = frame(10, 0, 50, true, 1);
        in.part_held[1] = true;
        pluck_step(s, params, in, &ev);
        CHECK(!ev.picked_up && s.hand == -1);
    }

    // Swing: accelerating +x swings her to -x first, then she settles; the
    // limbs lag; tilts stay within 1.2.
    {
        PluckState s; PluckEvents ev;
        float x = 10.0f;
        pluck_step(s, params, frame(x, 0, 50, false, 1), &ev);
        pluck_step(s, params, frame(x, 0, 50, true, 1), &ev);
        CHECK(s.hand == 1);
        for (int i = 0; i < 30; ++i)
            pluck_step(s, params, frame(x, 0, 50, true, 1), &ev);
        const float speed = 400.0f / 60.0f;     // 1 table-m/s per frame
        float min_tilt = 0.0f, max_tilt = 0.0f;
        bool lag_ok = false;
        for (int i = 0; i < 20; ++i)
        {
            x += speed;
            pluck_step(s, params, frame(x, 0, 50, true, 1), &ev);
            if (s.tilt[0] < min_tilt) min_tilt = s.tilt[0];
            if (s.tilt[0] > max_tilt) max_tilt = s.tilt[0];
            if (i == 2)
                lag_ok = fabsf(s.limb_tilt[2][0]) < fabsf(s.tilt[0]) &&
                         fabsf(s.limb_tilt[0][0]) < fabsf(s.tilt[0]) &&
                         fabsf(s.limb_tilt[2][0]) < fabsf(s.limb_tilt[0][0]);
            CHECK(fabsf(s.tilt[0]) <= 1.2f && fabsf(s.tilt[1]) <= 1.2f);
        }
        CHECK(min_tilt < -0.05f);           // opposite to the acceleration
        CHECK(lag_ok);
        // The body direction leans to -x: straight down turned by the rotation.
        {
            const Mat4& r = s.body_rotation;
            float v[3];
            for (int j = 0; j < 3; ++j) v[j] = -r.m[2][j];
            CHECK(near(v[0], sinf(s.tilt[0]), 1e-3f));
            CHECK(near(v[2], -sqrtf(1.0f - v[0]*v[0] - v[1]*v[1]), 1e-3f));
        }
        // Stop: constant-speed motion decays, still hand settles.
        for (int i = 0; i < 60 * 5; ++i)
            pluck_step(s, params, frame(x, 0, 50, true, 1), &ev);
        CHECK(fabsf(s.tilt[0]) < 0.01f && fabsf(s.tilt[1]) < 0.01f);
        CHECK(fabsf(s.tilt_vel[0]) < 0.05f);
    }

    // A violent jerk is clamped.
    {
        PluckState s; PluckEvents ev;
        pluck_step(s, params, frame(10, 0, 50, false, 1), &ev);
        pluck_step(s, params, frame(10, 0, 50, true, 1), &ev);
        float x = 10.0f;
        for (int i = 0; i < 200; ++i)
        {
            x += (i % 4 < 2) ? 80.0f : -80.0f;
            pluck_step(s, params, frame(x, 0, 50, true, 1), &ev);
            CHECK(fabsf(s.tilt[0]) <= 1.2f + 1e-6f);
            CHECK(fabsf(s.tilt[1]) <= 1.2f + 1e-6f);
        }
    }

    // dt outside (0, 0.1]: no physics step.
    {
        PluckState s; PluckEvents ev;
        pluck_step(s, params, frame(10, 0, 50, false, 1), &ev);
        pluck_step(s, params, frame(10, 0, 50, true, 1), &ev);
        PluckInput in = frame(40, 0, 50, true, 1);
        in.dt = 0.0f;
        pluck_step(s, params, in, &ev);
        in.dt = 0.5f;
        pluck_step(s, params, in, &ev);
        CHECK(s.tilt[0] == 0.0f && s.tilt_vel[0] == 0.0f);
        CHECK(near(s.target[0], 30.0f));        // she still follows the hand
    }

    // Release: with a floor -> dropped_to_floor; without -> not.
    for (int with_floor = 0; with_floor < 2; ++with_floor)
    {
        PluckState s; PluckEvents ev;
        pluck_step(s, params, frame(10, 0, 50, false, 1), &ev);
        pluck_step(s, params, frame(10, 0, 50, true, 1), &ev);
        PluckInput in = frame(60, 20, 150, true, 1);
        in.floor_valid = with_floor != 0;
        in.floor[2] = 3.0f;
        pluck_step(s, params, in, &ev);
        CHECK(!ev.released && s.hand == 1);
        in.grip[1] = false;
        pluck_step(s, params, in, &ev);
        CHECK(ev.released && ev.hand == 1);
        CHECK(ev.dropped_to_floor == (with_floor != 0));
        CHECK(near(ev.target[0], 50.0f) && near(ev.target[1], 20.0f) &&
              near(ev.target[2], 100.0f));
        CHECK(s.hand == -1);
        CHECK(s.carry[0] == 0.0f && s.carry[1] == 0.0f && s.carry[2] == 0.0f);
        // She can be picked up again.
        pluck_step(s, params, frame(10, 0, 50, true, 1), &ev);
        CHECK(ev.picked_up);
    }

    // pluck_reset while held clears; the old grip is not a new press.
    {
        PluckState s; PluckEvents ev;
        pluck_step(s, params, frame(10, 0, 50, false, 1), &ev);
        pluck_step(s, params, frame(30, 0, 50, true, 1), &ev);
        pluck_step(s, params, frame(60, 0, 50, true, 1), &ev);
        CHECK(s.hand == 1 && s.carry[0] != 0.0f);
        pluck_reset(s);
        CHECK(s.hand == -1);
        CHECK(s.carry[0] == 0.0f && s.carry[1] == 0.0f && s.carry[2] == 0.0f);
        pluck_step(s, params, frame(10, 0, 50, true, 1), &ev);
        CHECK(ev.picked_up);                    // fresh press after a reset
        // Disabled: dropped, and a held grip is not a press on return.
        params.enabled = false;
        pluck_step(s, params, frame(10, 0, 50, true, 1), &ev);
        CHECK(s.hand == -1);
        params.enabled = true;
        pluck_step(s, params, frame(10, 0, 50, true, 1), &ev);
        CHECK(!ev.picked_up);
    }

    // Body matrix: the grab point lands on the hang point, with a tilt and
    // with a wrist pivot; rotation turns straight down into the body direction.
    {
        PluckState s; PluckEvents ev;
        pluck_step(s, params, frame(10, 0, 50, false, 1), &ev);
        pluck_step(s, params, frame(10, 0, 50, true, 1), &ev);
        float x = 10.0f;
        for (int i = 0; i < 15; ++i)
        {
            x += 6.0f;
            pluck_step(s, params, frame(x, 3, 50, true, 1), &ev);
        }
        CHECK(fabsf(s.tilt[0]) > 0.02f);
        PluckInput in = frame(x, 3, 50, true, 1);
        for (int pass = 0; pass < 2; ++pass)
        {
            in.wrist_pivot_valid = pass == 1;
            in.wrist_pivot[0] = 4.0f; in.wrist_pivot[1] = 2.0f;
            in.wrist_pivot[2] = 30.0f;
            const Mat4 m = pluck_body_matrix(s, in);
            float g[3];
            for (int k = 0; k < 3; ++k)
                g[k] = pass ? in.wrist_pivot[k] : in.root[k] - s.grab_offset[k];
            float o[3];
            for (int j = 0; j < 3; ++j)
                o[j] = g[0]*m.m[0][j] + g[1]*m.m[1][j] + g[2]*m.m[2][j] +
                       m.m[3][j];
            CHECK(near(o[0], s.hang[0], 1e-3f) && near(o[1], s.hang[1], 1e-3f) &&
                  near(o[2], s.hang[2], 1e-3f));
        }
        // down * body_rotation == body direction
        const float tx = s.tilt[0], ty = s.tilt[1];
        const float bx = sinf(tx), by = sinf(ty);
        const float bz = -sqrtf(1.0f - bx*bx - by*by);
        const Mat4& r = s.body_rotation;
        CHECK(near(-r.m[2][0], bx, 1e-3f));
        CHECK(near(-r.m[2][1], by, 1e-3f));
        CHECK(near(-r.m[2][2], bz, 1e-3f));
    }

    // Grace: a grip closing just after the hand left the zone (the pinch
    // point sinks into the palm) still picks her up; later, it does not.
    for (int late = 0; late < 2; ++late)
    {
        PluckState s; PluckEvents ev;
        pluck_step(s, params, frame(10, 0, 50, false, 1), &ev);
        const int frames = late ? 18 : 6;       // 0.3 s / 0.1 s out
        for (int i = 0; i < frames; ++i)
            pluck_step(s, params, frame(60, 0, 50, false, 1), &ev);
        pluck_step(s, params, frame(60, 0, 50, true, 1), &ev);
        CHECK(ev.picked_up == !late);
    }

    // Settle: for 0.1 s after the grab the pinch moving (fingers closing)
    // does not move her; after it, she follows the hand again.
    {
        PluckParams sp = params;
        sp.settle_seconds = 0.1f;
        PluckState s; PluckEvents ev;
        pluck_step(s, sp, frame(10, 0, 50, false, 1), &ev);
        pluck_step(s, sp, frame(10, 0, 50, true, 1), &ev);
        CHECK(ev.picked_up);
        for (int i = 1; i <= 4; ++i)
        {
            pluck_step(s, sp, frame(10 - 5.0f * i, 0, 50, true, 1), &ev);
            CHECK(near(s.target[0], 0.0f) && near(s.carry[0], 0.0f));
            CHECK(s.hand_vel[0] == 0.0f);
        }
        for (int i = 0; i < 4; ++i)
            pluck_step(s, sp, frame(-10, 0, 50, true, 1), &ev);
        CHECK(s.settle <= 0.0f && near(s.target[0], 0.0f));
        pluck_step(s, sp, frame(0, 0, 50, true, 1), &ev);
        CHECK(near(s.target[0], 10.0f));
    }

    // Segment distance.
    {
        const float a[3] = { 0, 0, 0 }, b[3] = { 10, 0, 0 };
        const float p1[3] = { 5, 3, 0 }, p2[3] = { -4, 0, 3 }, p3[3] = { 13, 4, 0 };
        CHECK(near(pluck_segment_distance(a, b, p1), 3.0f));
        CHECK(near(pluck_segment_distance(a, b, p2), 5.0f));
        CHECK(near(pluck_segment_distance(a, b, p3), 5.0f));
    }

    // Arm zones: head ball at (0,0,170) radius 14; arm 0 from the shoulder
    // (20,0,140) to the fingertips (60,0,100), wrist (60,0,100), reach 60,
    // raised wrist (20,0,200); radius 0.05 x 100 + 0.01 x 400 = 9.
    {
        PluckParams ap = params;
        ap.head_only = true;
        ap.arm_radius = 0.05f;
        auto at = [&](float x, float y, float z, bool grip) {
            PluckInput in = frame(x, y, z, grip, 1);
            in.head_valid = true;
            in.head[2] = 170.0f;
            in.arm_valid[0] = true;
            const float sh[3] = { 20, 0, 140 }, wr[3] = { 60, 0, 100 }, pv[3] = { 20, 0, 200 };
            for (int k = 0; k < 3; ++k)
            {
                in.arm_shoulder[0][k] = sh[k];
                in.arm_end[0][k] = wr[k];
                in.arm_wrist[0][k] = wr[k];
                in.arm_pivot[0][k] = pv[k];
            }
            in.arm_reach[0] = 60.0f;
            return in;
        };
        // Off the arm (10 from it): nothing; on it: arm 0, she is not lifted.
        {
            PluckState s; PluckEvents ev;
            pluck_step(s, ap, at(47, 0, 127, false), &ev);
            pluck_step(s, ap, at(47.1f, 0, 127.1f, true), &ev);
            CHECK(!ev.picked_up);
        }
        {
            PluckState s; PluckEvents ev;
            pluck_step(s, ap, at(40, 0, 125, false), &ev);
            CHECK(s.inside[1]);
            pluck_step(s, ap, at(40, 0, 125, true), &ev);
            CHECK(ev.picked_up && ev.part == kPluckArm0 && s.part == kPluckArm0);
            CHECK(!s.lifted && !pluck_drawn(s));
            CHECK(near(s.arm_offset[0], 20.0f) && near(s.arm_offset[2], -25.0f));
            // The arm is nearly straight (56.6 of 60): 5 cm past that lifts.
            CHECK(near(s.lead_lift, sqrtf(3200.0f) + 5.0f, 0.01f));
            // A small pull: only the arm follows (wrist = pinch + offset).
            pluck_step(s, ap, at(20, 0, 150, true), &ev);
            CHECK(!s.lifted && !ev.lifted);
            CHECK(near(s.lead_target[0], 40.0f) && near(s.lead_target[2], 125.0f));
            CHECK(near(s.target[0], 0.0f) && near(s.target[2], 0.0f));
            // Let go before she leaves the ground: released, not moved.
            pluck_step(s, ap, at(20, 0, 150, false), &ev);
            CHECK(ev.released && !ev.moved && !ev.thrown && s.hand == -1);
        }
        // Pulled past the reach: lifted, starting along the arm's line.
        {
            PluckState s; PluckEvents ev;
            pluck_step(s, ap, at(40, 0, 125, false), &ev);
            pluck_step(s, ap, at(40, 0, 125, true), &ev);
            // Up and left over a second, to (-20,0,235): the wrist to (0,0,210).
            int lifts = 0;
            for (int i = 1; i <= 60; ++i)
            {
                const float f = i / 60.0f;
                pluck_step(s, ap, at(40 - 60 * f, 0, 125 + 110 * f, true), &ev);
                if (!ev.lifted)
                    continue;
                ++lifts;
                const float dx = s.hang[0] - 20.0f, dz = s.hang[2] - 140.0f;
                CHECK(sqrtf(dx * dx + dz * dz) > 60.0f);
                CHECK(near(s.tilt[0], asinf(-dx / sqrtf(dx * dx + dz * dz)), 0.05f));
            }
            CHECK(lifts == 1 && s.lifted && pluck_drawn(s));
            // Her feet hang below the raised wrist: hang + root - pivot.
            CHECK(near(s.hang[0], 0.0f) && near(s.hang[2], 210.0f));
            CHECK(near(s.target[0], -20.0f) && near(s.target[2], 10.0f));
            // The body matrix puts the raised wrist on the hang point.
            const PluckInput in = at(-20, 0, 235, true);
            const Mat4 m = pluck_body_matrix(s, in);
            const float* g = in.arm_pivot[0];
            float o[3];
            for (int j = 0; j < 3; ++j)
                o[j] = g[0]*m.m[0][j] + g[1]*m.m[1][j] + g[2]*m.m[2][j] + m.m[3][j];
            CHECK(near(o[0], 0.0f, 1e-3f) && near(o[2], 210.0f, 1e-3f));
            // She swings down under the hand.
            for (int i = 0; i < 60 * 5; ++i)
                pluck_step(s, ap, at(-20, 0, 235, true), &ev);
            CHECK(fabsf(s.tilt[0]) < 0.01f);
            pluck_step(s, ap, at(-20, 0, 235, false), &ev);
            CHECK(ev.released && ev.moved && s.hand == -1);
        }
        // No arm lead: lifted at the grab. Arm zones off: the arm is no zone.
        {
            PluckParams np = ap;
            np.arm_lead = false;
            PluckState s; PluckEvents ev;
            pluck_step(s, np, at(40, 0, 125, false), &ev);
            pluck_step(s, np, at(40, 0, 125, true), &ev);
            CHECK(ev.picked_up && s.lifted && pluck_drawn(s));
            np.arm_radius = 0.0f;
            PluckState s2;
            pluck_step(s2, np, at(40, 0, 125, false), &ev);
            pluck_step(s2, np, at(40, 0, 125, true), &ev);
            CHECK(!ev.picked_up);
        }
        // Near the head (12.8 of 14 from it, 23 from the arm): the head.
        {
            PluckState s; PluckEvents ev;
            pluck_step(s, ap, at(8, 0, 160, false), &ev);
            pluck_step(s, ap, at(8, 0, 160, true), &ev);
            CHECK(ev.picked_up && ev.part == kPluckHead && s.lifted);
        }
    }

    // Throwing: a fast release flies, a slow one is put down.
    {
        PluckParams tp = params;
        tp.throw_enabled = true;
        const float dt = 1.0f / 60.0f;
        auto hold_and_move = [&](PluckState& s, float speed_mps, int frames, float* x) {
            PluckEvents ev;
            pluck_step(s, tp, frame(*x, 0, 50, false, 1), &ev);
            pluck_step(s, tp, frame(*x, 0, 50, true, 1), &ev);
            for (int i = 0; i < frames; ++i)
            {
                *x += speed_mps * tp.table * dt;
                pluck_step(s, tp, frame(*x, 0, 50, true, 1), &ev);
            }
        };
        {
            PluckState s; PluckEvents ev;
            float x = 10.0f;
            hold_and_move(s, 0.2f, 30, &x);
            pluck_step(s, tp, frame(x, 0, 50, false, 1), &ev);
            CHECK(ev.released && !ev.thrown && !s.flying);
        }
        {
            PluckState s; PluckEvents ev;
            float x = 10.0f;
            hold_and_move(s, 2.0f, 30, &x);
            // The hand stops for two frames as the fingers open: the peak counts.
            pluck_step(s, tp, frame(x, 0, 50, true, 1), &ev);
            pluck_step(s, tp, frame(x, 0, 50, true, 1), &ev);
            const float feet_x = s.target[0];
            pluck_step(s, tp, frame(x, 0, 50, false, 1), &ev);
            CHECK(ev.released && ev.thrown && ev.moved);
            CHECK(near(ev.throw_speed, 2.0f, 0.1f));
            CHECK(s.flying && s.hand == -1 && pluck_drawn(s));
            CHECK(near(s.fly_vel[0], ev.throw_speed * tp.table, 1.0f));
            // Flight: x grows at the throw speed, z falls under 9.81 m/s^2.
            const float vx = s.fly_vel[0], z0 = s.target[2];
            int frames = 0;
            for (; frames < 20; ++frames)
            {
                pluck_step(s, tp, frame(x, 0, 50, false, 1), &ev);
                CHECK(!ev.flight_over);
            }
            const float t = frames * dt;
            CHECK(near(s.target[0], feet_x + vx * t, 1.0f));
            CHECK(near(s.fly_vel[2], -9.81f * tp.table * t, 1.0f));
            CHECK(s.target[2] < z0 - 0.5f * 9.81f * tp.table * t * t * 0.9f);
            CHECK(near(s.fly_prev[0], s.target[0] - vx * dt, 1.0f));
            // A wall: moved back, sent back at a quarter of the speed; the
            // drawn offset (hang - feet) is kept. A ceiling turns the rise.
            {
                PluckState w = s;
                const float off = w.hang[2] - w.target[2];
                const float back[3] = { w.target[0] - 5.0f, 0.0f, w.target[2] };
                const float vz = w.fly_vel[2];
                pluck_bounce(w, back, 0.25f, false);
                CHECK(near(w.target[0], back[0]) && near(w.fly_prev[0], back[0]));
                CHECK(near(w.hang[2] - w.target[2], off));
                CHECK(near(w.fly_vel[0], -0.25f * vx, 1.0f) && near(w.fly_vel[2], vz));
                w.fly_vel[2] = 100.0f;
                pluck_bounce(w, back, 0.25f, true);
                CHECK(near(w.fly_vel[2], -25.0f));
            }
            // No catching her in flight: a press in the zone does nothing.
            pluck_step(s, tp, frame(10, 0, 50, true, 1), &ev);
            CHECK(!ev.picked_up && s.flying);
            // The flight ends after throw_max_time.
            bool over = false;
            for (int i = 0; i < 200 && !over; ++i)
            {
                pluck_step(s, tp, frame(10, 0, 50, true, 1), &ev);
                over = ev.flight_over;
            }
            CHECK(over && near(s.fly_time, tp.throw_max_time, 0.02f));
            pluck_land(s);
            CHECK(!s.flying && !pluck_drawn(s) && s.part == kPluckNone);
            // Picked up again with a fresh press.
            pluck_step(s, tp, frame(10, 0, 50, false, 1), &ev);
            pluck_step(s, tp, frame(10, 0, 50, true, 1), &ev);
            CHECK(ev.picked_up);
        }
        // A one-frame jump of the hand (tracking, a script ending) is no
        // throw; a very fast swing is capped at throw_max_speed.
        {
            PluckState s; PluckEvents ev;
            float x = 10.0f;
            hold_and_move(s, 0.0f, 20, &x);
            pluck_step(s, tp, frame(x + 200.0f, 0, -100, true, 1), &ev);   // ~40 m/s
            pluck_step(s, tp, frame(x + 200.0f, 0, -100, false, 1), &ev);
            CHECK(ev.released && !ev.thrown);
        }
        {
            PluckState s; PluckEvents ev;
            float x = 10.0f;
            hold_and_move(s, 10.0f, 20, &x);
            pluck_step(s, tp, frame(x, 0, 50, false, 1), &ev);
            CHECK(ev.thrown && near(ev.throw_speed, tp.throw_max_speed, 0.01f));
            CHECK(near(s.fly_vel[0], tp.throw_max_speed * tp.table, 2.0f));
        }
        // Throwing off: the same release is put down.
        {
            tp.throw_enabled = false;
            PluckState s; PluckEvents ev;
            float x = 10.0f;
            hold_and_move(s, 2.0f, 30, &x);
            pluck_step(s, tp, frame(x, 0, 50, false, 1), &ev);
            CHECK(ev.released && !ev.thrown && !s.flying);
        }
    }
}
