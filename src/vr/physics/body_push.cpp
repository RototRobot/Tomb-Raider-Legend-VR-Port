#include "body_push.h"

#include <cmath>
#include <cstring>

namespace trlvr
{
    namespace
    {
        const float kTwoPi = 6.2831853f;
        const float kDegrees = 0.017453293f;
        // The kick of a hand arriving at the speed cap shows as this share of
        // the segment's maximum lean at its peak (a critically damped spring
        // started at v0 peaks at v0 / (omega * e)).
        const float kKickPeak = 0.6f;
        const float kE = 2.7182818f;
        // Same terms as soft_body's arrival: capped at 2.5 m/s, only when
        // moving into the surface faster than 0.2 m/s.
        const float kSpeedCap = 2.5f;
        const float kSpeedMin = 0.2f;
        const float kRestAngle = 1.0e-4f;       // radians
        const float kRestSpeed = 1.0e-3f;       // radians per second

        float dot(const float* a, const float* b)
        {
            return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
        }

        bool finite3(const float* a)
        {
            return std::isfinite(a[0]) && std::isfinite(a[1]) && std::isfinite(a[2]);
        }

        float clampf(float v, float lo, float hi)
        {
            return v < lo ? lo : v > hi ? hi : v;
        }

        // The lean after the soft limit: unchanged up to 80% of `max`, then
        // eased so it approaches `max` without a corner.
        void soft_limit(const float in[2], float max, float out[2])
        {
            const float m = std::sqrt(in[0] * in[0] + in[1] * in[1]);
            const float knee = 0.8f * max;
            if (!(max > 0.0f))
            {
                out[0] = out[1] = 0.0f;
                return;
            }
            if (m <= knee || m < 1.0e-9f)
            {
                out[0] = in[0];
                out[1] = in[1];
                return;
            }
            const float span = max - knee;
            const float limited = knee + span * std::tanh((m - knee) / span);
            out[0] = in[0] / m * limited;
            out[1] = in[1] / m * limited;
        }

        void make_basis(BodyPushState& s, const float up[3])
        {
            for (int k = 0; k < 3; ++k)
                s.up[k] = up[k];
            const float ref[3] = { std::fabs(up[0]) < 0.9f ? 1.0f : 0.0f,
                                   std::fabs(up[0]) < 0.9f ? 0.0f : 1.0f, 0.0f };
            const float d = dot(ref, up);
            float len = 0.0f;
            for (int k = 0; k < 3; ++k)
            {
                s.e1[k] = ref[k] - d * up[k];
                len += s.e1[k] * s.e1[k];
            }
            len = std::sqrt(len);
            for (int k = 0; k < 3; ++k)
                s.e1[k] /= len;
            // e2 = up x e1
            s.e2[0] = up[1] * s.e1[2] - up[2] * s.e1[1];
            s.e2[1] = up[2] * s.e1[0] - up[0] * s.e1[2];
            s.e2[2] = up[0] * s.e1[1] - up[1] * s.e1[0];
            s.valid = true;
        }

        void clear_motion(BodyPushState& s)
        {
            std::memset(s.angle, 0, sizeof(s.angle));
            std::memset(s.vel, 0, sizeof(s.vel));
            std::memset(s.shown, 0, sizeof(s.shown));
        }
    }

    void body_push_reset(BodyPushState& state)
    {
        state = BodyPushState();
    }

    void body_push_step(BodyPushState& s, const BodyPushParams& params,
                        const BodyPushContact* contacts, int n,
                        const float pivot[kPushSegments][3], const float up_in[3],
                        float metre, float dt)
    {
        float up[3] = { up_in[0], up_in[1], up_in[2] };
        const float up_len = finite3(up) ? std::sqrt(dot(up, up)) : 0.0f;
        if (!(up_len > 1.0e-6f))
        {
            body_push_reset(s);
            return;
        }
        for (float& c : up)
            c /= up_len;
        if (!s.valid || dot(up, s.up) < 0.9999f)
        {
            clear_motion(s);
            make_basis(s, up);
        }
        // Anything non-finite in the state is dropped, not propagated.
        for (int i = 0; i < kPushSegments; ++i)
            for (int k = 0; k < 2; ++k)
                if (!std::isfinite(s.angle[i][k]) || !std::isfinite(s.vel[i][k]))
                    clear_motion(s);

        float max_rad[kPushSegments];
        for (int i = 0; i < kPushSegments; ++i)
        {
            const float d = params.max_degrees[i];
            max_rad[i] = std::isfinite(d) && d > 0.0f ? d * kDegrees : 0.0f;
        }
        // A leg's foot moves reach x lean: cut the lean so it stays small.
        for (int leg = 0; leg < 2; ++leg)
        {
            const float reach = params.leg_reach_metres[leg];
            const float foot = params.foot_limit_metres;
            float cap = 0.0f;
            if (std::isfinite(reach) && std::isfinite(foot) && reach > 0.05f && foot > 0.0f)
                cap = std::asin(clampf(foot / reach, 0.0f, 1.0f));
            if (max_rad[kPushLegA + leg] > cap)
                max_rad[kPushLegA + leg] = cap;
        }
        const float strength = std::isfinite(params.strength) ? clampf(params.strength, 0.0f, 10.0f) : 0.0f;
        const float hz = std::isfinite(params.hz) ? clampf(params.hz, 0.2f, 20.0f) : 2.5f;
        const float zeta = std::isfinite(params.damping) ? clampf(params.damping, 0.05f, 3.0f) : 0.9f;
        const float omega = kTwoPi * hz;
        const bool stepping = dt > 0.0f && std::isfinite(dt) && !(dt > 1.0e6f);
        if (dt > 0.1f)
            dt = 0.1f;

        // What the contacts ask: a target lean and a kick per segment.
        float target[kPushSegments][2] = {};
        float kick[kPushSegments][2] = {};
        // What an arm passes to the chest: its drive with the lever's sign
        // dropped (the arm end follows the push, the chest goes with it).
        float arm_target[2][2] = {};
        float arm_kick[2][2] = {};
        const bool geometry_ok = std::isfinite(metre) && metre > 0.0f && pivot;
        for (int c = 0; stepping && geometry_ok && contacts && c < n; ++c)
        {
            const BodyPushContact& ct = contacts[c];
            const int seg = ct.segment;
            if (seg < 0 || seg >= kPushSegments || !finite3(ct.point) ||
                !finite3(ct.normal) || !finite3(pivot[seg]) || !std::isfinite(ct.depth) ||
                !std::isfinite(ct.weight) || ct.weight <= 0.0f)
                continue;
            float nrm[3] = { ct.normal[0], ct.normal[1], ct.normal[2] };
            const float nl = std::sqrt(dot(nrm, nrm));
            if (!(nl > 1.0e-6f))
                continue;
            for (float& v : nrm)
                v /= nl;
            // She is pushed along -normal; only its horizontal part leans her.
            const float vert = dot(nrm, up);
            float push[3];
            for (int k = 0; k < 3; ++k)
                push[k] = -(nrm[k] - vert * up[k]);
            const float a = dot(push, s.e1), b = dot(push, s.e2);
            // A push at the pivot does nothing; above it the top goes with
            // the push, below it the other way (a lever about the pivot).
            const float rel[3] = { ct.point[0] - pivot[seg][0], ct.point[1] - pivot[seg][1],
                                   ct.point[2] - pivot[seg][2] };
            const float height = dot(rel, up) / metre;
            float lever;
            if (seg == kPushHips)
                lever = 1.0f;       // a push at the pelvis tips the body above it
            else if (seg >= kPushArmA)
            {
                // A limb: the further out, the more it swings; hanging below
                // its pivot its end follows the push (negative lever).
                const float dist = std::sqrt(dot(rel, rel)) / metre;
                const bool arm = seg <= kPushArmB;
                const float ref = arm ? params.arm_lever_metres : params.leg_lever_metres;
                lever = clampf(dist / ref, 0.0f, 1.0f) * clampf(height / 0.05f, -1.0f, 1.0f);
            }
            else
                lever = clampf(height / params.lever_metres, -0.5f, 1.0f);
            if (!std::isfinite(lever))
                continue;
            const float gain = strength * ct.weight * lever;
            const int arm_index = seg - kPushArmA;
            const bool is_arm = arm_index == 0 || arm_index == 1;
            const float abs_gain = std::fabs(gain);

            if (ct.depth > 0.0f)
            {
                const float press = clampf(ct.depth / (params.press_metres * metre), 0.0f, 1.0f);
                const float mag = gain * max_rad[seg] * press;
                target[seg][0] += mag * a;
                target[seg][1] += mag * b;
                if (is_arm)
                {
                    arm_target[arm_index][0] += abs_gain * max_rad[seg] * press * a;
                    arm_target[arm_index][1] += abs_gain * max_rad[seg] * press * b;
                }
            }
            if (ct.arriving && ct.velocity_valid && finite3(ct.velocity))
            {
                const float speed = std::sqrt(dot(ct.velocity, ct.velocity)) / metre;
                if (speed > 0.0f && std::isfinite(speed))
                {
                    const float scale = speed > kSpeedCap ? kSpeedCap / speed : 1.0f;
                    const float into = -dot(ct.velocity, nrm) / metre * scale;
                    if (into > kSpeedMin)
                    {
                        const float mag = gain * kKickPeak * max_rad[seg] * omega * kE *
                                          (into / kSpeedCap);
                        kick[seg][0] += mag * a;
                        kick[seg][1] += mag * b;
                        if (is_arm)
                        {
                            const float kmag = abs_gain * kKickPeak * max_rad[seg] * omega * kE *
                                               (into / kSpeedCap);
                            arm_kick[arm_index][0] += kmag * a;
                            arm_kick[arm_index][1] += kmag * b;
                        }
                    }
                }
            }
        }

        // The head's push moves the chest a little, the chest's the waist;
        // the arms' (x arm_chain) join the chest before it passes on; the
        // waist's total moves the hips.
        auto share_of = [](float v) { return std::isfinite(v) ? clampf(v, 0.0f, 1.0f) : 0.0f; };
        for (int low = 1; low < 3; ++low)
        {
            const float share = share_of(params.chain[low - 1]);
            for (int k = 0; k < 2; ++k)
            {
                target[low][k] += share * target[low - 1][k];
                kick[low][k] += share * kick[low - 1][k];
                if (low == 1)
                    for (int arm = 0; arm < 2; ++arm)
                    {
                        target[low][k] += share_of(params.arm_chain) * arm_target[arm][k];
                        kick[low][k] += share_of(params.arm_chain) * arm_kick[arm][k];
                    }
            }
        }
        for (int k = 0; k < 2; ++k)
        {
            target[kPushHips][k] += share_of(params.hips_chain) * target[kPushWaist][k];
            kick[kPushHips][k] += share_of(params.hips_chain) * kick[kPushWaist][k];
        }
        // A sustained press asks for at most the segment's maximum.
        for (int i = 0; i < kPushSegments; ++i)
        {
            const float m = std::sqrt(target[i][0] * target[i][0] + target[i][1] * target[i][1]);
            if (m > max_rad[i] && m > 0.0f)
            {
                target[i][0] *= max_rad[i] / m;
                target[i][1] *= max_rad[i] / m;
            }
        }

        if (stepping)
        {
            for (int i = 0; i < kPushSegments; ++i)
                for (int k = 0; k < 2; ++k)
                    s.vel[i][k] += kick[i][k];
            const int steps = (int)std::ceil(dt / (1.0f / 240.0f));
            const float h = dt / (float)(steps > 0 ? steps : 1);
            for (int step = 0; step < steps && step < 64; ++step)
                for (int i = 0; i < kPushSegments; ++i)
                    for (int k = 0; k < 2; ++k)
                    {
                        const float acc = omega * omega * (target[i][k] - s.angle[i][k]) -
                                          2.0f * zeta * omega * s.vel[i][k];
                        s.vel[i][k] += acc * h;
                        s.angle[i][k] += s.vel[i][k] * h;
                    }
            for (int i = 0; i < kPushSegments; ++i)
            {
                bool bad = false;
                for (int k = 0; k < 2; ++k)
                    bad = bad || !std::isfinite(s.angle[i][k]) || !std::isfinite(s.vel[i][k]);
                const bool free_now = target[i][0] == 0.0f && target[i][1] == 0.0f;
                const bool at_rest = std::fabs(s.angle[i][0]) < kRestAngle &&
                                     std::fabs(s.angle[i][1]) < kRestAngle &&
                                     std::fabs(s.vel[i][0]) < kRestSpeed &&
                                     std::fabs(s.vel[i][1]) < kRestSpeed;
                if (bad || (free_now && at_rest))
                    for (int k = 0; k < 2; ++k)
                        s.angle[i][k] = s.vel[i][k] = 0.0f;
            }
        }
        for (int i = 0; i < kPushSegments; ++i)
            soft_limit(s.angle[i], max_rad[i], s.shown[i]);
    }

    Mat4 body_push_rotation(const BodyPushState& s, int segment, float fraction)
    {
        if (!s.valid || segment < 0 || segment >= kPushSegments || !std::isfinite(fraction))
            return Mat4::identity();
        const float a = s.shown[segment][0] * fraction, b = s.shown[segment][1] * fraction;
        if (!std::isfinite(a) || !std::isfinite(b))
            return Mat4::identity();
        // The top leans toward a*e1 + b*e2: rotate about up x that
        // = a*e2 - b*e1.
        float rv[3];
        for (int k = 0; k < 3; ++k)
            rv[k] = a * s.e2[k] - b * s.e1[k];
        return rotation_from_vector(rv);
    }

    bool body_push_active(const BodyPushState& s)
    {
        for (int i = 0; i < kPushSegments; ++i)
            for (int k = 0; k < 2; ++k)
                if (std::fabs(s.angle[i][k]) > kRestAngle || std::fabs(s.vel[i][k]) > kRestSpeed)
                    return true;
        return false;
    }

    int body_push_segment_for_volume(const char* name, int soft, int rear_soft)
    {
        if (soft >= 0)
            return soft == rear_soft ? kPushNone : kPushChest;
        if (!name)
            return kPushNone;
        auto is = [&](const char* w) { return std::strcmp(name, w) == 0; };
        auto starts = [&](const char* w) { return std::strncmp(name, w, std::strlen(w)) == 0; };
        if (is("head") || is("neck"))
            return kPushHead;
        if (is("chest") || is("upper chest") || starts("clavicle"))
            return kPushChest;
        if (is("abdomen"))
            return kPushWaist;
        if (is("pelvis"))
            return kPushHips;
        // Limbs: the chain is the name's last letter ("forearm A").
        const size_t len = std::strlen(name);
        if (len < 2 || name[len - 2] != ' ' || (name[len - 1] != 'A' && name[len - 1] != 'B'))
            return kPushNone;
        const int chain = name[len - 1] == 'B' ? 1 : 0;
        if (starts("upper arm") || starts("forearm") || starts("hand"))
            return kPushArmA + chain;
        if (starts("thigh") || starts("shin"))
            return kPushLegA + chain;
        return kPushNone;
    }
}
