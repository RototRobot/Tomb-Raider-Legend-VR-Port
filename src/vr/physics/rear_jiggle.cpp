#include "rear_jiggle.h"

#include <cmath>

namespace trlvr
{
    void jiggle_step(JiggleState& s, const float target[3], float dt, const JiggleParams& p,
                     float offset[3], const JiggleContact* contact)
    {
        bool reset = !s.valid || !(dt > 0.0f && dt <= 0.25f);
        float jump2 = 0.0f;
        for (int k = 0; k < 3; ++k)
        {
            if (!std::isfinite(target[k]) || !std::isfinite(s.pos[k]) || !std::isfinite(s.vel[k]))
                reset = true;
            const float d = target[k] - s.target[k];
            jump2 += d * d;
        }
        if (jump2 > p.reset_distance * p.reset_distance)
            reset = true;
        if (reset)
        {
            for (int k = 0; k < 3; ++k)
            {
                s.pos[k] = target[k];
                s.vel[k] = 0.0f;
                s.target[k] = target[k];
                offset[k] = 0.0f;
            }
            s.boost = 0.0f;
            s.valid = std::isfinite(target[0]) && std::isfinite(target[1]) && std::isfinite(target[2]);
            return;
        }
        // A hand on it: the limit loosens (eased in fast, out slowly), a
        // grip makes the spring stiff and near-critical so the mass goes
        // where the hand takes it, the press moves where it is pulled to,
        // and an arriving hand's kick goes into its velocity once.
        const bool touched = contact && (contact->touched || contact->gripped);
        const bool gripped = contact && contact->gripped;
        {
            const float want = touched ? 1.0f : 0.0f;
            const float tau = want > s.boost ? 0.08f : 0.5f;
            s.boost += (want - s.boost) * (1.0f - std::exp(-dt / tau));
        }
        float shift[3] = {};
        if (contact)
            for (int k = 0; k < 3; ++k)
            {
                if (std::isfinite(contact->shift[k]))
                    shift[k] = contact->shift[k];
                if (std::isfinite(contact->kick[k]))
                    s.vel[k] += contact->kick[k];
            }
        // Fixed-size sub-steps (at most 1/480 s), the target moving linearly
        // across the frame; damping acts on the velocity relative to the
        // target, so steady motion leaves no lag and only changes of motion
        // set the mass swinging.
        const float kMaxStep = 1.0f / 480.0f;
        int steps = (int)std::ceil(dt / kMaxStep);
        if (steps < 1)
            steps = 1;
        if (steps > 128)
            steps = 128;
        const float h = dt / (float)steps;
        float hz = p.hz > 0.05f ? p.hz : 0.05f, damping = p.damping > 0.0f ? p.damping : 0.0f;
        if (gripped)
        {
            hz = hz * 2.0f > 6.0f ? hz * 2.0f : 6.0f;
            damping = damping > 0.9f ? damping : 0.9f;
        }
        const float omega = 6.2831853f * hz;
        const float k_spring = omega * omega;
        const float c_damp = 2.0f * damping * omega;
        float vt[3];
        for (int k = 0; k < 3; ++k)
            vt[k] = (target[k] - s.target[k]) / dt;
        // Past the limit the tissue stiffens: an extra spring (kStiffen x
        // the base one) and a near-critical damper on outward motion bring a
        // hard swing to a rounded stop instead of a wall. The mass itself is
        // held within three times the limit only as a safety for hitches the
        // reset misses; what is shown eases toward the limit (below).
        const float shown_limit = (p.max_offset > 0.0f ? p.max_offset : 0.0f) +
                                  s.boost * (p.touch_offset > 0.0f ? p.touch_offset : 0.0f);
        const float limit = 3.0f * shown_limit;
        const float kStiffen = 6.0f;
        const float k_stiff = kStiffen * k_spring;
        const float c_stiff = 2.0f * std::sqrt(kStiffen) * omega;
        for (int i = 1; i <= steps; ++i)
        {
            // The force from the sub-step's starting target (semi-implicit
            // Euler then leaves no lag behind a steadily moving target), the
            // limit against its end.
            const float f0 = (float)(i - 1) / (float)steps, f = (float)i / (float)steps;
            float tgt[3], d[3], t0[3], a[3];
            float lag0[3], lag0_len2 = 0.0f;
            for (int k = 0; k < 3; ++k)
            {
                t0[k] = s.target[k] + (target[k] - s.target[k]) * f0;
                tgt[k] = s.target[k] + (target[k] - s.target[k]) * f;
                a[k] = k_spring * (t0[k] + shift[k] - s.pos[k]) - c_damp * (s.vel[k] - vt[k]);
                lag0[k] = s.pos[k] - t0[k];
                lag0_len2 += lag0[k] * lag0[k];
            }
            const float lag0_len = std::sqrt(lag0_len2);
            if (shown_limit > 0.0f && lag0_len > shown_limit)
            {
                float out_v = 0.0f;
                for (int k = 0; k < 3; ++k)
                    out_v += (s.vel[k] - vt[k]) * lag0[k] / lag0_len;
                const float push = k_stiff * (lag0_len - shown_limit) +
                                   (out_v > 0.0f ? c_stiff * out_v : 0.0f);
                for (int k = 0; k < 3; ++k)
                    a[k] -= push * lag0[k] / lag0_len;
            }
            for (int k = 0; k < 3; ++k)
            {
                s.vel[k] += a[k] * h;
                s.pos[k] += s.vel[k] * h;
                d[k] = s.pos[k] - tgt[k];
            }
            const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (len > limit)
            {
                // On the limit: held there, the outward part of the relative
                // velocity dropped.
                const float scale = len > 0.0f ? limit / len : 0.0f;
                float out_v = 0.0f;
                for (int k = 0; k < 3; ++k)
                    out_v += (s.vel[k] - vt[k]) * (len > 0.0f ? d[k] / len : 0.0f);
                for (int k = 0; k < 3; ++k)
                {
                    s.pos[k] = tgt[k] + d[k] * scale;
                    if (out_v > 0.0f && len > 0.0f)
                        s.vel[k] -= out_v * d[k] / len;
                }
            }
        }
        // Shown: the lag eased toward the limit (limit x tanh(len / limit):
        // linear when small, smooth, never past it), so a gait that swings
        // the mass hard rounds off instead of stopping at a wall.
        float lag[3], len2 = 0.0f;
        for (int k = 0; k < 3; ++k)
        {
            s.target[k] = target[k];
            lag[k] = s.pos[k] - target[k];
            len2 += lag[k] * lag[k];
        }
        const float len = std::sqrt(len2);
        const float scale = len > 1e-9f && shown_limit > 0.0f
            ? shown_limit * std::tanh(len / shown_limit) / len : 0.0f;
        for (int k = 0; k < 3; ++k)
            offset[k] = lag[k] * scale;
    }
}
