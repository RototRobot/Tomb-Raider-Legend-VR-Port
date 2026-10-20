// Tests for src/vr/body_volumes (included by mathtest.cpp).
#include "../vr/physics/body_volumes.h"

namespace
{
    // A standing Lara: identity rotations, z up, facing -y (the root's
    // -y row), bone heights from docs/AUDIT.md 17.2 (cm at 100 per metre).
    struct BodyRig
    {
        float bones[153 * 16];
        float metre;
        int soft_bones[2];
        int soft_tips[2];

        void place(int bone, float x_cm, float y_cm, float z_cm)
        {
            float* m = bones + bone * 16;
            for (int i = 0; i < 16; ++i)
                m[i] = (i % 5 == 0) ? 1.0f : 0.0f;
            m[12] = x_cm * metre / 100.0f;
            m[13] = y_cm * metre / 100.0f;
            m[14] = z_cm * metre / 100.0f;
        }

        explicit BodyRig(float metre_in = 280.0f) : metre(metre_in)
        {
            for (int b = 0; b < 153; ++b)
                place(b, 0, 0, 0);
            place(1, -8, 0, 126);
            place(2, 8, 0, 126);
            place(3, 0, 0, 119);
            place(14, 0, 0, 132);
            place(15, 0, 0, 138);
            place(16, 0, 0, 170);
            place(17, 0, 0, 175);
            place(18, 0, 0, 196);
            place(4, -9, 0, 111); place(5, -9, 0, 64); place(6, -9, 0, 11);
            place(7, -9, 0, 0);   place(8, -9, -10, 0);
            place(9, 9, 0, 111);  place(10, 9, 0, 64); place(11, 9, 0, 11);
            place(12, 9, 0, 0);   place(13, 9, -10, 0);
            place(52, -6, 0, 162); place(53, -27, 0, 161); place(54, -32, 0, 143);
            place(55, -36, 0, 126);
            for (int b = 56; b <= 71; ++b)
                place(b, -38, -4, 112);
            place(77, 6, 0, 162);  place(78, 27, 0, 161);  place(79, 32, 0, 143);
            place(80, 36, 0, 126);
            for (int b = 81; b <= 96; ++b)
                place(b, 38, -4, 112);
            // Soft parts: joint inside the chest, tip at the front.
            place(102, -8, -4, 150); place(103, -8, -12, 148);
            place(104, 8, -4, 150);  place(105, 8, -12, 148);
            soft_bones[0] = 102; soft_bones[1] = 104;
            soft_tips[0] = 103;  soft_tips[1] = 105;
        }

        void build(trlvr::BodyVolumes& v) const
        {
            trlvr::body_volumes_build(v, bones, 153, metre, soft_bones, 2, soft_tips);
        }

        // Turn about z by `angle` and move by (tx, ty, tz) units: row-vector
        // matrices, so every row (axes and position) is multiplied.
        void move(float angle, float tx, float ty, float tz)
        {
            const float c = std::cos(angle), s = std::sin(angle);
            for (int b = 0; b < 153; ++b)
            {
                float* m = bones + b * 16;
                for (int r = 0; r < 4; ++r)
                {
                    const float x = m[r * 4], y = m[r * 4 + 1];
                    m[r * 4] = x * c - y * s;
                    m[r * 4 + 1] = x * s + y * c;
                }
                m[12] += tx; m[13] += ty; m[14] += tz;
            }
        }

        void pt(float x_cm, float y_cm, float z_cm, float out[3]) const
        {
            out[0] = x_cm * metre / 100.0f;
            out[1] = y_cm * metre / 100.0f;
            out[2] = z_cm * metre / 100.0f;
        }
    };
}

static void test_body_volumes()
{
    using namespace trlvr;
    BodyRig rig;
    BodyVolumes vols;
    rig.build(vols);
    CHECK(vols.valid);
    CHECK(vols.count > 15 && vols.count <= kBodyVolumesMax);

    float p[3];
    BodyHit hit;

    // The chest's centre is inside the chest (firm).
    rig.pt(0, 0, 154, p);
    CHECK(body_volumes_test(vols, p, &hit));
    CHECK(hit.inside && hit.volume >= 0 && hit.soft == -1);
    CHECK(hit.depth > 0.0f);
    CHECK(std::strcmp(vols.volume[hit.volume].name, "chest") == 0);

    // A metre in front of it is outside; so is a point a metre above.
    rig.pt(0, -100, 154, p);
    CHECK(!body_volumes_test(vols, p, &hit));
    CHECK(!hit.inside && hit.depth < 0.0f);
    rig.pt(0, 0, 300, p);
    CHECK(!body_volumes_test(vols, p, nullptr));
    const float nan3[3] = { NAN, 0, 0 };
    CHECK(!body_volumes_test(vols, nan3, &hit));

    // In front of the breast, at breast height: the soft part (index 1 on
    // her +x side, index 0 on -x), with the normal out of her front (-y).
    for (int side = 0; side < 2; ++side)
    {
        rig.pt(side ? 8.0f : -8.0f, -14, 148, p);
        CHECK(body_volumes_test(vols, p, &hit));
        CHECK(hit.soft == side);
        CHECK(hit.normal[1] < -0.9f);
        // The surface point is on the way out along the normal.
        float out[3];
        for (int k = 0; k < 3; ++k)
            out[k] = p[k] + hit.normal[k] * hit.depth;
        CHECK(near(out[1], hit.surface[1], 1e-3f));
        // Pushed out by its depth plus a hair, it is no longer inside.
        BodyHit again;
        for (int k = 0; k < 3; ++k)
            out[k] = p[k] + hit.normal[k] * (hit.depth + 0.01f * rig.metre);
        CHECK(!body_volumes_test(vols, out, &again) || again.soft != side);
    }
    CHECK(body_volumes_nearest_soft(vols, p, 0.08f * rig.metre) == 1);
    rig.pt(0, -14, 100, p);
    CHECK(body_volumes_nearest_soft(vols, p, 0.08f * rig.metre) == -1);

    // The firm chest in front: its normal faces forward, deeper = more depth.
    float shallow[3], deep[3];
    rig.pt(0, -9, 160, shallow);        // 2 cm under the chest's front face
    rig.pt(0, -3, 160, deep);
    BodyHit h1, h2;
    CHECK(body_volumes_test(vols, shallow, &h1) && body_volumes_test(vols, deep, &h2));
    CHECK(h1.soft == -1 && h1.normal[1] < -0.9f && h2.depth > h1.depth);

    // Back of the chest points the other way.
    rig.pt(0, 8, 160, p);
    CHECK(body_volumes_test(vols, p, &hit) && hit.normal[1] > 0.9f);

    // Limbs and head are capsules: a point on the forearm, the head.
    rig.pt(-34, 0, 134, p);
    CHECK(body_volumes_test(vols, p, &hit) && hit.volume >= 0);
    CHECK(std::strstr(vols.volume[hit.volume].name, "arm") != nullptr);
    CHECK(hit.normal[0] != 0.0f || hit.normal[1] != 0.0f || hit.normal[2] != 0.0f);
    rig.pt(0, -5, 186, p);
    CHECK(body_volumes_test(vols, p, &hit) &&
          std::strcmp(vols.volume[hit.volume].name, "head") == 0);
    CHECK(hit.normal[1] < -0.3f);
    rig.pt(-9, 0, 40, p);
    CHECK(body_volumes_test(vols, p, &hit) &&
          std::strstr(vols.volume[hit.volume].name, "shin A") != nullptr);

    // Without the tip bones the soft volume sits in front of the joint.
    {
        BodyVolumes no_tips;
        body_volumes_build(no_tips, rig.bones, 153, rig.metre, rig.soft_bones, 2);
        rig.pt(-8, -12, 150, p);
        CHECK(no_tips.valid && body_volumes_test(no_tips, p, &hit) && hit.soft == 0);
    }

    // Scaling: the same skeleton at another metre gives the same hits.
    {
        BodyRig big(250.0f);
        BodyVolumes v2;
        big.build(v2);
        big.pt(0, 0, 154, p);
        CHECK(body_volumes_test(v2, p, &hit) && hit.soft == -1);
        big.pt(-8, -14, 148, p);
        CHECK(body_volumes_test(v2, p, &hit) && hit.soft == 0);
    }

    // The volumes move with the skeleton: turned a quarter circle about z
    // and moved, a hit in the new place and a miss in the old.
    {
        BodyRig moved;
        const float angle = 1.5707963f;
        moved.move(angle, 500.0f, -300.0f, 20.0f);
        BodyVolumes v2;
        moved.build(v2);
        CHECK(v2.valid);
        // Her old front (0, -y) is now along +x: forward = (cos, sin) of
        // (-y rotated by +90 degrees) = +x.
        CHECK(near(v2.forward[0], 1.0f, 1e-3f) && near(v2.forward[1], 0.0f, 1e-3f));
        float c[3] = { 0, 0, 154.0f * moved.metre / 100.0f };
        // centre of the chest: old (0,0,154) turned (no change) and moved.
        c[0] += 500.0f; c[1] += -300.0f; c[2] += 20.0f;
        CHECK(body_volumes_test(v2, c, &hit) && hit.soft == -1);
        CHECK(!body_volumes_test(vols, c, nullptr));
        // The left breast (-8, -14, 148) turned: (x,y) -> (x c - y s, x s + y c)
        // = (14, -8) cm, then moved.
        float q[3];
        moved.pt(14, -8, 148, q);
        q[0] += 500.0f; q[1] += -300.0f; q[2] += 20.0f;
        CHECK(body_volumes_test(v2, q, &hit) && hit.soft == 0);
        CHECK(hit.normal[0] > 0.9f);            // out of her front, now +x
    }

    // Bad input builds nothing and tests nothing.
    {
        BodyVolumes bad;
        body_volumes_build(bad, rig.bones, 10, rig.metre, nullptr, 0);
        CHECK(!bad.valid);
        body_volumes_build(bad, nullptr, 153, rig.metre, nullptr, 0);
        CHECK(!bad.valid);
        body_volumes_build(bad, rig.bones, 153, 0.0f, nullptr, 0);
        CHECK(!bad.valid);
        CHECK(!body_volumes_test(bad, p, &hit));
        BodyRig broken;
        broken.bones[16 * 3 + 12] = NAN;
        body_volumes_build(bad, broken.bones, 153, broken.metre, nullptr, 0);
        CHECK(!bad.valid);
    }
    // An added soft box (the buttock cheeks): hit by a point inside it,
    // refused on a bad set or bad axes.
    {
        BodyVolumes v2;
        rig.build(v2);
        const float* f = v2.forward;
        const float* u = v2.up;
        const float side[3] = { f[1] * u[2] - f[2] * u[1], f[2] * u[0] - f[0] * u[2], f[0] * u[1] - f[1] * u[0] };
        const float axes[3][3] = { { side[0], side[1], side[2] }, { f[0], f[1], f[2] }, { u[0], u[1], u[2] } };
        float c[3];
        rig.pt(0, 40, 60, c);               // well behind her, low: in nothing else
        CHECK(!body_volumes_test(v2, c, &hit));
        const float half[3] = { 0.05f * rig.metre, 0.05f * rig.metre, 0.05f * rig.metre };
        const int before = v2.count;
        CHECK(body_volumes_add_box(v2, "rear test", c, axes, half, 5));
        CHECK(v2.count == before + 1);
        CHECK(body_volumes_test(v2, c, &hit) && hit.soft == 5);
        const float bent[3][3] = { { 2, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
        CHECK(!body_volumes_add_box(v2, "bad", c, bent, half, 6));
        BodyVolumes none;
        CHECK(!body_volumes_add_box(none, "bad", c, axes, half, 6));
        // An ellipsoid (the cheeks' touch zones): inside along its thin axis
        // the depth is the way to its surface, outside the gap; the one-volume
        // test agrees and ignores the others.
        float e[3];
        rig.pt(0, 40, 20, e);
        const float semi[3] = { 10.0f, 5.0f, 20.0f };
        const int at = v2.count;
        CHECK(body_volumes_add_box(v2, "rear ellipsoid", e, axes, semi, 7, BodyVolumeKind::Ellipsoid));
        float q[3];
        for (int k = 0; k < 3; ++k)
            q[k] = e[k] + 4.0f * axes[1][k];
        CHECK(body_volumes_test_volume(v2, at, q, &hit) && hit.soft == 7);
        CHECK(near(hit.depth, 1.0f, 1e-3f) && near(hit.normal[0] * axes[1][0] + hit.normal[1] * axes[1][1] +
                                                   hit.normal[2] * axes[1][2], 1.0f, 1e-3f));
        CHECK(body_volumes_test(v2, q, &hit) && hit.soft == 7);
        for (int k = 0; k < 3; ++k)
            q[k] = e[k] + 6.0f * axes[1][k];
        CHECK(!body_volumes_test_volume(v2, at, q, &hit) && near(hit.depth, -1.0f, 1e-3f));
        for (int k = 0; k < 3; ++k)
            q[k] = e[k] + 4.0f * axes[0][k] + 3.0f * axes[1][k];   // 0.16 + 0.36 < 1: inside, not a box corner
        CHECK(body_volumes_test_volume(v2, at, q, &hit));
        for (int k = 0; k < 3; ++k)
            q[k] = e[k] + 8.0f * axes[0][k] + 4.0f * axes[1][k];   // inside the box, outside the ellipsoid
        CHECK(!body_volumes_test_volume(v2, at, q, &hit));
        CHECK(!body_volumes_test_volume(v2, -1, q, &hit) && !body_volumes_test_volume(v2, 99, q, &hit));
    }
}

// The Legend bone map: a standing skeleton with Legend's numbering (291 units
// a metre), 109 bones.
static void test_body_volumes_legend()
{
    using namespace trlvr;
    const float metre = 291.0f;
    float bones[109 * 16];
    auto place = [&](int bone, float x_cm, float y_cm, float z_cm) {
        float* m = bones + bone * 16;
        for (int i = 0; i < 16; ++i)
            m[i] = (i % 5 == 0) ? 1.0f : 0.0f;
        m[12] = x_cm * metre / 100.0f;
        m[13] = y_cm * metre / 100.0f;
        m[14] = z_cm * metre / 100.0f;
    };
    for (int b = 0; b < 109; ++b)
        place(b, 0, 0, 0);
    place(1, 0, 0, 105); place(2, 0, 0, 125); place(3, 0, 0, 145);
    place(4, 0, 0, 165); place(5, 0, 0, 185);
    place(100, 0, 0, 100);
    place(54, -8, -4, 148); place(55, -8, -12, 146);
    place(56, 8, -4, 148);  place(57, 8, -12, 146);
    place(58, -6, 0, 160); place(59, -26, 0, 158); place(60, -31, 0, 140); place(61, -35, 0, 122);
    place(79, 6, 0, 160);  place(80, 26, 0, 158);  place(81, 31, 0, 140);  place(82, 35, 0, 122);
    place(101, -9, 0, 95); place(102, -9, 0, 52); place(103, -9, 0, 8); place(104, -9, -10, 0);
    place(105, 9, 0, 95);  place(106, 9, 0, 52);  place(107, 9, 0, 8);  place(108, 9, -10, 0);

    BodyVolumes vols;
    body_volumes_build(vols, bones, 109, metre, body_bone_map_legend());
    CHECK(vols.valid && vols.count > 15);
    auto find = [&](const char* name) {
        for (int i = 0; i < vols.count; ++i)
            if (std::strcmp(vols.volume[i].name, name) == 0)
                return i;
        return -1;
    };
    CHECK(find("head") >= 0 && find("chest") >= 0 && find("abdomen") >= 0);
    CHECK(find("upper arm A") >= 0 && find("forearm B") >= 0 && find("hand A") >= 0);
    CHECK(find("thigh A") >= 0 && find("foot B") >= 0);
    // Chain A is her left arm and leg (negative x here), B the other.
    const BodyVolume& ua = vols.volume[find("upper arm A")];
    const BodyVolume& tb = vols.volume[find("thigh B")];
    CHECK(ua.a[0] < 0.0f && tb.a[0] > 0.0f);
    // The hand carries on along the forearm (no finger bones).
    const BodyVolume& hand = vols.volume[find("hand A")];
    CHECK(hand.b[2] < hand.a[2]);
    // Both breasts are soft parts, with the part's index in BodyHit::soft.
    int soft = 0;
    for (int i = 0; i < vols.count; ++i)
        soft += vols.volume[i].soft >= 0;
    CHECK(soft == 2);
    BodyHit hit;
    const float front[3] = { 8.0f * metre / 100.0f, -12.0f * metre / 100.0f, 146.0f * metre / 100.0f };
    CHECK(body_volumes_test(vols, front, &hit) && hit.soft == 1);
    // Bone 100 is needed, and a skeleton too short for the map is refused.
    BodyVolumes bad;
    body_volumes_build(bad, bones, 100, metre, body_bone_map_legend());
    CHECK(!bad.valid);
    // The Underworld map is unchanged by the Legend one.
    CHECK(body_bone_map_underworld().head == 18 && body_bone_map_legend().head == 5);
}
