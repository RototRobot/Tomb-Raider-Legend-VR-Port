#include "vr_math.h"

#include <cmath>
#include <cstring>

namespace trlvr
{
    namespace
    {
        bool nearly(float a, float b, float eps = 1e-4f)
        {
            return fabsf(a - b) <= eps;
        }
    }

    Mat4 Mat4::identity()
    {
        Mat4 r{};
        r.m[0][0] = r.m[1][1] = r.m[2][2] = r.m[3][3] = 1.0f;
        return r;
    }

    Mat4 operator*(const Mat4& a, const Mat4& b)
    {
        Mat4 r{};
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
            {
                float s = 0.0f;
                for (int k = 0; k < 4; ++k)
                    s += a.m[i][k] * b.m[k][j];
                r.m[i][j] = s;
            }
        return r;
    }

    Mat4 remove_yaw(const Mat4& view, float yaw)
    {
        // Take a heading out of a view matrix, leaving the pitch and roll.
        //
        // Which side the undo-rotation belongs on, and with which sign,
        // depends on the order the pose composed its axes in -- and a single
        // one-sided multiply cannot cancel a yaw that was applied on the
        // other side. Two candidates were tried first and neither worked
        // once the pose had pitch in it, which mathtest caught.
        //
        // So all four are built and the one that actually leaves no heading
        // behind is returned. That is a property of the result, checkable on
        // the spot, rather than a convention argued about in a comment.
        const Mat4 pos = yaw_rotation(yaw);
        const Mat4 neg = yaw_rotation(-yaw);
        const Mat4 candidates[4] = { neg * view, view * neg,
                                     pos * view, view * pos };

        int best = 0;
        float best_residual = fabsf(yaw_of(candidates[0]));
        for (int i = 1; i < 4; ++i)
        {
            const float r = fabsf(yaw_of(candidates[i]));
            if (r < best_residual)
            {
                best_residual = r;
                best = i;
            }
        }
        return candidates[best];
    }

    Mat4 set_yaw(const Mat4& view, float wanted)
    {
        // Give a view matrix a chosen heading, keeping its pitch and roll.
        //
        // The camera and the projection each carry part of the head rotation,
        // and how much depends on when the engine last rebuilt the camera --
        // measured at anywhere from 150 times a second down to zero while a
        // menu holds it. Whatever the camera has not applied has to come from
        // here, or the view simply stops following the head.
        //
        // Same self-verifying shape as remove_yaw: every composition is built
        // and the one that actually lands on the wanted heading is returned.
        const Mat4 flat = remove_yaw(view, yaw_of(view));

        const Mat4 pos = yaw_rotation(wanted);
        const Mat4 neg = yaw_rotation(-wanted);
        const Mat4 candidates[4] = { pos * flat, flat * pos,
                                     neg * flat, flat * neg };

        int best = 0;
        float best_error = fabsf(wrap_pi(yaw_of(candidates[0]) - wanted));
        for (int i = 1; i < 4; ++i)
        {
            const float e = fabsf(wrap_pi(yaw_of(candidates[i]) - wanted));
            if (e < best_error)
            {
                best_error = e;
                best = i;
            }
        }
        return candidates[best];
    }

    void rotate_yaw(float* xyz, float radians)
    {
        // The same rotation yaw_rotation builds, applied to a bare vector.
        // Kept beside it so the two cannot drift apart: a heading applied
        // one way to a matrix and another way to a position is the sort of
        // difference that shows up as walking sideways.
        const float c = cosf(radians), s = sinf(radians);
        const float x = xyz[0], z = xyz[2];
        xyz[0] = x * c + z * s;
        xyz[2] = -x * s + z * c;
    }

    Mat4 rigid_inverse(const Mat4& m)
    {
        // The inverse of a rotation plus a translation, without the general
        // case. For [R | 0; t | 1] the inverse is [R^T | 0; -t*R^T | 1] --
        // exact, and it cannot go singular the way a full inverse can on a
        // matrix that has been multiplied together a few times.
        Mat4 r = Mat4::identity();
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                r.m[i][j] = m.m[j][i];

        for (int j = 0; j < 3; ++j)
        {
            float acc = 0.0f;
            for (int i = 0; i < 3; ++i)
                acc += m.m[3][i] * r.m[i][j];
            r.m[3][j] = -acc;
        }
        return r;
    }

    float wrap_pi(float radians)
    {
        const float two_pi = 6.28318531f;
        while (radians >  3.14159265f) radians -= two_pi;
        while (radians < -3.14159265f) radians += two_pi;
        return radians;
    }

    float yaw_of(const Mat4& m)
    {
        // The heading of the direction this matrix looks along. For a view
        // matrix that direction is column 2: a view matrix is the inverse of
        // a pose, so the pose's basis vectors are its columns. Reading row 2
        // gives the negation for pure yaw and something simply wrong once
        // there is pitch.
        return atan2f(m.m[0][2], m.m[2][2]);
    }

    float steady_yaw_of(const Mat4& m)
    {
        const float forward_y = m.m[1][2];
        const float weight = forward_y * fabsf(forward_y);
        const float x = m.m[0][2] - weight * m.m[0][1];
        const float z = m.m[2][2] - weight * m.m[2][1];
        if (!(x * x + z * z > 1e-8f))
            return yaw_of(m);
        return atan2f(x, z);
    }

    Mat4 yaw_rotation(float radians)
    {
        Mat4 r = Mat4::identity();
        const float c = cosf(radians), s = sinf(radians);
        r.m[0][0] = c;  r.m[0][2] = -s;
        r.m[2][0] = s;  r.m[2][2] = c;
        return r;
    }

    Mat4 hand_alignment_rotation(float x_degrees, float y_degrees,
                                 float z_degrees)
    {
        const float to_radians = 0.017453292519943295f;
        const float x = x_degrees * to_radians;
        const float z = z_degrees * to_radians;
        Mat4 pitch = Mat4::identity();
        const float cx = cosf(x), sx = sinf(x);
        pitch.m[1][1] = cx;  pitch.m[1][2] = sx;
        pitch.m[2][1] = -sx; pitch.m[2][2] = cx;
        Mat4 roll = Mat4::identity();
        const float cz = cosf(z), sz = sinf(z);
        roll.m[0][0] = cz;  roll.m[0][1] = sz;
        roll.m[1][0] = -sz; roll.m[1][1] = cz;
        return pitch * yaw_rotation(y_degrees * to_radians) * roll;
    }

    Mat4 hand_pose_with_head_offset(const Mat4& controller_to_head,
                                    float x, float y, float z)
    {
        // Row-vector order matters: controller * translation keeps the wrist
        // position fixed while the controller rotates in place. Translation
        // * controller made a tuned wrist orbit the tracked grip origin.
        return controller_to_head * translation(x, y, z);
    }

    Mat4 rotation_between(const float* from3, const float* to3)
    {
        Mat4 result = Mat4::identity();
        float a[3] = { from3[0], from3[1], from3[2] };
        float b[3] = { to3[0], to3[1], to3[2] };
        const float al = sqrtf(a[0]*a[0] + a[1]*a[1] + a[2]*a[2]);
        const float bl = sqrtf(b[0]*b[0] + b[1]*b[1] + b[2]*b[2]);
        if (al < 1.0e-4f || bl < 1.0e-4f)
            return result;
        for (int i = 0; i < 3; ++i) { a[i] /= al; b[i] /= bl; }
        const float c = fmaxf(-1.0f, fminf(1.0f,
            a[0]*b[0] + a[1]*b[1] + a[2]*b[2]));
        if (c > 0.99999f)
            return result;

        if (c < -0.99999f)
        {
            // Any axis perpendicular to a gives the same half turn. Choose
            // one deterministically to avoid a sudden arbitrary flip.
            float axis[3] = { 0.0f, 0.0f, 0.0f };
            const int smallest = fabsf(a[0]) < fabsf(a[1])
                ? (fabsf(a[0]) < fabsf(a[2]) ? 0 : 2)
                : (fabsf(a[1]) < fabsf(a[2]) ? 1 : 2);
            axis[smallest] = 1.0f;
            const float d = axis[0]*a[0] + axis[1]*a[1] + axis[2]*a[2];
            for (int i = 0; i < 3; ++i) axis[i] -= d * a[i];
            const float n = sqrtf(axis[0]*axis[0] + axis[1]*axis[1] +
                                  axis[2]*axis[2]);
            for (int i = 0; i < 3; ++i) axis[i] /= n;
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    result.m[i][j] = 2.0f*axis[i]*axis[j] - (i == j ? 1.0f : 0.0f);
            return result;
        }

        const float cross[3] = {
            a[1]*b[2] - a[2]*b[1],
            a[2]*b[0] - a[0]*b[2],
            a[0]*b[1] - a[1]*b[0]
        };
        // Row-vector Rodrigues: R = I + K + K^2/(1+c), where a*K = a x cross.
        const float k[3][3] = {
            { 0.0f, cross[2], -cross[1] },
            { -cross[2], 0.0f, cross[0] },
            { cross[1], -cross[0], 0.0f }
        };
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
            {
                float k2 = 0.0f;
                for (int n = 0; n < 3; ++n) k2 += k[i][n] * k[n][j];
                result.m[i][j] += k[i][j] + k2 / (1.0f + c);
            }
        return result;
    }

    bool solve_two_bone_arm(const float* shoulder3, const float* elbow3,
                            const float* wrist3, const float* target3,
                            float* solved_elbow3, float* solved_wrist3)
    {
        float upper[3], lower[3], direction[3], old_reach[3];
        for (int i = 0; i < 3; ++i)
        {
            upper[i] = elbow3[i] - shoulder3[i];
            lower[i] = wrist3[i] - elbow3[i];
            direction[i] = target3[i] - shoulder3[i];
            old_reach[i] = wrist3[i] - shoulder3[i];
        }
        const float l1 = sqrtf(upper[0]*upper[0] + upper[1]*upper[1] + upper[2]*upper[2]);
        const float l2 = sqrtf(lower[0]*lower[0] + lower[1]*lower[1] + lower[2]*lower[2]);
        float d = sqrtf(direction[0]*direction[0] + direction[1]*direction[1] +
                        direction[2]*direction[2]);
        if (!std::isfinite(l1) || !std::isfinite(l2) ||
            l1 < 5.0f || l2 < 5.0f || l1 > 1000.0f || l2 > 1000.0f ||
            !std::isfinite(d))
            return false;
        if (d < 1.0e-4f)
        {
            memcpy(direction, old_reach, sizeof(direction));
            d = sqrtf(direction[0]*direction[0] + direction[1]*direction[1] +
                      direction[2]*direction[2]);
            if (d < 1.0e-4f)
                return false;
        }
        for (int i = 0; i < 3; ++i) direction[i] /= d;
        const float min_reach = fabsf(l1 - l2) + 1.0f;
        const float max_reach = l1 + l2 - 1.0f;
        if (max_reach <= min_reach)
            return false;
        d = fmaxf(min_reach, fminf(max_reach, d));

        const float along = (l1*l1 - l2*l2 + d*d) / (2.0f*d);
        const float height = sqrtf(fmaxf(0.0f, l1*l1 - along*along));
        float bend[3];
        const float projection = upper[0]*direction[0] +
                                 upper[1]*direction[1] +
                                 upper[2]*direction[2];
        for (int i = 0; i < 3; ++i)
            bend[i] = upper[i] - projection*direction[i];
        float bend_length = sqrtf(bend[0]*bend[0] + bend[1]*bend[1] +
                                  bend[2]*bend[2]);
        if (bend_length < 1.0e-3f)
        {
            const float fallback[3] = { 0.0f, 1.0f, 0.0f };
            const float dot = direction[1];
            for (int i = 0; i < 3; ++i)
                bend[i] = fallback[i] - dot*direction[i];
            bend_length = sqrtf(bend[0]*bend[0] + bend[1]*bend[1] +
                                bend[2]*bend[2]);
            if (bend_length < 1.0e-3f)
            {
                bend[0] = 1.0f - direction[0]*direction[0];
                bend[1] = -direction[0]*direction[1];
                bend[2] = -direction[0]*direction[2];
                bend_length = sqrtf(bend[0]*bend[0] + bend[1]*bend[1] +
                                    bend[2]*bend[2]);
            }
        }
        if (bend_length < 1.0e-4f)
            return false;
        for (int i = 0; i < 3; ++i)
        {
            solved_wrist3[i] = shoulder3[i] + direction[i]*d;
            solved_elbow3[i] = shoulder3[i] + direction[i]*along +
                               bend[i]*(height/bend_length);
        }
        return true;
    }

    Mat4 rotation_fraction(const Mat4& rotation, float fraction)
    {
        if (fraction <= 0.0f)
            return Mat4::identity();

        const float trace = rotation.m[0][0] + rotation.m[1][1]
                          + rotation.m[2][2];
        float qw = sqrtf(fmaxf(0.0f, 1.0f + trace)) * 0.5f;
        float qx = 0.0f, qy = 0.0f, qz = 0.0f;

        // The trace path is the most stable for the small and medium head
        // angles this sees. Choose a dominant diagonal near 180 degrees so a
        // recentered headset can never turn a numerical corner into a jump.
        if (qw > 1.0e-5f)
        {
            const float inv = 0.25f / qw;
            qx = (rotation.m[2][1] - rotation.m[1][2]) * inv;
            qy = (rotation.m[0][2] - rotation.m[2][0]) * inv;
            qz = (rotation.m[1][0] - rotation.m[0][1]) * inv;
        }
        else if (rotation.m[0][0] >= rotation.m[1][1] &&
                 rotation.m[0][0] >= rotation.m[2][2])
        {
            qx = sqrtf(fmaxf(0.0f, 1.0f + rotation.m[0][0]
                           - rotation.m[1][1] - rotation.m[2][2])) * 0.5f;
            const float inv = qx > 1.0e-5f ? 0.25f / qx : 0.0f;
            qy = (rotation.m[0][1] + rotation.m[1][0]) * inv;
            qz = (rotation.m[0][2] + rotation.m[2][0]) * inv;
            qw = (rotation.m[2][1] - rotation.m[1][2]) * inv;
        }
        else if (rotation.m[1][1] >= rotation.m[2][2])
        {
            qy = sqrtf(fmaxf(0.0f, 1.0f - rotation.m[0][0]
                           + rotation.m[1][1] - rotation.m[2][2])) * 0.5f;
            const float inv = qy > 1.0e-5f ? 0.25f / qy : 0.0f;
            qx = (rotation.m[0][1] + rotation.m[1][0]) * inv;
            qz = (rotation.m[1][2] + rotation.m[2][1]) * inv;
            qw = (rotation.m[0][2] - rotation.m[2][0]) * inv;
        }
        else
        {
            qz = sqrtf(fmaxf(0.0f, 1.0f - rotation.m[0][0]
                           - rotation.m[1][1] + rotation.m[2][2])) * 0.5f;
            const float inv = qz > 1.0e-5f ? 0.25f / qz : 0.0f;
            qx = (rotation.m[0][2] + rotation.m[2][0]) * inv;
            qy = (rotation.m[1][2] + rotation.m[2][1]) * inv;
            qw = (rotation.m[1][0] - rotation.m[0][1]) * inv;
        }

        float qlen = sqrtf(qx*qx + qy*qy + qz*qz + qw*qw);
        if (qlen <= 1.0e-6f)
            return Mat4::identity();
        qx /= qlen; qy /= qlen; qz /= qlen; qw /= qlen;

        // q and -q are the same orientation. Keep w non-negative so the
        // interpolation always takes the short path from identity.
        if (qw < 0.0f)
        {
            qx = -qx; qy = -qy; qz = -qz; qw = -qw;
        }

        if (fraction < 1.0f)
        {
            const float theta = acosf(fmaxf(-1.0f, fminf(1.0f, qw)));
            const float sin_theta = sinf(theta);
            if (fabsf(sin_theta) > 1.0e-6f)
            {
                const float xyz_scale = sinf(theta * fraction) / sin_theta;
                qx *= xyz_scale;
                qy *= xyz_scale;
                qz *= xyz_scale;
                qw = cosf(theta * fraction);
            }
            else
            {
                qx *= fraction;
                qy *= fraction;
                qz *= fraction;
                qw = 1.0f;
            }
        }

        const float xx = qx*qx, yy = qy*qy, zz = qz*qz;
        const float xy = qx*qy, xz = qx*qz, yz = qy*qz;
        const float xw = qx*qw, yw = qy*qw, zw = qz*qw;
        Mat4 r = Mat4::identity();
        r.m[0][0] = 1.0f - 2.0f * (yy + zz);
        r.m[0][1] = 2.0f * (xy - zw);
        r.m[0][2] = 2.0f * (xz + yw);
        r.m[1][0] = 2.0f * (xy + zw);
        r.m[1][1] = 1.0f - 2.0f * (xx + zz);
        r.m[1][2] = 2.0f * (yz - xw);
        r.m[2][0] = 2.0f * (xz - yw);
        r.m[2][1] = 2.0f * (yz + xw);
        r.m[2][2] = 1.0f - 2.0f * (xx + yy);
        return r;
    }

    Mat4 translation(float x, float y, float z)
    {
        Mat4 r = Mat4::identity();
        r.m[3][0] = x;      // row-vector: translation lives in the last row
        r.m[3][1] = y;
        r.m[3][2] = z;
        return r;
    }

    GameProjection projection_from_registers(const float* regs)
    {
        GameProjection gp{};
        gp.valid = false;
        if (!regs)
            return gp;

        // regs[i*4 + k] == M[k][i]
        const float m00 = regs[0 * 4 + 0];      // sx
        const float m20 = regs[0 * 4 + 2];      // ox
        const float m11 = regs[1 * 4 + 1];      // sy
        const float m21 = regs[1 * 4 + 2];      // oy
        const float m22 = regs[2 * 4 + 2];      // q
        const float m32 = regs[2 * 4 + 3];      // -q * near
        const float m23 = regs[3 * 4 + 2];      // must be 1: w = z_view

        // A projection has w = z, which is exactly what makes this not an
        // affine transform. Anything else in these registers is not ours.
        if (!nearly(m23, 1.0f) || m00 == 0.0f || m11 == 0.0f || m22 == 0.0f)
            return gp;
        if (!nearly(regs[3 * 4 + 0], 0.0f) || !nearly(regs[3 * 4 + 1], 0.0f) ||
            !nearly(regs[3 * 4 + 3], 0.0f))
            return gp;

        gp.sx = m00;
        gp.sy = m11;
        gp.ox = m20;
        gp.oy = m21;
        gp.q = m22;
        gp.near_z = -m32 / m22;

        // q = far / (far - near)  =>  far = q * near / (q - 1)
        gp.far_z = nearly(m22, 1.0f, 1e-9f) ? 0.0f : (m22 * gp.near_z) / (m22 - 1.0f);
        gp.valid = true;
        return gp;
    }

    Mat4 projection_from_tangents(float tan_left, float tan_right,
                                  float tan_top, float tan_bottom,
                                  float near_z, float far_z,
                                  bool flip_y)
    {
        const float dx = tan_right - tan_left;
        const float dy = tan_bottom - tan_top;

        Mat4 r{};
        if (dx == 0.0f || dy == 0.0f || far_z == near_z)
            return r;

        const float q = far_z / (far_z - near_z);

        r.m[0][0] = 2.0f / dx;
        r.m[1][1] = 2.0f / dy;

        // The off-centre terms, derived rather than remembered. NDC.x must be
        // -1 where x/z is the left tangent and +1 where it is the right:
        //
        //     NDC.x = 2*(t - l)/(r - l) - 1
        //           = (2/(r-l)) * t  -  (l + r)/(r - l)
        //
        // so the offset is NEGATIVE (l+r)/(r-l), matching D3DXMatrixPerspective-
        // OffCenterLH's (l+r)/(l-r). Written without the minus it comes out
        // exactly backwards, which shifts each eye the wrong way and doubles the
        // misalignment instead of correcting it -- about 19% of the screen width
        // on a Reverb G2. Each eye still looks fine on its own, and the two will
        // not fuse.
        r.m[2][0] = -(tan_left + tan_right) / dx;
        r.m[2][1] = -(tan_top + tan_bottom) / dy;
        r.m[2][2] = q;
        r.m[2][3] = 1.0f;                 // w = z_view
        r.m[3][2] = -q * near_z;

        // OpenVR's raw "bottom" is the tangent to the UP (+Y) edge and "top"
        // to the down edge (Valve's driver docs: "bottom and top are
        // backwards"), which is what the y-up rows above assume. A y-down
        // view (the game's, sy < 0) mirrors y itself, so only the scale
        // flips; the offset multiplies z and keeps its sign. Flipping it too
        // turned the frustum upside down about its centre -- invisible on a
        // Reverb G2 (top/bottom 1.050/1.056) but a pitch error of several
        // degrees on a Quest, felt as the world warping with head motion
        // (tester report 2026-10-05, Quest 3 over Virtual Desktop).
        if (flip_y)
            r.m[1][1] = -r.m[1][1];
        return r;
    }

    Mat4 projection_from_game(const GameProjection& gp)
    {
        Mat4 r{};
        r.m[0][0] = gp.sx;
        r.m[1][1] = gp.sy;
        r.m[2][0] = gp.ox;
        r.m[2][1] = gp.oy;
        r.m[2][2] = gp.q;
        r.m[2][3] = 1.0f;
        r.m[3][2] = -gp.q * gp.near_z;
        return r;
    }

    bool projection_depth_from_wvp(const Mat4& wvp,
                                   const GameProjection& shape,
                                   GameProjection* out)
    {
        if (!out)
            return false;

        // For an affine X and the game's projection P:
        //
        //   (X*P).col3.xyz = X.col2.xyz
        //   (X*P).col2.xyz = q * X.col2.xyz
        //
        // Least squares keeps the recovery stable if the uploaded floats
        // have accumulated a little rounding noise.
        float zz = 0.0f;
        float qz = 0.0f;
        for (int row = 0; row < 3; ++row)
        {
            const float z = wvp.m[row][3];
            zz += z * z;
            qz += wvp.m[row][2] * z;
        }
        if (!std::isfinite(zz) || zz < 1.0e-12f)
            return false;

        const float q = qz / zz;
        float error2 = 0.0f;
        for (int row = 0; row < 3; ++row)
        {
            const float error = wvp.m[row][2] - q * wvp.m[row][3];
            error2 += error * error;
        }
        if (!std::isfinite(q) || q <= 1.0f ||
            error2 > zz * 1.0e-6f)
            return false;

        const float q_near = q * wvp.m[3][3] - wvp.m[3][2];
        const float near_z = q_near / q;
        const float far_z = (q * near_z) / (q - 1.0f);
        if (!std::isfinite(near_z) || !std::isfinite(far_z) ||
            near_z <= 0.0f || far_z <= near_z || far_z > 1.0e9f)
            return false;

        *out = shape;
        out->q = q;
        out->near_z = near_z;
        out->far_z = far_z;
        out->valid = true;
        return true;
    }

    Mat4 reproject_ndc_to_frustum(const Mat4& source,
                                  const Mat4& reference,
                                  const Mat4& target,
                                  float scale)
    {
        Mat4 result = source;
        if (reference.m[0][0] == 0.0f || reference.m[1][1] == 0.0f)
            return result;

        // For X (and identically Y), a target-frustum NDC coordinate is:
        //
        //   target_ndc = target_s * ray + target_o
        //   ray = (scale * source_ndc - reference_o) / reference_s
        //
        // therefore target_ndc = a * source_ndc + b. In homogeneous clip
        // space that is target_clip = a * source_clip + b * source_w.
        const float x_ratio = target.m[0][0] / reference.m[0][0];
        const float y_ratio = target.m[1][1] / reference.m[1][1];
        const float x_a = scale * x_ratio;
        const float y_a = scale * y_ratio;
        const float x_b = target.m[2][0] - x_ratio * reference.m[2][0];
        const float y_b = target.m[2][1] - y_ratio * reference.m[2][1];

        for (int row = 0; row < 4; ++row)
        {
            result.m[row][0] = x_a * source.m[row][0]
                              + x_b * source.m[row][3];
            result.m[row][1] = y_a * source.m[row][1]
                              + y_b * source.m[row][3];
        }
        return result;
    }

    Mat4 reproject_ndc_to_world_locked(const Mat4& source,
                                       const Mat4& reference,
                                       const Mat4& target,
                                       const Mat4& head_view_rotation,
                                       float scale)
    {
        Mat4 inv_reference{};
        if (!invert(reference, &inv_reference))
            return reproject_ndc_to_frustum(source, reference, target, scale);

        // Source produces the game's clip coordinates. Scaling X/Y there is
        // the existing angular-size control. Inverting the centre projection
        // turns those coordinates back into rays in the menu's fixed frame;
        // the headset view then moves those rays across the live eye as the
        // player looks away from where the menu was opened.
        Mat4 clip_scale = Mat4::identity();
        clip_scale.m[0][0] = scale;
        clip_scale.m[1][1] = scale;

        Mat4 rotation = head_view_rotation;
        rotation.m[3][0] = rotation.m[3][1] = rotation.m[3][2] = 0.0f;
        return source * clip_scale * inv_reference * rotation * target;
    }

    void registers_from_matrix(const Mat4& m, float* out)
    {
        if (!out)
            return;
        for (int i = 0; i < 4; ++i)          // register i holds column i
            for (int k = 0; k < 4; ++k)
                out[i * 4 + k] = m.m[k][i];
    }

    // Turns an OpenVR pose into a view matrix in the engine's space.
    //
    // Three conversions happen here, and getting any one wrong produces a
    // picture that looks almost right, which is the hardest kind to debug:
    //
    //  1. OpenVR gives the head's pose (head -> tracking space). A view
    //     matrix is the inverse of that.
    //  2. OpenVR is right-handed, Y up, -Z forward. This engine is left-handed,
    //     **Y down**, +Z forward. The basis change is S = diag(1,-1,-1),
    //     applied as S*R*S to the rotation and S*t to the position.
    //
    //     Y down is not a guess. The projection's y scale is negative, and a
    //     negative sy with a Y-up view space would put anything above the
    //     camera at the bottom of the screen. It is only self-consistent if
    //     view-space Y points down.
    //
    //     Getting this wrong is quiet: a Y-flip leaves yaw alone, because a
    //     rotation about Y has no terms mixing Y with anything. It reverses
    //     pitch and roll, which both do. "Yaw right, pitch and roll inverted"
    //     is the signature of exactly this mistake, and is how it was found.
    //  3. OpenVR measures in metres and the engine does not.
    Mat4 view_from_pose(const float* pose34, float world_scale)
    {
    // Rotation and translation, right-handed, as OpenVR gave them.
        float R[3][3];
        float t[3];
        for (int row = 0; row < 3; ++row)
        {
            for (int col = 0; col < 3; ++col)
                R[row][col] = pose34[row * 4 + col];
            t[row] = pose34[row * 4 + 3];
        }

    // S*R*S multiplies element [i][j] by s[i]*s[j], so with s = (1,-1,-1) it
    // negates exactly those terms that mix X with Y or Z. The terms mixing Y
    // and Z survive, which is what leaves yaw alone while correcting pitch
    // and roll.
        R[0][1] = -R[0][1];
        R[0][2] = -R[0][2];
        R[1][0] = -R[1][0];
        R[2][0] = -R[2][0];
        t[1] = -t[1];
        t[2] = -t[2];

    // The view is the inverse of the pose. For a rigid transform in
    // row-vector form that means the rotation block is R itself, and
    // the translation row is -t * R.
    Mat4 v = Mat4::identity();
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                v.m[i][j] = R[i][j];

        for (int j = 0; j < 3; ++j)
        {
            float acc = 0.0f;
            for (int i = 0; i < 3; ++i)
                acc += t[i] * R[i][j];
            v.m[3][j] = -acc * world_scale;
        }
        return v;
    }

    bool is_perspective_registers(const float* regs)
    {
        if (!regs)
            return false;

        // c3 is the row that produces w. For a perspective transform w varies
        // with position, so c3's xyz cannot all be zero. A 2D or orthographic
        // pass leaves w constant, giving c3 = (0,0,0,k).
        const float x = regs[3 * 4 + 0];
        const float y = regs[3 * 4 + 1];
        const float z = regs[3 * 4 + 2];
        return (fabsf(x) + fabsf(y) + fabsf(z)) > 1e-6f;
    }

    bool invert(const Mat4& in, Mat4* out)
    {
        if (!out)
            return false;

        const float* m = &in.m[0][0];
        float inv[16];

        inv[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15]
                 + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
        inv[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15]
                 - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
        inv[8]  =  m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15]
                 + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
        inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14]
                 - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
        inv[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15]
                 - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
        inv[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15]
                 + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
        inv[9]  = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15]
                 - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
        inv[13] =  m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14]
                 + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
        inv[2]  =  m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15]
                 + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
        inv[6]  = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15]
                 - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
        inv[10] =  m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15]
                 + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
        inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14]
                 - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
        inv[3]  = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11]
                 - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
        inv[7]  =  m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11]
                 + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
        inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11]
                 - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
        inv[15] =  m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10]
                 + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];

        float det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
        if (fabsf(det) < 1e-20f)
            return false;

        det = 1.0f / det;
        float* o = &out->m[0][0];
        for (int i = 0; i < 16; ++i)
            o[i] = inv[i] * det;
        return true;
    }
}
