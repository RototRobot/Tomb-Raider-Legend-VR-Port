#include "god_throw.h"

#include <cmath>

namespace trlvr
{
    namespace
    {
        float tdot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
        float tlen(const float v[3]) { return sqrtf(tdot(v, v)); }
        void tcross(const float a[3], const float b[3], float out[3])
        {
            const float x = a[1] * b[2] - a[2] * b[1];
            const float y = a[2] * b[0] - a[0] * b[2];
            const float z = a[0] * b[1] - a[1] * b[0];
            out[0] = x; out[1] = y; out[2] = z;
        }
        bool finite3(const float v[3])
        {
            return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
        }

        // Sample i (0 = newest).
        int slot(const ThrowTracker& t, int i)
        {
            return (t.head - i + kThrowSamples * 2) % kThrowSamples;
        }

        // The mean of samples [first, first + n) (newest-first order) with
        // the worst outlier dropped: the sample farthest from the mean of
        // the others, when that is more than half the others' speed plus
        // 0.1 m/s. Needs n >= 3 to drop anything.
        void robust_mean(const ThrowTracker& t, int first, int n, float vel[3], float spin[3])
        {
            float sv[3] = {}, sw[3] = {};
            for (int i = 0; i < n; ++i)
            {
                const int s = slot(t, first + i);
                for (int k = 0; k < 3; ++k)
                {
                    sv[k] += t.vel[s][k];
                    sw[k] += t.spin[s][k];
                }
            }
            int drop = -1;
            if (n >= 3)
            {
                float worst = 0.0f;
                for (int i = 0; i < n; ++i)
                {
                    const int s = slot(t, first + i);
                    float others[3], d[3];
                    for (int k = 0; k < 3; ++k)
                    {
                        others[k] = (sv[k] - t.vel[s][k]) / (n - 1);
                        d[k] = t.vel[s][k] - others[k];
                    }
                    const float far = tlen(d) - (0.5f * tlen(others) + 0.1f);
                    if (far > worst)
                    {
                        worst = far;
                        drop = i;
                    }
                }
            }
            int used = n;
            if (drop >= 0)
            {
                const int s = slot(t, first + drop);
                for (int k = 0; k < 3; ++k)
                {
                    sv[k] -= t.vel[s][k];
                    sw[k] -= t.spin[s][k];
                }
                --used;
            }
            for (int k = 0; k < 3; ++k)
            {
                vel[k] = sv[k] / used;
                spin[k] = sw[k] / used;
            }
        }
    }

    void throw_track_reset(ThrowTracker& t)
    {
        t = ThrowTracker();
    }

    void throw_track_add(ThrowTracker& t, const float vel[3], const float spin[3], float dt)
    {
        if (!finite3(vel) || !(dt > 0.0f))
            return;
        t.head = (t.head + 1) % kThrowSamples;
        for (int k = 0; k < 3; ++k)
        {
            t.vel[t.head][k] = vel[k];
            t.spin[t.head][k] = spin && finite3(spin) ? spin[k] : 0.0f;
        }
        t.dt[t.head] = dt;
        if (t.count < kThrowSamples)
            ++t.count;
    }

    bool throw_track_estimate(const ThrowTracker& t, float within, float vel_out[3],
                              float spin_out[3])
    {
        float best_v[3] = {}, best_w[3] = {}, first_v[3] = {};
        float best = -1.0f;
        float age = 0.0f;               // age of sample e (0 = newest)
        for (int e = 0; e < t.count; ++e)
        {
            if (e > 0)
                age += t.dt[slot(t, e - 1)];
            if (age > within && best >= 0.0f)
                break;
            int n = t.count - e;
            if (n > kThrowWindow)
                n = kThrowWindow;
            // A window short of samples only while nothing fuller exists.
            if (n < kThrowWindow && best >= 0.0f)
                break;
            float v[3], w[3];
            robust_mean(t, e, n, v, w);
            const float speed = tlen(v);
            // Only windows moving the way the newest one does (within 60
            // degrees): a faster backswing before the forward swing is not
            // the throw. A hand at rest (newest under 0.05 m/s) takes any.
            if (e == 0)
                for (int k = 0; k < 3; ++k) first_v[k] = v[k];
            const float first_speed = tlen(first_v);
            if (e > 0 && first_speed >= 0.05f && tdot(v, first_v) < 0.5f * speed * first_speed)
                continue;
            if (speed > best)
            {
                best = speed;
                for (int k = 0; k < 3; ++k)
                {
                    best_v[k] = v[k];
                    best_w[k] = w[k];
                }
            }
        }
        for (int k = 0; k < 3; ++k)
        {
            if (vel_out) vel_out[k] = best_v[k];
            if (spin_out) spin_out[k] = best_w[k];
        }
        return best >= 0.0f;
    }

    void throw_release(const float v_hand[3], const float omega[3], const float hand_pos[3],
                       const float centre[3], float out[3])
    {
        const float r[3] = { centre[0] - hand_pos[0], centre[1] - hand_pos[1], centre[2] - hand_pos[2] };
        float wr[3];
        tcross(omega, r, wr);
        for (int k = 0; k < 3; ++k) out[k] = v_hand[k] + wr[k];
    }

    float throw_cap(float v[3], float max_speed)
    {
        const float s = tlen(v);
        if (s > max_speed && s > 0.0f)
            for (int k = 0; k < 3; ++k) v[k] *= max_speed / s;
        return s;
    }

    void throw_cap_spin(float w[3], float max)
    {
        throw_cap(w, max);
    }

    void throw_body_start(ThrowBody& b, const float pos[3], const float vel[3],
                          const float spin[3], float radius)
    {
        for (int k = 0; k < 3; ++k)
        {
            b.pos[k] = b.prev[k] = pos[k];
            b.vel[k] = vel[k];
            b.spin[k] = spin ? spin[k] : 0.0f;
        }
        b.radius = radius;
    }

    void throw_body_step(ThrowBody& b, float gravity, float spin_damping, float dt, float turn[3])
    {
        for (int k = 0; k < 3; ++k)
        {
            b.prev[k] = b.pos[k];
            b.pos[k] += b.vel[k] * dt;
        }
        b.pos[2] -= 0.5f * gravity * dt * dt;
        b.vel[2] -= gravity * dt;
        if (turn)
            for (int k = 0; k < 3; ++k) turn[k] = b.spin[k] * dt;
        const float fade = expf(-spin_damping * dt);
        for (float& w : b.spin) w *= fade;
    }

    void throw_body_move(ThrowBody& b, const float at[3])
    {
        for (int k = 0; k < 3; ++k) b.pos[k] = b.prev[k] = at[k];
    }

    bool throw_body_bounce(ThrowBody& b, const float n[3], float restitution, float friction)
    {
        const float into = tdot(b.vel, n);
        if (!(into < 0.0f))
            return false;
        float keep = 1.0f - friction;
        if (keep < 0.0f) keep = 0.0f;
        if (keep > 1.0f) keep = 1.0f;
        for (int k = 0; k < 3; ++k)
        {
            const float vn = into * n[k];
            const float vt = b.vel[k] - vn;
            b.vel[k] = vt * keep - restitution * vn;
        }
        return true;
    }

    bool throw_normal_valid(const float n[3], const float dir[3])
    {
        if (!finite3(n))
            return false;
        const float l = tlen(n);
        if (l < 0.9f || l > 1.1f)
            return false;
        // Facing back along the ray (a little slack for a grazing hit).
        const float dl = tlen(dir);
        return dl <= 0.0f || tdot(n, dir) / (l * dl) < 0.05f;
    }

    bool throw_plane_normal(const float a[3], const float b[3], const float c[3],
                            const float dir[3], float out[3])
    {
        const float u[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
        const float v[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
        float n[3];
        tcross(u, v, n);
        const float l = tlen(n);
        if (!(l > 1e-6f) || !finite3(n))
            return false;
        for (float& x : n) x /= l;
        if (tdot(n, dir) > 0.0f)
            for (float& x : n) x = -x;
        for (int k = 0; k < 3; ++k) out[k] = n[k];
        return true;
    }

    int throw_surface(const float n[3], float vz)
    {
        if (n[2] > 0.6f && vz <= 0.0f)
            return kThrowFloor;
        if (n[2] < -0.6f)
            return kThrowCeiling;
        return kThrowWall;
    }
}
