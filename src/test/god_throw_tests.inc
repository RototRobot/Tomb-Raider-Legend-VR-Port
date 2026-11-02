// Tests for src/vr/god_throw (included by mathtest.cpp).
#include "../vr/godmode/god_throw.h"

static void test_god_throw()
{
    using namespace trlvr;
    const float dt = 1.0f / 90.0f;

    // Parabola: the exact analytic path, whatever the step.
    {
        ThrowBody b;
        const float p0[3] = { 10, 20, 30 }, v0[3] = { 300, -100, 400 };
        throw_body_start(b, p0, v0, nullptr, 50.0f);
        const float g = 981.0f;
        bool ok = true;
        for (int i = 1; i <= 90; ++i)
        {
            const float prev_x = b.pos[0];
            throw_body_step(b, g, 0.0f, dt, nullptr);
            const float t = i * dt;
            if (!near(b.pos[0], p0[0] + v0[0] * t, 0.05f) || !near(b.pos[1], p0[1] + v0[1] * t, 0.05f) ||
                !near(b.pos[2], p0[2] + v0[2] * t - 0.5f * g * t * t, 0.1f) ||
                !near(b.vel[2], v0[2] - g * t, 0.05f) || !near(b.prev[0], prev_x))
                ok = false;
        }
        CHECK(ok);
        CHECK(near(b.radius, 50.0f));
    }

    // Release velocity: v + omega x (centre - hand).
    {
        const float v[3] = { 1, 0, 0 }, w[3] = { 0, 0, 2 }, hand[3] = { 0, 0, 0 }, c[3] = { 0.5f, 0, 0 };
        float out[3];
        throw_release(v, w, hand, c, out);
        CHECK(near(out[0], 1.0f) && near(out[1], 1.0f) && near(out[2], 0.0f));
        const float c2[3] = { 0, 0, 0.3f };     // on the spin axis: no extra
        throw_release(v, w, hand, c2, out);
        CHECK(near(out[0], 1.0f) && near(out[1], 0.0f));
    }

    // The averaged estimate.
    {
        // Steady 2 m/s along x.
        ThrowTracker t;
        const float fwd[3] = { 2, 0, 0 };
        for (int i = 0; i < 10; ++i) throw_track_add(t, fwd, nullptr, dt);
        float v[3], w[3];
        CHECK(throw_track_estimate(t, 0.15f, v, w) && near(v[0], 2.0f, 1e-3f));
        // A single jitter spike (8 m/s sideways, under the jump limit) is dropped.
        const float spike[3] = { 2, 8, 0 };
        throw_track_add(t, spike, nullptr, dt);
        for (int i = 0; i < 2; ++i) throw_track_add(t, fwd, nullptr, dt);
        throw_track_estimate(t, 0.15f, v, w);
        CHECK(near(v[0], 2.0f, 1e-3f) && fabsf(v[1]) < 1e-3f);
        // A backswing frame (-3 m/s) just before the forward swing.
        ThrowTracker b;
        const float back[3] = { -3, 0, 0 };
        for (int i = 0; i < 6; ++i) throw_track_add(b, back, nullptr, dt);
        throw_track_add(b, back, nullptr, dt);
        for (int i = 0; i < 4; ++i) throw_track_add(b, fwd, nullptr, dt);
        throw_track_estimate(b, 0.15f, v, w);
        CHECK(near(v[0], 2.0f, 1e-3f));
        // The fingers opening (the hand stops two frames): the window
        // before the stop counts.
        ThrowTracker s;
        const float stop[3] = { 0, 0, 0 };
        for (int i = 0; i < 8; ++i) throw_track_add(s, fwd, nullptr, dt);
        for (int i = 0; i < 2; ++i) throw_track_add(s, stop, nullptr, dt);
        throw_track_estimate(s, 0.15f, v, w);
        CHECK(near(v[0], 2.0f, 1e-3f));
        // Spin is averaged over the same frames.
        ThrowTracker r;
        const float spin[3] = { 0, 0, 3 };
        for (int i = 0; i < 6; ++i) throw_track_add(r, fwd, spin, dt);
        throw_track_estimate(r, 0.15f, v, w);
        CHECK(near(w[2], 3.0f, 1e-3f));
        // Empty: no estimate.
        ThrowTracker e;
        CHECK(!throw_track_estimate(e, 0.15f, v, w));
    }

    // Bounces.
    {
        // Glancing: along a wall at +x, slightly into it (normal -y): keeps
        // going +x (no reversal toward the thrower).
        ThrowBody b;
        const float p0[3] = {}, v0[3] = { 10, 2, 0 };
        throw_body_start(b, p0, v0, nullptr, 1.0f);
        const float wall[3] = { 0, -1, 0 };
        CHECK(throw_body_bounce(b, wall, 0.3f, 0.5f));
        CHECK(near(b.vel[0], 5.0f) && near(b.vel[1], -0.6f));
        // Head on: reversed at the restitution.
        const float v1[3] = { 10, 0, 0 }, front[3] = { -1, 0, 0 };
        throw_body_start(b, p0, v1, nullptr, 1.0f);
        throw_body_bounce(b, front, 0.3f, 0.5f);
        CHECK(near(b.vel[0], -3.0f) && near(b.vel[1], 0.0f));
        // Moving away from the surface: unchanged.
        CHECK(!throw_body_bounce(b, front, 0.3f, 0.5f) && near(b.vel[0], -3.0f));
        // Moved to the last free point.
        const float at[3] = { 1, 2, 3 };
        throw_body_move(b, at);
        CHECK(near(b.pos[1], 2.0f) && near(b.prev[2], 3.0f));
    }

    // Surfaces and normals.
    {
        const float up[3] = { 0, 0, 1 }, down[3] = { 0, 0, -1 }, side[3] = { 1, 0, 0 };
        CHECK(throw_surface(up, -1.0f) == kThrowFloor);      // falling onto a floor: lands
        CHECK(throw_surface(up, 1.0f) == kThrowWall);        // rising into it: a bounce
        CHECK(throw_surface(down, 1.0f) == kThrowCeiling);
        CHECK(throw_surface(side, -1.0f) == kThrowWall);
        const float dir[3] = { 0, 0, -1 };
        CHECK(throw_normal_valid(up, dir));
        CHECK(!throw_normal_valid(down, dir));               // facing along the ray
        const float junk[3] = { 3, 0, 0 };
        CHECK(!throw_normal_valid(junk, dir));
        // Three points on a 45 degree slope, normal back toward the ray.
        const float a[3] = { 0, 0, 0 }, b[3] = { 0, 1, 0 }, c[3] = { 1, 0, 1 };
        const float ray[3] = { 1, 0, 0 };
        float n[3];
        CHECK(throw_plane_normal(a, b, c, ray, n));
        CHECK(near(n[0], -0.7071f, 1e-3f) && near(n[2], 0.7071f, 1e-3f) && near(n[1], 0.0f));
        CHECK(!throw_plane_normal(a, a, c, ray, n));
    }

    // Spin cap and fade.
    {
        float w[3] = { 3, 4, 0 };
        throw_cap_spin(w, 2.0f);
        CHECK(near(sqrtf(w[0] * w[0] + w[1] * w[1]), 2.0f, 1e-4f) && near(w[0] / w[1], 0.75f, 1e-4f));
        ThrowBody b;
        const float p0[3] = {}, v0[3] = {}, sp[3] = { 0, 0, 2 };
        throw_body_start(b, p0, v0, sp, 1.0f);
        float turn[3];
        throw_body_step(b, 0.0f, 0.5f, 0.1f, turn);
        CHECK(near(turn[2], 0.2f) && near(b.spin[2], 2.0f * expf(-0.05f), 1e-4f));
    }
}
