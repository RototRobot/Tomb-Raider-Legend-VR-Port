// Tests for src/vr/rear_shape (included by mathtest.cpp).
#include "../vr/physics/rear_shape.h"

#include <vector>

static void test_rear_shape()
{
    using namespace trlvr::rear;
    const float metre = 291.0f;
    // The region weight: full at the centre and inside the core, none at
    // or past the surface, smooth and falling in between.
    {
        RearRegion r;
        r.centre[0] = 10.0f; r.centre[1] = 20.0f; r.centre[2] = 30.0f;
        r.radius[0] = 10.0f; r.radius[1] = 20.0f; r.radius[2] = 40.0f;
        r.core = 0.5f;
        const float c[3] = { 10.0f, 20.0f, 30.0f };
        CHECK(near(region_weight(r, c), 1.0f));
        const float in_core[3] = { 14.0f, 20.0f, 30.0f };          // d = 0.4
        CHECK(near(region_weight(r, in_core), 1.0f));
        const float surface[3] = { 10.0f, 40.0f, 30.0f };          // d = 1 along y
        CHECK(region_weight(r, surface) == 0.0f);
        const float out[3] = { 10.0f, 20.0f, 75.0f };
        CHECK(region_weight(r, out) == 0.0f);
        float last = 1.0f;
        bool falling = true, inside01 = true;
        for (int i = 0; i <= 20; ++i)
        {
            const float p[3] = { 10.0f + 0.5f * (float)i, 20.0f, 30.0f };
            const float w = region_weight(r, p);
            falling = falling && w <= last + 1e-6f;
            inside01 = inside01 && w >= 0.0f && w <= 1.0f;
            last = w;
        }
        CHECK(falling && inside01);
        // No core: only the centre is at full weight.
        r.core = 0.0f;
        CHECK(region_weight(r, in_core) < 0.99f && region_weight(r, in_core) > 0.5f);
    }
    // The seam: half on the line, each side keeps its own, none across.
    {
        CHECK(near(seam_weight(2.0f, 2.0f, 3.0f, 0), 0.5f));
        CHECK(near(seam_weight(2.0f, 2.0f, 3.0f, 1), 0.5f));
        CHECK(near(seam_weight(-1.0f, 2.0f, 3.0f, 0), 1.0f));
        CHECK(near(seam_weight(5.0f, 2.0f, 3.0f, 0), 0.0f, 1e-6f));
        CHECK(near(seam_weight(5.0f, 2.0f, 3.0f, 1), 1.0f));
        CHECK(near(seam_weight(-1.0f, 2.0f, 3.0f, 1), 0.0f, 1e-6f));
        CHECK(seam_weight(100.0f, 2.0f, 0.0f, 0) == 1.0f);         // no cut
        // The two sides add to 1 everywhere (no gap, no double at the seam).
        bool sum1 = true;
        for (int i = -10; i <= 10; ++i)
        {
            const float x = 2.0f + 0.4f * (float)i;
            sum1 = sum1 && near(seam_weight(x, 2.0f, 3.0f, 0) + seam_weight(x, 2.0f, 3.0f, 1), 1.0f);
        }
        CHECK(sum1);
    }
    // The dent: a point inside goes out to the surface, along the line from
    // the centre; outside, or no radius, stays.
    {
        const float c[3] = { 0.0f, 0.0f, 0.0f };
        float p[3] = { 0.0f, 1.0f, 0.0f };
        CHECK(dent_push(p, c, 3.0f));
        CHECK(near(p[0], 0.0f) && near(p[1], 3.0f, 1e-3f) && near(p[2], 0.0f));
        float q[3] = { 4.0f, 0.0f, 0.0f };
        CHECK(!dent_push(q, c, 3.0f));
        CHECK(q[0] == 4.0f && q[1] == 0.0f && q[2] == 0.0f);
        float z[3] = { 0.5f, 0.0f, 0.0f };
        CHECK(!dent_push(z, c, 0.0f) && z[0] == 0.5f);
        float centre_pt[3] = { 0.0f, 0.0f, 0.0f };                 // on the centre: finite
        dent_push(centre_pt, c, 3.0f);
        CHECK(std::isfinite(centre_pt[0]) && std::isfinite(centre_pt[1]));
    }
    // Weights and deform: offsets weighted, the cleft uncut, dents after.
    {
        RearShape s;
        for (int i = 0; i < 2; ++i)
        {
            s.cheek[i].centre[0] = i ? 20.0f : -20.0f;
            s.cheek[i].centre[1] = 20.0f;
            s.cheek[i].centre[2] = -25.0f;
            for (float& r : s.cheek[i].radius)
                r = 25.0f;
            s.cheek[i].core = 0.5f;
        }
        s.cleft.centre[1] = 20.0f;
        s.cleft.centre[2] = -25.0f;
        s.cleft.radius[0] = 6.0f;
        s.cleft.radius[1] = s.cleft.radius[2] = 25.0f;
        s.cleft.core = 0.3f;
        s.seam_x = 0.0f;
        s.seam_soft = 3.0f;
        const float left_peak[3] = { -20.0f, 30.0f, -25.0f };
        RearWeights w = rear_weights(s, left_peak);
        CHECK(near(w.cheek[0], 1.0f) && w.cheek[1] == 0.0f && w.cleft == 0.0f);
        const float mid[3] = { 0.0f, 20.0f, -25.0f };
        w = rear_weights(s, mid);
        CHECK(near(w.cheek[0], w.cheek[1]) && w.cheek[0] > 0.0f && w.cheek[0] <= 0.5f + 1e-4f);
        CHECK(near(w.cleft, 1.0f));
        const float far_[3] = { 0.0f, -60.0f, 40.0f };           // the front, the waist
        w = rear_weights(s, far_);
        CHECK(w.cheek[0] == 0.0f && w.cheek[1] == 0.0f && w.cleft == 0.0f);
        // Across the seam the other cheek's region does not reach.
        const float across[3] = { 6.0f, 20.0f, -25.0f };
        w = rear_weights(s, across);
        CHECK(w.cheek[0] == 0.0f && w.cheek[1] > 0.5f);

        const float offset[2][3] = { { 1.0f, 2.0f, 3.0f }, { -4.0f, 5.0f, -6.0f } };
        const float cleft[3] = { 0.0f, -2.0f, 0.0f };
        float out[3];
        RearWeights none;
        rear_deform(left_peak, none, offset, cleft, nullptr, 0, out);
        CHECK(out[0] == left_peak[0] && out[1] == left_peak[1] && out[2] == left_peak[2]);
        RearWeights one;
        one.cheek[0] = 1.0f;
        one.cleft = 0.5f;
        rear_deform(left_peak, one, offset, cleft, nullptr, 0, out);
        CHECK(near(out[0], -19.0f) && near(out[1], 31.0f) && near(out[2], -22.0f));
        RearDentSphere dent[2];
        dent[1].on = true;
        dent[1].centre[0] = -19.0f; dent[1].centre[1] = 30.0f; dent[1].centre[2] = -22.0f;
        dent[1].radius = 3.0f;
        rear_deform(left_peak, one, offset, cleft, dent, 2, out);   // after the offsets
        CHECK(near(out[1], 33.0f, 1e-3f) && near(out[0], -19.0f, 1e-3f));
        dent[1].on = false;
        rear_deform(left_peak, one, offset, cleft, dent, 2, out);
        CHECK(near(out[1], 31.0f));
    }
    // Rounding to the source shorts.
    {
        CHECK(round_short(1.49f) == 1 && round_short(1.5f) == 2 && round_short(-1.6f) == -2);
        CHECK(round_short(40000.0f) == 32767 && round_short(-40000.0f) == -32768);
        CHECK(round_short(NAN) == 0);
    }
    // The fit, on a made-up pair of cheeks: spheres of 10 cm at x +-8 cm,
    // y 2 cm, z -9 cm (peaks at y 12 cm), a belt at z 0 sticking out further
    // back (ignored: above the window), and her front.
    {
        std::vector<float> pts;
        auto add = [&](float x, float y, float z)
        {
            pts.push_back(x * metre);
            pts.push_back(y * metre);
            pts.push_back(z * metre);
        };
        for (int side = 0; side < 2; ++side)
        {
            const float cx = side ? 0.08f : -0.08f;
            for (int a = 0; a < 24; ++a)
                for (int b = 0; b < 12; ++b)
                {
                    const float th = 6.2831853f * (float)a / 24.0f;
                    const float ph = 3.1415927f * ((float)b + 0.5f) / 12.0f;
                    add(cx + 0.10f * std::sin(ph) * std::cos(th), 0.02f + 0.10f * std::sin(ph) * std::sin(th),
                        -0.09f + 0.10f * std::cos(ph));
                }
            add(cx, 0.12f, -0.09f);         // the very peak
        }
        for (int a = 0; a < 36; ++a)        // the belt
        {
            const float th = 6.2831853f * (float)a / 36.0f;
            add(0.17f * std::cos(th), 0.2f * std::sin(th), 0.0f);
        }
        RearFit fit;
        const bool ok = rear_fit(pts.data(), (int)pts.size() / 3, metre, fit);
        CHECK(ok && fit.ok && !fit.fallback);
        CHECK(fit.samples[0] > 12 && fit.samples[1] > 12);
        CHECK(near(fit.peak[0][1], 0.12f * metre, 0.06f));
        CHECK(fit.peak[0][1] > 0.11f * metre && fit.peak[1][1] > 0.11f * metre);
        CHECK(fit.peak[0][0] < -0.05f * metre && fit.peak[1][0] > 0.05f * metre);
        CHECK(std::fabs(fit.peak[0][2] + 0.09f * metre) < 0.02f * metre);
        CHECK(fit.centre[0][0] < 0.0f && fit.centre[1][0] > 0.0f);
        CHECK(std::fabs(fit.centre[0][0] + fit.centre[1][0]) < 0.005f * metre);   // symmetric
        CHECK(std::fabs(fit.seam_x) < 0.005f * metre);
        CHECK(fit.centre[0][1] < fit.peak[0][1]);                   // inside the skin
        for (int s = 0; s < 2; ++s)
            for (int k = 0; k < 3; ++k)
                CHECK(fit.radius[s][k] >= 0.05f * metre - 0.01f && fit.radius[s][k] <= 0.15f * metre + 0.01f);
        // At the default sliders the peak moves fully, her front not at all.
        RearAdjust adj;
        RearShape shape;
        rear_shape_from_fit(fit, adj, metre, shape);
        RearWeights w = rear_weights(shape, fit.peak[1]);
        CHECK(w.cheek[1] > 0.95f && w.cheek[0] == 0.0f);
        const float front[3] = { 0.08f * metre, -0.08f * metre, -0.09f * metre };
        w = rear_weights(shape, front);
        CHECK(w.cheek[0] == 0.0f && w.cheek[1] == 0.0f);
        const float waist[3] = { 0.08f * metre, 0.08f * metre, 0.12f * metre };
        w = rear_weights(shape, waist);
        CHECK(w.cheek[1] == 0.0f);
        // Spread and lift move it from where it was fitted; 8 cm = as fitted.
        CHECK(near(shape.cheek[1].centre[0], fit.centre[1][0]));
        adj.spread_cm = 10.0f;
        adj.lift_cm = -1.0f;
        adj.size = 2.0f;
        adj.width = 0.5f;
        RearShape moved;
        rear_shape_from_fit(fit, adj, metre, moved);
        CHECK(near(moved.cheek[1].centre[0], fit.centre[1][0] + 0.02f * metre));
        CHECK(near(moved.cheek[0].centre[0], fit.centre[0][0] - 0.02f * metre));
        CHECK(near(moved.cheek[0].centre[2], fit.centre[0][2] - 0.01f * metre));
        CHECK(near(moved.cheek[0].radius[0], fit.radius[0][0]) &&
              near(moved.cheek[0].radius[1], 2.0f * fit.radius[0][1]));
        CHECK(near(moved.cleft.radius[0], 0.02f * metre) && near(moved.cleft.centre[0], fit.seam_x));
        // The touch zone holds the peak, its back edge a little past it.
        float zc[3], zs[3], depth = 0.0f, n[3];
        rear_touch_zone(fit, RearAdjust(), 1, metre, 0.0f, zc, zs);
        CHECK(ellipsoid_contact(fit.peak[1], zc, zs, 0.0f, &depth, n));
        const float behind[3] = { fit.peak[1][0], fit.peak[1][1] + 0.02f * metre, fit.peak[1][2] };
        CHECK(!ellipsoid_contact(behind, zc, zs, 0.0f, &depth, n));
        float zs2[3], zc2[3];
        rear_touch_zone(fit, RearAdjust(), 1, metre, 2.0f, zc2, zs2);     // grown 2 cm
        CHECK(ellipsoid_contact(behind, zc2, zs2, 0.0f, &depth, n) && n[1] > 0.9f);
    }
    // No mesh to fit: the nominal shape, said so.
    {
        RearFit fit;
        CHECK(!rear_fit(nullptr, 0, metre, fit));
        CHECK(fit.fallback && !fit.ok);
        CHECK(fit.centre[0][0] < 0.0f && fit.centre[1][0] > 0.0f && fit.radius[0][1] > 0.0f);
        const float few[6] = { 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f };
        CHECK(!rear_fit(few, 2, metre, fit) && fit.fallback);
    }
    // Ellipsoid contact: depth and outward normal.
    {
        const float c[3] = { 0.0f, 0.0f, 0.0f };
        const float s[3] = { 10.0f, 20.0f, 30.0f };
        float depth = 0.0f, n[3];
        const float p[3] = { 0.0f, 18.0f, 0.0f };
        CHECK(ellipsoid_contact(p, c, s, 0.0f, &depth, n));
        CHECK(near(n[1], 1.0f) && near(depth, 2.1111f, 0.01f));   // (1 - 0.81) / (2 x 18/400)
        CHECK(ellipsoid_contact(c, c, s, 0.0f, &depth, n) && near(depth, 10.0f));
        const float q[3] = { 11.0f, 0.0f, 0.0f };
        CHECK(!ellipsoid_contact(q, c, s, 0.0f, &depth, n));
        CHECK(ellipsoid_contact(q, c, s, 2.0f, &depth, n) && near(n[0], 1.0f));
    }
    // The pelvis frame: round trips through a turned, scaled matrix.
    {
        // 90 deg about z, scale 2, at (5, 6, 7): local x -> world +y.
        const float m[16] = { 0, 2, 0, 0,  -2, 0, 0, 0,  0, 0, 2, 0,  5, 6, 7, 1 };
        const float local[3] = { 1.0f, 2.0f, 3.0f };
        float w[3], back[3];
        local_point_to_world(m, local, w);
        CHECK(near(w[0], 1.0f) && near(w[1], 8.0f) && near(w[2], 13.0f));
        CHECK(world_point_to_local(m, w, back));
        CHECK(near(back[0], 1.0f) && near(back[1], 2.0f) && near(back[2], 3.0f));
        const float dir[3] = { 0.0f, 4.0f, 0.0f };                 // world +y = local +x, scale 2
        CHECK(world_dir_to_local(m, dir, back) && near(back[0], 2.0f) && near(back[1], 0.0f));
        float d2[3];
        local_dir_to_world(m, back, d2);
        CHECK(near(d2[1], 4.0f) && near(d2[0], 0.0f));
        CHECK(near(matrix_scale(m), 2.0f));
        const float zero[16] = {};
        CHECK(!world_dir_to_local(zero, dir, back) && matrix_scale(zero) == 0.0f);
    }
    // Baseline fullness: outward from the cheek's centre by weight x
    // amount, straight back at the centre, nothing outside the region or
    // on the cleft, in (negative) the other way.
    {
        RearShape s;
        s.cheek[0].centre[0] = -8.0f;
        s.cheek[0].radius[0] = s.cheek[0].radius[1] = s.cheek[0].radius[2] = 10.0f;
        s.cheek[1] = s.cheek[0];
        s.cheek[1].centre[0] = 8.0f;
        s.cleft.radius[0] = s.cleft.radius[1] = s.cleft.radius[2] = 1.0f;
        RearWeights w;
        float delta[3] = {};
        const float centre[3] = { -8.0f, 0.0f, 0.0f };
        w.cheek[0] = 1.0f;
        rear_inflate(s, w, centre, 2.0f, delta);
        CHECK(near(delta[0], 0.0f) && near(delta[1], 2.0f) && near(delta[2], 0.0f));
        float d2[3] = {};
        const float side[3] = { -8.0f - 9.0f, 0.0f, 0.0f };
        rear_inflate(s, w, side, 2.0f, d2);
        CHECK(d2[0] < -0.5f && d2[1] > 0.0f);           // out to her side, a little back
        float d3[3] = {};
        w.cheek[0] = 0.5f;
        rear_inflate(s, w, centre, 2.0f, d3);
        CHECK(near(d3[1], 1.0f));                       // scaled by the weight
        float d4[3] = {};
        w.cheek[0] = 0.0f;
        w.cleft = 1.0f;
        rear_inflate(s, w, centre, 2.0f, d4);
        CHECK(d4[0] == 0.0f && d4[1] == 0.0f && d4[2] == 0.0f);
        float d5[3] = {};
        w.cheek[0] = 1.0f;
        rear_inflate(s, w, centre, -2.0f, d5);
        CHECK(near(d5[1], -2.0f));
        float d6[3] = {};
        rear_inflate(s, w, centre, 0.0f, d6);
        CHECK(d6[1] == 0.0f);
    }
    // The absolute limit: limit x tanh(len / limit) -- nearly untouched when
    // small, never above the limit however far it is pulled, still growing
    // past it (no wall), direction kept, off at 0.
    {
        float s[3] = { 0.3f, 0.4f, 0.0f };          // length 0.5, limit 10
        soft_limit_vector(s, 10.0f);
        CHECK(near(s[0], 0.3f, 0.002f) && near(s[1], 0.4f, 0.002f));
        float a[3] = { 3.0f, 4.0f, 0.0f };          // length 5: 10 tanh(0.5)
        soft_limit_vector(a, 10.0f);
        CHECK(near(std::sqrt(a[0] * a[0] + a[1] * a[1]), 10.0f * std::tanh(0.5f)) &&
              near(a[0] / a[1], 0.75f));
        float f[3] = { 15.0f, 0.0f, 0.0f }, g[3] = { 20.0f, 0.0f, 0.0f };
        soft_limit_vector(f, 10.0f);
        soft_limit_vector(g, 10.0f);
        CHECK(g[0] > f[0] + 0.3f && g[0] < 10.0f);  // past the limit it still moves
        float b[3] = { 0.0f, 0.0f, 1000.0f };
        soft_limit_vector(b, 10.0f);
        CHECK(b[2] <= 10.0f && b[2] > 9.9f && b[0] == 0.0f);
        float c[3] = { 6.0f, 8.0f, 0.0f };          // length 10: on the limit
        soft_limit_vector(c, 10.0f);
        const float len = std::sqrt(c[0] * c[0] + c[1] * c[1]);
        CHECK(near(len, 10.0f * std::tanh(1.0f)) && near(c[0] / c[1], 0.75f));
        float d[3] = { 50.0f, 0.0f, 0.0f };
        soft_limit_vector(d, 0.0f);
        CHECK(near(d[0], 50.0f));
        float e[3] = { NAN, 1.0f, 0.0f };
        soft_limit_vector(e, 10.0f);
        CHECK(e[0] == 0.0f && e[1] == 0.0f);
    }
    // The shown offset: gains, eased toward 1 by a hand, inward scaled, the
    // test push added along +y.
    {
        const float gain[3] = { 0.5f, 1.0f, 0.0f };
        float out[3];
        const float lag[3] = { 2.0f, -4.0f, 3.0f };
        cheek_offset(lag, gain, 0.0f, 0.5f, 0.0f, out);
        CHECK(near(out[0], 1.0f) && near(out[1], -2.0f) && near(out[2], 0.0f));
        cheek_offset(lag, gain, 1.0f, 0.5f, 0.0f, out);
        CHECK(near(out[0], 2.0f) && near(out[1], -2.0f) && near(out[2], 3.0f));
        const float back[3] = { 0.0f, 4.0f, 0.0f };
        cheek_offset(back, gain, 0.0f, 0.0f, 10.0f, out);
        CHECK(near(out[1], 14.0f));
    }
}
