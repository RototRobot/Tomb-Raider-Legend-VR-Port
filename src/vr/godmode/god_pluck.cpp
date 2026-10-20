#include "god_pluck.h"

#include <math.h>

namespace trlvr
{
    namespace
    {
        void hang_dir(float tx, float ty, float out[3])
        {
            out[0] = sinf(tx);
            out[1] = sinf(ty);
            const float rest = 1.0f - out[0] * out[0] - out[1] * out[1];
            out[2] = -sqrtf(rest > 0.05f ? rest : 0.05f);
        }

        float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

        float length3(const float v[3]) { return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

        void clear_hold(PluckState& s)
        {
            s.hand = -1;
            s.hover = false;
            s.part = kPluckNone;
            s.lifted = true;
            s.flying = false;
            s.fly_time = 0.0f;
            for (int k = 0; k < 3; ++k)
                s.carry[k] = s.arm_offset[k] = s.fly_vel[k] = 0.0f;
        }

        void clear_pendulum(PluckState& s)
        {
            for (int k = 0; k < 3; ++k)
                s.hand_vel[k] = s.vel_smooth[k] = s.peak_vel[k] = 0.0f;
            s.peak_age = 0.0f;
            for (int k = 0; k < 2; ++k)
            {
                s.tilt[k] = s.tilt_vel[k] = s.hand_acc[k] = 0.0f;
                for (auto& limb : s.limb_tilt) limb[k] = 0.0f;
            }
            s.body_rotation = Mat4::identity();
            for (auto& r : s.limb_rotation) r = Mat4::identity();
        }

        bool is_arm(int part) { return part == kPluckArm0 || part == kPluckArm1; }

        // The wrist she hangs from, if any: the raised arm of an arm grab, or
        // the caller's wrist pivot for a head grab.
        bool pivot_of(const PluckState& s, const PluckInput& in, float out[3])
        {
            if (is_arm(s.part))
            {
                const int c = s.part - kPluckArm0;
                if (!in.arm_valid[c])
                    return false;
                for (int k = 0; k < 3; ++k) out[k] = in.arm_pivot[c][k];
                return true;
            }
            if (!in.wrist_pivot_valid)
                return false;
            for (int k = 0; k < 3; ++k) out[k] = in.wrist_pivot[k];
            return true;
        }

        // The limbs follow the body's tilt on slower pendulums, with a little
        // idle sway.
        void swing_limbs(PluckState& s, float dt)
        {
            s.phase += dt * 6.2831853f * 1.3f;
            const float lag[4] = { 0.16f, 0.16f, 0.28f, 0.28f };
            for (int l = 0; l < 4; ++l)
                for (int k = 0; k < 2; ++k)
                {
                    const float follow = dt / (dt + lag[l]);
                    s.limb_tilt[l][k] += (s.tilt[k] - s.limb_tilt[l][k]) * follow;
                }
        }

        void update_rotations(PluckState& s)
        {
            const float down[3] = { 0.0f, 0.0f, -1.0f };
            float body[3];
            hang_dir(s.tilt[0], s.tilt[1], body);
            s.body_rotation = rotation_between(down, body);
            for (int l = 0; l < 4; ++l)
            {
                const float idle = 0.06f * sinf(s.phase + 1.7f * l);
                float limb[3];
                hang_dir(s.limb_tilt[l][0] + idle, s.limb_tilt[l][1] - 0.5f * idle, limb);
                s.limb_rotation[l] = rotation_between(body, limb);
            }
        }

        // The zone `pt` is deepest in (distance / radius), kPluckNone if none.
        int zone_of(const PluckParams& p, const PluckInput& in, const float pt[3])
        {
            int best = kPluckNone;
            float best_score = 1e30f;
            if (p.head_only)
            {
                if (in.head_valid)
                {
                    const float d[3] = { pt[0] - in.head[0], pt[1] - in.head[1], pt[2] - in.head[2] };
                    const float r = pluck_head_radius(p);
                    const float dist = length3(d);
                    if (dist <= r)
                    {
                        best = kPluckHead;
                        best_score = r > 0.0f ? dist / r : 0.0f;
                    }
                }
            }
            else
            {
                float radius, bottom, top;
                pluck_zone(p, in.root, &radius, &bottom, &top);
                const float dx = pt[0] - in.root[0];
                const float dy = pt[1] - in.root[1];
                if (dx * dx + dy * dy <= radius * radius && pt[2] >= bottom && pt[2] <= top)
                {
                    best = kPluckHead;  // the column: her whole body, held as before
                    best_score = 1.0f;
                }
            }
            if (p.arm_radius > 0.0f)
            {
                const float r = p.arm_radius * p.metre + 0.01f * p.table;
                for (int c = 0; c < 2; ++c)
                {
                    if (!in.arm_valid[c])
                        continue;
                    const float dist = pluck_segment_distance(in.arm_shoulder[c], in.arm_end[c], pt);
                    const float score = r > 0.0f ? dist / r : 0.0f;
                    if (dist <= r && score < best_score)
                    {
                        best = kPluckArm0 + c;
                        best_score = score;
                    }
                }
            }
            return best;
        }
    }

    void pluck_reset(PluckState& s)
    {
        clear_hold(s);
        for (int h = 0; h < 2; ++h)
        {
            s.was[h] = s.inside[h] = false;
            s.inside_age[h] = 1e9f;
            s.last_zone[h] = kPluckNone;
        }
        s.floor_valid = false;
        s.wrist_pivot = false;
        clear_pendulum(s);
    }

    float pluck_head_radius(const PluckParams& p)
    {
        return p.head_radius * p.metre + 0.01f * p.table;
    }

    float pluck_segment_distance(const float a[3], const float b[3], const float p[3])
    {
        float ab[3], ap[3];
        for (int k = 0; k < 3; ++k)
        {
            ab[k] = b[k] - a[k];
            ap[k] = p[k] - a[k];
        }
        const float len2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
        float t = len2 > 0.0f ? (ap[0] * ab[0] + ap[1] * ab[1] + ap[2] * ab[2]) / len2 : 0.0f;
        t = clampf(t, 0.0f, 1.0f);
        float d2 = 0.0f;
        for (int k = 0; k < 3; ++k)
        {
            const float d = ap[k] - ab[k] * t;
            d2 += d * d;
        }
        return sqrtf(d2);
    }

    bool pluck_near(const PluckParams& p, const float feet[3], const float head_top[3],
                    const float point[3], float reach)
    {
        const float r = reach * p.metre + 0.03f * p.table;
        return pluck_segment_distance(feet, head_top, point) <= r;
    }

    void pluck_zone(const PluckParams& p, const float root[3],
                    float* radius, float* bottom, float* top)
    {
        // The zone is her feet; with a palm platform it would be the carried
        // root instead (Legend adds the carry while palm >= 0).
        *radius = 0.30f * p.metre + 0.04f * p.table;
        *bottom = root[2] - 0.03f * p.table;
        *top = root[2] + 1.8f * p.metre + 0.04f * p.table;
    }

    bool pluck_drawn(const PluckState& s)
    {
        return (s.hand >= 0 && s.lifted) || s.flying;
    }

    void pluck_land(PluckState& s)
    {
        clear_hold(s);
        clear_pendulum(s);
    }

    void pluck_bounce(PluckState& s, const float feet[3], float keep, bool ceiling)
    {
        if (!s.flying)
            return;
        for (int k = 0; k < 3; ++k)
        {
            s.hang[k] += feet[k] - s.target[k];
            s.target[k] = s.fly_prev[k] = feet[k];
            s.carry[k] = s.target[k] - s.root[k];
        }
        if (ceiling)
        {
            if (s.fly_vel[2] > 0.0f)
                s.fly_vel[2] = -keep * s.fly_vel[2];
        }
        else
        {
            s.fly_vel[0] = -keep * s.fly_vel[0];
            s.fly_vel[1] = -keep * s.fly_vel[1];
        }
    }

    void pluck_step(PluckState& s, const PluckParams& p, const PluckInput& in,
                    PluckEvents* events)
    {
        PluckEvents none;
        PluckEvents& ev = events ? *events : none;
        ev = PluckEvents();

        if (!p.enabled)
        {
            // Same as leaving board gameplay: no pick-up, and the grips are
            // remembered so one held now is not a press when it comes back.
            pluck_reset(s);
            for (int h = 0; h < 2; ++h) s.was[h] = in.grip[h];
            return;
        }
        for (int k = 0; k < 3; ++k) s.root[k] = in.root[k];
        const float dt = in.dt;
        const bool step_ok = dt > 0.0f && dt <= 0.1f && p.table > 0.0f;

        // Thrown: the hang point flies under gravity; nothing holds her
        // straight, so she keeps turning as she left the hand, slowing a
        // little. The caller watches fly_prev -> target for what she hits.
        if (s.flying)
        {
            if (step_ok)
            {
                for (int k = 0; k < 3; ++k)
                {
                    s.fly_prev[k] = s.target[k];
                    s.hang[k] += s.fly_vel[k] * dt;
                }
                s.fly_vel[2] -= p.throw_gravity * p.table * dt;
                s.fly_time += dt;
                for (int k = 0; k < 2; ++k)
                {
                    s.tilt_vel[k] /= 1.0f + 1.5f * dt;
                    s.tilt[k] = clampf(s.tilt[k] + s.tilt_vel[k] * dt, -1.2f, 1.2f);
                }
                swing_limbs(s, dt);
            }
            for (int k = 0; k < 3; ++k)
            {
                s.target[k] = s.hang[k] + s.fly_offset[k];
                s.carry[k] = s.target[k] - in.root[k];
            }
            float pivot[3];
            s.wrist_pivot = pivot_of(s, in, pivot);
            if (s.wrist_pivot)
                for (int k = 0; k < 3; ++k) s.pivot[k] = pivot[k];
            update_rotations(s);
            if (s.fly_time >= p.throw_max_time)
                ev.flight_over = true;
        }

        s.hover = false;
        for (int h = 0; h < 2; ++h)
        {
            const bool pressed = in.grip[h] && !s.was[h];
            const bool released = !in.grip[h] && s.was[h];
            s.was[h] = in.grip[h];
            if (!in.point_valid[h])
            {
                s.inside[h] = false;
                s.inside_age[h] = 1e9f;
                // A held Lara is not stuck when her hand's point is lost
                // (Legend would have skipped the hand, release included).
                if (s.hand != h)
                    continue;
            }
            const float* pt = in.point[h];
            const bool free = s.hand < 0 && !s.flying;
            if (in.point_valid[h])
            {
                int zone = zone_of(p, in, pt);
                const bool inside = zone != kPluckNone;
                if (inside && !s.inside[h] && free)
                    ev.hover_enter[h] = true;
                s.inside[h] = inside;
                if (inside && free)
                    s.hover = true;
                if (inside)
                {
                    s.inside_age[h] = 0.0f;
                    s.last_zone[h] = zone;
                }
                else
                {
                    s.inside_age[h] += step_ok ? dt : 0.0f;
                    if (s.inside_age[h] <= kPluckGraceSeconds)
                        zone = s.last_zone[h];
                }

                if (free && pressed && zone != kPluckNone && !in.part_held[h])
                {
                    s.hand = h;
                    s.part = zone;
                    for (int k = 0; k < 3; ++k)
                    {
                        s.grab_offset[k] = in.root[k] - pt[k];
                        s.last_hand[k] = pt[k];
                    }
                    clear_pendulum(s);
                    s.lifted = true;
                    s.settle = p.settle_seconds;
                    for (int k = 0; k < 3; ++k)
                    {
                        s.arm_offset[k] = 0.0f;
                        s.anchor[k] = in.root[k];
                    }
                    if (is_arm(zone))
                    {
                        // Held where it was gripped: her wrist keeps its
                        // offset from the pinch, so nothing jumps.
                        const int c = zone - kPluckArm0;
                        float start[3];
                        for (int k = 0; k < 3; ++k)
                        {
                            s.arm_offset[k] = in.arm_wrist[c][k] - pt[k];
                            s.lead_target[k] = s.anchor[k] = in.arm_wrist[c][k];
                            start[k] = in.arm_wrist[c][k] - in.arm_shoulder[c][k];
                        }
                        // Her reach, or (an arm hanging nearly straight is
                        // already there) 5 cm at her size past where it was.
                        const float from = length3(start) + 0.05f * p.metre;
                        s.lead_lift = in.arm_reach[c] > from ? in.arm_reach[c] : from;
                        s.lifted = !p.arm_lead;
                    }
                    ev.picked_up = true;
                    ev.hand = h;
                    ev.part = zone;
                    s.hover = false;
                }
            }
            if (s.hand != h)
                continue;

            if (in.point_valid[h])
            {
                if (s.settle > 0.0f)
                {
                    // Settling: the anchor stays put under the moving pinch.
                    for (int k = 0; k < 3; ++k)
                    {
                        if (is_arm(s.part))
                            s.arm_offset[k] = s.anchor[k] - pt[k];
                        else
                            s.grab_offset[k] = s.anchor[k] - pt[k];
                        s.last_hand[k] = pt[k];
                    }
                    s.settle -= step_ok ? dt : 0.0f;
                }
                for (int k = 0; k < 3; ++k) s.hang[k] = pt[k] + s.arm_offset[k];
                float pivot[3];
                s.wrist_pivot = pivot_of(s, in, pivot);
                if (s.wrist_pivot)
                    for (int k = 0; k < 3; ++k) s.pivot[k] = pivot[k];

                // Arm lead: only her arm follows until the hand pulls past
                // its reach; then she leaves the ground hanging from that
                // wrist, her body starting along the arm's line (shoulder
                // and wrist stay put) and swinging down under the hand.
                if (!s.lifted)
                {
                    for (int k = 0; k < 3; ++k) s.lead_target[k] = s.hang[k];
                    const int c = s.part - kPluckArm0;
                    if (in.arm_valid[c] && in.arm_reach[c] > 0.0f)
                    {
                        float d[3];
                        for (int k = 0; k < 3; ++k) d[k] = s.hang[k] - in.arm_shoulder[c][k];
                        const float len = length3(d);
                        if (len > s.lead_lift)
                        {
                            s.lifted = true;
                            ev.lifted = true;
                            ev.hand = h;
                            for (int k = 0; k < 2; ++k)
                            {
                                s.tilt[k] = asinf(clampf(-d[k] / len, -0.93f, 0.93f));
                                s.tilt_vel[k] = 0.0f;
                                for (auto& limb : s.limb_tilt) limb[k] = s.tilt[k];
                            }
                        }
                    }
                }
                for (int k = 0; k < 3; ++k)
                {
                    if (!s.lifted)
                        s.target[k] = in.root[k];
                    else
                        s.target[k] = s.hang[k] + (s.wrist_pivot ? in.root[k] - pivot[k]
                                                                 : s.grab_offset[k]);
                    s.carry[k] = s.target[k] - in.root[k];
                }

                // Dangle: a pendulum of a doll-sized length hanging from the
                // grab point, driven by the hand's horizontal acceleration
                // in real metres; the limbs follow on slower, lagging
                // pendulums with a little idle sway.
                if (step_ok)
                {
                    const float to_metres = 1.0f / p.table;
                    const float smooth_v = dt / (dt + 0.03f);
                    float step_v[3];
                    for (int k = 0; k < 3; ++k)
                        step_v[k] = (pt[k] - s.last_hand[k]) / dt * to_metres;
                    // A tracking jump (or a script ending) is no throw.
                    const bool jump = length3(step_v) > 4.0f * p.throw_max_speed;
                    for (int k = 0; k < 3; ++k)
                    {
                        const float v = jump ? s.hand_vel[k] : step_v[k];
                        if (k < 2)
                        {
                            const float a = (v - s.hand_vel[k]) / dt;
                            const float smooth = dt / (dt + 0.03f);
                            s.hand_acc[k] += (a - s.hand_acc[k]) * smooth;
                        }
                        s.hand_vel[k] = v;
                        s.last_hand[k] = pt[k];
                        s.vel_smooth[k] += (v - s.vel_smooth[k]) * smooth_v;
                    }
                    // A throw takes the fastest of the last 0.15 s: the
                    // hand slows while the fingers open.
                    s.peak_age += dt;
                    if (length3(s.vel_smooth) >= length3(s.peak_vel) || s.peak_age > 0.15f)
                    {
                        for (int k = 0; k < 3; ++k) s.peak_vel[k] = s.vel_smooth[k];
                        s.peak_age = 0.0f;
                    }
                    if (s.lifted)
                    {
                        const float length = 0.09f;   // metres, doll scale
                        const float gravity = 9.81f;
                        for (int k = 0; k < 2; ++k)
                        {
                            const float accel = -gravity / length *
                                sinf(s.tilt[k]) - 3.0f * s.tilt_vel[k] -
                                s.hand_acc[k] / length;
                            s.tilt_vel[k] += accel * dt;
                            s.tilt[k] = clampf(s.tilt[k] + s.tilt_vel[k] * dt, -1.2f, 1.2f);
                        }
                        swing_limbs(s, dt);
                    }
                }
                update_rotations(s);
            }

            // The floor under the held Lara, probed by the caller from a
            // little above her feet (she may be pushed slightly into a
            // surface): it is what a release lands on.
            s.floor_valid = in.floor_valid;
            if (in.floor_valid)
                for (int k = 0; k < 3; ++k) s.floor[k] = in.floor[k];

            if (!released)
                continue;
            ev.released = true;
            ev.hand = h;
            ev.part = s.part;
            ev.dropped_to_floor = s.floor_valid;
            for (int k = 0; k < 3; ++k) ev.target[k] = s.target[k];
            if (!s.lifted)
            {
                // Her arm let go before she left the ground.
                ev.moved = false;
                clear_hold(s);
                clear_pendulum(s);
                continue;
            }
            float speed = length3(s.peak_vel);
            if (p.throw_enabled && speed >= p.throw_min_speed)
            {
                const float cap = speed > p.throw_max_speed ? p.throw_max_speed / speed : 1.0f;
                speed *= cap;
                ev.thrown = true;
                ev.throw_speed = speed;
                s.flying = true;
                s.fly_time = 0.0f;
                for (int k = 0; k < 3; ++k)
                {
                    s.fly_vel[k] = s.peak_vel[k] * cap * p.throw_scale * p.table;
                    s.fly_offset[k] = s.target[k] - s.hang[k];
                    s.fly_prev[k] = s.target[k];
                }
                s.hand = -1;
                s.hover = false;
                continue;
            }
            clear_hold(s);
            clear_pendulum(s);
        }
    }

    Mat4 pluck_body_matrix(const PluckState& s, const PluckInput& in)
    {
        float grab[3];
        if (!pivot_of(s, in, grab))
            for (int k = 0; k < 3; ++k)
                grab[k] = in.root[k] - s.grab_offset[k];
        return translation(-grab[0], -grab[1], -grab[2]) * s.body_rotation *
               translation(s.hang[0], s.hang[1], s.hang[2]);
    }
}
