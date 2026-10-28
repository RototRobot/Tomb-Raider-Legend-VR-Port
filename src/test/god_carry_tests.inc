// Included by mathtest.cpp: src/vr/god_carry.*
#include "../vr/godmode/god_carry.h"

static void test_god_carry()
{
    using namespace trlvr;
    // rotation_about_point: the pivot stays, a point at +x turns about z.
    {
        const float from[3] = { 1, 0, 0 }, to[3] = { 0, 1, 0 };
        const float pivot[3] = { 5, 5, 0 };
        const Mat4 r = rotation_about_point(rotation_between(from, to), pivot);
        const float p[3] = { 5, 5, 0 };
        float q[3];
        for (int k = 0; k < 3; ++k)
            q[k] = p[0] * r.m[0][k] + p[1] * r.m[1][k] + p[2] * r.m[2][k] + r.m[3][k];
        CHECK(near(q[0], 5) && near(q[1], 5) && near(q[2], 0));
        const float a[3] = { 6, 5, 0 };            // 1 to the +x of the pivot -> 1 to +y
        for (int k = 0; k < 3; ++k)
            q[k] = a[0] * r.m[0][k] + a[1] * r.m[1][k] + a[2] * r.m[2][k] + r.m[3][k];
        CHECK(near(q[0], 5, 1e-3f) && near(q[1], 6, 1e-3f));
    }
    // bones_transform_range: only first..last (clipped) move.
    {
        Mat4 bones[6];
        for (Mat4& b : bones) b = Mat4::identity();
        bones_transform_range(bones, 6, 2, 99, translation(1, 2, 3));
        CHECK(bones[1].m[3][0] == 0.0f);
        CHECK(bones[2].m[3][0] == 1.0f && bones[5].m[3][2] == 3.0f);
        bones_transform_range(bones, 6, -4, 0, translation(7, 0, 0));
        CHECK(bones[0].m[3][0] == 7.0f && bones[1].m[3][0] == 0.0f);
    }
    // hang_arm_target: wrist straight above the shoulder, palm toward the head.
    {
        const float shoulder[3] = { 10, 20, 100 }, up[3] = { 0, 0, 1 };
        const float toward[3] = { 3, 0, 5 };       // head to +x (and above: ignored)
        Mat4 t;
        CHECK(hang_arm_target(shoulder, up, 60.0f, toward, true, true, &t));
        CHECK(near(t.m[3][0], 10) && near(t.m[3][1], 20) && near(t.m[3][2], 160));
        CHECK(near(t.m[0][2], 1.0f));              // chain A: fingers up
        // Left hand: palm direction = -z row = toward the head (+x).
        CHECK(near(-t.m[2][0], 1.0f) && near(t.m[2][2], 0.0f));
        // Rows orthonormal.
        for (int a = 0; a < 3; ++a)
            for (int b = 0; b < 3; ++b)
            {
                float d = 0;
                for (int k = 0; k < 3; ++k) d += t.m[a][k] * t.m[b][k];
                CHECK(near(d, a == b ? 1.0f : 0.0f, 1e-3f));
            }
        Mat4 u;   // right arm, chain B: palm still toward the head (z row = +palm*r, r away)
        CHECK(hang_arm_target(shoulder, up, 60.0f, toward, false, false, &u));
        CHECK(near(u.m[0][2], -1.0f));
        CHECK(near(-u.m[2][0], 1.0f));
        Mat4 bad;
        const float para[3] = { 0, 0, 2 };
        CHECK(!hang_arm_target(shoulder, up, 60.0f, para, true, true, &bad));
        CHECK(!hang_arm_target(shoulder, up, 0.0f, toward, true, true, &bad));
    }
    // world_to_head_frame inverts "a hand at q in the head frame".
    {
        // An OpenXR head pose (3x4, column-vector): yaw 0.4 about y, pitch
        // -0.2 about x, then the translation in metres.
        float head[3][4] = {};
        {
            const float cy = std::cos(0.4f), sy = std::sin(0.4f);
            const float cp = std::cos(-0.2f), sp = std::sin(-0.2f);
            const float Ry[3][3] = { { cy, 0, sy }, { 0, 1, 0 }, { -sy, 0, cy } };
            const float Rx[3][3] = { { 1, 0, 0 }, { 0, cp, -sp }, { 0, sp, cp } };
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    head[i][j] = Ry[i][0] * Rx[0][j] + Ry[i][1] * Rx[1][j] + Ry[i][2] * Rx[2][j];
            head[0][3] = 0.05f; head[1][3] = 0.3f; head[2][3] = -0.1f;
        }
        // The game's view-space transform of such a pose (z mirrored, the
        // rotation transposed for row vectors, translation in game units).
        auto game_from_xr_pose = [](const float p[3][4], float world_scale) {
            const float s[3] = { 1.0f, 1.0f, -1.0f };
            Mat4 m = Mat4::identity();
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c)
                    m.m[c][r] = s[r] * p[r][c] * s[c];
            for (int c = 0; c < 3; ++c)
                m.m[3][c] = s[c] * p[c][3] * world_scale;
            return m;
        };
        const float ws = 291.0f * 12.0f;
        const Mat4 hv = game_from_xr_pose(head, ws);
        // Some camera: yawed, level (rows right, down, forward, position).
        Mat4 cam = Mat4::identity();
        const float fx = std::cos(0.7f), fy = std::sin(0.7f);
        cam.m[0][0] = fy;  cam.m[0][1] = -fx;  cam.m[0][2] = 0;
        cam.m[1][0] = 0;   cam.m[1][1] = 0;    cam.m[1][2] = -1;
        cam.m[2][0] = fx;  cam.m[2][1] = fy;   cam.m[2][2] = 0;
        cam.m[3][0] = 1000; cam.m[3][1] = -500; cam.m[3][2] = 300;
        const float q[3] = { 0.12f, -0.35f, -0.6f };
        // The hand's pose: the head's, moved by q in the head's own axes.
        float hand[3][4];
        for (int r = 0; r < 3; ++r)
        {
            for (int c = 0; c < 3; ++c) hand[r][c] = head[r][c];
            hand[r][3] = head[r][3] + head[r][0] * q[0] + head[r][1] * q[1] + head[r][2] * q[2];
        }
        const Mat4 gv = game_from_xr_pose(hand, ws);
        float v[3] = { gv.m[3][0], -gv.m[3][1], gv.m[3][2] };
        float w[3];
        for (int c = 0; c < 3; ++c)
            w[c] = v[0] * cam.m[0][c] + v[1] * cam.m[1][c] + v[2] * cam.m[2][c] + cam.m[3][c];
        float out[3];
        CHECK(world_to_head_frame(cam, hv, ws, w, out));
        CHECK(near(out[0], q[0], 1e-3f) && near(out[1], q[1], 1e-3f) && near(out[2], q[2], 1e-3f));
        Mat4 singular = Mat4::identity();
        for (int k = 0; k < 3; ++k) singular.m[k][k] = 0.0f;
        CHECK(!world_to_head_frame(singular, hv, ws, w, out));
    }
}
