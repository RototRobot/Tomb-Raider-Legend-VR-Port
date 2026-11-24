#include "god_pluck.h"

#include <math.h>

namespace trlvr
{
    namespace
    {
        // A tilt vector (feet toward +x by tx, +y by ty): a turn of |tilt|
        // about the horizontal axis (ty, -tx). Straight down becomes
        // (sin tx, 0, -cos tx) for a tilt about one axis (as the Legend
        // port's hang direction), and it keeps turning past a half turn
        // (a tumbling throw).
        Mat4 tilt_rotation(float tx, float ty)
        {
            const float rv[3] = { ty, -tx, 0.0f };
            return rotation_from_vector(rv);
        }

        constexpr float kBodyLength = 0.09f;    // metres, doll scale: the body pendulum
        constexpr float kGravity = 9.81f;

        // Each limb's pendulum (0/1 arms, 2/3 legs): effective length
        // (metres, doll scale), its joint's distance below the grab in body
        // lengths (a hip near her centre of swing is barely pushed by the
        // hand, a shoulder near the grab is), muscle tone toward her pose
        // (1/s^2), damping of its swing from the body (1/s), and the kick a
        // throw gives it (rad/s per real m/s, turned by `kick_turn` radians
        // so the four do not flail as one). The right limbs are a little
        // longer: not quite in step with the left.
        struct LimbTune { float length, reach, tone, damp, kick, kick_turn; };
        constexpr LimbTune kLimbTune[4] = {
            { 0.050f, 0.25f, 160.0f, 10.0f, 2.5f,  0.7f },
            { 0.056f, 0.25f, 160.0f, 10.0f, 2.5f, -0.7f },
            // Legs looser and longer than before (user 2026-10-20: they
            // stayed too still): they swing visibly fore and aft as she swings.
            { 0.085f, 0.80f,  75.0f,  6.0f, 2.0f,  0.35f },
            { 0.095f, 0.80f,  75.0f,  6.0f, 2.0f, -0.35f },
        };

        float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

        float length3(const float v[3]) { return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

        // An arm grab's wrist offset easing to `goal` (her hand in the
        // pinch) over about kArmSnapSeconds.
        constexpr float kArmSnapSeconds = 0.2f;
        void ease_arm_snap(float offset[3], const float goal[3], float dt)
        {
            const float f = dt / (dt + kArmSnapSeconds);
            for (int k = 0; k < 3; ++k)
                offset[k] += (goal[k] - offset[k]) * f;
        }

        void clear_hold(PluckState& s)
        {
            s.hand = -1;
            s.hover = false;
            s.part = kPluckNone;
            s.lifted = true;
            s.flying = false;
            s.fly_time = 0.0f;
            for (int k = 0; k < 3; ++k)
                s.carry[k] = s.arm_offset[k] = 0.0f;
            s.fly = ThrowBody();
            s.hand2 = -1;
            s.part2 = kPluckNone;
            s.settle2 = 0.0f;
            s.two_t = 0.0f;
            s.release_chain = -1;
            for (int k = 0; k < 3; ++k)
                s.arm_offset2[k] = s.two_rel[k] = 0.0f;
        }

        // The second hand's velocity history, cleared.
        void clear_second_velocity(PluckState& s)
        {
            for (int k = 0; k < 3; ++k)
                s.hand_vel2[k] = s.vel_smooth2[k] = s.peak_vel2[k] = 0.0f;
            s.peak_age2 = 0.0f;
            throw_track_reset(s.track2);
        }

        void clear_pendulum(PluckState& s)
        {
            for (int k = 0; k < 3; ++k)
                s.hand_vel[k] = s.vel_smooth[k] = s.peak_vel[k] = 0.0f;
            s.peak_age = 0.0f;
            throw_track_reset(s.track);
            for (int k = 0; k < 2; ++k)
            {
                s.tilt[k] = s.tilt_vel[k] = s.hand_acc[k] = 0.0f;
                for (int l = 0; l < 4; ++l)
                    s.limb_tilt[l][k] = s.limb_swing[l][k] = s.limb_swing_vel[l][k] = 0.0f;
            }
            for (int k = 0; k < 3; ++k) s.spin_offset[k] = 0.0f;
            s.yaw = 0.0f;
            s.body_rotation = Mat4::identity();
            for (auto& r : s.limb_rotation) r = Mat4::identity();
            for (auto& r : s.knee_rotation) r = Mat4::identity();
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

        // The limbs' own pendulums, their swing q from the body (radians,
        // tilt terms per axis), for a frame in which the body's tilt had the
        // angular acceleration `body_acc`. Held, a limb's world angle
        // (tilt + q) swings under gravity and its joint's acceleration (the
        // hand's, plus the body's swing at the joint):
        //   q'' = -g/L sin(tilt + q) - a_joint/L - tone q - damp q' - tilt''
        // Flying, she is in free fall: gravity and the fall cancel, and only
        // tone, damping and the body's change of spin act on the swing.
        void swing_limbs(PluckState& s, float dt, const float body_acc[2], bool flying)
        {
            s.phase += dt * 6.2831853f * 1.3f;
            const int steps = dt > 0.01f ? (int)ceilf(dt / 0.01f) : 1;
            const float h = dt / steps;
            const float limit = flying ? 1.5f : 0.9f;
            for (int l = 0; l < 4; ++l)
            {
                const LimbTune& t = kLimbTune[l];
                // Thrown, she goes slack: less tone, less damping (she flails).
                const float tone = flying ? 0.35f * t.tone : t.tone;
                const float damp = flying ? 0.4f * t.damp : t.damp;
                for (int k = 0; k < 2; ++k)
                {
                    float q = s.limb_swing[l][k], v = s.limb_swing_vel[l][k];
                    const float joint = s.hand_acc[k] + t.reach * kBodyLength * body_acc[k];
                    for (int i = 0; i < steps; ++i)
                    {
                        float a = -tone * q - damp * v - body_acc[k];
                        if (!flying)
                            a -= (kGravity * sinf(s.tilt[k] + q) + joint) / t.length;
                        v += a * h;
                        q += v * h;
                        if (q > limit || q < -limit)
                        {
                            q = clampf(q, -limit, limit);
                            if (v * q > 0.0f) v = 0.0f;
                        }
                    }
                    s.limb_swing[l][k] = q;
                    s.limb_swing_vel[l][k] = v;
                }
            }
        }

        // A kick to the limbs' swing (a throw, a bounce): each against the
        // velocity change (vx, vy) (real m/s across), turned a little per limb.
        void kick_limbs(PluckState& s, float vx, float vy, float scale)
        {
            for (int l = 0; l < 4; ++l)
            {
                const LimbTune& t = kLimbTune[l];
                const float c = cosf(t.kick_turn), n = sinf(t.kick_turn);
                s.limb_swing_vel[l][0] -= scale * t.kick * (c * vx - n * vy);
                s.limb_swing_vel[l][1] -= scale * t.kick * (n * vx + c * vy);
            }
        }

        void update_rotations(PluckState& s, const PluckParams& p, const PluckInput& in)
        {
            s.body_rotation = tilt_rotation(s.tilt[0], s.tilt[1]);
            // Turned (two hands): yaw first, about the vertical. The limbs'
            // swing is in world terms, their rotations in her own frame:
            // turned back by the yaw (exactly the old maths at yaw 0).
            const float yc = cosf(s.yaw), ys = sinf(s.yaw);
            if (s.yaw != 0.0f)
            {
                const float rv[3] = { 0.0f, 0.0f, s.yaw };
                s.body_rotation = rotation_from_vector(rv) * s.body_rotation;
            }
            const float face[2] = { yc * in.facing[0] - ys * in.facing[1],
                                    ys * in.facing[0] + yc * in.facing[1] };
            const float idle_amp = s.flying ? 0.02f : 0.06f;
            for (int l = 0; l < 4; ++l)
            {
                const float idle = idle_amp * sinf(s.phase + 1.7f * l);
                const float w[2] = { p.ragdoll * s.limb_swing[l][0] + idle,
                                     p.ragdoll * s.limb_swing[l][1] - 0.5f * idle };
                float q[2] = { yc * w[0] + ys * w[1], -ys * w[0] + yc * w[1] };
                if (l >= 2)
                {
                    // Legs a little apart: each out to her side.
                    const float side = l == 2 ? -p.leg_spread : p.leg_spread;
                    q[0] += side * in.right[0];
                    q[1] += side * in.right[1];
                }
                s.limb_tilt[l][0] = s.tilt[0] + yc * q[0] - ys * q[1];
                s.limb_tilt[l][1] = s.tilt[1] + ys * q[0] + yc * q[1];
                s.limb_rotation[l] = tilt_rotation(q[0], q[1]);
                if (l < 2)
                    continue;
                // Knees: a relaxed bend, more as the thigh swings forward
                // (held: forward of straight down, the shin hanging toward
                // gravity; flying: forward of the body). The shin turns
                // back about the knee.
                const float* swing = s.flying ? q : s.limb_tilt[l];
                const float* fc = s.flying ? in.facing : face;
                const float forward = swing[0] * fc[0] + swing[1] * fc[1];
                float bend = p.knee_bend + 0.6f * p.ragdoll * (forward > 0.0f ? forward : 0.0f);
                bend = clampf(bend, 0.0f, 1.3f);
                s.knee_rotation[l - 2] = tilt_rotation(-bend * in.facing[0], -bend * in.facing[1]);
            }
        }

        float smooth01(float t)
        {
            t = clampf(t, 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        }

        // Two hands: the hang's pivot moves from the raised wrist to her
        // shoulder midpoint by the blend weight (an arm hold, both arms known).
        bool two_blend_on(const PluckState& s, const PluckInput& in)
        {
            return s.two_t > 0.0f && is_arm(s.part) && in.arm_valid[0] && in.arm_valid[1];
        }

        // The wrist she hangs from, blended toward her shoulder midpoint by
        // a two-hand hold (pivot_of when one hand holds her).
        bool hold_pivot(const PluckState& s, const PluckInput& in, float out[3])
        {
            if (!pivot_of(s, in, out))
                return false;
            if (two_blend_on(s, in))
            {
                const float e = smooth01(s.two_t);
                for (int k = 0; k < 3; ++k)
                {
                    const float mid = 0.5f * (in.arm_shoulder[0][k] + in.arm_shoulder[1][k]);
                    out[k] += (mid - out[k]) * e;
                }
            }
            return true;
        }

        // The point she is held by: the wrist pivot, or her root less the grab offset.
        void grab_point(const PluckState& s, const PluckInput& in, float out[3])
        {
            if (!hold_pivot(s, in, out))
                for (int k = 0; k < 3; ++k)
                    out[k] = in.root[k] - s.grab_offset[k];
        }

        // The other arm's zone as she is drawn (a second grab): a capsule
        // from the drawn shoulder to the drawn fingertips, the arm radius.
        // Still standing (an arm lead, not lifted): that arm is her real one.
        bool in_drawn_arm(const PluckParams& p, const PluckInput& in, int c, const float pt[3],
                          bool standing)
        {
            if (c < 0 || c > 1 || p.arm_radius <= 0.0f)
                return false;
            const float r = p.arm_radius * p.metre + 0.01f * p.metre;
            if (standing)
                return in.arm_valid[c] && pluck_segment_distance(in.arm_wrist[c], in.arm_end[c], pt) <= r;
            if (!in.drawn_arm_valid[c])
                return false;
            return pluck_segment_distance(in.drawn_wrist[c], in.drawn_end[c], pt) <= r;
        }

        // The drawn hand velocity v with its speed capped to the real
        // controller's (real_speed < 0: unknown, unchanged). Only scales
        // down, so without an arm stretch it is the drawn velocity. (The
        // stretch's move used to be subtracted instead; a mismatch with the
        // drawn motion flipped fast throws back toward the pick-up: user
        // 2026-11-21, boomerang throws.)
        void cap_to_real(const float v[3], float real_speed, float out[3])
        {
            const float sp = length3(v);
            const float f = (real_speed >= 0.0f && sp > real_speed && sp > 1e-6f)
                                ? real_speed / sp : 1.0f;
            for (int k = 0; k < 3; ++k) out[k] = v[k] * f;
        }

        // A hand's velocity in real m/s from its pinch point, smoothed, and
        // the peak of the last 0.15 s (the second hand; the first hand's is
        // inline in pluck_step, where it also drives the pendulum).
        void track_hand(const PluckParams& p, float dt, const float pt[3],
                        float real_speed, float last[3],
                        float vel[3], float smooth[3], float peak[3], float& peak_age,
                        ThrowTracker& track)
        {
            float step_v[3], real_v[3];
            for (int k = 0; k < 3; ++k)
                step_v[k] = (pt[k] - last[k]) / dt / p.table;
            cap_to_real(step_v, real_speed, real_v);
            const bool jump = length3(step_v) > 4.0f * p.throw_max_speed;
            if (!jump)
                throw_track_add(track, real_v, nullptr, dt);
            const float sv = dt / (dt + 0.03f);
            for (int k = 0; k < 3; ++k)
            {
                const float v = jump ? vel[k] : step_v[k];
                vel[k] = v;
                last[k] = pt[k];
                smooth[k] += ((jump ? v : real_v[k]) - smooth[k]) * sv;
            }
            peak_age += dt;
            if (length3(smooth) >= length3(peak) || peak_age > 0.15f)
            {
                for (int k = 0; k < 3; ++k) peak[k] = smooth[k];
                peak_age = 0.0f;
            }
        }

        // What a throw takes from a hand (real m/s; second: the second
        // hand): throw_velocity 1 the tracker's estimate, 0 the old peak.
        void release_velocity(const PluckState& s, const PluckParams& p, bool second, float out[3])
        {
            const float* peak = second ? s.peak_vel2 : s.peak_vel;
            if (p.throw_velocity == 0 ||
                !throw_track_estimate(second ? s.track2 : s.track, 0.15f, out, nullptr))
                for (int k = 0; k < 3; ++k) out[k] = peak[k];
        }

        // Two hands, once per frame of the first hand's hold (s.hang = its
        // wrist target already): ramps the blend, and while both hold her
        // places her shoulder midpoint under the point between the wrists,
        // as high as the higher hand's arm needs, and turns her shoulder
        // line toward the hand-to-hand direction (eased). Then the hang
        // point moves from the first wrist by the eased weight.
        void two_hand_hang(PluckState& s, const PluckParams& p, const PluckInput& in,
                           float dt, bool step_ok)
        {
            const int c1 = s.part - kPluckArm0;
            for (int k = 0; k < 3; ++k) s.wrist_target[c1][k] = s.hang[k];
            const float rate = p.two_blend_seconds > 0.0f ? dt / p.two_blend_seconds : 1.0f;
            if (step_ok)
                s.two_t = clampf(s.two_t + (s.hand2 >= 0 ? rate : -rate), 0.0f, 1.0f);
            if (s.two_t <= 0.0f)
                s.release_chain = -1;
            if (s.hand2 >= 0 && is_arm(s.part2) && in.arm_valid[0] && in.arm_valid[1])
            {
                const int c2 = s.part2 - kPluckArm0;
                const float* wa = s.wrist_target[c1];
                const float* wb = s.wrist_target[c2];
                const float dx = wb[0] - wa[0], dy = wb[1] - wa[1];
                const float span = sqrtf(dx * dx + dy * dy);
                const float sx = in.arm_shoulder[c2][0] - in.arm_shoulder[c1][0];
                const float sy = in.arm_shoulder[c2][1] - in.arm_shoulder[c1][1];
                const float shoulders = sqrtf(sx * sx + sy * sy);
                // Each arm's horizontal reach out (or in) from its shoulder,
                // the hands apart past her shoulders; past 0.9 of the arm she
                // is held no higher (the arms then fall short of the hands).
                const float out = fabsf(0.5f * span - 0.5f * shoulders);
                float shoulder_z = -1e30f;
                for (int i = 0; i < 2; ++i)
                {
                    const int c = i ? c2 : c1;
                    const float r = in.arm_reach[c] > 0.0f ? in.arm_reach[c] : 0.6f * p.metre;
                    const float o = out < 0.9f * r ? out : 0.9f * r;
                    const float z = s.wrist_target[c][2] - sqrtf(r * r - o * o);
                    if (z > shoulder_z)
                        shoulder_z = z;
                }
                s.two_rel[0] = 0.5f * (wa[0] + wb[0]) - wa[0];
                s.two_rel[1] = 0.5f * (wa[1] + wb[1]) - wa[1];
                s.two_rel[2] = shoulder_z - wa[2];
                if (step_ok && span > 0.03f * p.metre && shoulders > 1e-3f)
                {
                    const float want = atan2f(dy, dx) - atan2f(sy, sx);
                    float d = want - s.yaw;
                    while (d > 3.14159265f) d -= 6.2831853f;
                    while (d < -3.14159265f) d += 6.2831853f;
                    s.yaw += d * (1.0f - expf(-dt / 0.15f));
                    while (s.yaw > 3.14159265f) s.yaw -= 6.2831853f;
                    while (s.yaw < -3.14159265f) s.yaw += 6.2831853f;
                }
            }
            if (!two_blend_on(s, in))
                return;
            const float e = smooth01(s.two_t);
            for (int k = 0; k < 3; ++k) s.hang[k] += e * s.two_rel[k];
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
                const float r = p.arm_radius * p.metre + 0.01f * p.metre;
                for (int c = 0; c < 2; ++c)
                {
                    if (!in.arm_valid[c])
                        continue;
                    const float dist = pluck_segment_distance(in.arm_wrist[c], in.arm_end[c], pt);
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

    // Every zone is sized at her scale, margins too (user 2026-11-12: the
    // hand margins were player metres, so a small Lara had huge zones).
    float pluck_head_radius(const PluckParams& p)
    {
        return p.head_radius * p.metre + 0.01f * p.metre;
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
        const float r = reach * p.metre + 0.03f * p.metre;
        return pluck_segment_distance(feet, head_top, point) <= r;
    }

    void pluck_zone(const PluckParams& p, const float root[3],
                    float* radius, float* bottom, float* top)
    {
        // The zone is her feet (no palm platform here: see god_pluck.h).
        *radius = 0.30f * p.metre + 0.04f * p.metre;
        *bottom = root[2] - 0.03f * p.metre;
        *top = root[2] + 1.8f * p.metre + 0.04f * p.metre;
    }

    bool pluck_drawn(const PluckState& s)
    {
        return (s.hand >= 0 && s.lifted) || s.flying;
    }

    bool pluck_nudge(PluckState& s, const float velocity[3], float strength, float dt)
    {
        if (!pluck_drawn(s) || !(s.table > 0.0f) || !(dt > 0.0f) || dt > 0.1f)
            return false;
        const float vx = velocity[0] / s.table, vy = velocity[1] / s.table;
        const float speed = sqrtf(vx * vx + vy * vy);
        if (!(speed > 0.05f))
            return false;
        const float cap = speed > 4.0f ? 4.0f / speed : 1.0f;
        const float ease = (dt * 8.0f < 1.0f ? dt * 8.0f : 1.0f) * strength * cap;
        if (!s.flying && s.hand >= 0)
        {
            // The body hangs from the hand: pushed along +x it swings toward +x.
            s.tilt_vel[0] += 0.1f * ease * vx / kBodyLength;
            s.tilt_vel[1] += 0.1f * ease * vy / kBodyLength;
        }
        kick_limbs(s, vx, vy, ease);
        return true;
    }

    void pluck_land(PluckState& s)
    {
        clear_hold(s);
        clear_pendulum(s);
    }

    void pluck_fly_move(PluckState& s, const float centre[3])
    {
        if (!s.flying)
            return;
        float d[3];
        for (int k = 0; k < 3; ++k) d[k] = centre[k] - s.fly.pos[k];
        throw_body_move(s.fly, centre);
        for (int k = 0; k < 3; ++k)
        {
            s.hang[k] += d[k];
            s.target[k] += d[k];
            s.fly_prev[k] = s.target[k];
            s.carry[k] = s.target[k] - s.root[k];
        }
    }

    void pluck_bounce(PluckState& s, const float centre[3], const float normal[3],
                      float restitution, float friction)
    {
        if (!s.flying)
            return;
        const float before[2] = { s.fly.vel[0], s.fly.vel[1] };
        pluck_fly_move(s, centre);
        throw_body_bounce(s.fly, normal, restitution, friction);
        // The knock (only the drawn pose): her spin turns back at half its
        // rate and the limbs fly on past the hit.
        for (int k = 0; k < 2; ++k) s.tilt_vel[k] *= -0.5f;
        if (s.table > 0.0f)
            kick_limbs(s, (s.fly.vel[0] - before[0]) / s.table,
                       (s.fly.vel[1] - before[1]) / s.table, 0.5f);
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
        s.table = p.table;
        const float dt = in.dt;
        const bool step_ok = dt > 0.0f && dt <= 0.1f && p.table > 0.0f;

        // Thrown: the hang point flies under gravity; nothing holds her
        // straight, so she keeps turning as she left the hand, slowing a
        // little. The caller watches fly_prev -> target for what she hits.
        if (s.flying)
        {
            if (step_ok)
            {
                for (int k = 0; k < 3; ++k) s.fly_prev[k] = s.target[k];
                throw_body_step(s.fly, p.throw_gravity * p.table, 0.0f, dt, nullptr);
                s.fly_time += dt;
                // No clamp: she may tumble (drawn about her centre,
                // pluck_body_matrix). The spin slows a little; its change
                // swings the limbs.
                float body_acc[2];
                for (int k = 0; k < 2; ++k)
                {
                    const float before = s.tilt_vel[k];
                    s.tilt_vel[k] /= 1.0f + 1.5f * dt;
                    body_acc[k] = (s.tilt_vel[k] - before) / dt;
                    s.tilt[k] += s.tilt_vel[k] * dt;
                }
                swing_limbs(s, dt, body_acc, true);
            }
            for (int k = 0; k < 3; ++k) s.hang[k] = s.fly.pos[k] - s.spin_offset[k];
            float pivot[3];
            s.wrist_pivot = pivot_of(s, in, pivot);
            if (s.wrist_pivot)
                for (int k = 0; k < 3; ++k) s.pivot[k] = pivot[k];
            update_rotations(s, p, in);
            // Her feet as drawn (turned with the tumble about her centre).
            {
                const Mat4 m = pluck_body_matrix(s, in);
                for (int k = 0; k < 3; ++k)
                {
                    s.target[k] = in.root[0] * m.m[0][k] + in.root[1] * m.m[1][k] +
                                  in.root[2] * m.m[2][k] + m.m[3][k];
                    s.carry[k] = s.target[k] - in.root[k];
                }
            }
            if (s.fly_time >= p.throw_max_time)
                ev.flight_over = true;
        }

        // The zones are round her real body (Legend carries no palm shift).
        const PluckInput& zin = in;

        s.hover = false;
        bool pressed_now[2], released_now[2];
        for (int h = 0; h < 2; ++h)
        {
            pressed_now[h] = in.grip[h] && !s.was[h];
            released_now[h] = !in.grip[h] && s.was[h];
        }

        // Two hands: the second hand first (the first hand's hang below
        // needs its wrist). It lets go (she hangs on by the first), the
        // first lets go (the second takes its place), or both (the first
        // hand's release below throws, with the faster hand's velocity).
        if (s.hand2 >= 0)
        {
            const int h2 = s.hand2;
            const int c2 = s.part2 - kPluckArm0;
            if (!p.two_hands || s.hand < 0 || s.flying || !is_arm(s.part2))
            {
                s.release_chain = is_arm(s.part2) ? c2 : -1;
                s.hand2 = -1;
                s.part2 = kPluckNone;
            }
            else if (released_now[h2] && released_now[s.hand])
            {
                float v1[3], v2[3];
                release_velocity(s, p, false, v1);
                release_velocity(s, p, true, v2);
                if (length3(v2) > length3(v1))
                {
                    for (int k = 0; k < 3; ++k) s.peak_vel[k] = s.peak_vel2[k];
                    s.track = s.track2;
                }
                s.hand2 = -1;       // part2 stays: both arms stay up in a flight
            }
            else if (released_now[h2])
            {
                ev.second_release = true;
                ev.hand = h2;
                ev.part = s.part2;
                s.release_chain = c2;
                s.hand2 = -1;
                s.part2 = kPluckNone;
            }
            else if (released_now[s.hand])
            {
                // The second hand takes the first's place. The hang point
                // stays where it was: two_rel is moved onto the new first
                // wrist, and the blend runs back from there.
                const int c1 = s.part - kPluckArm0;
                ev.second_release = true;
                ev.hand = s.hand;
                ev.part = s.part;
                for (int k = 0; k < 3; ++k)
                {
                    s.two_rel[k] += s.wrist_target[c1][k] - s.wrist_target[c2][k];
                    s.arm_offset[k] = s.arm_offset2[k];
                    s.arm_snap[k] = s.arm_snap2[k];
                    s.anchor[k] = s.anchor2[k];
                    s.last_hand[k] = s.last_hand2[k];
                    s.vel_smooth[k] = s.vel_smooth2[k];
                    s.peak_vel[k] = s.peak_vel2[k];
                }
                s.peak_age = s.peak_age2;
                s.track = s.track2;
                s.settle = s.settle2;
                s.lead_lift = s.lead_lift2;
                s.hand = h2;
                s.part = s.part2;
                s.release_chain = c1;
                s.hand2 = -1;
                s.part2 = kPluckNone;
            }
            else if (in.point_valid[h2])
            {
                const float* pt = in.point[h2];
                if (s.settle2 > 0.0f)
                {
                    for (int k = 0; k < 3; ++k)
                    {
                        s.arm_offset2[k] = s.anchor2[k] - pt[k];
                        s.last_hand2[k] = pt[k];
                    }
                    s.settle2 -= step_ok ? dt : 0.0f;
                }
                else if (step_ok)
                    ease_arm_snap(s.arm_offset2, s.arm_snap2, dt);
                if (step_ok)
                    track_hand(p, dt, pt, in.real_speed[s.hand2], s.last_hand2, s.hand_vel2, s.vel_smooth2,
                               s.peak_vel2, s.peak_age2, s.track2);
                for (int k = 0; k < 3; ++k) s.wrist_target[c2][k] = pt[k] + s.arm_offset2[k];
            }
        }

        for (int h = 0; h < 2; ++h)
        {
            const bool pressed = pressed_now[h];
            const bool released = released_now[h];
            s.was[h] = in.grip[h];
            const bool point_ok = in.point_valid[h];
            if (!point_ok)
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
            // Two hands: while the other hand holds her by an arm, this
            // hand's zone is her other arm as drawn.
            // (Also while she still stands, the first arm leading: user
            // 2026-10-20, both arms taken on the ground.)
            const bool second_ok = p.two_hands && s.hand >= 0 && s.hand != h && s.hand2 < 0 &&
                                   !s.flying && is_arm(s.part) &&
                                   in.arm_valid[0] && in.arm_valid[1];
            const int other_arm = is_arm(s.part) ? 1 - (s.part - kPluckArm0) : -1;
            if (point_ok)
            {
                int zone = second_ok ? (in_drawn_arm(p, in, other_arm, pt, !s.lifted)
                                            ? kPluckArm0 + other_arm : kPluckNone)
                                     : zone_of(p, zin, pt);
                const bool inside = zone != kPluckNone;
                if (inside && !s.inside[h] && (free || second_ok))
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
                        s.grab_offset[k] = zin.root[k] - pt[k];
                        s.last_hand[k] = pt[k];
                    }
                    clear_pendulum(s);
                    s.lifted = true;
                    s.settle = p.settle_seconds;
                    for (int k = 0; k < 3; ++k)
                    {
                        s.arm_offset[k] = 0.0f;
                        s.anchor[k] = zin.root[k];
                    }
                    if (is_arm(zone))
                    {
                        // Held where it was gripped: her wrist keeps its
                        // offset from the pinch, so nothing jumps.
                        const int c = zone - kPluckArm0;
                        float start[3];
                        for (int k = 0; k < 3; ++k)
                        {
                            s.arm_offset[k] = zin.arm_wrist[c][k] - pt[k];
                            s.arm_snap[k] = 0.5f * (zin.arm_wrist[c][k] - zin.arm_end[c][k]);
                            s.lead_target[k] = s.anchor[k] = zin.arm_wrist[c][k];
                            start[k] = zin.arm_wrist[c][k] - zin.arm_shoulder[c][k];
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
                    s.hand2 = -1;
                    s.part2 = kPluckNone;
                    s.two_t = 0.0f;
                    s.release_chain = -1;
                }
                else if (second_ok && pressed && zone == kPluckArm0 + other_arm &&
                         !in.part_held[h])
                {
                    // The other arm, held where it is drawn: its wrist keeps
                    // its offset from the pinch (and settles as the first).
                    s.hand2 = h;
                    s.part2 = zone;
                    s.settle2 = p.settle_seconds;
                    const float* wrist2 = s.lifted ? in.drawn_wrist[other_arm] : in.arm_wrist[other_arm];
                    const float* end2 = s.lifted ? in.drawn_end[other_arm] : in.arm_end[other_arm];
                    for (int k = 0; k < 3; ++k)
                        s.arm_snap2[k] = 0.5f * (wrist2[k] - end2[k]);
                    float start2[3];
                    for (int k = 0; k < 3; ++k)
                        start2[k] = wrist2[k] - in.arm_shoulder[other_arm][k];
                    const float from2 = length3(start2) + 0.05f * p.metre;
                    s.lead_lift2 = in.arm_reach[other_arm] > from2 ? in.arm_reach[other_arm] : from2;
                    for (int k = 0; k < 3; ++k)
                    {
                        s.anchor2[k] = wrist2[k];
                        s.arm_offset2[k] = s.anchor2[k] - pt[k];
                        s.last_hand2[k] = pt[k];
                        s.wrist_target[other_arm][k] = s.anchor2[k];
                    }
                    clear_second_velocity(s);
                    if (s.release_chain == other_arm)
                        s.release_chain = -1;
                    ev.second_grab = true;
                    ev.hand = h;
                    ev.part = zone;
                }
            }
            if (s.hand != h)
                continue;

            if (point_ok)
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
                else if (step_ok && is_arm(s.part))
                    ease_arm_snap(s.arm_offset, s.arm_snap, dt);
                for (int k = 0; k < 3; ++k) s.hang[k] = pt[k] + s.arm_offset[k];
                if (is_arm(s.part))
                    for (int k = 0; k < 3; ++k) s.wrist_target[s.part - kPluckArm0][k] = s.hang[k];
                if (s.lifted && is_arm(s.part) && (s.hand2 >= 0 || s.two_t > 0.0f))
                    two_hand_hang(s, p, in, dt, step_ok);
                float pivot[3];
                s.wrist_pivot = hold_pivot(s, in, pivot);
                if (s.wrist_pivot)
                    for (int k = 0; k < 3; ++k) s.pivot[k] = pivot[k];

                // Arm lead: only her arm follows until the hand pulls past
                // its reach; then she leaves the ground hanging from that
                // wrist, her body starting along the arm's line (shoulder
                // and wrist stay put) and swinging down under the hand.
                if (!s.lifted)
                {
                    for (int k = 0; k < 3; ++k) s.lead_target[k] = s.hang[k];
                    // Either held arm pulled past its reach lifts her (the
                    // second, if any, through wrist_target).
                    for (int which = 0; which < 2 && !s.lifted; ++which)
                    {
                        if (which == 1 && (s.hand2 < 0 || !is_arm(s.part2)))
                            break;
                        const int c = (which ? s.part2 : s.part) - kPluckArm0;
                        const float* w = which ? s.wrist_target[c] : s.hang;
                        const float lift = which ? s.lead_lift2 : s.lead_lift;
                        if (!in.arm_valid[c] || in.arm_reach[c] <= 0.0f)
                            continue;
                        float d[3];
                        for (int k = 0; k < 3; ++k) d[k] = w[k] - in.arm_shoulder[c][k];
                        const float len = length3(d);
                        if (len > lift)
                        {
                            s.lifted = true;
                            ev.lifted = true;
                            ev.hand = h;
                            for (int k = 0; k < 2; ++k)
                            {
                                s.tilt[k] = asinf(clampf(-d[k] / len, -0.93f, 0.93f));
                                s.tilt_vel[k] = 0.0f;
                                for (int l = 0; l < 4; ++l)
                                {
                                    s.limb_tilt[l][k] = s.tilt[k];
                                    s.limb_swing[l][k] = s.limb_swing_vel[l][k] = 0.0f;
                                }
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
                    // step_v: the drawn hand (drives the pendulum); real_v:
                    // capped to the real controller's speed, for the throw.
                    float step_v[3], real_v[3];
                    for (int k = 0; k < 3; ++k)
                        step_v[k] = (pt[k] - s.last_hand[k]) / dt * to_metres;
                    cap_to_real(step_v, in.real_speed[s.hand], real_v);
                    // A tracking jump (or a script ending) is no throw.
                    const bool jump = length3(step_v) > 4.0f * p.throw_max_speed;
                    if (!jump)
                        throw_track_add(s.track, real_v, nullptr, dt);
                    // Two hands: the pendulum is driven by the point between them.
                    const float two_e = s.hand2 >= 0 ? 0.5f * smooth01(s.two_t) : 0.0f;
                    for (int k = 0; k < 3; ++k)
                    {
                        const float v = jump ? s.hand_vel[k] : step_v[k];
                        const float drive = s.hand2 >= 0 ? v + two_e * (s.hand_vel2[k] - v) : v;
                        if (k < 2)
                        {
                            const float a = (drive - s.hand_vel[k]) / dt;
                            const float smooth = dt / (dt + 0.03f);
                            s.hand_acc[k] += (a - s.hand_acc[k]) * smooth;
                        }
                        s.hand_vel[k] = drive;
                        s.last_hand[k] = pt[k];
                        s.vel_smooth[k] += ((jump ? v : real_v[k]) - s.vel_smooth[k]) * smooth_v;
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
                        float body_acc[2];
                        for (int k = 0; k < 2; ++k)
                        {
                            const float accel = -kGravity / kBodyLength *
                                sinf(s.tilt[k]) - 3.0f * s.tilt_vel[k] -
                                s.hand_acc[k] / kBodyLength;
                            body_acc[k] = accel;
                            s.tilt_vel[k] += accel * dt;
                            s.tilt[k] = clampf(s.tilt[k] + s.tilt_vel[k] * dt, -1.2f, 1.2f);
                        }
                        swing_limbs(s, dt, body_acc, false);
                        // Both arms held: their own pendulums rest, so an
                        // arm let go swings on from where it is drawn.
                        if (s.hand2 >= 0)
                            for (int c = 0; c < 2; ++c)
                            {
                                const int l = in.arm_limb[c];
                                if (l < 0 || l > 1)
                                    continue;
                                for (int k = 0; k < 2; ++k)
                                    s.limb_swing[l][k] = s.limb_swing_vel[l][k] = 0.0f;
                            }
                    }
                }
                update_rotations(s, p, in);
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
            float rel_v[3];
            release_velocity(s, p, false, rel_v);
            float speed = length3(rel_v);
            if (p.throw_enabled && speed >= p.throw_min_speed)
            {
                const float cap = speed > p.throw_max_speed ? p.throw_max_speed / speed : 1.0f;
                speed *= cap;
                ev.thrown = true;
                ev.throw_speed = speed;
                s.flying = true;
                s.fly_time = 0.0f;
                float fly_vel[3];
                for (int k = 0; k < 3; ++k)
                {
                    fly_vel[k] = rel_v[k] * cap * p.throw_scale * p.table;
                    s.fly_prev[k] = s.target[k];
                }
                // Ragdoll: the throw starts a tumble across its direction
                // and kicks the limbs back against it; from here she turns
                // about her centre, drawn where it was at the release.
                {
                    const float vx = fly_vel[0] / p.table, vy = fly_vel[1] / p.table;
                    // The swing she had, at most 3 rad/s (a hand stopping
                    // dead as it lets go would spin her wildly otherwise:
                    // held, the tilt clamp hid that).
                    const float swing = sqrtf(s.tilt_vel[0] * s.tilt_vel[0] +
                                              s.tilt_vel[1] * s.tilt_vel[1]);
                    if (swing > 3.0f)
                        for (float& w : s.tilt_vel) w *= 3.0f / swing;
                    s.tilt_vel[0] += p.throw_spin * vx;
                    s.tilt_vel[1] += p.throw_spin * vy;
                    kick_limbs(s, vx, vy, 1.0f);
                    float grab[3], d[3];
                    grab_point(s, in, grab);
                    for (int k = 0; k < 3; ++k) d[k] = 0.5f * (in.root[k] - grab[k]);
                    const Mat4& r = s.body_rotation;
                    for (int k = 0; k < 3; ++k)
                        s.spin_offset[k] = d[0] * r.m[0][k] + d[1] * r.m[1][k] + d[2] * r.m[2][k];
                }
                // The projectile is her drawn centre (pluck_body_matrix's).
                float centre[3];
                for (int k = 0; k < 3; ++k) centre[k] = s.hang[k] + s.spin_offset[k];
                throw_body_start(s.fly, centre, fly_vel, nullptr,
                                 kThrowRadius * p.throw_radius * p.metre);
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
        grab_point(s, in, grab);
        if (s.flying)
        {
            float c[3];
            for (int k = 0; k < 3; ++k) c[k] = grab[k] + 0.5f * (in.root[k] - grab[k]);
            return translation(-c[0], -c[1], -c[2]) * s.body_rotation *
                   translation(s.hang[0] + s.spin_offset[0], s.hang[1] + s.spin_offset[1],
                               s.hang[2] + s.spin_offset[2]);
        }
        return translation(-grab[0], -grab[1], -grab[2]) * s.body_rotation *
               translation(s.hang[0], s.hang[1], s.hang[2]);
    }

    void pluck_centre(const PluckState& s, const PluckInput& in, float out[3])
    {
        if (s.flying)
        {
            for (int k = 0; k < 3; ++k) out[k] = s.fly.pos[k];
            return;
        }
        float grab[3], c[3];
        grab_point(s, in, grab);
        for (int k = 0; k < 3; ++k) c[k] = grab[k] + 0.5f * (in.root[k] - grab[k]);
        const Mat4 m = pluck_body_matrix(s, in);
        for (int k = 0; k < 3; ++k)
            out[k] = c[0] * m.m[0][k] + c[1] * m.m[1][k] + c[2] * m.m[2][k] + m.m[3][k];
    }

    float pluck_two_weight(const PluckState& s)
    {
        return is_arm(s.part) ? smooth01(s.two_t) : 0.0f;
    }

    bool pluck_arm_reaches(const PluckState& s, int chain)
    {
        if (!is_arm(s.part) || (s.two_t <= 0.0f && !is_arm(s.part2)))
            return false;
        return chain == s.part - kPluckArm0 || (is_arm(s.part2) && chain == s.part2 - kPluckArm0);
    }
}
