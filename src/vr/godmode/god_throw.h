#pragma once
// A thrown Lara as a projectile (user 2026-10-22: throws clipped mid-air,
// flew back toward the player or the wrong way). Shared by the pinch hold's
// flight (god_pluck) and the torso toss (god_torso). Pure maths -- no
// config(), no game memory, no Windows calls, no clocks -- so mathtest links it.
//
// Game space is z-up. The body is a ball of `radius` round her centre of
// mass (what is drawn turning: the caller draws her about ThrowBody::pos, so
// what is seen is what is probed). The caller sweeps prev -> pos (extended
// by the radius) against the level each step, and calls throw_body_bounce
// with the surface normal on a wall / ceiling hit.
//
// Release velocity (VR practice: SteamVR's VelocityEstimator, Unity XRI):
// the hand's per-frame velocities go into a ThrowTracker; the estimate is the
// fastest average of kThrowWindow consecutive frames ending within the last
// `within` seconds, each average dropping one outlier (a tracking jitter
// spike, the backswing frame before the forward swing), among the windows
// moving within 60 degrees of the newest one (so a faster backswing before
// the forward swing is not taken). The object gets
// v_hand + omega x (centre - hand) (throw_release).

namespace trlvr
{
    // Her centre's ball radius, metres at her size: about a third of her
    // 1.75 m (times [camera] god_throw_radius).
    constexpr float kThrowRadius = 0.58f;
    constexpr int kThrowSamples = 24;
    constexpr int kThrowWindow = 5;

    struct ThrowTracker
    {
        int count = 0;                  // samples kept (newest first from head)
        int head = 0;                   // index of the newest
        float vel[kThrowSamples][3] = {};
        float spin[kThrowSamples][3] = {};
        float dt[kThrowSamples] = {};
    };

    void throw_track_reset(ThrowTracker& t);
    // One frame's velocity (real m/s) and angular velocity (rad/s, may be
    // null = zero) over `dt` seconds. The caller leaves out tracking jumps.
    void throw_track_add(ThrowTracker& t, const float vel[3], const float spin[3], float dt);
    // The release estimate (see above). False with no samples.
    bool throw_track_estimate(const ThrowTracker& t, float within, float vel_out[3],
                              float spin_out[3]);

    // v_hand + omega x (centre - hand_pos). Units: vel and positions in any
    // one unit; omega rad/s.
    void throw_release(const float v_hand[3], const float omega[3], const float hand_pos[3],
                       const float centre[3], float out[3]);

    // Caps |v| at max_speed; returns |v| before the cap.
    float throw_cap(float v[3], float max_speed);

    struct ThrowBody
    {
        float pos[3] = {};              // her centre (game units)
        float prev[3] = {};             // ... last step (the caller sweeps prev -> pos)
        float vel[3] = {};              // game units / s
        float spin[3] = {};             // rad/s, world axis
        float radius = 0.0f;            // game units
    };

    void throw_body_start(ThrowBody& b, const float pos[3], const float vel[3],
                          const float spin[3], float radius);
    // One step under gravity (game units / s^2, down -z): the exact
    // parabola over dt. The spin fades by exp(-spin_damping * dt); turn =
    // the rotation vector of this step (spin * dt), may be null.
    void throw_body_step(ThrowBody& b, float gravity, float spin_damping, float dt, float turn[3]);
    // Moved to `at` (the last free point), prev too.
    void throw_body_move(ThrowBody& b, const float at[3]);
    // A hit on a surface of unit normal n (out of the surface): the speed
    // into it reversed times `restitution`, the speed along it kept times
    // (1 - friction). Moving away from the surface already: unchanged,
    // returns false.
    bool throw_body_bounce(ThrowBody& b, const float n[3], float restitution, float friction);

    // Caps |w| at max (rad/s).
    void throw_cap_spin(float w[3], float max);

    // Surface normals. A hit record's normal is used when it is a unit
    // vector facing back along the ray `dir`; else the normal of the plane
    // through three hit points (oriented against `dir`). False if degenerate.
    bool throw_normal_valid(const float n[3], const float dir[3]);
    bool throw_plane_normal(const float a[3], const float b[3], const float c[3],
                            const float dir[3], float out[3]);
    // 2 floor (n.z > 0.6 while descending, vz <= 0), 1 ceiling (n.z < -0.6), 0 wall.
    enum : int { kThrowWall = 0, kThrowCeiling = 1, kThrowFloor = 2 };
    int throw_surface(const float n[3], float vz);
}
