// Tests for the projection maths, against the matrix the game actually uses.
//
// The registers below are a real capture from Bolivia, not a made-up example.
// If the conventions in vr_math.h are right, feeding the game's own field of
// view back through the tangent path has to reproduce them exactly -- that is
// the test that matters, because everything else builds on it.

#include "../vr/vr_math.h"
#include "../vr/vr_gesture.h"

#include <cmath>
#include <cstdio>

using namespace trlvr;

namespace
{
    int g_failures = 0;

    void check(bool ok, const char* what)
    {
        printf("  [%s] %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok)
            ++g_failures;
    }

    bool close(float a, float b, float eps = 1e-4f)
    {
        return fabsf(a - b) <= eps;
    }

    void print_regs(const char* label, const float* r)
    {
        printf("    %s\n", label);
        for (int i = 0; i < 4; ++i)
            printf("      c%d = [ %12.5f %12.5f %12.5f %12.5f ]\n",
                   i, r[i * 4 + 0], r[i * 4 + 1], r[i * 4 + 2], r[i * 4 + 3]);
    }

    // The conversion as it was before the Y-flip was added: S = diag(1,1,-1).
    // Kept only so the tests can state what changed. In the headset this gave
    // correct yaw with pitch and roll both inverted.
    Mat4 legacy_view_from_pose(const float* pose34)
    {
        float R[3][3], t[3];
        for (int row = 0; row < 3; ++row)
        {
            for (int col = 0; col < 3; ++col)
                R[row][col] = pose34[row * 4 + col];
            t[row] = pose34[row * 4 + 3];
        }
        R[0][2] = -R[0][2];
        R[1][2] = -R[1][2];
        R[2][0] = -R[2][0];
        R[2][1] = -R[2][1];
        t[2] = -t[2];

        Mat4 v = Mat4::identity();
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                v.m[i][j] = R[i][j];
        for (int j = 0; j < 3; ++j)
        {
            float acc = 0.0f;
            for (int i = 0; i < 3; ++i)
                acc += t[i] * R[i][j];
            v.m[3][j] = -acc;
        }
        return v;
    }

    // Captured from trlvr.log during normal play.
    const float kGameRegs[16] = {
        2.0000f,  0.0000f,  0.0000f,   0.0000f,   // c0
        0.0000f, -2.2857f,  0.0000f,   0.0000f,   // c1
        0.0000f,  0.0000f,  1.0001f, -16.0020f,   // c2
        0.0000f,  0.0000f,  1.0000f,   0.0000f,   // c3
    };
}

int main()
{
    printf("projection maths tests\n\n");

    // --- reading the game's projection back out of its registers ----------
    GameProjection gp = projection_from_registers(kGameRegs);
    check(gp.valid, "recognised the game's registers as a projection");
    check(close(gp.sx, 2.0000f), "recovered x scale 2.0000");
    check(close(gp.sy, -2.2857f), "recovered y scale -2.2857 (renders flipped)");
    check(close(gp.ox, 0.0f) && close(gp.oy, 0.0f), "frustum is symmetric");
    check(close(gp.near_z, 16.0004f, 1e-3f), "recovered near plane 16");
    printf("    near=%.4f far=%.1f q=%.6f\n", gp.near_z, gp.far_z, gp.q);

    // --- rebuilding it must give back exactly what we started with --------
    float rebuilt[16];
    registers_from_matrix(projection_from_game(gp), rebuilt);
    bool same = true;
    for (int i = 0; i < 16; ++i)
        if (!close(rebuilt[i], kGameRegs[i]))
            same = false;
    check(same, "rebuilding from the recovered values reproduces the registers");
    if (!same)
    {
        print_regs("expected:", kGameRegs);
        print_regs("got:", rebuilt);
    }

    // --- the tangent path must agree with the game's own convention -------
    // Symmetric frustum with the game's field of view: sx = 2/(r-l) with
    // r = -l gives r = 1/sx, and likewise for y.
    const float r = 1.0f / gp.sx;
    const float b = 1.0f / fabsf(gp.sy);
    Mat4 fromTangents = projection_from_tangents(-r, r, -b, b,
                                                 gp.near_z, gp.far_z,
                                                 /*flip_y*/ gp.sy < 0.0f);
    float tang[16];
    registers_from_matrix(fromTangents, tang);
    same = true;
    for (int i = 0; i < 16; ++i)
        if (!close(tang[i], kGameRegs[i], 1e-3f))
            same = false;
    check(same, "building from tangents reproduces the game's matrix");
    if (!same)
    {
        print_regs("expected:", kGameRegs);
        print_regs("got:", tang);
    }

    // --- an eye offset is a translation in view space ---------------------
    // Moving the camera right by d is moving the world left by d, so the left
    // eye translates the world by +d and the right eye by -d.
    const float ipd_half = 0.032f;      // metres; scaled to game units later
    Mat4 eye = translation(-ipd_half, 0.0f, 0.0f) * projection_from_game(gp);
    float eyeRegs[16];
    registers_from_matrix(eye, eyeRegs);

    // Only c0's w should move, by tx * sx, and nothing else may change.
    check(close(eyeRegs[0 * 4 + 3], -ipd_half * gp.sx),
          "eye offset lands in c0.w as tx * sx");
    bool untouched = true;
    for (int i = 0; i < 16; ++i)
    {
        if (i == 0 * 4 + 3)
            continue;
        if (!close(eyeRegs[i], kGameRegs[i]))
            untouched = false;
    }
    check(untouched, "a lateral eye offset disturbs nothing else");
    if (!untouched)
        print_regs("eye matrix:", eyeRegs);

    // --- an off-centre frustum is what makes stereo correct ---------------
    //
    // Checking these for "non-zero" was not enough: the offsets were the right
    // magnitude with the wrong sign for a long time, which shifts each eye the
    // wrong way and stops the two fusing while each still looks fine alone.
    //
    // The real requirement is the definition of the frustum: a ray along the
    // left tangent must land on the left edge of the screen, and so on. Those
    // are the tangents from a real Reverb G2, strongly asymmetric, which is
    // where a sign error actually shows.
    {
        const float L = -1.3000f, R = 0.8862f, T = -1.0497f, B = 1.0560f;
        Mat4 f = projection_from_tangents(L, R, T, B, gp.near_z, gp.far_z, false);

        // Row-vector: clip = v * M, and for a point at x/z = tangent the NDC
        // works out as tangent * scale + offset.
        const float sx = f.m[0][0], ox = f.m[2][0];
        const float sy = f.m[1][1], oy = f.m[2][1];

        check(close(L * sx + ox, -1.0f, 1e-3f),
              "a ray along the left tangent lands on the left edge");
        check(close(R * sx + ox, 1.0f, 1e-3f),
              "a ray along the right tangent lands on the right edge");
        check(close(T * sy + oy, -1.0f, 1e-3f),
              "a ray along the top tangent lands on one vertical edge");
        check(close(B * sy + oy, 1.0f, 1e-3f),
              "a ray along the bottom tangent lands on the other");

        // The centre of an asymmetric frustum is not the centre of the screen,
        // and the offset has to point the right way.
        check(ox > 0.0f,
              "an eye whose frustum reaches further left offsets to the right");

        float offRegs[16];
        registers_from_matrix(f, offRegs);
        check(close(offRegs[3 * 4 + 2], 1.0f), "w is still z_view");
    }

    // --- a mono HUD still needs each lens's asymmetric projection ---------
    // Copying identical NDC coordinates to the two eye textures does not copy
    // identical rays: the Reverb G2's optical centres are about 19% off the
    // texture centre in opposite directions. Map the mono image through each
    // frustum while deliberately omitting positional eye separation.
    {
        const float T = -1.0497f, B = 1.0560f;
        Mat4 centre = projection_from_tangents(-1.0931f, 1.0931f, T, B,
                                               gp.near_z, gp.far_z, false);
        Mat4 left = projection_from_tangents(-1.3000f, 0.8862f, T, B,
                                             gp.near_z, gp.far_z, false);
        Mat4 right = projection_from_tangents(-0.8862f, 1.3000f, T, B,
                                              gp.near_z, gp.far_z, false);
        const float ui_scale = 0.6f;
        Mat4 source = projection_from_game(gp);
        Mat4 left_ui = reproject_ndc_to_frustum(source, centre, left, ui_scale);
        Mat4 right_ui = reproject_ndc_to_frustum(source, centre, right, ui_scale);

        const float source_tangent = 0.2f;
        const float source_ndc = source_tangent * source.m[0][0] + source.m[2][0];
        const float wanted_ray = (ui_scale * source_ndc - centre.m[2][0])
                               / centre.m[0][0];
        const float left_ndc = source_tangent * left_ui.m[0][0] + left_ui.m[2][0];
        const float right_ndc = source_tangent * right_ui.m[0][0] + right_ui.m[2][0];
        const float left_ray = (left_ndc - left.m[2][0]) / left.m[0][0];
        const float right_ray = (right_ndc - right.m[2][0]) / right.m[0][0];

        check(close(left_ray, wanted_ray, 1e-4f) &&
              close(right_ray, wanted_ray, 1e-4f),
              "HUD coordinates become the same physical ray in both eyes");
        check(close(left_ui.m[2][0], left.m[2][0], 1e-4f) &&
              close(right_ui.m[2][0], right.m[2][0], 1e-4f),
              "lens-centre correction is not accidentally reduced by UI scale");

        bool depth_untouched = true;
        for (int row = 0; row < 4; ++row)
            if (!close(left_ui.m[row][2], source.m[row][2]) ||
                !close(left_ui.m[row][3], source.m[row][3]))
                depth_untouched = false;
        check(depth_untouched,
              "HUD lens correction leaves clip W and depth untouched");

        // World locking rotates a real perspective plane. Give its target
        // projection a close overlay near plane so the source plane (which
        // starts at the game's near depth) is not clipped as it rotates.
        const Mat4 left_overlay = projection_from_tangents(
            -1.3000f, 0.8862f, T, B, 1.0f, gp.far_z, false);
        const Mat4 locked_identity = reproject_ndc_to_world_locked(
            source, centre, left_overlay, Mat4::identity(), ui_scale);
        const float identity_x = source_tangent * locked_identity.m[0][0]
                               + locked_identity.m[2][0];
        const float identity_w = source_tangent * locked_identity.m[0][3]
                               + locked_identity.m[2][3];
        const float identity_ray = (identity_x / identity_w - left.m[2][0]) /
                                   left.m[0][0];
        check(close(identity_ray, wanted_ray, 1e-4f),
              "a newly anchored menu keeps the head-locked HUD ray");

        const float menu_yaw = 0.25f;
        const Mat4 locked_yaw = reproject_ndc_to_world_locked(
            source, centre, left_overlay, yaw_rotation(menu_yaw), ui_scale);
        const float clip_x = locked_yaw.m[2][0] + locked_yaw.m[3][0];
        const float clip_w = locked_yaw.m[2][3] + locked_yaw.m[3][3];
        const float locked_ndc = clip_x / clip_w;
        const float locked_ray = (locked_ndc - left.m[2][0]) / left.m[0][0];
        check(close(locked_ray, tanf(menu_yaw), 1e-3f),
              "turning the head moves a world-locked menu by the same angle");

        const Mat4 partial_yaw = rotation_fraction(yaw_rotation(menu_yaw), 0.15f);
        check(close(yaw_of(partial_yaw),
                    0.15f * yaw_of(yaw_rotation(menu_yaw)), 1e-4f),
              "gameplay HUD drag retains the requested fraction of head yaw");
        const Mat4 no_drag = rotation_fraction(yaw_rotation(menu_yaw), 0.0f);
        check(close(no_drag.m[0][0], 1.0f) && close(no_drag.m[2][2], 1.0f) &&
              close(no_drag.m[0][2], 0.0f) && close(no_drag.m[2][0], 0.0f),
              "full HUD follow reduces the residual rotation to identity");

        Mat4 pitch = Mat4::identity();
        pitch.m[1][1] = cosf(0.30f);  pitch.m[1][2] = -sinf(0.30f);
        pitch.m[2][1] = sinf(0.30f);  pitch.m[2][2] =  cosf(0.30f);
        Mat4 roll = Mat4::identity();
        roll.m[0][0] = cosf(-0.20f); roll.m[0][1] = -sinf(-0.20f);
        roll.m[1][0] = sinf(-0.20f); roll.m[1][1] =  cosf(-0.20f);
        const Mat4 combined = yaw_rotation(menu_yaw) * pitch * roll;
        const Mat4 half = rotation_fraction(combined, 0.5f);
        const Mat4 rebuilt_rotation = half * half;
        bool full_rotation = true;
        for (int row = 0; row < 3; ++row)
            for (int column = 0; column < 3; ++column)
                if (!close(rebuilt_rotation.m[row][column],
                           combined.m[row][column], 1e-4f))
                    full_rotation = false;
        check(full_rotation,
              "HUD drag scales combined yaw, pitch and roll on one rigid arc");

        const float menu_z = gp.near_z;
        const float rotated_clip_z = menu_z * locked_yaw.m[2][2]
                                   + locked_yaw.m[3][2];
        const float rotated_clip_w = menu_z * locked_yaw.m[2][3]
                                   + locked_yaw.m[3][3];
        check(rotated_clip_z >= 0.0f &&
              rotated_clip_z <= rotated_clip_w,
              "overlay near plane keeps a rotated menu inside clip depth");
    }

    // --- refuse to touch registers that are not a projection --------------
    float affine[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        5, 6, 7, 1,
    };
    check(!projection_from_registers(affine).valid,
          "an affine matrix is rejected, not mistaken for a projection");

    float lights[16] = {
        -33480.15f, 3862.18f, 54620.36f, 1250.0f,
        0.0001f, 0.0001f, 0.0001f, 0.0f,
        -33921.61f, 4197.66f, 54792.84f, 500.0f,
        0.0002f, 0.0002f, 0.0002f, 0.0f,
    };
    check(!projection_from_registers(lights).valid,
          "the light block at c16 is rejected too");

    // --- decomposing a world-view-projection -------------------------------
    // Not all geometry arrives in view space. World geometry gets a full
    // world-view-projection in c0..c3, and it has to be taken apart against the
    // game's own projection so a different one can be put back.
    {
        Mat4 P = projection_from_game(gp);
        Mat4 Pinv{};
        check(invert(P, &Pinv), "the game's projection is invertible");

        Mat4 should_be_id = P * Pinv;
        bool id = true;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                if (!close(should_be_id.m[i][j], i == j ? 1.0f : 0.0f, 1e-3f))
                    id = false;
        check(id, "projection times its inverse is the identity");

        // A world-view transform, rotation and translation, of the kind the
        // engine would fold in front of the projection.
        Mat4 X = Mat4::identity();
        X.m[0][0] =  0.6f; X.m[0][2] = 0.8f;
        X.m[2][0] = -0.8f; X.m[2][2] = 0.6f;
        X.m[3][0] = 120.0f; X.m[3][1] = -40.0f; X.m[3][2] = 900.0f;

        const Mat4 M = X * P;
        const Mat4 recovered = M * Pinv;
        bool same_x = true;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                if (!close(recovered.m[i][j], X.m[i][j], 1e-2f))
                    same_x = false;
        check(same_x, "M * inverse(P) recovers the world-view transform exactly");

        // Accurate aim changes the camera depth range independently of some
        // pure-projection uploads. Recovering an actual near=50 WVP with a
        // cached near=16 projection used to leave a projective shear in X.
        GameProjection depth_changed = gp;
        depth_changed.near_z = 50.0f;
        depth_changed.q = depth_changed.far_z /
                          (depth_changed.far_z - depth_changed.near_z);
        const Mat4 changed_m = X * projection_from_game(depth_changed);
        GameProjection recovered_depth{};
        check(projection_depth_from_wvp(changed_m, gp, &recovered_depth),
              "world-view-projection exposes its own depth range");
        check(close(recovered_depth.near_z, depth_changed.near_z, 0.05f) &&
              close(recovered_depth.far_z, depth_changed.far_z, 300.0f),
              "recovered WVP near/far are independent of the cached projection");
        Mat4 changed_inv{};
        bool recovered_x = invert(projection_from_game(recovered_depth),
                                  &changed_inv);
        const Mat4 changed_x = changed_m * changed_inv;
        for (int i = 0; recovered_x && i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                if (!close(changed_x.m[i][j], X.m[i][j], 2.0e-3f))
                    recovered_x = false;
        check(recovered_x,
              "per-draw WVP depth recovery removes the camera transform cleanly");

        // And such a matrix must be recognised as perspective, while a 2D pass
        // must not be.
        float wvp[16];
        registers_from_matrix(M, wvp);
        check(is_perspective_registers(wvp),
              "a world-view-projection is recognised as perspective");

        const float ortho[16] = {
            2, 0, 0, 0,
            0, 2, 0, 0,
            0, 0, 1, 0,
            0, 0, 0, 1,          // c3 = (0,0,0,1): w is constant
        };
        check(!is_perspective_registers(ortho),
              "a 2D pass is not mistaken for perspective");
    }

    // --- pose to view -----------------------------------------------------
    // OpenVR poses are 3x4 row-major, right-handed, Y up, -Z forward, metres.
    {
        const float identity_pose[12] = {
            1, 0, 0, 0,
            0, 1, 0, 0,
            0, 0, 1, 0,
        };
        Mat4 v = view_from_pose(identity_pose, 100.0f);
        bool is_id = true;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                if (!close(v.m[i][j], i == j ? 1.0f : 0.0f))
                    is_id = false;
        check(is_id, "an identity pose gives an identity view");

        // Head one metre to the right. The world must move a metre left, in
        // game units.
        const float right_pose[12] = {
            1, 0, 0, 1.0f,
            0, 1, 0, 0,
            0, 0, 1, 0,
        };
        v = view_from_pose(right_pose, 100.0f);
        check(close(v.m[3][0], -100.0f), "stepping right moves the world left, in game units");
        check(close(v.m[3][1], 0.0f) && close(v.m[3][2], 0.0f),
              "a sideways step moves nothing else");

        // Forward is -Z in OpenVR and +Z here, so a step forward has to come
        // out as -Z in the engine's space.
        const float fwd_pose[12] = {
            1, 0, 0, 0,
            0, 1, 0, 0,
            0, 0, 1, -1.0f,
        };
        v = view_from_pose(fwd_pose, 100.0f);
        check(close(v.m[3][2], -100.0f),
              "stepping forward (-Z in OpenVR) moves the world to -Z here");

        // 90 degrees of yaw, head turning left: a positive rotation about Y in
        // OpenVR's right-handed frame.
        const float cs = 0.0f, sn = 1.0f;
        const float yaw_pose[12] = {
             cs, 0, sn, 0,
              0, 1,  0, 0,
            -sn, 0, cs, 0,
        };
        v = view_from_pose(yaw_pose, 100.0f);

        // A point straight ahead in engine space is +Z. Turn the head left and
        // it should swing round to the right, +X.
        const float pz = 1.0f;
        const float ox = pz * v.m[2][0];
        const float oz = pz * v.m[2][2];
        check(close(ox, 1.0f) && close(oz, 0.0f),
              "turning the head left swings what was ahead round to the right");

        // Pitch and roll are what a missing Y-flip gets wrong while leaving yaw
        // untouched. Rather than argue the absolute direction of each from
        // first principles -- which is easy to talk yourself into backwards --
        // these pin the relationship that was actually observed in the headset:
        // with the old diag(1,1,-1) conversion yaw was correct and pitch and
        // roll were both inverted. So the current conversion must agree with
        // the old one on yaw and oppose it on the other two.
        {
            // 90 degrees about each axis, right-handed, as OpenVR reports them.
            const float yaw_p[12]   = {  cs,0,sn,0,   0,1,0,0,   -sn,0,cs,0 };
            const float pitch_p[12] = { 1,0,0,0,   0,cs,-sn,0,   0,sn,cs,0 };
            const float roll_p[12]  = { cs,-sn,0,0,   sn,cs,0,0,   0,0,1,0 };

            struct Case { const char* name; const float* pose; bool same_as_old; };
            const Case cases[3] = {
                { "yaw is unchanged by the Y-flip",        yaw_p,   true  },
                { "pitch is reversed by the Y-flip",       pitch_p, false },
                { "roll is reversed by the Y-flip",        roll_p,  false },
            };

            for (const Case& c : cases)
            {
                const Mat4 now = view_from_pose(c.pose, 1.0f);
                const Mat4 old = legacy_view_from_pose(c.pose);

                bool identical = true;
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j)
                        if (!close(now.m[i][j], old.m[i][j]))
                            identical = false;

                check(identical == c.same_as_old, c.name);
            }
        }

        // Standing up moves the world down the screen, which in a Y-down space
        // is +Y.
        const float up_pose[12] = {
            1, 0, 0, 0,
            0, 1, 0, 1.0f,
            0, 0, 1, 0,
        };
        v = view_from_pose(up_pose, 100.0f);
        check(close(v.m[3][1], 100.0f),
              "standing up moves the world down the screen (+Y here)");

        // If this drifts the whole world shears.
        bool ortho = true;
        for (int i = 0; i < 3; ++i)
        {
            float len = 0.0f;
            for (int j = 0; j < 3; ++j)
                len += v.m[i][j] * v.m[i][j];
            if (!close(len, 1.0f))
                ortho = false;
        }
        check(ortho, "the view rotation stays orthonormal");
    }

    // -- the heading, and why it must come from the forward column --
    //
    // yaw_of was first written against row 2 of the view matrix. For a pose
    // carrying nothing but yaw that is merely the negation, which reads as a
    // sign to flip and hides the fault entirely. Add pitch and it stops
    // being a heading at all: in the headset that showed up as the yaw
    // inverting on the way round to facing backwards, and a sloping horizon.
    //
    // So these assert the definition -- the heading of a pose built with a
    // known yaw is that yaw, whatever pitch is present -- rather than that
    // it is non-zero, which the broken version satisfied perfectly.
    {
        const float yaw = 0.6f;      // about 34 degrees
        const float pitch = 0.4f;    // enough to matter

        // An OpenVR pose, right-handed with -Z forward: yaw about Y then
        // pitch about X, written as the 3x4 the runtime hands over.
        const float cy = cosf(yaw),   sy = sinf(yaw);
        const float cp = cosf(pitch), sp = sinf(pitch);
        float pose[12] = {
              cy,  sy * sp,  sy * cp, 0.0f,
            0.0f,       cp,      -sp, 0.0f,
             -sy,  cy * sp,  cy * cp, 0.0f,
        };
        float flat[12] = {
              cy,     0.0f,       sy, 0.0f,
            0.0f,     1.0f,     0.0f, 0.0f,
             -sy,     0.0f,       cy, 0.0f,
        };

        const Mat4 v = view_from_pose(pose, 100.0f);
        const float got = yaw_of(v);
        const float got_flat = yaw_of(view_from_pose(flat, 100.0f));

        check(close(got, got_flat, 1e-3f),
              "the heading is the same with pitch as without it");
        check(fabsf(got) > 0.1f, "and it is a real angle, not zero");

        // Taking the heading back out has to leave none behind: the property
        // the runtime leans on when the camera is carrying the yaw. Two
        // candidates were tried at first and neither worked with pitch in
        // the pose -- that failure is why remove_yaw exists.
        const Mat4 residual = remove_yaw(v, got);
        check(fabsf(yaw_of(residual)) < 1e-3f,
              "removing the heading leaves none of it behind");

        // And it has to leave the pitch alone, or it is not removing a
        // heading, it is just finding some rotation with no heading in it.
        const float pitch_before = asinf(v.m[1][2]);
        const float pitch_after  = asinf(residual.m[1][2]);
        check(close(pitch_before, pitch_after, 1e-3f),
              "and it leaves the pitch untouched");

        // set_yaw is what splits the head rotation between the camera and
        // the projection. The camera carries whatever it was last built
        // with; the projection owes the difference. Getting this wrong does
        // not look like a maths error in the headset -- the view just stops
        // following the head whenever the engine skips a camera rebuild.
        for (int i = 0; i < 5; ++i)
        {
            const float wanted = -1.2f + 0.6f * (float)i;
            const Mat4 aimed = set_yaw(v, wanted);
            check(fabsf(wrap_pi(yaw_of(aimed) - wanted)) < 1e-3f,
                  "set_yaw lands on the heading it was asked for");
            check(close(asinf(v.m[1][2]), asinf(aimed.m[1][2]), 1e-3f),
                  "  and keeps the pitch while doing it");
        }

        // The two cases the runtime actually hits: the camera fully up to
        // date, and the camera frozen behind a menu.
        check(fabsf(yaw_of(set_yaw(v, 0.0f))) < 1e-3f,
              "a camera holding all the yaw leaves the projection none");
        check(fabsf(wrap_pi(yaw_of(set_yaw(v, got)) - got)) < 1e-3f,
              "a frozen camera leaves the projection all of it");
    }

    // -- the first-person heading looking steeply down or up --
    //
    // Lara faces and walks along the headset heading. yaw_of reads it from
    // the forward column alone, which looking at the feet is nearly
    // vertical: a 10 degree head turn there read as 45 and swung Lara with
    // it (review 2026-10-02). steady_yaw_of has to agree with yaw_of for any
    // plain yaw-and-pitch pose, and stay near 1:1 for a turn of the head.
    {
        // OpenVR rotation: yaw about Y, then pitch about X, then a turn of
        // the head about its own up axis.
        auto pose_of = [](float yaw, float pitch, float turn, float* pose) {
            const float cy = cosf(yaw), sy = sinf(yaw);
            const float cp = cosf(pitch), sp = sinf(pitch);
            const float ct = cosf(turn), st = sinf(turn);
            const float Y[3][3] = { { cy, 0, sy }, { 0, 1, 0 }, { -sy, 0, cy } };
            const float P[3][3] = { { 1, 0, 0 }, { 0, cp, -sp }, { 0, sp, cp } };
            const float T[3][3] = { { ct, 0, st }, { 0, 1, 0 }, { -st, 0, ct } };
            float YP[3][3], R[3][3];
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                {
                    YP[i][j] = 0.0f;
                    for (int k = 0; k < 3; ++k)
                        YP[i][j] += Y[i][k] * P[k][j];
                }
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                {
                    R[i][j] = 0.0f;
                    for (int k = 0; k < 3; ++k)
                        R[i][j] += YP[i][k] * T[k][j];
                }
            for (int i = 0; i < 3; ++i)
            {
                for (int j = 0; j < 3; ++j)
                    pose[i * 4 + j] = R[i][j];
                pose[i * 4 + 3] = 0.0f;
            }
        };

        bool agrees = true;
        for (int y = -3; y <= 3; ++y)
            for (int p = -8; p <= 8; ++p)
            {
                float pose[12];
                pose_of(0.9f * (float)y, 0.17f * (float)p, 0.0f, pose);
                const Mat4 v = view_from_pose(pose, 100.0f);
                agrees = agrees &&
                    fabsf(wrap_pi(steady_yaw_of(v) - yaw_of(v))) < 1e-3f;
            }
        check(agrees, "steady_yaw_of equals yaw_of for yaw and pitch alone");

        float level[12], down[12], up[12];
        pose_of(0.5f, -1.5690f, 0.0f, down);   // 89.9 degrees down
        pose_of(0.5f, 1.5690f, 0.0f, up);
        pose_of(0.5f, 0.0f, 0.0f, level);
        const float heading = yaw_of(view_from_pose(level, 100.0f));
        check(fabsf(wrap_pi(steady_yaw_of(view_from_pose(down, 100.0f)) -
                            heading)) < 1e-2f &&
              fabsf(wrap_pi(steady_yaw_of(view_from_pose(up, 100.0f)) -
                            heading)) < 1e-2f,
              "  and keeps the heading looking straight down or up");

        // 80 degrees down at the feet, head turned 10 degrees.
        float turned[12];
        const float turn = 0.1745f;
        pose_of(0.5f, -1.3963f, turn, turned);
        const Mat4 t = view_from_pose(turned, 100.0f);
        const float steady = fabsf(wrap_pi(steady_yaw_of(t) - heading));
        const float plain = fabsf(wrap_pi(yaw_of(t) - heading));
        printf("    80 deg down, 10 deg head turn: yaw_of moves %.1f deg, "
               "steady_yaw_of %.1f\n", plain * 57.29578f, steady * 57.29578f);
        check(plain > 0.5f, "yaw_of swings with a small turn at the feet");
        check(steady < 1.3f * turn,
              "  steady_yaw_of follows it at most about 1:1");
        float level_turned[12];
        pose_of(0.5f, 0.0f, turn, level_turned);
        check(fabsf(wrap_pi(steady_yaw_of(view_from_pose(level_turned,
                                                          100.0f)) -
                            heading) - turn) < 1e-3f ||
              fabsf(wrap_pi(steady_yaw_of(view_from_pose(level_turned,
                                                          100.0f)) -
                            heading) + turn) < 1e-3f,
              "  and a level head turn moves it exactly");
    }

    // -- a heading applied to a vector must match the matrix --
    //
    // The camera takes a heading as a matrix and the head displacement takes
    // it as a vector. If those two ever disagree, walking forward comes out
    // sideways, which is exactly what it did.
    {
        const float angle = 0.7f;
        float v[3] = { 1.0f, 2.0f, 3.0f };
        const Mat4 r = yaw_rotation(angle);
        const float mx = v[0] * r.m[0][0] + v[1] * r.m[1][0] + v[2] * r.m[2][0];
        const float mz = v[0] * r.m[0][2] + v[1] * r.m[1][2] + v[2] * r.m[2][2];

        rotate_yaw(v, angle);
        check(close(v[0], mx, 1e-4f) && close(v[2], mz, 1e-4f),
              "a heading rotates a vector the same way it rotates a matrix");
        check(close(v[1], 2.0f),
              "and leaves the vertical alone");

        // Undoing it has to return the vector, or the correction that takes
        // the head yaw back out of a room-space displacement is lossy.
        rotate_yaw(v, -angle);
        check(close(v[0], 1.0f, 1e-4f) && close(v[2], 3.0f, 1e-4f),
              "and the opposite heading puts it back");
    }

    // -- the rigid inverse --
    //
    // The camera rewrite rotates the world-to-camera matrix and then has to
    // rebuild its opposite, because the engine only recomputes that on one of
    // the several paths that reach the transform. If these two disagree, the
    // culling frustum is built from a camera that is not where the picture is
    // drawn from -- which looks like everything and nothing.
    {
        Mat4 m = yaw_rotation(0.9f);
        m.m[3][0] = 120.0f;
        m.m[3][1] = -45.0f;
        m.m[3][2] = 300.0f;

        const Mat4 back = rigid_inverse(m) * m;
        bool identity = true;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                identity = identity &&
                    close(back.m[i][j], (i == j) ? 1.0f : 0.0f, 1e-3f);
        check(identity, "a rigid transform times its inverse is the identity");

        // The other order too: a camera-to-world built this way has to undo
        // the world-to-camera it came from, not merely resemble it.
        const Mat4 fwd = m * rigid_inverse(m);
        identity = true;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                identity = identity &&
                    close(fwd.m[i][j], (i == j) ? 1.0f : 0.0f, 1e-3f);
        check(identity, "and the other way round as well");

        // The isolated-hand draw moves a wrist and every finger by the same
        // rigid delta. The wrist must reach the controller while the fingers
        // keep their local pose; otherwise the glove would stretch apart.
        Mat4 target = yaw_rotation(-0.4f);
        target.m[3][0] = -30.0f;
        target.m[3][1] = 55.0f;
        target.m[3][2] = 470.0f;
        const Mat4 finger = translation(12.0f, 0.0f, 4.0f) * m;
        const Mat4 delta = rigid_inverse(m) * target;
        const Mat4 moved_wrist = m * delta;
        const Mat4 moved_finger = finger * delta;
        bool wrist_matches = true;
        bool finger_matches = true;
        const Mat4 expected_finger = translation(12.0f, 0.0f, 4.0f) * target;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
            {
                wrist_matches = wrist_matches &&
                    close(moved_wrist.m[i][j], target.m[i][j], 1e-3f);
                finger_matches = finger_matches &&
                    close(moved_finger.m[i][j], expected_finger.m[i][j], 1e-3f);
            }
        check(wrist_matches, "isolated wrist reaches the controller pose");
        check(finger_matches, "finger pose stays rigid beside the wrist");

        const Mat4 zero_alignment = hand_alignment_rotation(0, 0, 0);
        bool zero_is_identity = true;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                zero_is_identity = zero_is_identity &&
                    close(zero_alignment.m[i][j],
                          i == j ? 1.0f : 0.0f, 1e-5f);
        check(zero_is_identity,
              "zero live hand rotation preserves the existing wrist pose");
        const Mat4 x_turn = hand_alignment_rotation(90, 0, 0);
        const Mat4 y_turn = hand_alignment_rotation(0, 90, 0);
        const Mat4 z_turn = hand_alignment_rotation(0, 0, 90);
        check(close(x_turn.m[1][2], 1.0f, 1e-4f) &&
              close(y_turn.m[2][0], 1.0f, 1e-4f) &&
              close(z_turn.m[0][1], 1.0f, 1e-4f),
              "hand X/Y/Z tuning turns around the named local axis");

        // A position correction should not make a wrist orbit when the
        // physical controller rotates without moving its tracked origin.
        Mat4 controller_zero = Mat4::identity();
        controller_zero.m[3][0] = 40.0f;
        controller_zero.m[3][1] = -30.0f;
        controller_zero.m[3][2] = 110.0f;
        Mat4 controller_turned = hand_alignment_rotation(63, -41, 27);
        for (int axis = 0; axis < 3; ++axis)
            controller_turned.m[3][axis] = controller_zero.m[3][axis];
        const Mat4 wrist_zero = hand_pose_with_head_offset(
            controller_zero, 12.0f, -20.0f, 8.0f);
        const Mat4 wrist_turned = hand_pose_with_head_offset(
            controller_turned, 12.0f, -20.0f, 8.0f);
        bool pivot_still = true;
        bool rotation_follows = true;
        for (int axis = 0; axis < 3; ++axis)
        {
            pivot_still = pivot_still &&
                close(wrist_zero.m[3][axis], wrist_turned.m[3][axis], 1e-4f);
            for (int other = 0; other < 3; ++other)
                rotation_follows = rotation_follows &&
                    close(wrist_turned.m[axis][other],
                          controller_turned.m[axis][other], 1e-4f);
        }
        check(pivot_still, "hand wrist stays put when controller rotates in place");
        check(rotation_follows, "hand rotation still follows controller rotation");
    }

    // -- residual motion for a camera that stops rebuilding while paused --
    //
    // A frozen world matrix already contains the head pose from the last
    // camera rebuild. Removing that anchor and composing the current pose must
    // be exactly the same as if the game had rebuilt its camera normally.
    {
        Mat4 base = yaw_rotation(-0.35f);
        base.m[3][0] = 80.0f;
        base.m[3][1] = -25.0f;
        base.m[3][2] = 410.0f;

        Mat4 anchor = yaw_rotation(0.2f);
        anchor.m[3][0] = -3.0f;
        anchor.m[3][1] = 1.5f;
        anchor.m[3][2] = 2.0f;

        Mat4 current = yaw_rotation(-0.65f);
        current.m[3][0] = 4.0f;
        current.m[3][1] = -2.5f;
        current.m[3][2] = -1.0f;

        const Mat4 frozen = base * anchor;
        const Mat4 residual = rigid_inverse(anchor) * current;
        const Mat4 updated = frozen * residual;
        const Mat4 expected_current = base * current;

        bool residual_matches = true;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                residual_matches = residual_matches &&
                    close(updated.m[i][j], expected_current.m[i][j], 1e-3f);
        check(residual_matches,
              "pause residual advances a frozen camera to the current head pose");
    }

    // -- wrapping --
    {
        const float two_pi = 6.28318531f;
        check(close(wrap_pi(0.5f), 0.5f),
              "an angle already in range is left alone");
        check(close(wrap_pi(6.0f), 6.0f - two_pi),
              "an angle past pi comes back as the short way round");
        check(close(wrap_pi(-6.0f), two_pi - 6.0f),
              "and the same going the other way");
        check(fabsf(wrap_pi(9.4247779f)) <= 3.14159266f,
              "several turns still land inside the range");
    }

    // -- immersive controller gesture zones --
    // Coordinates are metres relative to the player's yaw-only body frame.
    // The classifier should accept a natural upper-chest reach but reject
    // normal play positions at the side, face and lap.
    {
        check(vr_gesture_in_chest_zone(-0.13f, -0.25f, 0.03f),
              "immersive light accepts the centre of the chest zone");
        check(vr_gesture_in_chest_zone(-0.18f, -0.28f, 0.06f),
              "immersive light accepts a natural left-hand chest reach");
        check(!vr_gesture_in_chest_zone(0.13f, -0.25f, 0.03f),
              "immersive light rejects the medipack pouch on the right");
        check(!vr_gesture_in_chest_zone(-0.60f, -0.35f, 0.10f),
              "immersive light rejects a hand held out at the side");
        check(!vr_gesture_in_chest_zone(-0.10f, -0.78f, 0.10f),
              "immersive light rejects a controller resting in the lap");
        check(!vr_gesture_in_chest_zone(-0.24f, -0.50f, 0.08f),
              "immersive light rejects a reach toward the binocular belt");
        check(!vr_gesture_in_chest_zone(-0.10f, -0.08f, 0.02f),
              "immersive light rejects a controller raised near the face");
        check(vr_gesture_in_holster_zone(true, -0.259f, -0.63f, -0.08f) &&
              vr_gesture_in_holster_zone(false, 0.22f, -0.62f, -0.06f),
              "both pistol holsters accept their own hip reach");
        check(!vr_gesture_in_holster_zone(true, -0.21f, -0.41f, 0.12f) &&
              !vr_gesture_in_holster_zone(false, 0.20f, -0.41f, -0.04f),
              "ordinary high hand positions fall outside the smaller zones");
        VrHolsterZone custom = vr_default_holster_zone();
        custom.left_x = -0.50f;
        check(vr_gesture_in_holster_zone(true, -0.50f, custom.left_y,
                                        custom.left_z, &custom) &&
              !vr_gesture_in_holster_zone(true, -0.21f, -0.59f,
                                          0.12f, &custom),
              "developer zone centre changes the tested grab position");
        VrHolsterZone placed = vr_default_holster_zone();
        placed.right_y = -0.40f;
        placed.right_z = 0.20f;
        check(vr_gesture_in_holster_zone(false, placed.right_x, -0.40f,
                                         0.20f, &placed) &&
              !vr_gesture_in_holster_zone(false, placed.right_x,
                                          placed.left_y, placed.left_z,
                                          &placed) &&
              vr_gesture_in_holster_zone(true, placed.left_x,
                                         placed.left_y, placed.left_z,
                                         &placed),
              "a tuned holster moves only its own side");
        const float light_centre[3] = { 0.10f, -0.20f, 0.15f };
        check(vr_gesture_in_chest_zone(0.10f, -0.20f, 0.15f, light_centre) &&
              !vr_gesture_in_chest_zone(-0.16f, -0.34f, 0.09f,
                                        light_centre),
              "a tuned light zone replaces the default chest zone");
        check(!vr_gesture_in_holster_zone(true, 0.25f, -0.76f, 0.04f) &&
              !vr_gesture_in_holster_zone(false, -0.25f, -0.76f, 0.04f),
              "a pistol holster rejects the opposite hip");
        check(!vr_gesture_in_holster_zone(true, -0.12f, -0.38f, 0.12f),
              "the flashlight chest gesture cannot draw a pistol");
        check(vr_gesture_binoculars_at_face(-0.10f, -0.07f, 0.18f),
              "held binoculars reach the left eye");
        check(!vr_gesture_binoculars_at_face(-0.24f, -0.38f, 0.08f) &&
              !vr_gesture_binoculars_at_face(-0.24f, -0.63f, -0.07f),
              "chest and hip reaches do not open binocular view");
        check(vr_gesture_grapple_throw(0.42f, 0.31f, 1.6f),
              "a fast forward release throws the grapple");
        check(!vr_gesture_grapple_throw(0.42f, 0.31f, 0.5f) &&
              !vr_gesture_grapple_throw(0.10f, 0.31f, 1.6f) &&
              !vr_gesture_grapple_throw(0.42f, -0.12f, 1.6f),
              "slow, short and rearward releases do not throw the grapple");
        check(vr_gesture_grapple_pull(0.30f, 0.25f) &&
              vr_gesture_grapple_pull(0.16f, 1.2f),
              "a full pull or shorter fast yank tugs an attached grapple");
        check(!vr_gesture_grapple_pull(0.10f, 1.5f) &&
              !vr_gesture_grapple_pull(0.20f, 0.3f) &&
              !vr_gesture_grapple_pull(-0.25f, 2.0f),
              "short, slow and forward motions do not tug the grapple");
    }

    // -- first-person arm geometry --
    {
        const float x[3] = { 1.0f, 0.0f, 0.0f };
        const float y[3] = { 0.0f, 1.0f, 0.0f };
        const float negx[3] = { -1.0f, 0.0f, 0.0f };
        const Mat4 quarter = rotation_between(x, y);
        const Mat4 half = rotation_between(x, negx);
        check(close(quarter.m[0][0], 0.0f) &&
              close(quarter.m[0][1], 1.0f) &&
              close(half.m[0][0], -1.0f),
              "arm rotations map directions in the engine's row-vector order");

        const float shoulder[3] = { 0.0f, 0.0f, 0.0f };
        const float elbow[3] = { 100.0f, 0.0f, 0.0f };
        const float wrist[3] = { 100.0f, 100.0f, 0.0f };
        const float target[3] = { 0.0f, 100.0f, 0.0f };
        float solved_elbow[3], solved_wrist[3];
        const bool valid = solve_two_bone_arm(shoulder, elbow, wrist, target,
                                               solved_elbow, solved_wrist);
        const float upper_length = sqrtf(
            solved_elbow[0]*solved_elbow[0] +
            solved_elbow[1]*solved_elbow[1] +
            solved_elbow[2]*solved_elbow[2]);
        const float lower_length = sqrtf(
            (solved_wrist[0]-solved_elbow[0])*
            (solved_wrist[0]-solved_elbow[0]) +
            (solved_wrist[1]-solved_elbow[1])*
            (solved_wrist[1]-solved_elbow[1]) +
            (solved_wrist[2]-solved_elbow[2])*
            (solved_wrist[2]-solved_elbow[2]));
        check(valid && close(upper_length, 100.0f) &&
              close(lower_length, 100.0f) &&
              close(solved_wrist[0], target[0]) &&
              close(solved_wrist[1], target[1]),
              "arm solve reaches the controller without stretching either bone");

        const float old_upper[3] = {
            elbow[0]-shoulder[0], elbow[1]-shoulder[1], 0.0f
        };
        const float new_upper[3] = {
            solved_elbow[0]-shoulder[0],
            solved_elbow[1]-shoulder[1], 0.0f
        };
        const Mat4 upper_turn = rotation_between(old_upper, new_upper);
        float wrist_after_upper[3] = {
            wrist[0]*upper_turn.m[0][0] + wrist[1]*upper_turn.m[1][0],
            wrist[0]*upper_turn.m[0][1] + wrist[1]*upper_turn.m[1][1],
            wrist[0]*upper_turn.m[0][2] + wrist[1]*upper_turn.m[1][2]
        };
        const float old_lower[3] = {
            wrist_after_upper[0]-solved_elbow[0],
            wrist_after_upper[1]-solved_elbow[1],
            wrist_after_upper[2]-solved_elbow[2]
        };
        const float new_lower[3] = {
            solved_wrist[0]-solved_elbow[0],
            solved_wrist[1]-solved_elbow[1],
            solved_wrist[2]-solved_elbow[2]
        };
        const Mat4 lower_turn = rotation_between(old_lower, new_lower);
        float wrist_after_both[3] = { solved_elbow[0], solved_elbow[1],
                                      solved_elbow[2] };
        for (int j = 0; j < 3; ++j)
            for (int i = 0; i < 3; ++i)
                wrist_after_both[j] += old_lower[i]*lower_turn.m[i][j];
        check(close(wrist_after_both[0], solved_wrist[0], 1e-3f) &&
              close(wrist_after_both[1], solved_wrist[1], 1e-3f) &&
              close(wrist_after_both[2], solved_wrist[2], 1e-3f),
              "upper and lower arm rotations put the wrist at the solved point");

        const float distant[3] = { 1000.0f, 0.0f, 0.0f };
        check(solve_two_bone_arm(shoulder, elbow, wrist, distant,
                                 solved_elbow, solved_wrist) &&
              solved_wrist[0] < 200.0f &&
              solved_wrist[0] > 190.0f,
              "arm solve bounds an unreachable controller to full reach");
    }


    printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "PASSED",
           g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
