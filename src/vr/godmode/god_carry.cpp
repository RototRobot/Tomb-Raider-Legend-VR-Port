#include "god_carry.h"

#include <math.h>
#include <cmath>

namespace trlvr
{
    Mat4 rotation_about_point(const Mat4& rotation, const float p[3])
    {
        return translation(-p[0], -p[1], -p[2]) * rotation * translation(p[0], p[1], p[2]);
    }

    void bones_transform_range(Mat4* bones, int bone_count, int first, int last,
                               const Mat4& m)
    {
        if (!bones)
            return;
        if (first < 0) first = 0;
        if (last >= bone_count) last = bone_count - 1;
        for (int i = first; i <= last; ++i)
            bones[i] = bones[i] * m;
    }

    bool hang_arm_target(const float shoulder[3], const float up[3], float reach,
                         const float toward[3], bool left_arm, bool chain_a,
                         Mat4* target)
    {
        if (!target || !(reach > 0.0f))
            return false;
        // Horizontal (perpendicular to up) direction toward her head.
        const float d = toward[0] * up[0] + toward[1] * up[1] + toward[2] * up[2];
        float p[3] = { toward[0] - up[0] * d, toward[1] - up[1] * d, toward[2] - up[2] * d };
        const float n = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
        if (!(n > 1e-6f) || !std::isfinite(n))
            return false;
        for (float& c : p) c /= n;
        const float s = left_arm ? 1.0f : -1.0f;        // hand's right axis
        const float r[3] = { p[0] * s, p[1] * s, p[2] * s };
        // down = forward(=up) x right, as the camera's rows (right, down,
        // forward) are related.
        const float dn[3] = { up[1] * r[2] - up[2] * r[1],
                              up[2] * r[0] - up[0] * r[2],
                              up[0] * r[1] - up[1] * r[0] };
        const float fx = chain_a ? 1.0f : -1.0f;
        const float palm = left_arm ? -1.0f : 1.0f;
        Mat4 m = Mat4::identity();
        for (int k = 0; k < 3; ++k)
        {
            m.m[0][k] = fx * up[k];
            m.m[1][k] = dn[k];
            m.m[2][k] = palm * r[k];
            m.m[3][k] = shoulder[k] + up[k] * reach;
        }
        *target = m;
        return true;
    }

    bool world_to_head_frame(const Mat4& camera, const Mat4& head_view,
                             float units_per_metre, const float world[3],
                             float out[3])
    {
        Mat4 inverse;
        if (!invert(camera, &inverse) || !(units_per_metre > 0.0f))
            return false;
        // world = view * F * camera  ->  view = world * camera^-1 * F.
        float v[3];
        for (int c = 0; c < 3; ++c)
            v[c] = world[0] * inverse.m[0][c] + world[1] * inverse.m[1][c] +
                   world[2] * inverse.m[2][c] + inverse.m[3][c];
        v[1] = -v[1];
        // view = head_view.t + units * (q_game * head_rotation).
        float q[3];
        for (int r = 0; r < 3; ++r)
        {
            float sum = 0.0f;
            for (int c = 0; c < 3; ++c)
                sum += (v[c] - head_view.m[3][c]) * head_view.m[r][c];   // x R^T
            q[r] = sum / units_per_metre;
        }
        out[0] = q[0];
        out[1] = q[1];
        out[2] = -q[2];
        return true;
    }
}
