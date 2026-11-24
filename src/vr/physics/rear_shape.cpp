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
        const float kTouchBehind = 0.01f;                   // touch zone's back edge past the peak
        const float kTouchSide = 0.02f;                     // ... and past the cap at the sides
        const float kTouchDepth = 0.08f;                    // its depth semi-axis
        // Landmarks: how far the back outline / column falls below the
        // peak's depth at the glute's outer edge, the hip side and the
        // crease; the regions' margin past the glute and depth inside it.
        const float kEdgeDrop = 0.025f;
        const float kHipDrop = 0.06f;
        const float kFoldDrop = 0.03f;
        const float kRegionMargin = 1.3f;
        const float kRegionInside = 0.05f;
        const float kRegionDepth = 0.12f;           // depth radius: the surface moves ~0.9
        // The teardrop: centre this far up from the crease (of the crease-
        // to-top height), reaching kBelowFold under the crease with the
        // quartic fall, kUpReach x the centre-to-top height above it, the
        // top bent kLean toward her front.
        const float kCentreUp = 0.4f;
        const float kBelowFold = 0.005f;
        const float kUpReach = 1.3f;
        const float kLean = 0.03f;
        // Rest-shape fields (metres): the flare's fade above / below the
        // glute and toward her front; the cleft close's rise and fall.
        const float kFlareFade = 0.06f;
        const float kFlareFront = -0.06f;
        const float kCloseRise = 0.03f;
        const float kCloseFall = 0.06f;

        // The region round one glute from its landmarks (metres, signed x).
        void place(int side, RearFit& out)
        {
            const float edge = out.edge_x[side];
            out.centre[side][0] = 0.5f * edge;
            out.centre[side][1] = out.peak[side][1] - kRegionInside;
            const float fold = out.fold_z[side], top = out.top_z[side];
            const float cz = fold + kCentreUp * (top - fold);
            out.centre[side][2] = cz;
            out.radius[side][0] = clampf(kRegionMargin * 0.5f * std::fabs(edge), 0.04f, 0.16f);
            out.radius[side][1] = kRegionDepth;
            out.radius[side][2] = clampf(cz - fold + kBelowFold, 0.04f, 0.14f);
            out.up[side] = clampf(kUpReach * (top - cz), 0.05f, 0.20f);
            out.lean[side] = kLean;
        }

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
            out.edge_x[side] = sign * 0.14f;
            out.hip_x[side] = sign * 0.16f;
            out.fold_z[side] = -0.20f;
            out.top_z[side] = -0.035f;
            out.landmarks[side] = false;
            place(side, out);
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
                out.edge_x[s] *= metre;
                out.hip_x[s] *= metre;
                out.fold_z[s] *= metre;
                out.top_z[s] *= metre;
                out.up[s] *= metre;
                out.lean[s] *= metre;
            }
            out.seam_x *= metre;
        }
    }

    void region_local(const RearRegion& r, const float p[3], float t[3])
    {
        const float dz = p[2] - r.centre[2];
        const float up = r.up > 1e-3f ? r.up : r.radius[2];
        const float rz = dz > 0.0f ? up : r.radius[2];
        float axis_y = r.centre[1];
        if (dz > 0.0f && r.lean != 0.0f && up > 1e-3f)
        {
            const float f = dz / up;
            axis_y -= r.lean * f * f;
        }
        const float d[3] = { p[0] - r.centre[0], p[1] - axis_y, dz };
        const float rad[3] = { r.radius[0], r.radius[1], rz };
        for (int k = 0; k < 3; ++k)
            t[k] = d[k] * (rad[k] > 1e-3f ? 1.0f / rad[k] : 1e3f);
    }

    float region_weight(const RearRegion& r, const float p[3])
    {
        float t[3];
        region_local(r, p, t);
        const float tz2 = t[2] * t[2];
        const float d2 = t[0] * t[0] + t[1] * t[1] +
                         (r.sharp_bottom && t[2] < 0.0f ? tz2 * tz2 : tz2);
        const float core = clampf(r.core, 0.0f, 0.95f);
        return smooth01((1.0f - d2) / (1.0f - core * core));
    }

    void region_outline(const RearRegion& r, int u, int v, float a, float scale, float out[3])
    {
        // Unit-circle point in the region's own units, onto the d^2 = s^2
        // surface (tz^4 below with sharp_bottom), then back to the mesh.
        float t[3] = {};
        t[u] = std::cos(a) * scale;
        t[v] = std::sin(a) * scale;
        if (r.sharp_bottom && t[2] < 0.0f)
            t[2] = -std::sqrt(-t[2]);           // tz^4 = (s sin a)^2: tz^2 = s |sin a|
        const float up = r.up > 1e-3f ? r.up : r.radius[2];
        const float dz = t[2] * (t[2] > 0.0f ? up : r.radius[2]);
        float axis_y = r.centre[1];
        if (dz > 0.0f && r.lean != 0.0f && up > 1e-3f)
            axis_y -= r.lean * (dz / up) * (dz / up);
        out[0] = r.centre[0] + t[0] * r.radius[0];
        out[1] = axis_y + t[1] * r.radius[1];
        out[2] = r.centre[2] + dz;
    }

    float seam_weight(float x, float seam_x, float seam_soft, int side, float inset)
    {
        if (!(seam_soft > 0.0f))
            return 1.0f;
        const float sign = side ? 1.0f : -1.0f;
        return smooth01(0.5f + (sign * (x - seam_x) - (inset > 0.0f ? inset : 0.0f)) /
                                   (2.0f * seam_soft));
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
            w.cheek[i] = r > 0.0f ? r * seam_weight(p[0], s.seam_x, s.seam_soft, i, s.seam_inset)
                                  : 0.0f;
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
            region_local(r, p, d);
            d[1] += 0.6f;                           // toward her back
            const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (!(len > 1e-4f))
                continue;
            for (int k = 0; k < 3; ++k)
                delta[k] += w.cheek[i] * amount * d[k] / len;
        }
    }

    float smooth_between(float v, float lo, float hi)
    {
        if (!(hi > lo))
            return v >= hi ? 1.0f : 0.0f;
        return smooth01((v - lo) / (hi - lo));
    }

    namespace
    {
        // Full from zlo to zhi, fading over `fade` above and below.
        float flare_height(const RearShape& s, float z)
        {
            return smooth_between(z, s.flare_zlo - s.flare_fade, s.flare_zlo) *
                   (1.0f - smooth_between(z, s.flare_zhi, s.flare_zhi + s.flare_fade));
        }
    }

    void rear_flare(const RearShape& s, const RearWeights& w, const float p[3], float amount,
                    float delta[3])
    {
        (void)w;
        if (!std::isfinite(amount) || amount == 0.0f)
            return;
        const int side = p[0] >= s.seam_x ? 1 : 0;
        const float sign = side ? 1.0f : -1.0f;
        const float d = sign * (p[0] - s.seam_x);
        const float f = smooth_between(d, s.flare_x0[side], s.flare_x1[side]) *
                        flare_height(s, p[2]) * smooth_between(p[1], s.flare_y0, s.flare_y1);
        delta[0] += sign * amount * f;
    }

    void rear_cleft_close(const RearShape& s, const float p[3], float amount, float delta[3])
    {
        if (!std::isfinite(amount) || amount == 0.0f)
            return;
        const int side = p[0] >= s.seam_x ? 1 : 0;
        const float sign = side ? 1.0f : -1.0f;
        const float d = sign * (p[0] - s.seam_x);
        // Rising over at least twice the amount keeps every point on its own
        // side (x - amount x c(x) stays increasing): the cheeks meet, never cross.
        const float rise = fmaxf(s.close_x1, 2.0f * std::fabs(amount));
        const float fall = s.close_x2 - s.close_x1;
        const float c = smooth_between(d, 0.0f, rise) * (1.0f - smooth_between(d, rise, rise + fall));
        const float f = c * flare_height(s, p[2]) * smooth_between(p[1], s.close_y0, s.close_y1);
        delta[0] -= sign * amount * f;
    }

    void rear_baseline(const RearShape& s, const RearWeights& w, const float p[3], float fullness,
                       float flare, float cleft_close, float delta[3])
    {
        rear_inflate(s, w, p, fullness, delta);
        rear_flare(s, w, p, flare, delta);
        rear_cleft_close(s, p, cleft_close, delta);
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
            // The landmarks. The back outline at the peak's height (the
            // back-most point per 1 cm of |x|), walked out from the peak:
            // the glute's outer edge, then the hip side. Gear (pouches,
            // holsters) sits above this band or off her back, so it does
            // not count; empty bins are stepped over.
            {
                const int kBins = 30;
                float outline[kBins];
                for (float& v : outline)
                    v = -1e9f;
                float column[kBins];               // per 1 cm down from the peak
                for (float& v : column)
                    v = -1e9f;
                for (int i = 0; i < count; ++i)
                {
                    const float* p = points + 3 * i;
                    const float x = sign * p[0] * inv, y = p[1] * inv, z = p[2] * inv;
                    if (!(y > 0.0f) || !std::isfinite(x) || !std::isfinite(z))
                        continue;
                    if (x > 0.0f && std::fabs(z - peak[2]) <= 0.03f)
                    {
                        const int b = (int)(x / 0.01f);
                        if (b < kBins)
                            outline[b] = fmaxf(outline[b], y);
                    }
                    if (std::fabs(x - sign * peak[0]) <= 0.025f && z <= peak[2])
                    {
                        const int b = (int)((peak[2] - z) / 0.01f);
                        if (b < kBins)
                            column[b] = fmaxf(column[b], y);
                    }
                }
                const auto cross = [&](const float* bins, int first, float level,
                                       float* at) -> bool
                {
                    float px = -1.0f, py = 0.0f;
                    for (int b = first; b < kBins; ++b)
                    {
                        if (bins[b] < -1e8f)
                            continue;
                        const float bx = (float)b * 0.01f + 0.005f, by = bins[b];
                        if (by < level)
                        {
                            *at = px < 0.0f ? bx
                                : px + (bx - px) * clampf((py - level) / fmaxf(py - by, 1e-6f),
                                                          0.0f, 1.0f);
                            return true;
                        }
                        px = bx;
                        py = by;
                    }
                    return false;
                };
                const int b0 = (int)clampf(sign * peak[0] / 0.01f, 0.0f, (float)(kBins - 1));
                float edge = 0.0f, hip = 0.0f, fold = 0.0f;
                const bool has_edge = cross(outline, b0, peak[1] - kEdgeDrop, &edge);
                const bool has_hip = has_edge && cross(outline, b0, peak[1] - kHipDrop, &hip);
                const bool has_fold = cross(column, 0, peak[1] - kFoldDrop, &fold);
                if (!has_edge)
                    edge = sign * peak[0] + 0.08f;
                if (!has_hip)
                    hip = edge + 0.025f;
                out.edge_x[side] = sign * clampf(edge, 0.06f, 0.20f);
                out.hip_x[side] = sign * clampf(fmaxf(hip, edge), 0.06f, 0.23f);
                out.fold_z[side] = has_fold ? peak[2] - clampf(fold, 0.04f, 0.18f)
                                            : peak[2] - 0.11f;
                out.top_z[side] = peak[2] + fminf(0.5f * (peak[2] - out.fold_z[side]), 0.08f);
                out.landmarks[side] = has_edge && has_fold;
            }
            place(side, out);
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
            r.up = fmaxf(fit.up[i] * shape[2], 0.5f * cm);
            r.lean = fit.lean[i] * shape[2];
            r.sharp_bottom = true;
            r.centre[0] += sign * (adj.spread_cm - kSpreadDefaultCm) * cm;
            r.centre[2] += adj.lift_cm * cm;
            r.core = clampf(adj.core, 0.0f, 0.95f);
        }
        out.seam_x = fit.seam_x;
        out.seam_soft = adj.seam_cm > 0.0f ? adj.seam_cm * cm : 0.0f;
        out.seam_inset = adj.seam_inset_cm > 0.0f ? adj.seam_inset_cm * cm : 0.0f;
        // The rest-shape fields from the landmarks (moved up / down with
        // the regions).
        const float start = adj.flare_start_cm > 0.0f ? adj.flare_start_cm * cm : 0.0f;
        for (int i = 0; i < 2; ++i)
        {
            out.flare_x0[i] = start;
            out.flare_x1[i] = fmaxf(std::fabs(fit.edge_x[i] - fit.seam_x), start + 2.0f * cm);
        }
        out.flare_zlo = fminf(fit.fold_z[0], fit.fold_z[1]) + adj.lift_cm * cm;
        out.flare_zhi = fmaxf(fit.top_z[0], fit.top_z[1]) + adj.lift_cm * cm;
        out.flare_fade = kFlareFade * metre;
        out.flare_y0 = kFlareFront * metre;
        out.flare_y1 = 0.0f;
        out.close_x1 = kCloseRise * metre;
        out.close_x2 = (kCloseRise + kCloseFall) * metre;
        const float peak_y = 0.5f * (fit.peak[0][1] + fit.peak[1][1]);
        out.close_y0 = peak_y - 8.0f * cm;
        out.close_y1 = peak_y - 4.0f * cm;
        // The cleft: a narrow band round her midline over the cheeks' depth
        // and height.
        RearRegion& c = out.cleft;
        c.centre[0] = fit.seam_x;
        for (int k = 1; k < 3; ++k)
        {
            c.centre[k] = 0.5f * (out.cheek[0].centre[k] + out.cheek[1].centre[k]);
            c.radius[k] = 0.5f * (out.cheek[0].radius[k] + out.cheek[1].radius[k]);
        }
        c.up = 0.5f * (out.cheek[0].up + out.cheek[1].up);
        c.lean = 0.5f * (out.cheek[0].lean + out.cheek[1].lean);
        c.sharp_bottom = true;
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
                      float test_push, float out[3], float event)
    {
        const float b = clampf(boost, 0.0f, 1.0f);
        const float e = clampf(event, 0.0f, 1.0f);
        for (int r = 0; r < 3; ++r)
        {
            float g = gain[r] + (1.0f - gain[r]) * b;
            if (gain[r] < 1.0f)
                g += (1.0f - g) * e;
            if (r == 1 && lag_local[r] < 0.0f)
                g *= inward;
            out[r] = lag_local[r] * g;
        }
        out[1] += test_push;
    }

    float event_ramp(const float lag[3], float ramp, float max_lag)
    {
        if (!(ramp > 0.0f))
            return 0.0f;
        const float length = fminf(ramp, 0.6f * fmaxf(max_lag, 0.0f));
        if (!(length > 0.0f))
            return 0.0f;
        const float len = std::sqrt(lag[0] * lag[0] + lag[1] * lag[1] + lag[2] * lag[2]);
        return smooth01(len / length);
    }
}
}
