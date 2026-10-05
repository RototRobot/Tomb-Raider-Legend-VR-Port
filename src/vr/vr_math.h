#pragma once
// Matrix maths for building the per-eye projection.
//
// Conventions, because getting these wrong is the whole game:
//
//  * The engine uses row-vector maths: clip = position * M. So M's rows are
//    what a column-vector library would call columns.
//
//  * The four constant registers c0..c3 hold the *columns* of M. The shader
//    does `dp4 oPos.x, v0, c0`, so c0 dotted with the position gives clip.x,
//    which makes c0 column 0. Registers are stored here as 16 floats with
//    regs[i*4 + k] == M[k][i].
//
//  * Positions arriving from the vertex buffer are already in view space
//    (confirmed by disassembly and by 195 runtime samples), so an eye offset
//    is a plain translation in that space and composes with the projection.
//
// Rather than assume the game's near plane, far plane, or the sign of its Y
// scale, we read them back out of the matrix the game itself uploaded and
// build the eye matrix to match. That keeps us right even where we do not yet
// understand why the game chose a value -- its 8/7 aspect ratio, for one.

namespace trlvr
{
    struct Mat4
    {
        float m[4][4];      // m[row][col], row-vector convention

        static Mat4 identity();
    };

    Mat4 operator*(const Mat4& a, const Mat4& b);

    Mat4 translation(float x, float y, float z);

    // Rotation about the vertical axis of the engine's view space, which is
    // where a heading lives. Used to take the yaw back out of the head pose
    // once the game camera is carrying it.
    // Heading of the direction a matrix looks along, radians. One definition
    // used everywhere, because two spellings of it is how the horizon ended
    // up sloping.
    float yaw_of(const Mat4& m);

    // yaw_of for a head that may look steeply down or up. The forward
    // column's horizontal part shrinks to nothing there, so a small head
    // turn looking at the feet swung yaw_of by tens of degrees. This adds
    // the matrix's vertical axis (column 1), whose horizontal part lines up
    // with the facing direction as the forward column dips, weighted by the
    // forward column's vertical part squared: equal to yaw_of for any pure
    // yaw-and-pitch pose, and a head turn moves it at most about 1:1.
    float steady_yaw_of(const Mat4& m);

    // Fold an angle back into -pi..pi. Subtracting a recentring offset from
    // a heading can land outside it, and a camera that swings the long way
    // round is the visible result.
    float wrap_pi(float radians);

    // Inverse of a rotation-plus-translation. Used to rebuild the camera's
    // camera-to-world transform after the world-to-camera one has been
    // rotated, since the engine only recomputes that itself on one of the
    // several paths that reach the transform.
    Mat4 rigid_inverse(const Mat4& m);

    // Apply a heading to a bare vector, matching yaw_rotation exactly.
    void rotate_yaw(float* xyz, float radians);

    Mat4 yaw_rotation(float radians);

    // Controller-local visual hand correction. Angles are degrees around
    // X, then Y, then Z in the engine's row-vector convention.
    Mat4 hand_alignment_rotation(float x_degrees, float y_degrees,
                                 float z_degrees);

    // Place the rendered wrist relative to the controller origin in HMD
    // axes. The offset follows the headset, not controller rotation.
    Mat4 hand_pose_with_head_offset(const Mat4& controller_to_head,
                                    float x, float y, float z);

    // Rigid row-vector rotation taking one direction to another. Used by the
    // first-person arm solver; returns identity for a degenerate direction.
    Mat4 rotation_between(const float* from3, const float* to3);

    // Keep the wrist within the two animated arm lengths, and preserve the
    // original elbow's bend side. Returns false for a degenerate arm.
    bool solve_two_bone_arm(const float* shoulder3, const float* elbow3,
                            const float* wrist3, const float* target3,
                            float* solved_elbow3, float* solved_wrist3);

    // The shortest rigid rotation from identity to `rotation`, scaled by a
    // fraction. Used by the gameplay HUD to retain a small, comfortable part
    // of the current head angle without temporal smoothing or drift.
    Mat4 rotation_fraction(const Mat4& rotation, float fraction);

    // Strip a heading out of a view matrix, keeping pitch and roll. Used
    // when the game camera has been given the yaw and the projection must
    // not apply it a second time.
    Mat4 remove_yaw(const Mat4& view, float yaw);

    // Give a view matrix a chosen heading, keeping pitch and roll. The camera
    // and the projection split the head rotation between them, and the split
    // moves with how often the engine rebuilds its camera -- so what the
    // projection owes is a residual, not a constant.
    Mat4 set_yaw(const Mat4& view, float wanted);

    // What the game told the GPU, recovered from its own constant registers.
    struct GameProjection
    {
        float sx, sy;       // x and y scale
        float ox, oy;       // off-centre offsets (zero for a symmetric frustum)
        float q;            // far / (far - near)
        float near_z;
        float far_z;
        bool  valid;        // false if the registers were not a projection
    };

    // Read a projection back out of c0..c3. Returns valid=false if the shape
    // is not a projection, which is how we avoid overwriting something else
    // that happens to live in those registers.
    GameProjection projection_from_registers(const float* regs16);

    // Off-centre projection from tangent half-angles, as OpenVR reports them.
    // `flip_y` negates the Y row to match a game that renders upside down --
    // this one does, its Y scale is negative.
    Mat4 projection_from_tangents(float tan_left, float tan_right,
                                  float tan_top, float tan_bottom,
                                  float near_z, float far_z,
                                  bool flip_y);

    // Rebuild the game's own projection from what we recovered. Used to prove
    // the conventions are right: it must reproduce the original registers.
    Mat4 projection_from_game(const GameProjection& gp);

    // Recover the depth terms of the projection folded into a world-view-
    // projection matrix.  The camera can change near/far without a matching
    // pure-projection upload; using stale depth terms to remove P shears the
    // recovered world transform.  X may contain any affine model transform:
    // columns 2 and 3 of X*P still carry q and q*near exactly.
    bool projection_depth_from_wvp(const Mat4& wvp,
                                   const GameProjection& shape,
                                   GameProjection* out);

    // Treat scale * source NDC as a point in a reference frustum, then move it
    // to the NDC position for the same ray in a target frustum. This is the
    // clip-space equivalent of drawing a mono surface through each headset
    // lens: the two render images differ, but the rays seen by the eyes agree.
    // W and depth are left exactly as the source produced them.
    Mat4 reproject_ndc_to_frustum(const Mat4& source,
                                  const Mat4& reference_projection,
                                  const Mat4& target_projection,
                                  float scale);

    // As above, but reinterpret the mono UI rays in a fixed reference view,
    // rotate that view by the headset, then project it through the live eye.
    // Identity view produces the ordinary head-locked correction exactly.
    Mat4 reproject_ndc_to_world_locked(const Mat4& source,
                                       const Mat4& reference_projection,
                                       const Mat4& target_projection,
                                       const Mat4& head_view_rotation,
                                       float scale);

    // An OpenVR pose (3x4 row-major, right-handed, metres) turned into a view
    // matrix in the engine's space. Three conversions at once -- inverse,
    // handedness, units -- and each is easy to get subtly wrong, so this is
    // kept separate from the OpenVR types purely so it can be tested.
    Mat4 view_from_pose(const float* pose34, float world_scale);

    // General 4x4 inverse. Returns false if the matrix is singular.
    //
    // Needed because not all geometry reaches the GPU in view space: world
    // geometry gets a full world-view-projection in c0..c3. Knowing the game's
    // own projection P, the world-view part is recovered as X = M * inverse(P),
    // which can then be re-composed with a different projection.
    bool invert(const Mat4& m, Mat4* out);

    // True if these registers hold a perspective transform whose w is an affine
    // function of position -- either a pure projection or something multiplied
    // by one. False for a 2D or orthographic pass, where w is constant.
    bool is_perspective_registers(const float* regs16);

    // Write a matrix back out as the four constant registers.
    void registers_from_matrix(const Mat4& m, float* out_regs16);
}
