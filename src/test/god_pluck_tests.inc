// Tests for src/vr/god_pluck (included by mathtest.cpp).
#include "../vr/godmode/god_pluck.h"

static void test_god_pluck()
{
    using namespace trlvr;
    PluckParams params;
    params.metre = 100.0f;
    params.table = 400.0f;      // radius 30 + 4 = 34, z from -3 to 184 (margins at her size)

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
                                  { 0, 35, 10 } };
        for (const auto& o : out)
        {
            pluck_step(s, params, frame(o[0], o[1], o[2], false, 1), &ev);
            pluck_step(s, params, frame(o[0], o[1], o[2], true, 1), &ev);
            CHECK(!ev.picked_up);
            CHECK(s.hand == -1);
            pluck_step(s, params, frame(o[0], o[1], o[2], false, 1), &ev);
        }
        // The edges of the zone are inside.
        pluck_step(s, params, frame(34, 0, -3, false, 1), &ev);
        pluck_step(s, params, frame(34, 0, 184, true, 1), &ev);
        CHECK(ev.picked_up);
    }

    // Head only: a grip at her middle does nothing; at her head (radius
    // 0.10 m x 100 + 0.01 x 100 = 11) it picks her up; no head, no pick-up.
    {
        PluckParams hp = params;
        hp.head_only = true;
        CHECK(near(pluck_head_radius(hp), 11.0f));
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
        pluck_step(s, hp, at(8, 165, false, false), &ev);
        pluck_step(s, hp, at(8, 165, true, false), &ev);
        CHECK(!ev.picked_up && s.hand == -1);
        pluck_step(s, hp, at(8, 165, false, true), &ev);
        CHECK(s.inside[1]);
        pluck_step(s, hp, at(8, 165, true, true), &ev);
        CHECK(ev.picked_up && s.hand == 1);
    }

    // Near her: a capsule from feet (0,0,0) to head top (0,0,180), reach
    // 0.6 m x 100 + 0.03 x 100 = 63.
    {
        const float feet[3] = { 0, 0, 0 }, top[3] = { 0, 0, 180 };
        const float in1[3] = { 62, 0, 90 }, out1[3] = { 64, 0, 90 };
        const float above[3] = { 0, 0, 242 }, far_above[3] = { 0, 0, 250 };
        CHECK(pluck_near(params, feet, top, in1, 0.6f));
        CHECK(!pluck_near(params, feet, top, out1, 0.6f));
        CHECK(pluck_near(params, feet, top, above, 0.6f));
        CHECK(!pluck_near(params, feet, top, far_above, 0.6f));
        PluckParams wide = params;
        wide.head_radius = 0.3f;
        CHECK(near(pluck_head_radius(wide), 31.0f));
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
            // The legs (hips near her centre of swing, barely pushed by the
            // hand) lag the body as it starts to swing.
            if (i == 2)
                lag_ok = fabsf(s.limb_tilt[2][0]) < fabsf(s.tilt[0]) &&
                         fabsf(s.limb_tilt[3][0]) < fabsf(s.tilt[0]);
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
        // down * body_rotation == body direction: a turn of |tilt| toward
        // the tilt (sin tx for a tilt about one axis).
        const float tx = s.tilt[0], ty = s.tilt[1];
        const float phi = sqrtf(tx * tx + ty * ty);
        const float bx = tx / phi * sinf(phi), by = ty / phi * sinf(phi);
        const float bz = -cosf(phi);
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

    // Arm zones: head ball at (0,0,170) radius 11; arm 0 from the shoulder
    // (20,0,140), wrist (60,0,100), the hand on to the fingertips (70,0,90), reach 60,
    // raised wrist (20,0,200); the zone is the hand only, radius 0.05 x 100 + 0.01 x 100 = 6.
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
            const float en[3] = { 70, 0, 90 };
            for (int k = 0; k < 3; ++k)
            {
                in.arm_shoulder[0][k] = sh[k];
                in.arm_end[0][k] = en[k];
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
            pluck_step(s, ap, at(65, 0, 95, false), &ev);
            CHECK(s.inside[1]);
            pluck_step(s, ap, at(65, 0, 95, true), &ev);
            CHECK(ev.picked_up && ev.part == kPluckArm0 && s.part == kPluckArm0);
            CHECK(!s.lifted && !pluck_drawn(s));
            CHECK(near(s.arm_offset[0], -5.0f) && near(s.arm_offset[2], 5.0f));
            // The arm is nearly straight (56.6 of 60): 5 cm past that lifts.
            CHECK(near(s.lead_lift, sqrtf(3200.0f) + 5.0f, 0.01f));
            // A small pull: only the arm follows (wrist = pinch + offset).
            pluck_step(s, ap, at(45, 0, 120, true), &ev);
            CHECK(!s.lifted && !ev.lifted);
            CHECK(near(s.lead_target[0], 40.0f) && near(s.lead_target[2], 125.0f));
            CHECK(near(s.target[0], 0.0f) && near(s.target[2], 0.0f));
            // Let go before she leaves the ground: released, not moved.
            pluck_step(s, ap, at(45, 0, 120, false), &ev);
            CHECK(ev.released && !ev.moved && !ev.thrown && s.hand == -1);
        }
        // Pulled past the reach: lifted, starting along the arm's line.
        {
            PluckState s; PluckEvents ev;
            pluck_step(s, ap, at(65, 0, 95, false), &ev);
            pluck_step(s, ap, at(65, 0, 95, true), &ev);
            // Up and left over a second, to (-20,0,235): the wrist to (0,0,210).
            int lifts = 0;
            for (int i = 1; i <= 60; ++i)
            {
                const float f = i / 60.0f;
                pluck_step(s, ap, at(65 - 60 * f, 0, 95 + 110 * f, true), &ev);
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
            const PluckInput in = at(5, 0, 205, true);
            const Mat4 m = pluck_body_matrix(s, in);
            const float* g = in.arm_pivot[0];
            float o[3];
            for (int j = 0; j < 3; ++j)
                o[j] = g[0]*m.m[0][j] + g[1]*m.m[1][j] + g[2]*m.m[2][j] + m.m[3][j];
            CHECK(near(o[0], 0.0f, 1e-3f) && near(o[2], 210.0f, 1e-3f));
            // She swings down under the hand.
            for (int i = 0; i < 60 * 5; ++i)
                pluck_step(s, ap, at(5, 0, 205, true), &ev);
            CHECK(fabsf(s.tilt[0]) < 0.01f);
            pluck_step(s, ap, at(5, 0, 205, false), &ev);
            CHECK(ev.released && ev.moved && s.hand == -1);
        }
        // No arm lead: lifted at the grab. Arm zones off: the arm is no zone.
        {
            PluckParams np = ap;
            np.arm_lead = false;
            PluckState s; PluckEvents ev;
            pluck_step(s, np, at(65, 0, 95, false), &ev);
            pluck_step(s, np, at(65, 0, 95, true), &ev);
            CHECK(ev.picked_up && s.lifted && pluck_drawn(s));
            np.arm_radius = 0.0f;
            PluckState s2;
            pluck_step(s2, np, at(65, 0, 95, false), &ev);
            pluck_step(s2, np, at(65, 0, 95, true), &ev);
            CHECK(!ev.picked_up);
        }
        // Near the head (9.2 of 11 from it, far from the arm): the head.
        {
            PluckState s; PluckEvents ev;
            pluck_step(s, ap, at(6, 0, 163, false), &ev);
            pluck_step(s, ap, at(6, 0, 163, true), &ev);
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
            pluck_step(s, tp, frame(x, 0, 50, false, 1), &ev);
            CHECK(ev.released && ev.thrown && ev.moved);
            CHECK(near(ev.throw_speed, 2.0f, 0.1f));
            CHECK(s.flying && s.hand == -1 && pluck_drawn(s));
            CHECK(near(s.fly.vel[0], ev.throw_speed * tp.table, 1.0f));
            // The projectile is her drawn centre, radius a third of her height.
            CHECK(near(s.fly.pos[0], s.hang[0] + s.spin_offset[0], 1e-3f) &&
                  near(s.fly.pos[2], s.hang[2] + s.spin_offset[2], 1e-3f));
            CHECK(near(s.fly.radius, kThrowRadius * tp.metre, 1e-3f));
            // Flight (2026-10-22: the centre, not the feet, which turn with
            // the tumble): x grows at the throw speed, z falls under 9.81 m/s^2.
            const float vx = s.fly.vel[0], x0 = s.fly.pos[0], z0 = s.fly.pos[2];
            const float vz0 = s.fly.vel[2];
            int frames = 0;
            for (; frames < 20; ++frames)
            {
                pluck_step(s, tp, frame(x, 0, 50, false, 1), &ev);
                CHECK(!ev.flight_over);
            }
            const float t = frames * dt;
            CHECK(near(s.fly.pos[0], x0 + vx * t, 1.0f));
            CHECK(near(s.fly.vel[2], vz0 - 9.81f * tp.table * t, 1.0f));
            CHECK(near(s.fly.pos[2], z0 + vz0 * t - 0.5f * 9.81f * tp.table * t * t, 1.0f));
            CHECK(near(s.fly.prev[0], s.fly.pos[0] - vx * dt, 1.0f));
            CHECK(near(s.hang[0] + s.spin_offset[0], s.fly.pos[0], 1e-3f));
            // A wall (normal -x, head on): moved to the given centre, sent
            // back at the restitution; z kept (friction 0 here). A
            // ceiling turns the rise.
            {
                PluckState w = s;
                const float off = w.hang[2] - w.target[2];
                const float back[3] = { w.fly.pos[0] - 5.0f, 0.0f, w.fly.pos[2] };
                const float vz = w.fly.vel[2];
                const float wall_n[3] = { -1, 0, 0 }, ceil_n[3] = { 0, 0, -1 };
                pluck_bounce(w, back, wall_n, 0.25f, 0.0f);
                CHECK(near(w.fly.pos[0], back[0]) && near(w.fly.prev[0], back[0]));
                CHECK(near(w.hang[2] - w.target[2], off));
                CHECK(near(w.fly.vel[0], -0.25f * vx, 1.0f) && near(w.fly.vel[2], vz));
                w.fly.vel[2] = 100.0f;
                pluck_bounce(w, back, ceil_n, 0.25f, 0.0f);
                CHECK(near(w.fly.vel[2], -25.0f));
                // A glancing hit on a wall along x keeps her going along it.
                PluckState gl = s;
                const float side_n[3] = { 0, -1, 0 };
                gl.fly.vel[1] = 0.2f * gl.fly.vel[0];
                pluck_bounce(gl, gl.fly.pos, side_n, 0.3f, 0.5f);
                CHECK(gl.fly.vel[0] > 0.4f * vx && gl.fly.vel[1] < 0.0f);
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
            CHECK(near(s.fly.vel[0], tp.throw_max_speed * tp.table, 2.0f));
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

    // Ragdoll limbs (2026-10-20): held, the legs swing on their own
    // pendulums when the hand stops (past straight down and back, then
    // settling), not quite in step, a little apart, knees a little bent.
    {
        PluckParams rp = params;
        auto rframe = [&](float x, bool grip) {
            PluckInput in = frame(x, 0, 50, grip, 1);
            in.facing[0] = 1.0f;    // she faces +x, her right is -y
            in.right[1] = -1.0f;
            return in;
        };
        auto run = [&](PluckState& s, const PluckParams& pp, float* max_rel, int* crossings,
                       float* max_apart, float* legs_after) {
            PluckEvents ev;
            float x = 10.0f;
            pluck_step(s, pp, rframe(x, false), &ev);
            pluck_step(s, pp, rframe(x, true), &ev);
            CHECK(s.hand == 1);
            for (int i = 0; i < 90; ++i)    // 1.5 s at 1 m/s
            {
                x += 1.0f * pp.table / 60.0f;
                pluck_step(s, pp, rframe(x, true), &ev);
            }
            *max_rel = 0.0f; *crossings = 0; *max_apart = 0.0f;
            float last = s.limb_swing[2][0];
            for (int i = 0; i < 60 * 6; ++i)    // the hand stops (the body settles in ~5 s)
            {
                pluck_step(s, pp, rframe(x, true), &ev);
                const float q = s.limb_swing[2][0];
                if (fabsf(q) > *max_rel) *max_rel = fabsf(q);
                if ((q > 0.002f && last < -0.002f) || (q < -0.002f && last > 0.002f))
                {
                    ++*crossings;
                    last = q;
                }
                else if (fabsf(q) > 0.002f)
                    last = q;
                const float apart = fabsf(s.limb_swing[2][0] - s.limb_swing[3][0]);
                if (apart > *max_apart) *max_apart = apart;
                CHECK(fabsf(s.tilt[0]) <= 1.2f);
            }
            *legs_after = fabsf(s.limb_swing[2][0]) + fabsf(s.limb_swing[3][0]) +
                          fabsf(s.limb_swing_vel[2][0]);
        };
        {
            PluckState s;
            float max_rel, apart, after;
            int crossings;
            run(s, rp, &max_rel, &crossings, &apart, &after);
            printf("  ragdoll held: leg swing peak %.3f rad, %d sign changes, legs apart %.3f\n",
                   max_rel, crossings, apart);
            CHECK(max_rel > 0.03f && max_rel < 0.9f);   // a bit, not wild
            CHECK(crossings >= 2);                      // overshoots, swings back
            CHECK(apart > 0.003f);                      // not in step
            CHECK(after < 0.01f);                       // and settles
            // At rest: legs out to her sides (left +y, right -y here; the
            // idle sway is 0.03 across), the shins hanging back from the
            // knees by knee_bend (more as the thigh is forward).
            CHECK(near(s.limb_tilt[2][1], rp.leg_spread, 0.035f));
            CHECK(near(s.limb_tilt[3][1], -rp.leg_spread, 0.035f));
            for (int leg = 0; leg < 2; ++leg)
            {
                const float fwd = s.limb_tilt[2 + leg][0];
                const float bend = rp.knee_bend + 0.6f * (fwd > 0.0f ? fwd : 0.0f);
                const Mat4& r = s.knee_rotation[leg];
                CHECK(near(-r.m[2][0], -sinf(bend), 0.01f));
                CHECK(near(-r.m[2][2], -cosf(bend), 0.01f));
                CHECK(bend >= rp.knee_bend);
            }
            // A limb's drawn turn takes straight down to its drawn tilt
            // (relative to the body).
            const float q0 = s.limb_tilt[2][0] - s.tilt[0], q1 = s.limb_tilt[2][1] - s.tilt[1];
            const Mat4& lr = s.limb_rotation[2];
            CHECK(near(-lr.m[2][1], sinf(q1), 0.02f) && fabsf(-lr.m[2][0] - sinf(q0)) < 0.02f);
        }
        {
            // ragdoll 0: the limbs turn with the body (idle sway and spread only).
            PluckParams stiff = rp;
            stiff.ragdoll = 0.0f;
            PluckState s;
            float max_rel, apart, after;
            int crossings;
            run(s, stiff, &max_rel, &crossings, &apart, &after);
            CHECK(fabsf(s.limb_tilt[2][0] - s.tilt[0]) <= 0.061f);
        }

        // Thrown: the limbs flail relative to the body, the body tumbles past
        // 1.2 rad, drawn about her centre (no jump at the release), and the
        // flight path is the same with or without the ragdoll.
        PluckParams tp2 = rp;
        tp2.throw_enabled = true;
        PluckState s; PluckEvents ev;
        float x = 10.0f;
        pluck_step(s, tp2, rframe(x, false), &ev);
        pluck_step(s, tp2, rframe(x, true), &ev);
        for (int i = 0; i < 30; ++i)
        {
            x += 2.0f * tp2.table / 60.0f;
            pluck_step(s, tp2, rframe(x, true), &ev);
        }
        PluckState held = s;
        pluck_step(s, tp2, rframe(x, false), &ev);
        CHECK(ev.thrown && s.flying);
        {
            // The release frame: the flight matrix equals the held one.
            PluckState as_held = s;
            as_held.flying = false;
            const PluckInput in = rframe(x, false);
            const Mat4 a = pluck_body_matrix(s, in), b = pluck_body_matrix(as_held, in);
            bool same = true;
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 3; ++c)
                    same = same && near(a.m[r][c], b.m[r][c], 1e-2f);
            CHECK(same);
            // The projectile starts at her drawn centre as held (what is
            // probed is what is drawn).
            float ch[3], cf[3];
            pluck_centre(as_held, in, ch);
            pluck_centre(s, in, cf);
            CHECK(near(ch[0], s.fly.pos[0], 1e-2f) && near(ch[1], s.fly.pos[1], 1e-2f) &&
                  near(ch[2], s.fly.pos[2], 1e-2f) && near(cf[2], s.fly.pos[2]));
        }
        PluckState plain = held;    // the same throw without spin or limbs
        PluckParams tp3 = tp2;
        tp3.throw_spin = 0.0f;
        tp3.ragdoll = 0.0f;
        PluckEvents ev3;
        pluck_step(plain, tp3, rframe(x, false), &ev3);
        float max_tilt = 0.0f, min_q = 1e9f, max_q = -1e9f;
        for (int i = 0; i < 60; ++i)
        {
            pluck_step(s, tp2, rframe(x, false), &ev);
            pluck_step(plain, tp3, rframe(x, false), &ev3);
            if (fabsf(s.tilt[0]) > max_tilt) max_tilt = fabsf(s.tilt[0]);
            const float q = s.limb_swing[0][1];
            if (q < min_q) min_q = q;
            if (q > max_q) max_q = q;
            // The path is her centre's (2026-10-22: the feet turn with her).
            CHECK(near(s.fly.pos[0], plain.fly.pos[0], 1e-3f) &&
                  near(s.fly.pos[2], plain.fly.pos[2], 1e-3f));
            // Her centre stays at hang + spin_offset.
            const PluckInput in = rframe(x, false);
            const Mat4 m = pluck_body_matrix(s, in);
            float g[3], c[3];
            for (int k = 0; k < 3; ++k) g[k] = in.root[k] - s.grab_offset[k];
            for (int k = 0; k < 3; ++k) c[k] = g[k] + 0.5f * (in.root[k] - g[k]);
            float o[3];
            for (int j = 0; j < 3; ++j)
                o[j] = c[0] * m.m[0][j] + c[1] * m.m[1][j] + c[2] * m.m[2][j] + m.m[3][j];
            CHECK(near(o[0], s.hang[0] + s.spin_offset[0], 1e-2f) &&
                  near(o[2], s.hang[2] + s.spin_offset[2], 1e-2f));
        }
        printf("  ragdoll flight: tilt up to %.2f rad, arm swing %.2f..%.2f rad\n",
               max_tilt, min_q, max_q);
        CHECK(max_tilt > 1.2f);
        CHECK(max_q - min_q > 0.1f);    // the arm moves relative to the body
        // Spin from the throw is the tumble; without it, she keeps her swing.
        CHECK(fabsf(plain.tilt[0]) < max_tilt);
        // A bounce knocks the limbs and turns the spin back.
        {
            PluckState w = s;
            const float v0 = w.limb_swing_vel[2][0], spin = w.tilt_vel[0];
            const float back[3] = { w.fly.pos[0] - 5.0f, 0.0f, w.fly.pos[2] };
            const float wall_n[3] = { -1, 0, 0 };
            pluck_bounce(w, back, wall_n, 0.25f, 0.5f);
            CHECK(w.limb_swing_vel[2][0] != v0);
            CHECK(near(w.tilt_vel[0], -0.5f * spin, 1e-3f));
        }
    }

    // Two hands. Her real feet at the origin; arm 0 from the shoulder
    // (20,0,140) to the wrist (60,0,100), arm 1 mirrored (-20,0,140) to
    // (-60,0,100), reach 60, raised wrists (+-20,0,200); arm radius 9.
    // Hand 1 takes arm 0 at its hand (65,0,95) (no arm lead): its wrist target is
    // pinch + (-5,0,5), and the raised wrist hangs there, so the body is
    // moved by (40,0,-100): arm 1 is drawn from (20,0,40) to (-20,0,0).
    {
        PluckParams tp = params;
        tp.head_only = true;
        tp.arm_radius = 0.05f;
        tp.arm_lead = false;
        tp.throw_enabled = true;
        tp.two_hands = true;
        auto tf = [&](const float p1[3], bool g1, const float p0[3], bool g0, float dt) {
            PluckInput in;
            in.dt = dt;
            in.head_valid = true;
            in.head[2] = 170.0f;
            for (int c = 0; c < 2; ++c)
            {
                const float sx = c ? -1.0f : 1.0f;
                in.arm_valid[c] = true;
                const float sh[3] = { 20 * sx, 0, 140 }, wr[3] = { 60 * sx, 0, 100 },
                            en[3] = { 70 * sx, 0, 90 },
                            pv[3] = { 20 * sx, 0, 200 };
                for (int k = 0; k < 3; ++k)
                {
                    in.arm_shoulder[c][k] = sh[k];
                    in.arm_wrist[c][k] = wr[k];
                    in.arm_end[c][k] = en[k];
                    in.arm_pivot[c][k] = pv[k];
                }
                in.arm_reach[c] = 60.0f;
            }
            in.drawn_arm_valid[1] = true;
            const float ds[3] = { 20, 0, 40 }, dw[3] = { -20, 0, 0 }, de[3] = { -20, 0, 10 };
            for (int k = 0; k < 3; ++k)
            {
                in.drawn_shoulder[1][k] = ds[k];
                in.drawn_wrist[1][k] = dw[k];
                in.drawn_end[1][k] = de[k];
                in.point[1][k] = p1[k];
                in.point[0][k] = p0[k];
            }
            in.point_valid[0] = in.point_valid[1] = true;
            in.grip[1] = g1;
            in.grip[0] = g0;
            return in;
        };
        const float dt = 1.0f / 60.0f;
        const float a1[3] = { 65, 0, 95 }, far0[3] = { -1000, 0, 0 }, b0[3] = { -20, 0, 5 };
        auto map = [](const Mat4& m, const float g[3], float o[3]) {
            for (int j = 0; j < 3; ++j)
                o[j] = g[0] * m.m[0][j] + g[1] * m.m[1][j] + g[2] * m.m[2][j] + m.m[3][j];
        };
        auto held_two = [&](PluckState& s, PluckEvents& ev) {
            pluck_step(s, tp, tf(a1, false, far0, false, dt), &ev);
            pluck_step(s, tp, tf(a1, true, far0, false, dt), &ev);
            CHECK(ev.picked_up && s.part == kPluckArm0 && s.lifted);
            pluck_step(s, tp, tf(a1, true, b0, false, dt), &ev);
            CHECK(s.inside[0] && ev.hover_enter[0]);
            pluck_step(s, tp, tf(a1, true, b0, true, dt), &ev);
            CHECK(ev.second_grab && ev.hand == 0 && ev.part == kPluckArm1);
            CHECK(s.hand == 1 && s.hand2 == 0 && s.part2 == kPluckArm1);
            CHECK(near(s.arm_offset2[2], -5.0f) && near(s.arm_offset2[0], 0.0f));
        };
        // Off: the other hand's grip on the drawn arm takes nothing.
        {
            PluckParams off = tp;
            off.two_hands = false;
            PluckState s; PluckEvents ev;
            pluck_step(s, off, tf(a1, false, far0, false, dt), &ev);
            pluck_step(s, off, tf(a1, true, far0, false, dt), &ev);
            pluck_step(s, off, tf(a1, true, b0, false, dt), &ev);
            pluck_step(s, off, tf(a1, true, b0, true, dt), &ev);
            CHECK(!ev.second_grab && s.hand2 == -1 && s.hand == 1);
        }
        PluckState s; PluckEvents ev;
        held_two(s, ev);
        // No jump at the second grab: the blend starts at the one-hand hang.
        CHECK(near(s.hang[0], 60.0f, 1.0f) && near(s.hang[2], 100.0f, 1.0f));
        CHECK(pluck_arm_reaches(s, 0) && pluck_arm_reaches(s, 1));
        float last_target[3] = { s.target[0], s.target[1], s.target[2] };
        float max_step = 0.0f;
        for (int i = 0; i < 60; ++i)
        {
            pluck_step(s, tp, tf(a1, true, b0, true, dt), &ev);
            float d[3];
            for (int k = 0; k < 3; ++k) d[k] = s.target[k] - last_target[k];
            max_step = fmaxf(max_step, sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]));
            for (int k = 0; k < 3; ++k) last_target[k] = s.target[k];
        }
        // Wrists (60,0,100) and (-20,0,0), 80 apart, shoulders 40: each arm
        // reaches 20 out, sqrt(60^2 - 20^2) = 56.57 up, from the higher hand:
        // her shoulder midpoint at (20, 0, 43.43).
        CHECK(near(pluck_two_weight(s), 1.0f));
        CHECK(near(s.hang[0], 20.0f, 0.01f) && near(s.hang[1], 0.0f, 0.01f) &&
              near(s.hang[2], 100.0f - sqrtf(3200.0f), 0.01f));
        CHECK(max_step < 4.0f);     // eased: 40 x 57 moved over 0.25 s
        CHECK(fabsf(s.yaw) < 0.01f);
        {
            const PluckInput in = tf(a1, true, b0, true, dt);
            const float mid[3] = { 0, 0, 140 };
            float o[3];
            map(pluck_body_matrix(s, in), mid, o);
            CHECK(near(o[0], s.hang[0], 1e-2f) && near(o[2], s.hang[2], 1e-2f));
        }
        // Hand 0 moves round to (60, 80, 105) over a second: wrist (60,80,100),
        // level with the other; her shoulders turn to follow (yaw -pi/2).
        float p0[3] = { b0[0], b0[1], b0[2] };
        for (int i = 1; i <= 60; ++i)
        {
            const float f = i / 60.0f;
            p0[0] = -20 + 80 * f; p0[1] = 80 * f; p0[2] = 5 + 100 * f;
            pluck_step(s, tp, tf(a1, true, p0, true, dt), &ev);
        }
        for (int i = 0; i < 120; ++i)
            pluck_step(s, tp, tf(a1, true, p0, true, dt), &ev);
        CHECK(near(s.yaw, -1.5707963f, 0.02f));
        CHECK(near(s.hang[0], 60.0f, 0.05f) && near(s.hang[1], 40.0f, 0.05f) &&
              near(s.hang[2], 100.0f - sqrtf(3200.0f), 0.05f));
        // Each drawn shoulder is within its arm's reach of its wrist target.
        {
            const PluckInput in = tf(a1, true, p0, true, dt);
            const Mat4 m = pluck_body_matrix(s, in);
            for (int c = 0; c < 2; ++c)
            {
                float o[3];
                map(m, in.arm_shoulder[c], o);
                const float* w = s.wrist_target[c];
                const float d = sqrtf((o[0] - w[0]) * (o[0] - w[0]) + (o[1] - w[1]) * (o[1] - w[1]) +
                                      (o[2] - w[2]) * (o[2] - w[2]));
                CHECK(d <= 60.5f && d > 50.0f);
            }
        }
        // Hands pulled far apart (wrist 2 m away): she is held no higher than
        // 0.9 of the reach out allows, between them.
        {
            PluckState w = s;
            const float wide[3] = { 60, 280, 105 };
            for (int i = 0; i < 120; ++i)
                pluck_step(w, tp, tf(a1, true, wide, true, dt), &ev);
            CHECK(near(w.hang[1], 140.0f, 0.5f));
            CHECK(near(w.hang[2], 100.0f - sqrtf(3600.0f - 54.0f * 54.0f), 0.5f));
        }
        // Let go of the second hand: no jump; she hangs by the first again.
        {
            PluckState w = s;
            const float before[3] = { w.target[0], w.target[1], w.target[2] };
            pluck_step(w, tp, tf(a1, true, p0, false, dt), &ev);
            CHECK(ev.second_release && ev.hand == 0 && !ev.released && !ev.thrown);
            CHECK(w.hand == 1 && w.hand2 == -1 && w.part == kPluckArm0 && w.release_chain == 1);
            float d = 0.0f;
            for (int k = 0; k < 3; ++k) d += (w.target[k] - before[k]) * (w.target[k] - before[k]);
            CHECK(sqrtf(d) < 4.0f);
            for (int i = 0; i < 60; ++i)
                pluck_step(w, tp, tf(a1, true, p0, false, dt), &ev);
            CHECK(near(pluck_two_weight(w), 0.0f) && w.release_chain == -1);
            CHECK(!pluck_arm_reaches(w, 0) && !pluck_arm_reaches(w, 1));
            CHECK(near(w.hang[0], 60.0f, 1e-3f) && near(w.hang[2], 100.0f, 1e-3f));
            CHECK(near(w.yaw, -1.5707963f, 0.03f));     // the turn is kept
        }
        // Let go of the first hand: the second takes its place, no jump,
        // and she hangs from arm 1's wrist (60,80,100).
        {
            PluckState w = s;
            const float before[3] = { w.target[0], w.target[1], w.target[2] };
            pluck_step(w, tp, tf(a1, false, p0, true, dt), &ev);
            CHECK(ev.second_release && ev.hand == 1 && !ev.released);
            CHECK(w.hand == 0 && w.part == kPluckArm1 && w.hand2 == -1 && w.release_chain == 0);
            float d = 0.0f;
            for (int k = 0; k < 3; ++k) d += (w.target[k] - before[k]) * (w.target[k] - before[k]);
            CHECK(sqrtf(d) < 4.0f);
            for (int i = 0; i < 60; ++i)
                pluck_step(w, tp, tf(a1, false, p0, true, dt), &ev);
            CHECK(near(w.hang[0], 60.0f, 1e-3f) && near(w.hang[1], 80.0f, 1e-3f) &&
                  near(w.hang[2], 100.0f, 1e-3f));
            // A fast flick of the last hand throws.
            float q[3] = { p0[0], p0[1], p0[2] };
            for (int i = 0; i < 6; ++i)
            {
                q[0] += 2.0f * 400.0f * dt;     // 2 m/s at the table's scale
                pluck_step(w, tp, tf(a1, false, q, true, dt), &ev);
            }
            pluck_step(w, tp, tf(a1, false, q, false, dt), &ev);
            CHECK(ev.released && ev.thrown && w.flying);
            CHECK(w.fly.vel[0] > 400.0f);
        }
        // Both let go in one frame, hand 0 flicking fast and hand 1 still:
        // thrown with hand 0's velocity; both arms stay up in the flight.
        {
            PluckState w = s;
            float q[3] = { p0[0], p0[1], p0[2] };
            for (int i = 0; i < 6; ++i)
            {
                q[1] += 2.0f * 400.0f * dt;
                pluck_step(w, tp, tf(a1, true, q, true, dt), &ev);
            }
            pluck_step(w, tp, tf(a1, false, q, false, dt), &ev);
            CHECK(ev.released && ev.thrown && w.flying && w.hand == -1 && w.hand2 == -1);
            CHECK(w.fly.vel[1] > 400.0f && fabsf(w.fly.vel[0]) < 100.0f);
            CHECK(pluck_arm_reaches(w, 0) && pluck_arm_reaches(w, 1));
            // The flight starts where she was drawn (the matrix is unchanged).
            pluck_land(w);
            CHECK(w.part2 == kPluckNone && w.two_t == 0.0f && w.yaw == 0.0f);
        }
        // Both let go slowly: dropped, everything cleared.
        {
            PluckState w = s;
            pluck_step(w, tp, tf(a1, false, p0, false, dt), &ev);
            CHECK(ev.released && !ev.thrown && w.hand == -1 && w.hand2 == -1 &&
                  w.part2 == kPluckNone && w.two_t == 0.0f);
        }
    }
}
