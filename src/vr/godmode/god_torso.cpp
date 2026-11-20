#include "god_torso.h"

#include <cmath>
#include <math.h>

namespace trlvr
{
    namespace
    {
        constexpr float kGravity = 9.81f;
        constexpr float kPeakSeconds = 0.15f;
        constexpr float kMaxJointAcc = 50.0f;   // real m/s^2: a tracking jerk is no swing
        constexpr float kMaxHandSpin = 40.0f;   // rad/s: past this, a tracking jump

        // Each limb's pendulum length as a fraction of her apparent height
        // (arms, legs, head): roughly the distance from the joint to the
        // limb's centre of swing.
        constexpr float kLimbLength[kTorsoLimbs] = { 0.20f, 0.21f, 0.28f, 0.29f, 0.07f };

        float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
        float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
        float len3(const float v[3]) { return sqrtf(dot3(v, v)); }
        void cross3(const float a[3], const float b[3], float out[3])
        {
            const float x = a[1] * b[2] - a[2] * b[1];
            const float y = a[2] * b[0] - a[0] * b[2];
            const float z = a[0] * b[1] - a[1] * b[0];
            out[0] = x; out[1] = y; out[2] = z;
        }
        bool normalize3(float v[3])
        {
            const float l = len3(v);
            if (!(l > 1e-6f))
                return false;
            for (int k = 0; k < 3; ++k) v[k] /= l;
            return true;
        }
        // p * M (a point) and v * M (a direction, rotation part only).
        void xform_point(const float p[3], const Mat4& m, float out[3])
        {
            float r[3];
            for (int k = 0; k < 3; ++k)
                r[k] = p[0] * m.m[0][k] + p[1] * m.m[1][k] + p[2] * m.m[2][k] + m.m[3][k];
            out[0] = r[0]; out[1] = r[1]; out[2] = r[2];
        }
        void xform_dir(const float v[3], const Mat4& m, float out[3])
        {
            float r[3];
            for (int k = 0; k < 3; ++k)
                r[k] = v[0] * m.m[0][k] + v[1] * m.m[1][k] + v[2] * m.m[2][k];
            out[0] = r[0]; out[1] = r[1]; out[2] = r[2];
        }
        // v * transpose(M's rotation): the inverse turn of xform_dir.
        void xform_dir_inv(const float v[3], const Mat4& m, float out[3])
        {
            float r[3];
            for (int k = 0; k < 3; ++k)
                r[k] = v[0] * m.m[k][0] + v[1] * m.m[k][1] + v[2] * m.m[k][2];
            out[0] = r[0]; out[1] = r[1]; out[2] = r[2];
        }

        // M made rigid: its axis rows orthonormalised (a uniform scale goes),
        // the position kept.
        // Rigid pose blend (rotation by slerp, position by lerp) for the
        // follow smoothing. A local copy of the one-matrix case of
        // Underworld's pose_blend, which Legend does not have.
        struct Q { float x, y, z, w; };

        Q quat_of(const Mat4& m)
        {
            const Q id = { 0.0f, 0.0f, 0.0f, 1.0f };
            float c[3][3];
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                {
                    c[i][j] = m.m[j][i];
                    if (!std::isfinite(c[i][j])) return id;
                }
            Q q;
            const float tr = c[0][0] + c[1][1] + c[2][2];
            if (tr > 0.0f)
            {
                const float k = sqrtf(tr + 1.0f) * 2.0f;
                q.w = 0.25f * k;
                q.x = (c[2][1] - c[1][2]) / k;
                q.y = (c[0][2] - c[2][0]) / k;
                q.z = (c[1][0] - c[0][1]) / k;
            }
            else if (c[0][0] > c[1][1] && c[0][0] > c[2][2])
            {
                const float k = sqrtf(1.0f + c[0][0] - c[1][1] - c[2][2]) * 2.0f;
                q.w = (c[2][1] - c[1][2]) / k;
                q.x = 0.25f * k;
                q.y = (c[0][1] + c[1][0]) / k;
                q.z = (c[0][2] + c[2][0]) / k;
            }
            else if (c[1][1] > c[2][2])
            {
                const float k = sqrtf(1.0f + c[1][1] - c[0][0] - c[2][2]) * 2.0f;
                q.w = (c[0][2] - c[2][0]) / k;
                q.x = (c[0][1] + c[1][0]) / k;
                q.y = 0.25f * k;
                q.z = (c[1][2] + c[2][1]) / k;
            }
            else
            {
                const float k = sqrtf(1.0f + c[2][2] - c[0][0] - c[1][1]) * 2.0f;
                q.w = (c[1][0] - c[0][1]) / k;
                q.x = (c[0][2] + c[2][0]) / k;
                q.y = (c[1][2] + c[2][1]) / k;
                q.z = 0.25f * k;
            }
            const float n = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
            if (!(n > 1e-12f) || !std::isfinite(n)) return id;
            q.x /= n; q.y /= n; q.z /= n; q.w /= n;
            return q;
        }

        Q slerp_q(const Q& a, const Q& b, float t)
        {
            float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
            const float sg = d < 0.0f ? -1.0f : 1.0f;
            d *= sg;
            float wa = 1.0f - t, wb = sg * t;
            if (d <= 0.9995f)
            {
                const float theta = acosf(d);
                const float sn = sinf(theta);
                wa = sinf((1.0f - t) * theta) / sn;
                wb = sg * sinf(t * theta) / sn;
            }
            Q r = { a.x * wa + b.x * wb, a.y * wa + b.y * wb,
                    a.z * wa + b.z * wb, a.w * wa + b.w * wb };
            const float n = sqrtf(r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w);
            if (n > 1e-12f) { r.x /= n; r.y /= n; r.z /= n; r.w /= n; }
            return r;
        }

        Mat4 blend_rigid(const Mat4& from, const Mat4& to, float t)
        {
            if (!(t > 0.0f)) return from;
            if (t >= 1.0f) return to;
            const Q q = slerp_q(quat_of(from), quat_of(to), t);
            const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
            const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
            const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
            Mat4 r = Mat4::identity();
            r.m[0][0] = 1 - 2 * (yy + zz); r.m[0][1] = 2 * (xy + wz); r.m[0][2] = 2 * (xz - wy);
            r.m[1][0] = 2 * (xy - wz); r.m[1][1] = 1 - 2 * (xx + zz); r.m[1][2] = 2 * (yz + wx);
            r.m[2][0] = 2 * (xz + wy); r.m[2][1] = 2 * (yz - wx); r.m[2][2] = 1 - 2 * (xx + yy);
            for (int k = 0; k < 3; ++k)
                r.m[3][k] = from.m[3][k] + (to.m[3][k] - from.m[3][k]) * t;
            return r;
        }

        Mat4 rigid(const Mat4& m)
        {
            Mat4 r = Mat4::identity();
            float x[3] = { m.m[0][0], m.m[0][1], m.m[0][2] };
            float y[3] = { m.m[1][0], m.m[1][1], m.m[1][2] };
            if (!normalize3(x))
            {
                x[0] = 1.0f; x[1] = x[2] = 0.0f;
            }
            const float d = dot3(x, y);
            for (int k = 0; k < 3; ++k) y[k] -= d * x[k];
            if (!normalize3(y))
            {
                const float alt[3] = { 0.0f, 0.0f, 1.0f };
                cross3(alt, x, y);
                if (!normalize3(y))
                {
                    y[0] = 0.0f; y[1] = 1.0f; y[2] = 0.0f;
                }
            }
            float z[3];
            cross3(x, y, z);
            // Keep the handedness of the input's z row.
            if (z[0] * m.m[2][0] + z[1] * m.m[2][1] + z[2] * m.m[2][2] < 0.0f)
                for (float& v : z) v = -v;
            for (int k = 0; k < 3; ++k)
            {
                r.m[0][k] = x[k];
                r.m[1][k] = y[k];
                r.m[2][k] = z[k];
                r.m[3][k] = m.m[3][k];
            }
            return r;
        }

        // Closest point of segment a-b to p.
        void segment_closest(const float a[3], const float b[3], const float p[3], float out[3])
        {
            float ab[3], ap[3];
            for (int k = 0; k < 3; ++k)
            {
                ab[k] = b[k] - a[k];
                ap[k] = p[k] - a[k];
            }
            const float l2 = dot3(ab, ab);
            const float t = l2 > 0.0f ? clampf(dot3(ap, ab) / l2, 0.0f, 1.0f) : 0.0f;
            for (int k = 0; k < 3; ++k) out[k] = a[k] + ab[k] * t;
        }

        void clear_track(TorsoState& s)
        {
            s.track_ok = false;
            for (int k = 0; k < 3; ++k)
                s.vel_smooth[k] = s.peak_vel[k] = s.spin_smooth[k] = s.peak_spin[k] = 0.0f;
            s.peak_age = 0.0f;
            throw_track_reset(s.track);
        }

        void clear_limbs(TorsoState& s)
        {
            s.limbs_ok = false;
            for (int l = 0; l < kTorsoLimbs; ++l)
            {
                s.joint_ok[l] = false;
                for (int k = 0; k < 3; ++k)
                    s.limb_dir[l][k] = s.limb_vel[l][k] = s.joint_prev[l][k] =
                        s.joint_vel[l][k] = s.joint_acc[l][k] = 0.0f;
                s.limb_rotation[l] = Mat4::identity();
            }
            s.knee_rotation[0] = s.knee_rotation[1] = Mat4::identity();
        }

        void clear_hold(TorsoState& s)
        {
            s.hand = -1;
            s.flying = false;
            s.settle = 0.0f;
            s.fly_time = 0.0f;
            s.fly = ThrowBody();
            s.rel = s.drawn = s.body_matrix = Mat4::identity();
            clear_track(s);
            clear_limbs(s);
        }

        // Her drawn centre and feet from s.drawn and this frame's real pose.
        void place(TorsoState& s, const TorsoInput& in)
        {
            s.body_matrix = rigid_inverse(rigid(in.body)) * s.drawn;
            xform_point(s.centre_local, s.drawn, s.centre);
            xform_point(in.root, s.body_matrix, s.feet);
        }

        // The cone limit (radians) of limb l for a deviation from its rest
        // direction whose components along her forward / outward (in the
        // plane across the rest direction) are cf, co (cf^2 + co^2 = 1).
        float cone_limit(const TorsoParams& p, int l, float cf, float co)
        {
            float f, b, o, i;
            if (l <= kTorsoArm1)
            {
                f = p.arm_forward; b = p.arm_back; o = p.arm_out; i = p.arm_in;
            }
            else if (l <= kTorsoLeg1)
            {
                f = p.leg_forward; b = p.leg_back; o = p.leg_out; i = p.leg_in;
            }
            else
            {
                f = p.head_forward; b = p.head_back; o = i = p.head_side;
            }
            return (cf >= 0.0f ? f : b) * cf * cf + (co >= 0.0f ? o : i) * co * co;
        }

        // Limb frame across the rest direction r (all unit, world): fwd
        // along her front, out away from her body (or her side for the head).
        void cross_basis(const float r[3], const float front[3], const float up[3],
                         const float side[3], float fwd[3], float out[3])
        {
            const float d = dot3(front, r);
            for (int k = 0; k < 3; ++k) fwd[k] = front[k] - d * r[k];
            if (!normalize3(fwd))
            {
                const float e = dot3(up, r);
                for (int k = 0; k < 3; ++k) fwd[k] = up[k] - e * r[k];
                if (!normalize3(fwd))
                {
                    fwd[0] = 1.0f; fwd[1] = fwd[2] = 0.0f;
                }
            }
            cross3(r, fwd, out);
            normalize3(out);
            if (dot3(out, side) < 0.0f)
                for (int k = 0; k < 3; ++k) out[k] = -out[k];
        }

        // Keeps u within limb l's cone round r; removes the angular velocity
        // pushing it further out. Returns the deviation's angle and its
        // forward share (cf) for the knee.
        void clamp_cone(const TorsoParams& p, int l, const float r[3], const float fwd[3],
                        const float out[3], float u[3], float w[3], float* angle, float* cf_out)
        {
            const float c = dot3(u, r);
            float t[3];
            for (int k = 0; k < 3; ++k) t[k] = u[k] - c * r[k];
            const float tl = len3(t);
            const float theta = atan2f(tl, c);
            *angle = theta;
            *cf_out = 0.0f;
            if (tl < 1e-6f)
                return;
            for (int k = 0; k < 3; ++k) t[k] /= tl;
            const float cf = dot3(t, fwd), co = dot3(t, out);
            const float n2 = cf * cf + co * co;
            const float cfn = n2 > 0.0f ? cf / sqrtf(n2) : 0.0f;
            const float con = n2 > 0.0f ? co / sqrtf(n2) : 0.0f;
            *cf_out = cfn;
            const float lim = cone_limit(p, l, cfn, con);
            if (theta <= lim)
                return;
            *angle = lim;
            for (int k = 0; k < 3; ++k) u[k] = r[k] * cosf(lim) + t[k] * sinf(lim);
            normalize3(u);
            float n[3];
            cross3(r, u, n);
            if (normalize3(n))
            {
                const float wn = dot3(w, n);
                if (wn > 0.0f)
                    for (int k = 0; k < 3; ++k) w[k] -= wn * n[k];
            }
        }

        // The limb pendulums for this frame (s.body_matrix already placed).
        void swing_limbs(TorsoState& s, const TorsoParams& p, const TorsoInput& in, float dt,
                         bool step_ok)
        {
            const Mat4& D = s.body_matrix;
            const float height = in.apparent_height > 0.05f ? in.apparent_height
                                                             : (s.height > 0.05f ? s.height : 0.3f);
            s.height = height;
            float front_r[3] = { in.forward[0], in.forward[1], in.forward[2] };
            float up_r[3] = { in.up[0], in.up[1], in.up[2] };
            if (!normalize3(up_r))
            {
                up_r[0] = up_r[1] = 0.0f; up_r[2] = 1.0f;
            }
            if (!normalize3(front_r))
            {
                front_r[0] = 0.0f; front_r[1] = -1.0f; front_r[2] = 0.0f;
            }
            float side_r[3];
            cross3(up_r, front_r, side_r);
            if (!normalize3(side_r))
            {
                side_r[0] = 1.0f; side_r[1] = side_r[2] = 0.0f;
            }
            float centre_r[3];
            for (int k = 0; k < 3; ++k) centre_r[k] = 0.5f * (in.torso_a[k] + in.torso_b[k]);
            float front_w[3], up_w[3];
            xform_dir(front_r, D, front_w);
            xform_dir(up_r, D, up_w);
            // The body's spin: the hand's while held, the flight's when thrown.
            const float* wb = s.flying ? s.fly.spin : s.spin_smooth;
            for (int l = 0; l < kTorsoLimbs; ++l)
            {
                if (!in.limb_valid[l])
                {
                    s.joint_ok[l] = false;
                    s.limb_rotation[l] = Mat4::identity();
                    if (l == kTorsoLeg0 || l == kTorsoLeg1)
                        s.knee_rotation[l - kTorsoLeg0] = Mat4::identity();
                    continue;
                }
                float r_r[3];
                for (int k = 0; k < 3; ++k) r_r[k] = in.limb_end[l][k] - in.limb_joint[l][k];
                if (!normalize3(r_r))
                {
                    s.joint_ok[l] = false;
                    s.limb_rotation[l] = Mat4::identity();
                    if (l == kTorsoLeg0 || l == kTorsoLeg1)
                        s.knee_rotation[l - kTorsoLeg0] = Mat4::identity();
                    continue;
                }
                // Outward: her side the joint is on (the head: either).
                float side_l[3];
                float to_joint[3];
                for (int k = 0; k < 3; ++k) to_joint[k] = in.limb_joint[l][k] - centre_r[k];
                const float sgn = l == kTorsoHead ? 1.0f : (dot3(to_joint, side_r) >= 0.0f ? 1.0f : -1.0f);
                for (int k = 0; k < 3; ++k) side_l[k] = sgn * side_r[k];
                float r[3], side_w[3], joint_w[3];
                xform_dir(r_r, D, r);
                xform_dir(side_l, D, side_w);
                xform_point(in.limb_joint[l], D, joint_w);
                float fwd[3], out[3];
                cross_basis(r, front_w, up_w, side_w, fwd, out);

                float* u = s.limb_dir[l];
                float* w = s.limb_vel[l];
                if (!s.limbs_ok || !s.joint_ok[l])
                {
                    for (int k = 0; k < 3; ++k)
                    {
                        u[k] = r[k];
                        w[k] = s.joint_vel[l][k] = s.joint_acc[l][k] = 0.0f;
                        s.joint_prev[l][k] = joint_w[k];
                    }
                    s.joint_ok[l] = true;
                }
                else if (step_ok)
                {
                    // The joint's acceleration in real m/s^2, smoothed.
                    float v[3];
                    for (int k = 0; k < 3; ++k) v[k] = (joint_w[k] - s.joint_prev[l][k]) / dt / p.table;
                    const bool jump = len3(v) > 4.0f * p.throw_max_speed;
                    const float sm = dt / (dt + 0.03f);
                    for (int k = 0; k < 3; ++k)
                    {
                        const float a = jump ? 0.0f : (v[k] - s.joint_vel[l][k]) / dt;
                        s.joint_acc[l][k] += (a - s.joint_acc[l][k]) * sm;
                        s.joint_vel[l][k] = jump ? 0.0f : v[k];
                        s.joint_prev[l][k] = joint_w[k];
                    }
                    const float al = len3(s.joint_acc[l]);
                    if (al > kMaxJointAcc)
                        for (int k = 0; k < 3; ++k) s.joint_acc[l][k] *= kMaxJointAcc / al;
                }

                if (step_ok)
                {
                    const float L = kLimbLength[l] * height;
                    const float w0sq = kGravity / L;
                    const bool arm = l == kTorsoArm0 || l == kTorsoArm1;
                    const float tone_k = s.flying ? p.flight_tone
                                                  : l == kTorsoHead ? p.head_tone
                                                  : arm ? p.arm_tone : p.limb_tone;
                    const float tone = tone_k * w0sq;
                    const float ratio = s.flying ? p.flight_damping
                                                 : arm ? p.arm_damping : p.limb_damping;
                    // Critical damping of the stiffer of gravity and tone.
                    const float stiff = s.flying ? tone : w0sq + tone;
                    const float damp = 2.0f * ratio * sqrtf(stiff > 0.0f ? stiff : 0.0f);
                    // What swings the limb, as an acceleration at its end
                    // (pivot frame): held, gravity less the joint's
                    // acceleration; in free fall only the air drag against
                    // her travel (they trail behind).
                    float g[3];
                    if (s.flying)
                    {
                        for (int k = 0; k < 3; ++k) g[k] = -p.trail * s.fly.vel[k] / p.table;
                    }
                    else
                    {
                        for (int k = 0; k < 3; ++k) g[k] = -s.joint_acc[l][k];
                        g[2] -= kGravity;
                    }
                    const float gl = len3(g);
                    const int steps = (int)ceilf(dt / 0.004f);
                    const float h = dt / (steps > 0 ? steps : 1);
                    for (int i = 0; i < steps; ++i)
                    {
                        float ge[3] = { g[0], g[1], g[2] };
                        // Straight against the pull (held upside down) is a
                        // balance nothing tips: a limb whose tone cannot
                        // hold it there (below g/L) tips forward.
                        float ug[3];
                        cross3(u, g, ug);
                        if (tone_k < 1.0f && gl > 0.0f && dot3(u, g) < 0.0f && len3(ug) < 0.05f * gl)
                            for (int k = 0; k < 3; ++k) ge[k] += 0.2f * gl * fwd[k];
                        float acc[3];
                        cross3(u, ge, acc);
                        for (int k = 0; k < 3; ++k) acc[k] /= L;
                        // Tone toward her pose (axis * angle).
                        float ax[3];
                        cross3(u, r, ax);
                        const float sn = len3(ax);
                        if (sn > 1e-6f)
                        {
                            const float ang = atan2f(sn, dot3(u, r));
                            for (int k = 0; k < 3; ++k) acc[k] += tone * ang * ax[k] / sn;
                        }
                        // Damping of the swing relative to the body.
                        for (int k = 0; k < 3; ++k) acc[k] -= damp * (w[k] - wb[k]);
                        for (int k = 0; k < 3; ++k) w[k] += acc[k] * h;
                        // No twist about the limb.
                        const float wu = dot3(w, u);
                        for (int k = 0; k < 3; ++k) w[k] -= wu * u[k];
                        float du[3];
                        cross3(w, u, du);
                        for (int k = 0; k < 3; ++k) u[k] += du[k] * h;
                        normalize3(u);
                        float a_dummy, cf_dummy;
                        clamp_cone(p, l, r, fwd, out, u, w, &a_dummy, &cf_dummy);
                    }
                }
                float angle = 0.0f, cf = 0.0f;
                clamp_cone(p, l, r, fwd, out, u, w, &angle, &cf);

                // The turn in her real frame: rest -> the drawn direction
                // taken back through the body's rotation.
                float u_r[3];
                xform_dir_inv(u, D, u_r);
                s.limb_rotation[l] = rotation_between(r_r, u_r);
                if (l == kTorsoLeg0 || l == kTorsoLeg1)
                {
                    // A relaxed knee, bending more as the thigh swings
                    // forward; the shin turns back about her side axis
                    // (up x forward takes the shin's down toward her back).
                    const float flex = cf > 0.0f ? angle * cf : 0.0f;
                    const float bend = clampf(p.knee_bend + 0.6f * flex, 0.0f, 1.3f);
                    float axis[3];
                    cross3(up_r, front_r, axis);
                    normalize3(axis);
                    for (float& v : axis) v *= bend;
                    s.knee_rotation[l - kTorsoLeg0] = rotation_from_vector(axis);
                }
            }
            s.limbs_ok = true;
        }

        // The hand's motion while held: her centre's velocity (real m/s)
        // and the hand's spin (rad/s), smoothed, and their peak of the last
        // 0.15 s.
        void track(TorsoState& s, const TorsoParams& p, const Mat4& hand, float dt, bool step_ok)
        {
            if (step_ok && s.track_ok)
            {
                float v[3];
                for (int k = 0; k < 3; ++k) v[k] = (s.centre[k] - s.last_centre[k]) / dt / p.table;
                const bool jump = len3(v) > 4.0f * p.throw_max_speed;
                float rv[3] = {};
                const Mat4 delta = rigid_inverse(s.last_hand) * hand;
                const bool spin_ok = rotation_to_vector(delta, rv) && len3(rv) / dt <= kMaxHandSpin;
                // The tracker: the hand origin's velocity and its spin.
                {
                    float hv[3], hw[3] = {};
                    for (int k = 0; k < 3; ++k)
                        hv[k] = (hand.m[3][k] - s.last_hand.m[3][k]) / dt / p.table;
                    if (spin_ok)
                        for (int k = 0; k < 3; ++k) hw[k] = rv[k] / dt;
                    if (len3(hv) <= 4.0f * p.throw_max_speed)
                        throw_track_add(s.track, hv, hw, dt);
                }
                const float sm = dt / (dt + 0.03f);
                for (int k = 0; k < 3; ++k)
                {
                    if (!jump)
                        s.vel_smooth[k] += (v[k] - s.vel_smooth[k]) * sm;
                    if (spin_ok)
                        s.spin_smooth[k] += (rv[k] / dt - s.spin_smooth[k]) * sm;
                }
                s.peak_age += dt;
                if (len3(s.vel_smooth) >= len3(s.peak_vel) || s.peak_age > kPeakSeconds)
                {
                    for (int k = 0; k < 3; ++k)
                    {
                        s.peak_vel[k] = s.vel_smooth[k];
                        s.peak_spin[k] = s.spin_smooth[k];
                    }
                    s.peak_age = 0.0f;
                }
            }
            for (int k = 0; k < 3; ++k) s.last_centre[k] = s.centre[k];
            s.last_hand = hand;
            s.track_ok = true;
        }

        bool hand_blocked(const TorsoHand& hd)
        {
            return !hd.valid || hd.part_held || hd.other_grab;
        }
    }

    void torso_reset(TorsoState& s)
    {
        clear_hold(s);
        for (int h = 0; h < 2; ++h)
        {
            s.was_fist[h] = false;
            s.aimed[h] = false;
            s.aim_age[h] = 1e9f;
        }
        for (int k = 0; k < 3; ++k) s.feet[k] = s.feet_prev[k] = s.centre[k] = 0.0f;
    }

    bool torso_zone(const TorsoParams& p, const TorsoInput& in, float a[3], float b[3],
                    float* radius)
    {
        if (!in.body_valid)
            return false;
        float axis[3], len2 = 0.0f;
        for (int k = 0; k < 3; ++k)
        {
            axis[k] = in.torso_b[k] - in.torso_a[k];
            len2 += axis[k] * axis[k];
        }
        const float shift = len2 > 0.0f ? p.zone_lower * p.metre / sqrtf(len2) : 0.0f;
        for (int k = 0; k < 3; ++k)
        {
            a[k] = in.torso_a[k] - axis[k] * shift;
            b[k] = in.torso_b[k] - axis[k] * shift;
        }
        *radius = p.torso_radius * p.metre + p.hand_margin * p.metre;
        return true;
    }

    bool torso_aim(const TorsoParams& p, const TorsoInput& in, int h)
    {
        if (h < 0 || h > 1 || !p.enabled)
            return false;
        const TorsoHand& hd = in.hand[h];
        if (hand_blocked(hd))
            return false;
        if (!(in.apparent_height > 0.0f) || in.apparent_height > p.max_height)
            return false;
        float a[3], b[3], radius;
        if (!torso_zone(p, in, a, b, &radius))
            return false;
        float c[3];
        segment_closest(a, b, hd.palm_centre, c);
        float d[3];
        for (int k = 0; k < 3; ++k) d[k] = c[k] - hd.palm_centre[k];
        const float dist = len3(d);
        if (dist > radius)
            return false;
        // On her axis: the palm is in her, whichever way it faces.
        if (dist < 1e-4f * (radius > 1.0f ? radius : 1.0f))
            return true;
        float n[3] = { hd.palm_normal[0], hd.palm_normal[1], hd.palm_normal[2] };
        if (!normalize3(n))
            return false;
        return dot3(n, d) / dist >= p.facing_dot;
    }

    bool torso_drawn(const TorsoState& s)
    {
        return s.hand >= 0 || s.flying;
    }

    void torso_step(TorsoState& s, const TorsoParams& p, const TorsoInput& in,
                    TorsoEvents* events)
    {
        TorsoEvents none;
        TorsoEvents& ev = events ? *events : none;
        ev = TorsoEvents();

        if (!p.enabled)
        {
            torso_reset(s);
            for (int h = 0; h < 2; ++h) s.was_fist[h] = in.hand[h].valid && in.hand[h].fist;
            return;
        }
        s.table = p.table;
        const float dt = in.dt;
        const bool step_ok = dt > 0.0f && dt <= 0.1f && p.table > 0.0f;

        bool pressed[2], fist[2];
        for (int h = 0; h < 2; ++h)
        {
            fist[h] = in.hand[h].valid && in.hand[h].fist;
            pressed[h] = fist[h] && !s.was_fist[h];
            s.was_fist[h] = fist[h];
        }

        // Thrown: her centre flies a parabola, she turns about it at a
        // slowly fading spin. The caller probes feet_prev -> feet.
        if (s.flying)
        {
            if (step_ok)
            {
                for (int k = 0; k < 3; ++k) s.feet_prev[k] = s.feet[k];
                float turn[3];
                throw_body_step(s.fly, p.throw_gravity * p.table, p.spin_damping, dt, turn);
                const float* c_new = s.fly.pos;
                s.drawn = rigid(s.drawn * translation(-s.centre[0], -s.centre[1], -s.centre[2]) *
                                rotation_from_vector(turn) * translation(c_new[0], c_new[1], c_new[2]));
                s.fly_time += dt;
            }
            place(s, in);
            swing_limbs(s, p, in, dt, step_ok);
            if (s.fly_time >= p.throw_max_time)
            {
                ev.flight_over = true;
                for (int k = 0; k < 3; ++k) ev.feet[k] = s.feet[k];
            }
        }

        // Aiming (a free Lara only).
        const bool free = s.hand < 0 && !s.flying && in.body_valid;
        for (int h = 0; h < 2; ++h)
        {
            const bool aim = free && torso_aim(p, in, h);
            ev.aim[h] = aim;
            if (aim && !s.aimed[h])
                ev.hover_enter[h] = true;
            s.aimed[h] = aim;
            if (aim)
                s.aim_age[h] = 0.0f;
            else if (step_ok)
                s.aim_age[h] += dt;
            if (!free || !pressed[h] || hand_blocked(in.hand[h]))
                continue;
            if (!(in.apparent_height > 0.0f) || in.apparent_height > p.max_height)
                continue;
            if (aim || s.aim_age[h] <= p.grace_seconds)
                ev.wants[h] = true;
        }
        if (free)
        {
            for (int h = 0; h < 2 && s.hand < 0; ++h)
            {
                if (!ev.wants[h])
                    continue;
                clear_hold(s);
                s.hand = h;
                const Mat4 body = rigid(in.body);
                const Mat4 hand = rigid(in.hand[h].pose);
                s.drawn = body;
                s.rel = body * rigid_inverse(hand);
                s.settle = p.settle_seconds;
                float centre_r[3];
                for (int k = 0; k < 3; ++k) centre_r[k] = 0.5f * (in.torso_a[k] + in.torso_b[k]);
                xform_point(centre_r, rigid_inverse(body), s.centre_local);
                place(s, in);
                track(s, p, hand, dt, false);
                swing_limbs(s, p, in, dt, false);
                ev.grabbed = true;
                ev.hand = h;
                for (int o = 0; o < 2; ++o) s.aimed[o] = false;
                return;
            }
        }

        if (s.hand < 0)
            return;

        // Held. Let go (the fist opened, or the hand was lost): a toss if
        // she was moving fast enough, else a put-down.
        const int h = s.hand;
        if (!fist[h] || !in.body_valid)
        {
            ev.released = true;
            ev.hand = h;
            for (int k = 0; k < 3; ++k) ev.feet[k] = s.feet[k];
            // Release velocity (real m/s) and spin (rad/s): the tracker's
            // hand estimate carried to her centre (v + omega x r), or the
            // old peak of her centre's smoothed velocity.
            float rel_v[3], rel_w[3];
            {
                float hv[3], hw[3];
                if (p.throw_velocity != 0 && throw_track_estimate(s.track, kPeakSeconds, hv, hw))
                {
                    float r_hand[3], r_centre[3];
                    for (int k = 0; k < 3; ++k)
                    {
                        r_hand[k] = s.last_hand.m[3][k] / p.table;
                        r_centre[k] = s.centre[k] / p.table;
                    }
                    throw_release(hv, hw, r_hand, r_centre, rel_v);
                    for (int k = 0; k < 3; ++k) rel_w[k] = hw[k];
                }
                else
                    for (int k = 0; k < 3; ++k)
                    {
                        rel_v[k] = s.peak_vel[k];
                        rel_w[k] = s.peak_spin[k];
                    }
            }
            float speed = len3(rel_v);
            if (p.throw_enabled && in.hand[h].valid && in.body_valid && speed >= p.throw_min_speed)
            {
                const float cap = speed > p.throw_max_speed ? p.throw_max_speed / speed : 1.0f;
                speed *= cap;
                ev.thrown = true;
                ev.throw_speed = speed;
                s.flying = true;
                s.fly_time = 0.0f;
                s.hand = -1;
                float fly_vel[3], fly_spin[3];
                for (int k = 0; k < 3; ++k)
                {
                    fly_vel[k] = rel_v[k] * cap * p.throw_scale * p.table;
                    fly_spin[k] = rel_w[k] * p.spin_scale;
                    s.feet_prev[k] = s.feet[k];
                }
                throw_cap_spin(fly_spin, p.spin_max);
                throw_body_start(s.fly, s.centre, fly_vel, fly_spin,
                                 kThrowRadius * p.throw_radius * p.metre);
                // The limbs keep their swing (momentum carries into the flight).
                return;
            }
            ev.put_down = true;
            clear_hold(s);
            return;
        }

        const Mat4 hand = rigid(in.hand[h].pose);
        if (s.settle > 0.0f)
        {
            // Settling: she stays put under the closing fingers.
            s.rel = s.drawn * rigid_inverse(hand);
            if (step_ok)
                s.settle -= dt;
        }
        else
        {
            const Mat4 want = s.rel * hand;
            if (p.follow_seconds > 0.0f && step_ok)
            {
                const float t = 1.0f - expf(-dt / p.follow_seconds);
                s.drawn = rigid(blend_rigid(s.drawn, want, t));
            }
            else
                s.drawn = want;
        }
        place(s, in);
        track(s, p, hand, dt, step_ok);
        swing_limbs(s, p, in, dt, step_ok);
    }

    Mat4 torso_body_matrix(const TorsoState& s, const TorsoInput& in)
    {
        if (!torso_drawn(s))
            return Mat4::identity();
        return rigid_inverse(rigid(in.body)) * s.drawn;
    }

    Mat4 torso_limb_rotation(const TorsoState& s, int limb)
    {
        if (limb < 0 || limb >= kTorsoLimbs || !torso_drawn(s))
            return Mat4::identity();
        return s.limb_rotation[limb];
    }

    Mat4 torso_limb_turn(const TorsoState& s, int limb, const float joint[3])
    {
        return translation(-joint[0], -joint[1], -joint[2]) * torso_limb_rotation(s, limb) *
               translation(joint[0], joint[1], joint[2]);
    }

    bool torso_nudge(TorsoState& s, const TorsoParams& p, const TorsoInput& in,
                     const float point[3], const float velocity[3], float radius,
                     float strength, float dt)
    {
        if (!torso_drawn(s) || !s.limbs_ok || !(p.table > 0.0f) || !(s.height > 0.05f) ||
            !(dt > 0.0f) || dt > 0.1f)
            return false;
        float v[3] = { velocity[0] / p.table, velocity[1] / p.table, velocity[2] / p.table };
        const float speed = len3(v);
        if (!(speed > 0.05f))
            return false;
        if (speed > p.throw_max_speed)
            for (float& x : v) x *= p.throw_max_speed / speed;
        const float ease = dt * 8.0f < 1.0f ? dt * 8.0f : 1.0f;
        bool touched = false;
        for (int l = 0; l < kTorsoLimbs; ++l)
        {
            if (!s.joint_ok[l] || !in.limb_valid[l])
                continue;
            float r[3];
            for (int k = 0; k < 3; ++k) r[k] = in.limb_end[l][k] - in.limb_joint[l][k];
            const float length = len3(r);
            if (!(length > 1e-3f))
                continue;
            float end[3], near_pt[3];
            for (int k = 0; k < 3; ++k) end[k] = s.joint_prev[l][k] + s.limb_dir[l][k] * length;
            segment_closest(s.joint_prev[l], end, point, near_pt);
            float d[3];
            for (int k = 0; k < 3; ++k) d[k] = point[k] - near_pt[k];
            if (len3(d) > radius)
                continue;
            const float L = kLimbLength[l] * s.height;
            float kick[3];
            cross3(s.limb_dir[l], v, kick);
            for (int k = 0; k < 3; ++k) s.limb_vel[l][k] += strength * ease * kick[k] / L;
            touched = true;
        }
        return touched;
    }

    void torso_land(TorsoState& s)
    {
        clear_hold(s);
    }

    void torso_fly_move(TorsoState& s, const float centre[3])
    {
        if (!s.flying)
            return;
        float d[3];
        for (int k = 0; k < 3; ++k)
        {
            d[k] = centre[k] - s.centre[k];
            s.drawn.m[3][k] += d[k];
            s.centre[k] = centre[k];
            s.body_matrix.m[3][k] += d[k];
            s.feet[k] += d[k];
            s.feet_prev[k] = s.feet[k];
        }
        throw_body_move(s.fly, centre);
    }

    void torso_bounce(TorsoState& s, const float centre[3], const float normal[3],
                      float restitution, float friction)
    {
        if (!s.flying)
            return;
        torso_fly_move(s, centre);
        const float before[3] = { s.fly.vel[0], s.fly.vel[1], s.fly.vel[2] };
        throw_body_bounce(s.fly, normal, restitution, friction);
        for (float& w : s.fly.spin) w *= -0.5f;
        // The limbs fly on past the knock: each end keeps its velocity
        // while the body's changes by dv, half of that as a swing.
        if (s.table > 0.0f && s.height > 0.05f)
        {
            float dv[3];
            for (int k = 0; k < 3; ++k) dv[k] = -(s.fly.vel[k] - before[k]) / s.table;
            for (int l = 0; l < kTorsoLimbs; ++l)
            {
                if (!s.joint_ok[l])
                    continue;
                const float L = kLimbLength[l] * s.height;
                float kick[3];
                cross3(s.limb_dir[l], dv, kick);
                for (int k = 0; k < 3; ++k) s.limb_vel[l][k] += 0.5f * kick[k] / L;
            }
        }
    }
}
