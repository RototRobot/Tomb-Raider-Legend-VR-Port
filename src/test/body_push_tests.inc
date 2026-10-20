// Included by mathtest.cpp: src/vr/body_push.* (general push response), the
// rotation-vector helpers in vr_math.
#include "../vr/physics/body_push.h"

namespace
{
    // A point through a row-vector matrix.
    void bp_apply(const Mat4& m, const float p[3], float out[3])
    {
        for (int k = 0; k < 3; ++k)
            out[k] = p[0] * m.m[0][k] + p[1] * m.m[1][k] + p[2] * m.m[2][k] + m.m[3][k];
    }

    // A point turned about a pivot by a segment's rotation.
    void bp_turn(const Mat4& rot, const float pivot[3], const float p[3], float out[3])
    {
        const float rel[3] = { p[0] - pivot[0], p[1] - pivot[1], p[2] - pivot[2] };
        for (int k = 0; k < 3; ++k)
            out[k] = pivot[k] + rel[0] * rot.m[0][k] + rel[1] * rot.m[1][k] + rel[2] * rot.m[2][k];
    }

    const float kBpMetre = 280.0f;
    const float kBpUp[3] = { 0.0f, 0.0f, 1.0f };
    // Head 17, chest 15, waist 14 at 1.75 / 1.38 / 1.29 m; hips (bone 1) 1.0 m;
    // shoulders 53 / 78 at 1.45 m, hips 4 / 9 at 0.95 m (x = 0: the tests
    // measure along the vertical).
    const float kBpPivot[8][3] = { { 0, 0, 1.75f * 280 }, { 0, 0, 1.38f * 280 }, { 0, 0, 1.29f * 280 },
                                   { 0, 0, 1.0f * 280 }, { 0, 0, 1.45f * 280 }, { 0, 0, 1.45f * 280 },
                                   { 0, 0, 0.95f * 280 }, { 0, 0, 0.95f * 280 } };

    // A hand pressing the segment from the +x side (outward normal +x), at
    // `height_m` above the pivot, `depth_m` into the body.
    BodyPushContact bp_contact(int segment, float height_m, float depth_m)
    {
        BodyPushContact c;
        c.segment = segment;
        c.point[0] = 0.0f;
        c.point[1] = 0.0f;
        c.point[2] = kBpPivot[segment][2] + height_m * kBpMetre;
        c.normal[0] = 1.0f;
        c.depth = depth_m * kBpMetre;
        return c;
    }

    float bp_angle(const BodyPushState& s, int seg)
    {
        return std::sqrt(s.shown[seg][0] * s.shown[seg][0] + s.shown[seg][1] * s.shown[seg][1]);
    }

    bool bp_identity(const Mat4& m, float eps = 1e-5f)
    {
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                if (std::fabs(m.m[r][c] - (r == c ? 1.0f : 0.0f)) > eps)
                    return false;
        return true;
    }

    void bp_run(BodyPushState& s, const BodyPushParams& p, const BodyPushContact* c, int n,
                float seconds, float dt = 1.0f / 60)
    {
        for (float t = 0.0f; t < seconds; t += dt)
            body_push_step(s, p, c, n, kBpPivot, kBpUp, kBpMetre, dt);
    }
}


static void test_body_push()
{
    using namespace trlvr;
    const float rad = 0.017453293f;

    // rotation_from_vector / rotation_to_vector: right-hand rule like
    // rotation_between, and a round trip (also near a half turn).
    {
        const float rv[3] = { 0, 0, 1.5707963f };
        const Mat4 a = rotation_from_vector(rv);
        const float from[3] = { 1, 0, 0 }, to[3] = { 0, 1, 0 };
        const Mat4 b = rotation_between(from, to);
        bool same = true;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                same = same && std::fabs(a.m[r][c] - b.m[r][c]) < 1e-5f;
        CHECK(same);
        const float vectors[][3] = { { 0.1f, -0.2f, 0.3f }, { -1.0f, 0.5f, 0.2f },
                                     { 0.0f, 0.0f, 0.0f }, { 2.0f, 1.0f, -1.0f },
                                     { 0.0f, 3.0f, 0.0f } };
        for (const auto& v : vectors)
        {
            float back[3] = {};
            CHECK(rotation_to_vector(rotation_from_vector(v), back));
            for (int k = 0; k < 3; ++k)
                CHECK(near(back[k], v[k], 1e-3f) || std::fabs(back[k] - v[k]) < 1e-3f);
        }
        const float bad[3] = { NAN, 0, 0 };
        CHECK(bp_identity(rotation_from_vector(bad)));
        Mat4 nan_matrix = Mat4::identity();
        nan_matrix.m[1][1] = NAN;
        float out[3];
        CHECK(!rotation_to_vector(nan_matrix, out));
    }

    // No contact: identity, stays at rest, not active.
    {
        BodyPushState s;
        BodyPushParams p;
        bp_run(s, p, nullptr, 0, 2.0f);
        CHECK(!body_push_active(s));
        for (int i = 0; i < kPushSegments; ++i)
            CHECK(bp_identity(body_push_rotation(s, i)));
        CHECK(bp_identity(body_push_rotation(s, 8)));      // out of range
        CHECK(bp_identity(body_push_rotation(s, -1)));
    }

    // A sustained press on the chest from +x tilts her away (top toward -x),
    // within the maximum, and the waist follows a little.
    {
        BodyPushState s;
        BodyPushParams p;
        const BodyPushContact c = bp_contact(kPushChest, 0.13f, 0.05f);
        bp_run(s, p, &c, 1, 1.5f);
        CHECK(body_push_active(s));
        const float max = p.max_degrees[kPushChest] * rad;
        const float a = bp_angle(s, kPushChest);
        CHECK(a <= max + 1e-6f);
        CHECK(a > 0.6f * max);
        float top[3];
        const float point[3] = { 0, 0, kBpPivot[kPushChest][2] + 0.3f * kBpMetre };
        bp_turn(body_push_rotation(s, kPushChest), kBpPivot[kPushChest], point, top);
        CHECK(top[0] < -0.01f * kBpMetre * 0.3f * a);       // away from the hand
        CHECK(std::fabs(top[1]) < 1e-3f * kBpMetre);        // not sideways
        // The pivot itself stays put.
        float stay[3];
        bp_turn(body_push_rotation(s, kPushChest), kBpPivot[kPushChest], kBpPivot[kPushChest], stay);
        CHECK(near(stay[2], kBpPivot[kPushChest][2], 1e-5f));
        // Chain: waist follows by 0.4 of the chest, the head not at all.
        CHECK(bp_angle(s, kPushWaist) > 0.0f);
        CHECK(bp_angle(s, kPushWaist) < a);
        CHECK(near(s.angle[kPushWaist][0], 0.4f * s.angle[kPushChest][0], 0.02f));
        CHECK(bp_identity(body_push_rotation(s, kPushHead)));

        // Released: comes back to exactly rest without swinging past it.
        const float dir[2] = { s.angle[kPushChest][0] / a, s.angle[kPushChest][1] / a };
        float peak = 0.0f, worst = 0.0f;
        for (int i = 0; i < 180; ++i)
        {
            body_push_step(s, p, nullptr, 0, kBpPivot, kBpUp, kBpMetre, 1.0f / 60);
            for (int seg = 0; seg < 3; ++seg)
            {
                const float along = s.angle[seg][0] * dir[0] + s.angle[seg][1] * dir[1];
                if (seg == kPushChest)
                    peak = std::fmax(peak, std::fabs(along));
                worst = std::fmin(worst, along);
            }
        }
        CHECK(worst > -0.1f * a);
        CHECK(!body_push_active(s));
        CHECK(bp_identity(body_push_rotation(s, kPushChest), 1e-7f));
    }

    // A press from the other side (-x) tilts the other way; a push from +y
    // tilts toward -y.
    {
        BodyPushState s;
        BodyPushParams p;
        BodyPushContact c = bp_contact(kPushHead, 0.1f, 0.05f);
        c.normal[0] = -1.0f;
        bp_run(s, p, &c, 1, 1.0f);
        float top[3];
        const float point[3] = { 0, 0, kBpPivot[kPushHead][2] + 0.2f * kBpMetre };
        bp_turn(body_push_rotation(s, kPushHead), kBpPivot[kPushHead], point, top);
        CHECK(top[0] > 0.0f);
        BodyPushState s2;
        BodyPushContact c2 = bp_contact(kPushHead, 0.1f, 0.05f);
        c2.normal[0] = 0.0f;
        c2.normal[1] = 1.0f;
        bp_run(s2, p, &c2, 1, 1.0f);
        bp_turn(body_push_rotation(s2, kPushHead), kBpPivot[kPushHead], point, top);
        CHECK(top[1] < 0.0f && std::fabs(top[0]) < 1e-3f * kBpMetre);
        // A push straight down on the head (normal up) leans nothing.
        BodyPushState s3;
        BodyPushContact c3 = bp_contact(kPushHead, 0.1f, 0.05f);
        c3.normal[0] = 0.0f;
        c3.normal[2] = 1.0f;
        bp_run(s3, p, &c3, 1, 1.0f);
        CHECK(!body_push_active(s3));
    }

    // A push at the pivot height does nothing; below it the lean reverses.
    {
        BodyPushState s;
        BodyPushParams p;
        const BodyPushContact at = bp_contact(kPushWaist, 0.0f, 0.05f);
        bp_run(s, p, &at, 1, 1.0f);
        CHECK(!body_push_active(s));
        CHECK(bp_identity(body_push_rotation(s, kPushWaist), 1e-7f));
        const BodyPushContact below = bp_contact(kPushWaist, -0.06f, 0.05f);
        bp_run(s, p, &below, 1, 1.0f);
        float top[3];
        const float point[3] = { 0, 0, kBpPivot[kPushWaist][2] + 0.3f * kBpMetre };
        bp_turn(body_push_rotation(s, kPushWaist), kBpPivot[kPushWaist], point, top);
        CHECK(top[0] > 0.0f);
        // A deeper press tilts more, up to the cap.
        BodyPushState shallow, deep;
        const BodyPushContact c1 = bp_contact(kPushChest, 0.13f, 0.01f);
        const BodyPushContact c2 = bp_contact(kPushChest, 0.13f, 0.03f);
        bp_run(shallow, p, &c1, 1, 1.5f);
        bp_run(deep, p, &c2, 1, 1.5f);
        CHECK(bp_angle(deep, kPushChest) > 2.0f * bp_angle(shallow, kPushChest));
    }

    // The arrival kick: capped at 2.5 m/s, needs 0.2 m/s into the surface,
    // and never goes past the segment's maximum.
    {
        BodyPushParams p;
        auto peak_for = [&](float into_mps, bool arriving) {
            BodyPushState s;
            BodyPushContact c = bp_contact(kPushChest, 0.13f, 0.0f);
            c.velocity[0] = -into_mps * kBpMetre;       // into the surface (normal +x)
            c.arriving = arriving;
            body_push_step(s, p, &c, 1, kBpPivot, kBpUp, kBpMetre, 1.0f / 60);
            float peak = bp_angle(s, kPushChest);
            for (int i = 0; i < 120; ++i)
            {
                body_push_step(s, p, nullptr, 0, kBpPivot, kBpUp, kBpMetre, 1.0f / 60);
                peak = std::fmax(peak, bp_angle(s, kPushChest));
            }
            return peak;
        };
        const float max = p.max_degrees[kPushChest] * rad;
        const float at_cap = peak_for(2.5f, true);
        const float huge = peak_for(500.0f, true);
        CHECK(at_cap > 0.3f * max);
        CHECK(at_cap <= max);
        CHECK(huge <= max);
        CHECK(std::fabs(huge - at_cap) < 0.02f * max);
        const float mid = peak_for(1.0f, true);
        CHECK(mid > 0.0f && mid < at_cap);
        CHECK(peak_for(0.1f, true) == 0.0f);            // too slow
        CHECK(peak_for(-2.0f, true) == 0.0f);           // moving away
        CHECK(peak_for(2.5f, false) == 0.0f);           // not the arrival frame
    }

    // Chains: a head push moves the chest by 0.3 and the waist by 0.4 of that.
    {
        BodyPushState s;
        BodyPushParams p;
        p.max_degrees[kPushChest] = 15.0f;      // keep the caps out of the ratio
        p.max_degrees[kPushWaist] = 15.0f;
        const BodyPushContact c = bp_contact(kPushHead, 0.1f, 0.05f);
        bp_run(s, p, &c, 1, 2.0f);
        const float head = std::fabs(s.angle[kPushHead][0]);
        CHECK(head > 0.5f * p.max_degrees[kPushHead] * rad);
        CHECK(near(std::fabs(s.angle[kPushChest][0]), 0.3f * head, 0.02f));
        CHECK(near(std::fabs(s.angle[kPushWaist][0]), 0.12f * head, 0.02f));
        // Chest push: the head is untouched.
        BodyPushState s2;
        const BodyPushContact c2 = bp_contact(kPushChest, 0.13f, 0.05f);
        bp_run(s2, p, &c2, 1, 1.0f);
        CHECK(s2.angle[kPushHead][0] == 0.0f && s2.angle[kPushHead][1] == 0.0f);
        // Two hands add (and the total stays inside the cap).
        BodyPushState s3;
        const BodyPushContact two[2] = { bp_contact(kPushChest, 0.13f, 0.05f),
                                         bp_contact(kPushChest, 0.13f, 0.05f) };
        p.max_degrees[kPushChest] = 4.0f;
        bp_run(s3, p, two, 2, 1.5f);
        CHECK(bp_angle(s3, kPushChest) <= 4.0f * rad + 1e-6f);
        // A soft hit (weight 0.5) leans half as far.
        BodyPushState s4, s5;
        BodyPushContact light = bp_contact(kPushChest, 0.13f, 0.01f);
        BodyPushContact full = light;
        light.weight = 0.5f;
        bp_run(s4, p, &light, 1, 1.5f);
        bp_run(s5, p, &full, 1, 1.5f);
        CHECK(near(bp_angle(s4, kPushChest), 0.5f * bp_angle(s5, kPushChest), 0.02f));
    }

    // The settings: strength 0 and a zero maximum do nothing; strength 2 leans more.
    {
        BodyPushParams p;
        const BodyPushContact c = bp_contact(kPushChest, 0.13f, 0.01f);
        BodyPushState a, b, d;
        p.strength = 0.0f;
        bp_run(a, p, &c, 1, 1.0f);
        CHECK(!body_push_active(a));
        p.strength = 1.0f;
        bp_run(b, p, &c, 1, 1.5f);
        p.strength = 2.0f;
        bp_run(d, p, &c, 1, 1.5f);
        CHECK(bp_angle(d, kPushChest) > 1.5f * bp_angle(b, kPushChest));
        BodyPushState e;
        p.max_degrees[kPushChest] = 0.0f;
        bp_run(e, p, &c, 1, 1.0f);
        CHECK(bp_identity(body_push_rotation(e, kPushChest)));
    }

    // Frame-rate independence (roughly): 30 and 120 Hz settle to the same lean.
    {
        BodyPushParams p;
        const BodyPushContact c = bp_contact(kPushChest, 0.13f, 0.02f);
        BodyPushState a, b;
        bp_run(a, p, &c, 1, 2.0f, 1.0f / 30);
        bp_run(b, p, &c, 1, 2.0f, 1.0f / 120);
        CHECK(near(bp_angle(a, kPushChest), bp_angle(b, kPushChest), 0.01f));
    }

    // Garbage in: nothing non-finite comes out, and it recovers.
    {
        BodyPushState s;
        BodyPushParams p;
        BodyPushContact bad = bp_contact(kPushChest, 0.13f, 0.05f);
        bad.point[1] = NAN;
        BodyPushContact bad2 = bp_contact(kPushChest, 0.13f, 0.05f);
        bad2.normal[0] = INFINITY;
        BodyPushContact bad3 = bp_contact(kPushChest, 0.13f, 0.0f);     // arrival only
        bad3.velocity[2] = NAN;
        bad3.arriving = true;
        BodyPushContact bad4 = bp_contact(kPushChest, 0.13f, 0.05f);
        bad4.segment = 9;
        BodyPushContact bad5 = bp_contact(kPushChest, 0.13f, NAN);
        BodyPushContact zero = bp_contact(kPushChest, 0.13f, 0.05f);
        zero.normal[0] = 0.0f;
        const BodyPushContact all[6] = { bad, bad2, bad3, bad4, bad5, zero };
        bp_run(s, p, all, 6, 0.5f);
        CHECK(!body_push_active(s));
        float nan_pivot[8][3];
        std::memcpy(nan_pivot, kBpPivot, sizeof(nan_pivot));
        nan_pivot[1][2] = NAN;
        const BodyPushContact good = bp_contact(kPushChest, 0.13f, 0.05f);
        body_push_step(s, p, &good, 1, nan_pivot, kBpUp, kBpMetre, 1.0f / 60);
        body_push_step(s, p, &good, 1, kBpPivot, kBpUp, NAN, 1.0f / 60);
        body_push_step(s, p, &good, 1, kBpPivot, kBpUp, kBpMetre, NAN);
        body_push_step(s, p, &good, 1, kBpPivot, kBpUp, kBpMetre, -1.0f);
        body_push_step(s, p, &good, 1, kBpPivot, kBpUp, kBpMetre, 50.0f);   // one big step
        bool finite = true;
        for (int i = 0; i < kPushSegments; ++i)
        {
            const Mat4 r = body_push_rotation(s, i);
            for (int a = 0; a < 4; ++a)
                for (int b = 0; b < 4; ++b)
                    finite = finite && std::isfinite(r.m[a][b]);
        }
        CHECK(finite);
        // NaN in the state itself is dropped.
        s.angle[1][0] = NAN;
        body_push_step(s, p, nullptr, 0, kBpPivot, kBpUp, kBpMetre, 1.0f / 60);
        CHECK(!body_push_active(s));
        CHECK(bp_identity(body_push_rotation(s, kPushChest)));
        // A bad up resets; a good contact works afterwards.
        const float nan_up[3] = { NAN, 0, 1 };
        bp_run(s, p, &good, 1, 0.5f);
        CHECK(body_push_active(s));
        body_push_step(s, p, &good, 1, kBpPivot, nan_up, kBpMetre, 1.0f / 60);
        CHECK(!body_push_active(s));
        bp_run(s, p, &good, 1, 0.5f);
        CHECK(body_push_active(s));
        body_push_reset(s);
        CHECK(!body_push_active(s));
        // Null contacts with a count, null pivot.
        body_push_step(s, p, nullptr, 3, nullptr, kBpUp, kBpMetre, 1.0f / 60);
        CHECK(!body_push_active(s));
    }

    // A tilted "up" still leans away from the hand.
    {
        BodyPushState s;
        BodyPushParams p;
        const float up[3] = { 0.0f, 0.6f, 0.8f };
        BodyPushContact c = bp_contact(kPushChest, 0.13f, 0.05f);
        c.point[0] = 0.0f;
        c.point[1] = 0.13f * kBpMetre * up[1];
        c.point[2] = kBpPivot[kPushChest][2] + 0.13f * kBpMetre * up[2];
        for (float t = 0.0f; t < 1.5f; t += 1.0f / 60)
            body_push_step(s, p, &c, 1, kBpPivot, up, kBpMetre, 1.0f / 60);
        float top[3];
        const float point[3] = { 0, kBpPivot[kPushChest][1] + 0.3f * kBpMetre * up[1],
                                 kBpPivot[kPushChest][2] + 0.3f * kBpMetre * up[2] };
        bp_turn(body_push_rotation(s, kPushChest), kBpPivot[kPushChest], point, top);
        CHECK(top[0] < 0.0f);
        // The axis is horizontal to her up: the top stays on her up's plane.
        const float rel[3] = { top[0] - kBpPivot[kPushChest][0], top[1] - kBpPivot[kPushChest][1],
                               top[2] - kBpPivot[kPushChest][2] };
        const float len = std::sqrt(rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2]);
        CHECK(near(len, 0.3f * kBpMetre, 1e-3f));
    }

    // Which volume pushes which segment.
    {
        CHECK(body_push_segment_for_volume("head", -1, 2) == kPushHead);
        CHECK(body_push_segment_for_volume("neck", -1, 2) == kPushHead);
        CHECK(body_push_segment_for_volume("chest", -1, 2) == kPushChest);
        CHECK(body_push_segment_for_volume("upper chest", -1, 2) == kPushChest);
        CHECK(body_push_segment_for_volume("clavicle B", -1, 2) == kPushChest);
        CHECK(body_push_segment_for_volume("soft", 0, 2) == kPushChest);
        CHECK(body_push_segment_for_volume("soft", 1, 2) == kPushChest);
        CHECK(body_push_segment_for_volume("soft", 2, 2) == kPushNone);
        CHECK(body_push_segment_for_volume("abdomen", -1, 2) == kPushWaist);
        CHECK(body_push_segment_for_volume("pelvis", -1, 2) == kPushHips);
        CHECK(body_push_segment_for_volume("upper arm A", -1, 2) == kPushArmA);
        CHECK(body_push_segment_for_volume("forearm A", -1, 2) == kPushArmA);
        CHECK(body_push_segment_for_volume("hand A", -1, 2) == kPushArmA);
        CHECK(body_push_segment_for_volume("upper arm B", -1, 2) == kPushArmB);
        CHECK(body_push_segment_for_volume("forearm B", -1, 2) == kPushArmB);
        CHECK(body_push_segment_for_volume("hand B", -1, 2) == kPushArmB);
        CHECK(body_push_segment_for_volume("thigh A", -1, 2) == kPushLegA);
        CHECK(body_push_segment_for_volume("shin A", -1, 2) == kPushLegA);
        CHECK(body_push_segment_for_volume("thigh B", -1, 2) == kPushLegB);
        CHECK(body_push_segment_for_volume("shin B", -1, 2) == kPushLegB);
        CHECK(body_push_segment_for_volume("foot A", -1, 2) == kPushNone);
        CHECK(body_push_segment_for_volume("hand", -1, 2) == kPushNone);
        CHECK(body_push_segment_for_volume("hand C", -1, 2) == kPushNone);
        CHECK(body_push_segment_for_volume(nullptr, -1, 2) == kPushNone);
    }

    // Arms (AUDIT 22.6): the arm end follows the push, within 8 degrees; a
    // hand push swings more than an upper-arm push of the same depth; the
    // chest gets 0.2 of the drive; both chains work; it comes back.
    {
        BodyPushParams p;
        p.max_degrees[kPushChest] = 15.0f;      // keep the caps out of the ratio
        for (int arm = kPushArmA; arm <= kPushArmB; ++arm)
        {
            BodyPushState hand, upper;
            const BodyPushContact ch = bp_contact(arm, -0.6f, 0.05f);
            const BodyPushContact cu = bp_contact(arm, -0.15f, 0.05f);
            bp_run(hand, p, &ch, 1, 1.5f);
            bp_run(upper, p, &cu, 1, 1.5f);
            const float max = p.max_degrees[arm] * rad;
            CHECK(bp_angle(hand, arm) <= max + 1e-6f);
            CHECK(bp_angle(hand, arm) > 0.6f * max);
            CHECK(bp_angle(upper, arm) > 0.0f);
            CHECK(bp_angle(hand, arm) > 1.5f * bp_angle(upper, arm));
            // The hand (0.6 m below the shoulder) moves with the push (-x).
            float end[3];
            const float point[3] = { 0, 0, kBpPivot[arm][2] - 0.6f * kBpMetre };
            bp_turn(body_push_rotation(hand, arm), kBpPivot[arm], point, end);
            CHECK(end[0] < -0.01f * kBpMetre);
            CHECK(std::fabs(end[1]) < 1e-3f * kBpMetre);
            // The chest tilts away by 0.2 of the arm's drive, the other arm not.
            const float chest = std::fabs(hand.angle[kPushChest][0]);
            CHECK(chest > 0.0f);
            CHECK(near(chest, 0.2f * std::fabs(hand.angle[arm][0]), 0.03f));
            float top[3];
            const float up_point[3] = { 0, 0, kBpPivot[kPushChest][2] + 0.3f * kBpMetre };
            bp_turn(body_push_rotation(hand, kPushChest), kBpPivot[kPushChest], up_point, top);
            CHECK(top[0] < 0.0f);
            const int other = arm == kPushArmA ? kPushArmB : kPushArmA;
            CHECK(hand.angle[other][0] == 0.0f && hand.angle[other][1] == 0.0f);
            // The clavicle's share is a plain fraction of the arm's turn.
            const Mat4 full = body_push_rotation(hand, arm);
            const Mat4 part = body_push_rotation(hand, arm, 0.25f);
            float rf[3], rp[3];
            CHECK(rotation_to_vector(full, rf) && rotation_to_vector(part, rp));
            for (int k = 0; k < 3; ++k)
                CHECK(near(rp[k], 0.25f * rf[k], 1e-3f) || std::fabs(rp[k] - 0.25f * rf[k]) < 1e-5f);
            // Released: back to exactly rest.
            for (int i = 0; i < 240; ++i)
                body_push_step(hand, p, nullptr, 0, kBpPivot, kBpUp, kBpMetre, 1.0f / 60);
            CHECK(!body_push_active(hand));
            CHECK(bp_identity(body_push_rotation(hand, arm), 1e-7f));
        }
        // An arm's setting of 0 turns it off.
        BodyPushState off;
        p.max_degrees[kPushArmA] = 0.0f;
        const BodyPushContact c = bp_contact(kPushArmA, -0.6f, 0.05f);
        bp_run(off, p, &c, 1, 1.0f);
        CHECK(bp_identity(body_push_rotation(off, kPushArmA)));
    }

    // Hips: a pelvis push tips the upper body away within 3 degrees and it
    // returns; a waist push moves the hips by 0.3 of the waist's lean.
    {
        BodyPushParams p;
        BodyPushState s;
        const BodyPushContact c = bp_contact(kPushHips, 0.0f, 0.05f);
        bp_run(s, p, &c, 1, 1.5f);
        const float max = p.max_degrees[kPushHips] * rad;
        CHECK(bp_angle(s, kPushHips) <= max + 1e-6f);
        CHECK(bp_angle(s, kPushHips) > 0.6f * max);
        float top[3];
        const float point[3] = { 0, 0, kBpPivot[kPushHips][2] + 0.5f * kBpMetre };
        bp_turn(body_push_rotation(s, kPushHips), kBpPivot[kPushHips], point, top);
        CHECK(top[0] < 0.0f && std::fabs(top[1]) < 1e-3f * kBpMetre);
        CHECK(bp_identity(body_push_rotation(s, kPushWaist)));      // no chain upward
        for (int i = 0; i < 240; ++i)
            body_push_step(s, p, nullptr, 0, kBpPivot, kBpUp, kBpMetre, 1.0f / 60);
        CHECK(!body_push_active(s));

        BodyPushState w;
        p.max_degrees[kPushWaist] = 15.0f;
        p.max_degrees[kPushHips] = 15.0f;
        const BodyPushContact cw = bp_contact(kPushWaist, 0.06f, 0.05f);
        bp_run(w, p, &cw, 1, 2.0f);
        CHECK(std::fabs(w.angle[kPushWaist][0]) > 0.0f);
        CHECK(near(std::fabs(w.angle[kPushHips][0]), 0.3f * std::fabs(w.angle[kPushWaist][0]), 0.02f));
        // A chest push reaches the hips through the waist: 0.4 x 0.3.
        BodyPushState ch;
        const BodyPushContact cc = bp_contact(kPushChest, 0.13f, 0.05f);
        bp_run(ch, p, &cc, 1, 2.0f);
        CHECK(near(std::fabs(ch.angle[kPushHips][0]), 0.12f * std::fabs(ch.angle[kPushChest][0]), 0.02f));
    }

    // Legs: tiny, and the foot moves under 2 cm however hard it is pushed.
    {
        BodyPushParams p;
        p.strength = 3.0f;
        p.max_degrees[kPushLegA] = 8.0f;            // the foot limit must still hold
        p.max_degrees[kPushLegB] = 8.0f;
        for (int leg = kPushLegA; leg <= kPushLegB; ++leg)
        {
            BodyPushState s;
            BodyPushContact c = bp_contact(leg, -0.3f, 0.05f);
            c.velocity[0] = -2.5f * kBpMetre;
            c.arriving = true;
            float worst = 0.0f;
            for (float t = 0.0f; t < 2.0f; t += 1.0f / 60)
            {
                body_push_step(s, p, &c, 1, kBpPivot, kBpUp, kBpMetre, 1.0f / 60);
                c.arriving = false;
                float foot[3];
                const float toe[3] = { 0, 0, kBpPivot[leg][2] - 0.9f * kBpMetre };
                bp_turn(body_push_rotation(s, leg), kBpPivot[leg], toe, foot);
                const float moved = std::sqrt((foot[0] - toe[0]) * (foot[0] - toe[0]) +
                                              (foot[1] - toe[1]) * (foot[1] - toe[1]) +
                                              (foot[2] - toe[2]) * (foot[2] - toe[2]));
                worst = std::fmax(worst, moved);
            }
            CHECK(worst <= 0.0201f * kBpMetre);
            CHECK(worst > 0.005f * kBpMetre);           // it does move a little
            CHECK(bp_angle(s, leg) <= 2.3f * rad);
            // The foot goes with the push (-x), and it all comes back.
            float foot[3];
            const float toe[3] = { 0, 0, kBpPivot[leg][2] - 0.9f * kBpMetre };
            bp_turn(body_push_rotation(s, leg), kBpPivot[leg], toe, foot);
            CHECK(foot[0] < toe[0]);
            for (int i = 0; i < 240; ++i)
                body_push_step(s, p, nullptr, 0, kBpPivot, kBpUp, kBpMetre, 1.0f / 60);
            CHECK(!body_push_active(s));
        }
        // A short leg is cut harder; the other chain is untouched by a push.
        BodyPushState s;
        p.leg_reach_metres[0] = 0.45f;
        const BodyPushContact c = bp_contact(kPushLegA, -0.3f, 0.05f);
        bp_run(s, p, &c, 1, 1.5f);
        CHECK(bp_angle(s, kPushLegA) <= std::asin(0.02f / 0.45f) + 1e-6f);
        CHECK(s.angle[kPushLegB][0] == 0.0f && s.angle[kPushLegB][1] == 0.0f);
    }
}
