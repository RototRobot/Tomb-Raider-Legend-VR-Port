#include "rear_shape.h"

#include <cmath>

namespace trlvr
{
namespace rear
{
    namespace
    {
        float clampf(float v, float lo, float hi)
        {
            return v < lo ? lo : (v > hi ? hi : v);
        }

        float smooth01(float u)
        {
            u = clampf(u, 0.0f, 1.0f);
            return u * u * (3.0f - 2.0f * u);
        }

        // The fit's constants, metres.
        const float kFitZLo = -0.20f, kFitZHi = -0.02f;     // below the belt, above the thigh
        const float kFitXLo = 0.015f, kFitXHi = 0.17f;      // off the midline, inside the hip
        const float kCapDepth = 0.025f;                     // skin within this of the peak
        const float kDepthRadius = 0.09f;
        const float kCentreInside = 0.42f;                  // x kDepthRadius inside the peak
        const float kTouchBehind = 0.01f;                   // touch zone's back edge past the peak
        const float kTouchSide = 0.02f;                     // ... and past the cap at the sides
        const float kTouchDepth = 0.08f;                    // its depth semi-axis

        // The nominal cheek (metres) when her mesh does not fit.
        void nominal(int side, RearFit& out)
        {
            const float sign = side ? 1.0f : -1.0f;
            out.peak[side][0] = sign * 0.085f;
            out.peak[side][1] = 0.108f;
            out.peak[side][2] = -0.09f;
            out.cap_lo[side][0] = sign > 0.0f ? 0.03f : -0.14f;
            out.cap_hi[side][0] = sign > 0.0f ? 0.14f : -0.03f;
            out.cap_lo[side][1] = -0.15f;
            out.cap_hi[side][1] = -0.03f;
            out.centre[side][0] = sign * 0.085f;
            out.centre[side][1] = 0.108f - kCentreInside * kDepthRadius;
            out.centre[side][2] = -0.09f;
            out.radius[side][0] = 0.09f;
            out.radius[side][1] = kDepthRadius;
            out.radius[side][2] = 0.10f;
        }

        void scale_fit(RearFit& out, float metre)
        {
            for (int s = 0; s < 2; ++s)
            {
                for (int k = 0; k < 3; ++k)
                {
                    out.peak[s][k] *= metre;
                    out.centre[s][k] *= metre;
                    out.radius[s][k] *= metre;
                }
                for (int k = 0; k < 2; ++k)
                {
                    out.cap_lo[s][k] *= metre;
                    out.cap_hi[s][k] *= metre;
                }
            }
            out.seam_x *= metre;
        }
    }

    float region_weight(const RearRegion& r, const float p[3])
    {
        float d2 = 0.0f;
        for (int k = 0; k < 3; ++k)
        {
            const float inv = r.radius[k] > 1e-3f ? 1.0f / r.radius[k] : 1e3f;
            const float t = (p[k] - r.centre[k]) * inv;
            d2 += t * t;
        }
        const float core = clampf(r.core, 0.0f, 0.95f);
        return smooth01((1.0f - d2) / (1.0f - core * core));
    }

    float seam_weight(float x, float seam_x, float seam_soft, int side)
    {
        if (!(seam_soft > 0.0f))
            return 1.0f;
        const float sign = side ? 1.0f : -1.0f;
        return smooth01(0.5f + sign * (x - seam_x) / (2.0f * seam_soft));
    }

    bool dent_push(float p[3], const float centre[3], float radius)
    {
        if (!(radius > 0.0f))
            return false;
        float d[3], len2 = 0.0f;
        for (int k = 0; k < 3; ++k)
        {
            d[k] = p[k] - centre[k];
            len2 += d[k] * d[k];
        }
        // As the shader: eps keeps the direction finite (a point on the
        // centre stays where it is).
        const float len = std::sqrt(len2 + 1e-6f);
        const float in = radius - len;
        if (!(in > 0.0f))
            return false;
        for (int k = 0; k < 3; ++k)
            p[k] += d[k] / len * in;
        return true;
    }

    RearWeights rear_weights(const RearShape& s, const float p[3])
    {
        RearWeights w;
        for (int i = 0; i < 2; ++i)
        {
            const float r = region_weight(s.cheek[i], p);
            w.cheek[i] = r > 0.0f ? r * seam_weight(p[0], s.seam_x, s.seam_soft, i) : 0.0f;
        }
        w.cleft = region_weight(s.cleft, p);
        return w;
    }

    void rear_deform(const float bind[3], const RearWeights& w, const float offset[2][3],
                     const float cleft_offset[3], const RearDentSphere* dents, int dent_count,
                     float out[3])
    {
        for (int k = 0; k < 3; ++k)
            out[k] = bind[k] + w.cheek[0] * offset[0][k] + w.cheek[1] * offset[1][k] +
                     w.cleft * cleft_offset[k];
        for (int j = 0; dents && j < dent_count; ++j)
            if (dents[j].on)
                dent_push(out, dents[j].centre, dents[j].radius);
    }

    void rear_inflate(const RearShape& s, const RearWeights& w, const float p[3], float amount,
                      float delta[3])
    {
        if (!std::isfinite(amount) || amount == 0.0f)
            return;
        for (int i = 0; i < 2; ++i)
        {
            if (!(w.cheek[i] > 0.0f))
                continue;
            const RearRegion& r = s.cheek[i];
            float d[3];
            for (int k = 0; k < 3; ++k)
                d[k] = (p[k] - r.centre[k]) / fmaxf(r.radius[k], 1e-3f);
            d[1] += 0.6f;                           // toward her back
            const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (!(len > 1e-4f))
                continue;
            for (int k = 0; k < 3; ++k)
                delta[k] += w.cheek[i] * amount * d[k] / len;
        }
    }

    void soft_limit_vector(float v[3], float limit)
    {
        if (!(limit > 0.0f))
            return;
        const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (!std::isfinite(len))
        {
            v[0] = v[1] = v[2] = 0.0f;
            return;
        }
        // limit x tanh(len / limit) all the way (as the chest's soft parts):
        // one to one when small, bending gradually from about half the
        // limit, never reaching it. A knee late in the range (it was 0.8)
        // squeezes everything past it into the last few millimetres, so a
        // big swing looks as if it hit a wall.
        if (!(len > 1e-9f))
            return;
        const float scale = limit * std::tanh(len / limit) / len;
        for (int k = 0; k < 3; ++k)
            v[k] *= scale;
    }

    short round_short(float v)
    {
        if (!std::isfinite(v))
            return 0;
        const float r = std::floor(v + 0.5f);
        return (short)clampf(r, -32768.0f, 32767.0f);
    }

    bool rear_fit(const float* points, int count, float metre, RearFit& out)
    {
        out = RearFit();
        if (!(metre > 0.0f))
            metre = 291.0f;
        const float inv = 1.0f / metre;
        bool ok = points && count > 0;
        for (int side = 0; side < 2 && ok; ++side)
        {
            const float sign = side ? 1.0f : -1.0f;
            const auto use = [&](const float* p) -> bool
            {
                const float x = sign * p[0] * inv, y = p[1] * inv, z = p[2] * inv;
                return std::isfinite(x) && std::isfinite(y) && std::isfinite(z) &&
                       x >= kFitXLo && x <= kFitXHi && z >= kFitZLo && z <= kFitZHi && y > 0.0f;
            };
            int n = 0;
            for (int i = 0; i < count; ++i)
                if (use(points + 3 * i))
                    ++n;
            out.samples[side] = n;
            if (n < 12)
            {
                ok = false;
                break;
            }
            // The peak: the mean of the few rearmost points (one stray
            // vertex does not make it).
            const int K = n / 20 < 3 ? 3 : (n / 20 > 8 ? 8 : n / 20);
            int top[8];
            int have = 0;
            for (int i = 0; i < count; ++i)
            {
                if (!use(points + 3 * i))
                    continue;
                const float y = points[3 * i + 1];
                if (have < K)
                {
                    top[have++] = i;
                    continue;
                }
                int low = 0;
                for (int t = 1; t < have; ++t)
                    if (points[3 * top[t] + 1] < points[3 * top[low] + 1])
                        low = t;
                if (y > points[3 * top[low] + 1])
                    top[low] = i;
            }
            float peak[3] = {};
            for (int t = 0; t < have; ++t)
                for (int k = 0; k < 3; ++k)
                    peak[k] += points[3 * top[t] + k] * inv / (float)have;
            if (peak[1] < 0.03f || peak[1] > 0.25f)
            {
                ok = false;
                break;
            }
            // The cap: the skin within kCapDepth of the peak's depth.
            float xlo = 1e9f, xhi = -1e9f, zlo = 1e9f, zhi = -1e9f;
            int cap = 0;
            for (int i = 0; i < count; ++i)
            {
                const float* p = points + 3 * i;
                if (!use(p) || p[1] * inv < peak[1] - kCapDepth)
                    continue;
                ++cap;
                xlo = fminf(xlo, p[0] * inv);
                xhi = fmaxf(xhi, p[0] * inv);
                zlo = fminf(zlo, p[2] * inv);
                zhi = fmaxf(zhi, p[2] * inv);
            }
            if (cap < 4)
            {
                ok = false;
                break;
            }
            for (int k = 0; k < 3; ++k)
                out.peak[side][k] = peak[k];
            out.cap_lo[side][0] = xlo;
            out.cap_hi[side][0] = xhi;
            out.cap_lo[side][1] = zlo;
            out.cap_hi[side][1] = zhi;
            out.centre[side][0] = 0.5f * (xlo + xhi);
            out.centre[side][1] = peak[1] - kCentreInside * kDepthRadius;
            // Centred on the peak's height, not the cap's middle: the cap
            // runs down the back of the thigh (the simulator log had its
            // middle 4.5 cm below the peak, a region sitting on her leg).
            out.centre[side][2] = peak[2];
            out.radius[side][0] = clampf(1.5f * 0.5f * (xhi - xlo), 0.05f, 0.14f);
            out.radius[side][1] = kDepthRadius;
            out.radius[side][2] = clampf(1.5f * 0.5f * (zhi - zlo), 0.06f, 0.15f);
        }
        if (ok)
            out.seam_x = clampf(0.5f * (out.centre[0][0] + out.centre[1][0]), -0.02f, 0.02f);
        else
        {
            const int s0 = out.samples[0], s1 = out.samples[1];
            out = RearFit();
            out.samples[0] = s0;
            out.samples[1] = s1;
            out.fallback = true;
            nominal(0, out);
            nominal(1, out);
            out.seam_x = 0.0f;
        }
        out.ok = ok;
        scale_fit(out, metre);
        return ok;
    }

    void rear_shape_from_fit(const RearFit& fit, const RearAdjust& adj, float metre, RearShape& out)
    {
        const float cm = metre * 0.01f;
        const float shape[3] = { adj.size * adj.width, adj.size, adj.size * adj.height };
        for (int i = 0; i < 2; ++i)
        {
            const float sign = i ? 1.0f : -1.0f;
            RearRegion& r = out.cheek[i];
            for (int k = 0; k < 3; ++k)
            {
                r.centre[k] = fit.centre[i][k];
                r.radius[k] = fmaxf(fit.radius[i][k] * shape[k], 0.5f * cm);
            }
            r.centre[0] += sign * (adj.spread_cm - kSpreadDefaultCm) * cm;
            r.centre[2] += adj.lift_cm * cm;
            r.core = clampf(adj.core, 0.0f, 0.95f);
        }
        out.seam_x = fit.seam_x;
        out.seam_soft = adj.seam_cm > 0.0f ? adj.seam_cm * cm : 0.0f;
        // The cleft: a narrow band round her midline over the cheeks' depth
        // and height.
        RearRegion& c = out.cleft;
        c.centre[0] = fit.seam_x;
        for (int k = 1; k < 3; ++k)
        {
            c.centre[k] = 0.5f * (out.cheek[0].centre[k] + out.cheek[1].centre[k]);
            c.radius[k] = 0.5f * (out.cheek[0].radius[k] + out.cheek[1].radius[k]);
        }
        c.radius[0] = fmaxf(adj.cleft_width_cm, 0.5f) * cm;
        c.core = 0.3f;
    }

    void rear_touch_zone(const RearFit& fit, const RearAdjust& adj, int side, float metre,
                         float touch_cm, float centre[3], float semi[3])
    {
        side = side ? 1 : 0;
        const float cm = metre * 0.01f;
        const float sign = side ? 1.0f : -1.0f;
        centre[0] = 0.5f * (fit.cap_lo[side][0] + fit.cap_hi[side][0]) +
                    sign * (adj.spread_cm - kSpreadDefaultCm) * cm;
        centre[1] = fit.peak[side][1] + (kTouchBehind - kTouchDepth) * metre;
        centre[2] = 0.5f * (fit.cap_lo[side][1] + fit.cap_hi[side][1]) + adj.lift_cm * cm;
        semi[0] = 0.5f * (fit.cap_hi[side][0] - fit.cap_lo[side][0]) + kTouchSide * metre;
        semi[1] = kTouchDepth * metre;
        semi[2] = 0.5f * (fit.cap_hi[side][1] - fit.cap_lo[side][1]) + kTouchSide * metre;
        for (int k = 0; k < 3; ++k)
            semi[k] = fmaxf(semi[k] + touch_cm * cm, 2.0f * cm);
    }

    bool ellipsoid_contact(const float p[3], const float centre[3], const float semi[3],
                           float margin, float* depth, float normal[3])
    {
        float l[3], h[3], s2 = 0.0f;
        for (int k = 0; k < 3; ++k)
        {
            l[k] = p[k] - centre[k];
            h[k] = fmaxf(semi[k] + margin, 1e-3f);
            s2 += (l[k] / h[k]) * (l[k] / h[k]);
        }
        if (!(s2 <= 1.0f))
            return false;
        float g[3], g2 = 0.0f;
        for (int k = 0; k < 3; ++k)
        {
            g[k] = l[k] / (h[k] * h[k]);
            g2 += g[k] * g[k];
        }
        const float glen = std::sqrt(g2);
        if (glen > 1e-12f)
        {
            for (int k = 0; k < 3; ++k)
                normal[k] = g[k] / glen;
            *depth = (1.0f - s2) / (2.0f * glen);
        }
        else
        {
            normal[0] = 0.0f;
            normal[1] = 1.0f;           // the centre: out the back
            normal[2] = 0.0f;
            *depth = fminf(h[0], fminf(h[1], h[2]));
        }
        return true;
    }

    bool world_dir_to_local(const float m[16], const float v[3], float out[3])
    {
        for (int r = 0; r < 3; ++r)
        {
            const float* row = m + 4 * r;
            const float len2 = row[0] * row[0] + row[1] * row[1] + row[2] * row[2];
            if (!(len2 > 1e-12f) || !std::isfinite(len2))
                return false;
            out[r] = (v[0] * row[0] + v[1] * row[1] + v[2] * row[2]) / len2;
        }
        return true;
    }

    bool world_point_to_local(const float m[16], const float p[3], float out[3])
    {
        const float d[3] = { p[0] - m[12], p[1] - m[13], p[2] - m[14] };
        return world_dir_to_local(m, d, out);
    }

    void local_point_to_world(const float m[16], const float p[3], float out[3])
    {
        for (int j = 0; j < 3; ++j)
            out[j] = p[0] * m[j] + p[1] * m[4 + j] + p[2] * m[8 + j] + m[12 + j];
    }

    void local_dir_to_world(const float m[16], const float v[3], float out[3])
    {
        for (int j = 0; j < 3; ++j)
            out[j] = v[0] * m[j] + v[1] * m[4 + j] + v[2] * m[8 + j];
    }

    float matrix_scale(const float m[16])
    {
        float sum = 0.0f;
        for (int r = 0; r < 3; ++r)
        {
            const float* row = m + 4 * r;
            const float len = std::sqrt(row[0] * row[0] + row[1] * row[1] + row[2] * row[2]);
            if (!(len > 1e-6f) || !std::isfinite(len))
                return 0.0f;
            sum += len;
        }
        return sum / 3.0f;
    }

    void cheek_offset(const float lag_local[3], const float gain[3], float boost, float inward,
                      float test_push, float out[3])
    {
        const float b = clampf(boost, 0.0f, 1.0f);
        for (int r = 0; r < 3; ++r)
        {
            float g = gain[r] + (1.0f - gain[r]) * b;
            if (r == 1 && lag_local[r] < 0.0f)
                g *= inward;
            out[r] = lag_local[r] * g;
        }
        out[1] += test_push;
    }
}
}
