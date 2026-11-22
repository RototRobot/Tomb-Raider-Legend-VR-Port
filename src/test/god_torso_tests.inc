// Tests for src/vr/god_torso (included by mathtest.cpp).
#include "../vr/godmode/god_torso.h"

static void test_god_torso()
{
    using namespace trlvr;
    // She is 175 units tall (metre 100) and looks 0.3 m tall to the player:
    // table = 175 / 0.3. Her torso axis runs from (0,0,95) to (0,0,145);
    // the zone radius is 0.16 * 100 + 0.04 * 100 = 20 (margins at her size).
    TorsoParams params;
    params.metre = 100.0f;
    params.table = 175.0f / 0.3f;
    const float table = params.table;
    const float dt = 1.0f / 60.0f;
    const float kPi = 3.14159265f;

    const float hand_pos[3] = { 0.0f, -15.0f, 120.0f };     // in front of her belly
    auto frame = [&](const Mat4& pose, bool fist, const float normal[3]) {
        TorsoInput in;
        in.dt = dt;
        in.body_valid = true;
        in.body = Mat4::identity();
        in.torso_a[2] = 95.0f;
        in.torso_b[2] = 145.0f;
        in.forward[1] = -1.0f;
        in.up[2] = 1.0f;
        in.apparent_height = 0.3f;
        const float joints[5][3] = { { -20, 0, 140 }, { 20, 0, 140 }, { -10, 0, 90 },
                                     { 10, 0, 90 }, { 0, 0, 150 } };
        const float ends[5][3] = { { -24, 0, 80 }, { 24, 0, 80 }, { -11, 0, 5 },
                                   { 11, 0, 5 }, { 0, 0, 175 } };
        for (int l = 0; l < kTorsoLimbs; ++l)
        {
            in.limb_valid[l] = true;
            for (int k = 0; k < 3; ++k)
            {
                in.limb_joint[l][k] = joints[l][k];
                in.limb_end[l][k] = ends[l][k];
            }
        }
        in.hand[0].valid = true;
        in.hand[0].pose = translation(1000.0f, 0.0f, 0.0f);
        in.hand[0].palm_centre[0] = 1000.0f;
        in.hand[0].palm_normal[0] = 1.0f;
        TorsoHand& hd = in.hand[1];
        hd.valid = true;
        hd.fist = fist;
        hd.pose = pose;
        for (int k = 0; k < 3; ++k)
        {
            hd.palm_centre[k] = pose.m[3][k];
            hd.palm_normal[k] = normal[k];
        }
        return in;
    };
    const float toward[3] = { 0.0f, 1.0f, 0.0f };   // palm facing her
    const float away[3] = { 0.0f, -1.0f, 0.0f };
    const Mat4 at_belly = translation(hand_pos[0], hand_pos[1], hand_pos[2]);

    // Open, then close the fist: TorsoEvents of the closing frame.
    auto try_grab = [&](TorsoState& s, TorsoInput in) {
        TorsoEvents ev;
        in.hand[1].fist = false;
        torso_step(s, params, in, &ev);
        in.hand[1].fist = true;
        torso_step(s, params, in, &ev);
        return ev;
    };

    // Zone helper.
    {
        float a[3], b[3], r;
        CHECK(torso_zone(params, frame(at_belly, false, toward), a, b, &r));
        CHECK(near(r, 16.0f + 0.04f * params.metre) && a[2] == 95.0f && b[2] == 145.0f);
        TorsoInput none = frame(at_belly, false, toward);
        none.body_valid = false;
        CHECK(!torso_zone(params, none, a, b, &r));
    }

    // A fist with the palm toward her torso grabs.
    {
        TorsoState s;
        TorsoInput in = frame(at_belly, false, toward);
        CHECK(torso_aim(params, in, 1));
        const TorsoEvents ev = try_grab(s, in);
        CHECK(ev.grabbed && ev.hand == 1 && ev.wants[1]);
        CHECK(s.hand == 1 && torso_drawn(s));
    }
    // The back of the hand toward her does not.
    {
        TorsoState s;
        TorsoInput in = frame(at_belly, false, away);
        CHECK(!torso_aim(params, in, 1));
        const TorsoEvents ev = try_grab(s, in);
        CHECK(!ev.grabbed && !ev.wants[1] && s.hand == -1);
    }
    // An open hand never grabs, however long it stays.
    {
        TorsoState s; TorsoEvents ev;
        TorsoInput in = frame(at_belly, false, toward);
        for (int i = 0; i < 30; ++i)
            torso_step(s, params, in, &ev);
        CHECK(ev.aim[1] && !ev.grabbed && s.hand == -1);
    }
    // Too big to the player: no grab. Exactly max_height still grabs.
    {
        TorsoState s;
        TorsoInput in = frame(at_belly, false, toward);
        in.apparent_height = 0.7f;
        CHECK(!torso_aim(params, in, 1));
        CHECK(!try_grab(s, in).grabbed);
        in.apparent_height = 0.0f;
        CHECK(!try_grab(s, in).grabbed);
        in.apparent_height = params.max_height;
        CHECK(try_grab(s, in).grabbed);
    }
    // A hand on a soft part, or a hand / Lara taken by the pluck: no grab.
    {
        TorsoState s;
        TorsoInput in = frame(at_belly, false, toward);
        in.hand[1].part_held = true;
        CHECK(!try_grab(s, in).grabbed);
        in.hand[1].part_held = false;
        in.hand[1].other_grab = true;
        const TorsoEvents ev = try_grab(s, in);
        CHECK(!ev.grabbed && !ev.wants[1] && !ev.aim[1]);
    }
    // Outside the capsule (past the radius, above her chest end): no grab.
    // A fist closing just after the hand left the zone (grace) still grabs.
    {
        TorsoState s;
        CHECK(!try_grab(s, frame(translation(0, -25, 120), false, toward)).grabbed);
        CHECK(!try_grab(s, frame(translation(0, -10, 190), false, toward)).grabbed);
        TorsoState g; TorsoEvents ev;
        torso_step(g, params, frame(at_belly, false, toward), &ev);
        CHECK(ev.aim[1] && ev.hover_enter[1]);
        torso_step(g, params, frame(translation(0, -25, 120), false, toward), &ev);
        CHECK(!ev.aim[1]);
        torso_step(g, params, frame(translation(0, -25, 120), true, toward), &ev);
        CHECK(ev.grabbed);
    }

    auto point_through = [](const Mat4& m, float x, float y, float z, float out[3]) {
        for (int k = 0; k < 3; ++k)
            out[k] = x * m.m[0][k] + y * m.m[1][k] + z * m.m[2][k] + m.m[3][k];
    };
    // Hold: during the settle the drawn pose stays put under a moving hand;
    // after it she follows the hand exactly (translation, 90 deg about z,
    // 180 deg about x, all about the hand's origin).
    {
        TorsoState s; TorsoEvents ev;
        TorsoInput in = frame(at_belly, false, toward);
        CHECK(try_grab(s, in).grabbed);
        Mat4 m = torso_body_matrix(s, in);
        CHECK(near(m.m[3][0], 0.0f) && near(m.m[3][2], 0.0f));
        // The settle: the fist pulls the hand 3 units in; she does not move.
        for (int i = 0; i < 6; ++i)
        {
            in = frame(translation(0, -15 + 0.5f * (i + 1), 120), true, toward);
            torso_step(s, params, in, &ev);
        }
        m = torso_body_matrix(s, in);
        CHECK(near(m.m[3][1], 0.0f, 1e-3f));
        for (int i = 0; i < 20; ++i)
            torso_step(s, params, in, &ev);
        CHECK(s.settle <= 0.0f && s.hand == 1);
        const float hy = -12.0f;    // where the hand ended up
        in = frame(translation(50, hy, 120), true, toward);
        torso_step(s, params, in, &ev);
        m = torso_body_matrix(s, in);
        CHECK(near(m.m[3][0], 50.0f) && near(m.m[3][1], 0.0f, 1e-3f) && near(m.m[3][2], 0.0f, 1e-3f));
        CHECK(near(s.feet[0], 50.0f));
        // 90 deg about z at the hand (back at x 0): her belly point
        // (0,0,120), (0,12,0) from the hand, goes to (-12,0,0) from it.
        const float rz[3] = { 0, 0, kPi / 2 };
        in = frame(rotation_from_vector(rz) * translation(0, hy, 120), true, toward);
        torso_step(s, params, in, &ev);
        m = torso_body_matrix(s, in);
        float p[3];
        point_through(m, 0, 0, 120, p);
        CHECK(near(p[0], hy, 1e-3f) && near(p[1], hy, 1e-3f) && near(p[2], 120.0f, 1e-3f));
        // 180 deg about x: (0,12,0) -> (0,-12,0); her feet (0,12,-120) -> (0,-12,120).
        const float rx[3] = { kPi, 0, 0 };
        in = frame(rotation_from_vector(rx) * translation(0, hy, 120), true, toward);
        torso_step(s, params, in, &ev);
        m = torso_body_matrix(s, in);
        point_through(m, 0, 0, 120, p);
        CHECK(near(p[0], 0.0f, 1e-3f) && near(p[1], 2 * hy, 1e-3f) && near(p[2], 120.0f, 1e-3f));
        CHECK(near(s.feet[1], 2 * hy, 1e-3f) && near(s.feet[2], 240.0f, 1e-3f));
        // Upside down: her up is world down.
        CHECK(near(m.m[2][2], -1.0f, 1e-4f));
        // Light smoothing: the drawn pose lags, then arrives.
        TorsoParams sp = params;
        sp.follow_seconds = 0.05f;
        TorsoState t;
        TorsoInput tin = frame(at_belly, false, toward);
        tin.hand[1].fist = false;
        torso_step(t, sp, tin, &ev);
        tin.hand[1].fist = true;
        torso_step(t, sp, tin, &ev);
        for (int i = 0; i < 20; ++i)
            torso_step(t, sp, tin, &ev);
        tin = frame(translation(40, -15, 120), true, toward);
        torso_step(t, sp, tin, &ev);
        CHECK(t.body_matrix.m[3][0] > 1.0f && t.body_matrix.m[3][0] < 39.0f);
        for (int i = 0; i < 60; ++i)
            torso_step(t, sp, tin, &ev);
        CHECK(near(t.body_matrix.m[3][0], 40.0f, 1e-3f));
    }

    // Limbs. The world direction of limb l as the caller draws it: its rest
    // direction (joint -> end, real pose) turned by limb_rotation, then by
    // the body matrix's rotation.
    auto limb_world = [&](const TorsoState& s, const TorsoInput& in, int l, float out[3]) {
        float r[3];
        for (int k = 0; k < 3; ++k) r[k] = in.limb_end[l][k] - in.limb_joint[l][k];
        const float n = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
        for (float& v : r) v /= n;
        const Mat4 t = torso_limb_rotation(s, l) * torso_body_matrix(s, in);
        for (int k = 0; k < 3; ++k)
            out[k] = r[0] * t.m[0][k] + r[1] * t.m[1][k] + r[2] * t.m[2][k];
    };
    auto angle_between = [](const float a[3], const float b[3]) {
        const float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
        const float la = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
        const float lb = std::sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
        float c = d / (la * lb);
        c = c > 1.0f ? 1.0f : c < -1.0f ? -1.0f : c;
        return std::acos(c);
    };
    // The rest direction as drawn (no limb turn).
    auto rest_world = [&](const TorsoState& s, const TorsoInput& in, int l, float out[3]) {
        float r[3];
        for (int k = 0; k < 3; ++k) r[k] = in.limb_end[l][k] - in.limb_joint[l][k];
        const Mat4 t = torso_body_matrix(s, in);
        for (int k = 0; k < 3; ++k)
            out[k] = r[0] * t.m[0][k] + r[1] * t.m[1][k] + r[2] * t.m[2][k];
    };
    const float down[3] = { 0.0f, 0.0f, -1.0f };
    const float deg20 = 20.0f * kPi / 180.0f;
    // Held, turned to `turn` radians about world x over half a second, then
    // held still for 3 s. Every frame each leg stays in its cone (its widest
    // limit, forward), the head in its own.
    auto hold_turned = [&](float turn, TorsoState& s, TorsoInput& in, float* worst_leg) {
        in = frame(at_belly, false, toward);
        CHECK(try_grab(s, in).grabbed);
        in.hand[1].fist = true;
        TorsoEvents ev;
        for (int i = 0; i < 20; ++i)
            torso_step(s, params, in, &ev);
        *worst_leg = 0.0f;
        for (int i = 0; i < 30 + 180; ++i)
        {
            const float f = i < 30 ? (i + 1) / 30.0f : 1.0f;
            const float rv[3] = { turn * f, 0, 0 };
            in = frame(rotation_from_vector(rv) * at_belly, true, toward);
            torso_step(s, params, in, &ev);
            for (int l = kTorsoLeg0; l <= kTorsoLeg1; ++l)
            {
                float w[3], r[3];
                limb_world(s, in, l, w);
                rest_world(s, in, l, r);
                const float a = angle_between(w, r);
                if (a > *worst_leg) *worst_leg = a;
            }
            float w[3], r[3];
            limb_world(s, in, kTorsoHead, w);
            rest_world(s, in, kTorsoHead, r);
            CHECK(angle_between(w, r) <= params.head_forward + 0.01f);
        }
        CHECK(s.hand == 1);
    };
    {
        // Upright: everything but the head hangs straight down.
        TorsoState s; TorsoInput in; float worst;
        hold_turned(0.0f, s, in, &worst);
        for (int l = 0; l < 4; ++l)
        {
            float w[3];
            limb_world(s, in, l, w);
            CHECK(angle_between(w, down) < deg20);
        }
        // The head stays up on her neck.
        {
            float w[3], r[3];
            limb_world(s, in, kTorsoHead, w);
            rest_world(s, in, kTorsoHead, r);
            CHECK(angle_between(w, r) < 0.02f);
        }
        // The knees: a relaxed bend about her side axis (x).
        float rv[3];
        CHECK(rotation_to_vector(s.knee_rotation[0], rv));
        CHECK(near(std::fabs(rv[0]), params.knee_bend, 0.05f) && std::fabs(rv[1]) < 1e-3f);
        // The hand jerked toward +x (2 m/s in 0.1 s): the arms and legs
        // swing back, their ends behind the joints (-x).
        TorsoEvents ev;
        for (int i = 1; i <= 6; ++i)
        {
            const float x = 0.5f * 20.0f * (i * dt) * (i * dt) * table;
            in = frame(translation(hand_pos[0] + x, hand_pos[1], hand_pos[2]), true, toward);
            torso_step(s, params, in, &ev);
        }
        for (int l = 0; l < 4; ++l)
        {
            float w[3];
            limb_world(s, in, l, w);
            CHECK(w[0] < -0.1f);
        }
    }
    {
        // Face down (+90 deg about x: her front -y turns to -z): arms (45 deg: they keep tone) and
        // legs hang toward the floor (legs at most 80 deg forward of her pose).
        TorsoState s; TorsoInput in; float worst;
        hold_turned(kPi / 2, s, in, &worst);
        for (int l = 0; l < 4; ++l)
        {
            float w[3];
            limb_world(s, in, l, w);
            CHECK(angle_between(w, down) < (l < 2 ? kPi / 4 : deg20));  // arms keep some tone
        }
        CHECK(worst <= params.leg_forward + 0.01f);
        // The thighs swing forward: the knees bend more.
        float rv[3];
        CHECK(rotation_to_vector(s.knee_rotation[0], rv));
        CHECK(std::fabs(rv[0]) > params.knee_bend + 0.3f);
    }
    {
        // Upside down (180 deg about x): the arms fall overhead (tone holds them up some) toward the
        // floor; the legs only as far as the hips bend (80 deg).
        TorsoState s; TorsoInput in; float worst;
        hold_turned(kPi, s, in, &worst);
        for (int l = 0; l < 2; ++l)
        {
            float w[3];
            limb_world(s, in, l, w);
            CHECK(angle_between(w, down) < kPi / 2);    // arms keep tone: below horizontal at least
        }
        CHECK(worst <= params.leg_forward + 0.01f);
        for (int l = kTorsoLeg0; l <= kTorsoLeg1; ++l)
        {
            float w[3];
            limb_world(s, in, l, w);
            CHECK(angle_between(w, down) < 2.0f);     // tipped well off straight up
        }
    }
    {
        // Face up (-90 deg): the legs fall backward only 20 deg (or roll a
        // little out to her sides, 45 deg); the arms drop below level.
        TorsoState s; TorsoInput in; float worst;
        hold_turned(-kPi / 2, s, in, &worst);
        CHECK(worst <= params.leg_out + 0.01f);
        for (int l = 0; l < 4; ++l)
        {
            float w[3];
            limb_world(s, in, l, w);
            CHECK(angle_between(w, down) <= kPi / 2 - params.leg_back + 0.02f);
        }
    }

    // A grab, settled, then the hand moved along `vel` (real m/s) while
    // turning at `spin` rad/s about z for 0.3 s; returns the last input.
    auto swing = [&](TorsoState& s, float vx, float vz, float spin) {
        TorsoInput in = frame(at_belly, false, toward);
        CHECK(try_grab(s, in).grabbed);
        in.hand[1].fist = true;
        TorsoEvents ev;
        for (int i = 0; i < 20; ++i)
            torso_step(s, params, in, &ev);
        for (int i = 1; i <= 18; ++i)
        {
            const float t = i * dt;
            const float rv[3] = { 0, 0, spin * t };
            in = frame(rotation_from_vector(rv) *
                       translation(hand_pos[0] + vx * table * t, hand_pos[1], hand_pos[2] + vz * table * t),
                       true, toward);
            torso_step(s, params, in, &ev);
        }
        return in;
    };
    // The arm stretch: a 2.5 m/s drawn swing. Real speed unknown (-1): she
    // flies at 2.5 m/s as without a stretch; real 1.0 m/s: capped to 1.0
    // along the drawn direction; real 4.0 (above the drawn): not amplified.
    {
        const float real[3] = { -1.0f, 1.0f, 4.0f };
        const float want[3] = { 2.5f, 1.0f, 2.5f };
        for (int c = 0; c < 3; ++c)
        {
            TorsoState s; TorsoEvents ev;
            TorsoInput in = frame(at_belly, false, toward);
            CHECK(try_grab(s, in).grabbed);
            in.hand[1].fist = true;
            for (int i = 0; i < 20; ++i)
                torso_step(s, params, in, &ev);
            for (int i = 1; i <= 18; ++i)
            {
                const float t = i * dt;
                in = frame(translation(hand_pos[0] + 2.5f * table * t, hand_pos[1], hand_pos[2]),
                           true, toward);
                in.hand[1].real_speed = real[c];
                torso_step(s, params, in, &ev);
            }
            in.hand[1].fist = false;
            in.hand[1].real_speed = -1.0f;
            torso_step(s, params, in, &ev);
            CHECK(ev.released && ev.thrown);
            CHECK(near(s.fly.vel[0], want[c] * table, 0.03f));
        }
    }
    // A fast release: she flies at the hand's velocity, no spin, along a
    // parabola under doll-scale gravity.
    {
        TorsoState s; TorsoEvents ev;
        TorsoInput in = swing(s, 1.5f, 0.5f, 0.0f);
        in.hand[1].fist = false;
        torso_step(s, params, in, &ev);
        CHECK(ev.released && ev.thrown && !ev.put_down && ev.hand == 1);
        CHECK(s.flying && torso_drawn(s) && s.hand == -1);
        CHECK(near(s.fly.vel[0], 1.5f * table, 0.03f) && near(s.fly.vel[2], 0.5f * table, 0.03f));
        CHECK(near(ev.throw_speed, std::sqrt(1.5f * 1.5f + 0.5f * 0.5f), 0.03f));
        CHECK(std::fabs(s.fly.spin[0]) + std::fabs(s.fly.spin[1]) + std::fabs(s.fly.spin[2]) < 1e-3f);
        const float c0[3] = { s.centre[0], s.centre[1], s.centre[2] };
        const float f0[3] = { s.feet[0], s.feet[1], s.feet[2] };
        const float v0[3] = { s.fly.vel[0], s.fly.vel[1], s.fly.vel[2] };
        const float g = params.throw_gravity * table;
        bool ok = true, probe_ok = true;
        for (int i = 1; i <= 40; ++i)
        {
            const float prev[3] = { s.feet[0], s.feet[1], s.feet[2] };
            torso_step(s, params, in, &ev);
            const float t = i * dt;
            const float want[3] = { c0[0] + v0[0] * t, c0[1] + v0[1] * t,
                                    c0[2] + v0[2] * t - 0.5f * g * t * t };
            for (int k = 0; k < 3; ++k)
            {
                if (std::fabs(s.centre[k] - want[k]) > 0.2f) ok = false;
                // No spin: her feet keep their offset from the centre.
                if (std::fabs(s.feet[k] - s.centre[k] - (f0[k] - c0[k])) > 0.2f) ok = false;
                if (s.feet_prev[k] != prev[k]) probe_ok = false;
            }
        }
        CHECK(ok);
        CHECK(probe_ok);
        CHECK(!ev.flight_over);
        // The limbs trail behind the travel a little and stay in their cones.
        for (int l = kTorsoLeg0; l <= kTorsoLeg1; ++l)
        {
            float w[3], r[3];
            limb_world(s, in, l, w);
            rest_world(s, in, l, r);
            CHECK(angle_between(w, r) <= params.leg_forward + 0.01f);
        }

        // The projectile is her drawn centre.
        CHECK(near(s.fly.pos[0], s.centre[0], 1e-3f) && near(s.fly.pos[2], s.centre[2], 1e-3f));
        // A wall head on (normal -x): her centre goes to the given point
        // (the feet with it), the speed into the wall turns back at the
        // restitution; the rise is kept (friction 0).
        const float wall[3] = { s.centre[0] - 5.0f, s.centre[1], s.centre[2] };
        const float feet_off = s.feet[0] - s.centre[0];
        const float vz = s.fly.vel[2];
        const float wall_n[3] = { -1, 0, 0 }, ceil_n[3] = { 0, 0, -1 };
        torso_bounce(s, wall, wall_n, 0.5f, 0.0f);
        CHECK(near(s.centre[0], wall[0]) && near(s.fly.pos[0], wall[0]) &&
              near(s.feet[0] - s.centre[0], feet_off, 1e-3f) && near(s.feet_prev[0], s.feet[0]));
        CHECK(near(s.fly.vel[0], -0.75f * table, 0.03f) && s.fly.vel[2] == vz);
        CHECK(near(torso_body_matrix(s, in).m[3][0] - s.body_matrix.m[3][0], 0.0f, 1e-3f));
        float p[3];
        point_through(torso_body_matrix(s, in), 0, 0, 0, p);
        CHECK(near(p[0], s.feet[0], 1e-3f));
        // A ceiling on the way down changes nothing; on the way up the
        // rise turns into a fall.
        s.fly.vel[2] = -100.0f;
        torso_bounce(s, wall, ceil_n, 0.5f, 0.0f);
        CHECK(s.fly.vel[2] == -100.0f);
        s.fly.vel[2] = 100.0f;
        torso_bounce(s, wall, ceil_n, 0.5f, 0.0f);
        CHECK(near(s.fly.vel[2], -50.0f));
        // Landed: nothing drawn.
        torso_land(s);
        CHECK(!torso_drawn(s) && !s.flying);
        const Mat4 m = torso_body_matrix(s, in);
        CHECK(m.m[3][0] == 0.0f && m.m[0][0] == 1.0f);
        // Not flying: a bounce does nothing.
        torso_bounce(s, wall, wall_n, 0.5f, 0.0f);
        CHECK(!s.flying);
    }
    // A spinning toss: the spin is a quarter of the hand's, capped at 2 rad/s.
    {
        TorsoState s; TorsoEvents ev;
        TorsoInput in = swing(s, 1.5f, 0.0f, 4.0f);
        in.hand[1].fist = false;
        torso_step(s, params, in, &ev);
        CHECK(ev.thrown);
        CHECK(near(s.fly.spin[2], 1.0f, 0.05f));
        TorsoState f;
        // (2026-10-22: 3 m/s, not 1.5: the release now takes the doll's
        // velocity at the release, v + omega x r, and at 20 rad/s about the
        // fist her centre happens to move at only ~0.6 m/s there; the old
        // peak took the fastest moment of the last 0.15 s.)
        in = swing(f, 3.0f, 0.0f, 20.0f);
        in.hand[1].fist = false;
        torso_step(f, params, in, &ev);
        CHECK(ev.thrown);
        const float w = std::sqrt(f.fly.spin[0] * f.fly.spin[0] + f.fly.spin[1] * f.fly.spin[1] +
                                  f.fly.spin[2] * f.fly.spin[2]);
        CHECK(w <= params.spin_max + 1e-4f && w > 1.9f);
        // The spin fades slowly in the air.
        for (int i = 0; i < 60; ++i)
            torso_step(f, params, in, &ev);
        CHECK(f.fly.spin[2] < w && f.fly.spin[2] > 0.5f * w);
        // The time limit: flight_over after throw_max_time.
        bool over = false;
        for (int i = 0; i < 200 && !over; ++i)
        {
            torso_step(f, params, in, &ev);
            over = ev.flight_over;
        }
        CHECK(over && f.fly_time >= params.throw_max_time && f.fly_time < params.throw_max_time + 0.05f);
    }
    // A slow release puts her down where she is drawn; no flight.
    {
        TorsoState s; TorsoEvents ev;
        TorsoInput in = swing(s, 0.2f, 0.0f, 0.0f);
        const float feet_x = s.feet[0];
        in.hand[1].fist = false;
        torso_step(s, params, in, &ev);
        CHECK(ev.released && ev.put_down && !ev.thrown);
        CHECK(near(ev.feet[0], feet_x) && !torso_drawn(s) && s.hand == -1);
        // Throws off: a fast release is a put-down too.
        TorsoParams np = params;
        np.throw_enabled = false;
        TorsoState t;
        TorsoInput tin = swing(t, 2.0f, 0.0f, 0.0f);
        tin.hand[1].fist = false;
        torso_step(t, np, tin, &ev);
        CHECK(ev.put_down && !t.flying);
    }
    // Disabled: nothing held, and a fist held through enabling is no press.
    {
        TorsoState s; TorsoEvents ev;
        TorsoInput in = frame(at_belly, false, toward);
        CHECK(try_grab(s, in).grabbed);
        TorsoParams off = params;
        off.enabled = false;
        in.hand[1].fist = true;
        torso_step(s, off, in, &ev);
        CHECK(!torso_drawn(s));
        torso_step(s, params, in, &ev);
        CHECK(!ev.grabbed);
        // Bad limb index / not drawn: identity.
        CHECK(torso_limb_rotation(s, 7).m[0][0] == 1.0f);
        const float j[3] = { 1, 2, 3 };
        CHECK(torso_limb_turn(s, 0, j).m[3][0] == 0.0f);
    }
    // A free hand knocking a drawn limb sets it swinging; far away, at rest,
    // or with nothing held, it does nothing.
    {
        TorsoState s; TorsoEvents ev;
        TorsoInput in = frame(at_belly, false, toward);
        CHECK(try_grab(s, in).grabbed);
        in.hand[1].fist = true;
        for (int i = 0; i < 5; ++i)
            torso_step(s, params, in, &ev);
        const float fast[3] = { 300.0f, 0.0f, 0.0f };
        const float still[3] = { 0.0f, 0.0f, 0.0f };
        // The left arm's drawn joint (limb 0), a little along it.
        const float on_arm[3] = { s.joint_prev[0][0] + s.limb_dir[0][0] * 5.0f,
                                  s.joint_prev[0][1] + s.limb_dir[0][1] * 5.0f,
                                  s.joint_prev[0][2] + s.limb_dir[0][2] * 5.0f };
        const float far_pt[3] = { 5000.0f, 5000.0f, 5000.0f };
        const float w0 = sqrtf(s.limb_vel[0][0] * s.limb_vel[0][0] + s.limb_vel[0][1] * s.limb_vel[0][1] +
                               s.limb_vel[0][2] * s.limb_vel[0][2]);
        CHECK(!torso_nudge(s, params, in, far_pt, fast, 10.0f, 1.0f, dt));
        CHECK(!torso_nudge(s, params, in, on_arm, still, 10.0f, 1.0f, dt));
        CHECK(torso_nudge(s, params, in, on_arm, fast, 10.0f, 1.0f, dt));
        const float w1 = sqrtf(s.limb_vel[0][0] * s.limb_vel[0][0] + s.limb_vel[0][1] * s.limb_vel[0][1] +
                               s.limb_vel[0][2] * s.limb_vel[0][2]);
        CHECK(w1 > w0 + 1e-4f);
        TorsoState idle;
        CHECK(!torso_nudge(idle, params, in, on_arm, fast, 10.0f, 1.0f, dt));
    }
}
