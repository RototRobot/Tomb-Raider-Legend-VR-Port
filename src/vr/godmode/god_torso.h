#pragma once
// Grabbing Lara by the torso with a fist and holding her as a doll (user
// 2026-10-20). Pure maths -- no config(), no game memory, no Windows calls,
// no clocks -- so mathtest links it. The caller feeds one TorsoInput per
// frame and reads the state back; it mirrors god_pluck so the flight can
// reuse the pluck's collision code.
//
// Game space is z-up. Matrices are vr_math's Mat4: row vectors (p' = p * M),
// rows 0..2 the axes, row 3 the position; A * B applies A first.
//
// Grab: a FIST whose palm faces her torso (the palm normal within
// facing_dot of the direction from the palm centre to the nearest point of
// her torso's axis), the palm centre inside the torso capsule (pelvis to
// chest, torso_radius plus hand_margin, both at her size),
// while she looks at most max_height tall to the player. A hand on one of
// her soft parts (part_held) or a hand / Lara taken by another grab
// (other_grab: the pluck) does not grab. The fist closing is the press;
// the zone counts for grace_seconds after the hand was last aimed, since
// closing the fist moves the palm.
//
// Hold: she follows the hand rigidly. At the grab her reference frame
// (TorsoInput::body, e.g. her bone 0 world matrix) is stored relative to the
// hand pose: rel = body * inverse(hand). Each frame drawn = rel * hand (her
// reference as drawn); for settle_seconds the drawn frame stays put and rel
// re-anchors under the closing fingers. torso_body_matrix =
// inverse(body) * drawn takes every bone of her real pose to where she is
// drawn.
//
// Limbs (0 her left arm, 1 her right arm, 2 left leg, 3 right leg, 4 head):
// each is a 3D damped pendulum about its joint (shoulder, hip, neck),
// pointing from the joint along limb_dir, its rest direction her own pose
// (joint -> end) carried by the body. Held: pulled toward world down by real
// gravity (9.81 m/s^2 in player metres, at a pendulum length that is a
// fraction of her apparent height) and by the joint's acceleration (the
// hand's motion and rotation at that joint, by differencing the joint's
// drawn position), with a weak tone toward her pose. Flying (free fall) only
// tone, momentum and a little air drag that trails them behind. Each stays
// in a cone round its rest direction (wider forward / outward, narrow
// backward / inward: a hip bends forward ~80 deg, a shoulder can go
// overhead). limb_rotation[l] is the turn from her real pose to that
// direction, in HER REAL-POSE FRAME (world axes, before the body matrix),
// to be applied about the joint's REAL position:
//   bone' = bone * translate(-joint) * limb_rotation * translate(joint)
// then every bone by torso_body_matrix (the pluck's order). The knees bend
// first: knee_rotation[0/1] (legs 2/3) about each knee's real position,
// same frame. The head turns bones after the neck about the neck.
//
// Throw: let go while her centre moves faster than throw_min_speed (real
// m/s, peak of the last 0.15 s) and she flies as a rigid body: velocity *
// throw_scale, gravity throw_gravity (real m/s^2 times `table`), the hand's
// angular velocity * spin_scale capped at spin_max, slowed by spin_damping.
// A slower release is a put-down (TorsoEvents::put_down at `feet`).
// Flight: each step feet_prev -> feet (her root under the drawn pose) is
// the segment to probe; torso_land ends it, torso_bounce reflects it.

#include "../vr_math.h"
#include "god_throw.h"

namespace trlvr
{
    constexpr int kTorsoLimbs = 5;
    enum : int { kTorsoArm0 = 0, kTorsoArm1 = 1, kTorsoLeg0 = 2, kTorsoLeg1 = 3, kTorsoHead = 4 };

    struct TorsoParams
    {
        float metre = 1.0f;             // game units per real metre at her size
        float table = 1.0f;             // game units per player metre (metre * board factor)
        bool enabled = true;            // false: behaves like torso_reset
        // Grab.
        float max_height = 0.6f;        // player metres: no grab while she looks taller
        float torso_radius = 0.16f;     // metres at her size
        float hand_margin = 0.04f;      // metres at her size added to the radius
        // The zone slid down her spine (pelvis -> chest axis) toward her
        // hips by this many metres at her size; the hold is unchanged.
        float zone_lower = 0.0f;
        float facing_dot = 0.5f;        // palm normal . (palm -> her axis) at least this
        float grace_seconds = 0.25f;    // the fist may close this long after aiming
        float settle_seconds = 0.15f;   // re-anchoring after the grab
        float follow_seconds = 0.0f;    // light smoothing of the hold (0 = rigid, exact)
        // Limbs (held). Tone is a fraction of the pendulum's g/L: small, so
        // gravity wins (0.08: an arm held overhead by gravity rests ~14 deg
        // from straight down). Damping is a ratio of critical.
        float limb_tone = 0.08f;        // legs
        // Arms (user 2026-10-20: 0.08 looked lifeless, no muscle tension or
        // friction): enough tone that they keep some shape against gravity
        // (held out sideways they sag ~50 deg, not hang dead), and more
        // damping, so a swing settles instead of ringing.
        float arm_tone = 0.35f;
        float arm_damping = 0.65f;
        // The head barely moves (user 2026-10-20: much less head ragdoll):
        // above 1 it stands on the neck when she is upright (an upturned
        // pendulum needs tone > g/L); 8 lets it sag only ~6 deg when she
        // lies flat (2 sagged ~25 deg).
        float head_tone = 8.0f;
        float limb_damping = 0.35f;     // legs and head
        float knee_bend = 0.21f;        // radians, relaxed; more as the thigh swings forward
        // Cones, radians from the rest direction: forward / backward (her
        // front), outward / inward (away from / across her body).
        float arm_forward = 2.97f, arm_back = 0.87f, arm_out = 2.97f, arm_in = 0.52f;  // 170/50/170/30 deg
        float leg_forward = 1.40f, leg_back = 0.35f, leg_out = 0.79f, leg_in = 0.26f;  // 80/20/45/15 deg
        float head_forward = 0.35f, head_back = 0.26f, head_side = 0.26f;              // 20/15/15 deg (was 45/35/35)
        // Throw.
        bool throw_enabled = true;
        float throw_min_speed = 0.6f;   // real m/s
        float throw_scale = 1.0f;
        float throw_gravity = 9.81f;    // real m/s^2
        float throw_max_speed = 6.0f;   // cap (and samples over 4x are tracking jumps)
        float throw_max_time = 3.0f;    // seconds; then events.flight_over
        float spin_scale = 0.25f;       // of the hand's angular velocity
        float spin_max = 2.0f;          // rad/s
        float spin_damping = 0.5f;      // 1/s
        // Release velocity ([camera] god_throw_velocity): 1 = the hand's
        // fastest 5-frame average of the last 0.15 s (one outlier dropped),
        // v + omega x (centre - hand) (god_throw); 0 = the old peak of her
        // centre's smoothed velocity. throw_radius scales kThrowRadius.
        int throw_velocity = 1;
        float throw_radius = 1.0f;
        // Limbs in flight: tone (fraction of g/L), damping ratio, and the
        // air drag that trails them behind the direction of travel
        // (m/s^2 per m/s).
        float flight_tone = 0.25f;
        float flight_damping = 0.15f;
        float trail = 0.6f;
    };

    struct TorsoHand
    {
        bool valid = false;
        bool fist = false;              // grabbing with a fist (HandGesture::Fist, with hysteresis)
        // The hand's frame in game space: any rigid frame fixed to the hand
        // (the grip pose or the hand model's model_to_world; a uniform scale
        // is divided out). Only its motion matters: she keeps her pose
        // relative to it, and its turn is her spin on a throw.
        Mat4 pose = Mat4::identity();
        float palm_centre[3] = {};      // HandPoints::palm_centre
        float palm_normal[3] = {};      // HandPoints::palm_normal (unit, out of the palm)
        bool part_held = false;         // the hand is on / holds a soft part (chest, rear)
        bool other_grab = false;        // the hand, or she, is taken by another grab (pluck)
    };

    struct TorsoInput
    {
        float dt = 0.0f;                // seconds; outside (0, 0.1] no physics this frame
        TorsoHand hand[2];
        // Her REAL pose (game space), not as drawn.
        bool body_valid = false;
        Mat4 body = Mat4::identity();   // her reference frame (bone 0 or pelvis world matrix), rigid
        float root[3] = {};             // her feet (bone 0's position)
        float torso_a[3] = {};          // torso axis: pelvis
        float torso_b[3] = {};          // torso axis: chest / neck base
        float forward[3] = {};          // the way she faces, unit
        float up[3] = {};               // her up, unit
        float apparent_height = 0.0f;   // player metres (1.75 / player scale); <= 0: no grab
        // Limbs (kTorsoArm0..kTorsoHead): the joint (shoulder, hip, neck)
        // and a point down the limb (wrist / ankle / head top) in her real pose.
        bool limb_valid[kTorsoLimbs] = {};
        float limb_joint[kTorsoLimbs][3] = {};
        float limb_end[kTorsoLimbs][3] = {};
    };

    struct TorsoEvents
    {
        bool aim[2] = {};               // a fist now would grab (zone + facing + size + not blocked)
        bool hover_enter[2] = {};       // aim just became true
        bool wants[2] = {};             // this hand's fist closed on her this frame (grab or not)
        bool grabbed = false;
        bool released = false;
        bool thrown = false;            // released into a flight
        bool put_down = false;          // released slowly: put her down at `feet`
        int hand = -1;                  // the hand of grabbed / released
        float throw_speed = 0.0f;       // thrown: real m/s (after the cap)
        float feet[3] = {};             // released: her feet as drawn
        bool flight_over = false;       // flying for throw_max_time: put her down at `feet`
    };

    struct TorsoState
    {
        int hand = -1;                  // holding hand, -1 none
        bool was_fist[2] = {};
        float aim_age[2] = { 1e9f, 1e9f };  // seconds since the hand was last aimed
        bool aimed[2] = {};
        float settle = 0.0f;
        Mat4 rel = Mat4::identity();    // drawn = rel * hand
        Mat4 drawn = Mat4::identity();  // her reference frame as drawn (game space)
        Mat4 body_matrix = Mat4::identity();  // last step: inverse(in.body) * drawn
        float centre_local[3] = {};     // her torso centre in her reference frame
        float centre[3] = {};           // ... as drawn (game space)
        // Velocity of her drawn centre (real m/s): smoothed, and its peak
        // over the last 0.15 s; the hand's angular velocity (rad/s, world)
        // smoothed, and its value at that peak.
        bool track_ok = false;
        float last_centre[3] = {};
        float vel_smooth[3] = {};
        float peak_vel[3] = {};
        float peak_age = 0.0f;
        Mat4 last_hand = Mat4::identity();
        float spin_smooth[3] = {};
        float peak_spin[3] = {};
        // Per-frame hand origin velocity (real m/s) and spin (throw_velocity 1).
        ThrowTracker track;
        // Flight (2026-10-22): her drawn centre is the projectile `fly`
        // (god_throw; vel game units / s, spin rad/s world axis); the caller
        // sweeps fly.prev -> fly.pos with fly.radius.
        bool flying = false;
        ThrowBody fly;
        float fly_time = 0.0f;
        float feet[3] = {};             // her feet as drawn (held or flying)
        float feet_prev[3] = {};        // flying: last step's feet (probe feet_prev -> feet)
        float table = 0.0f;             // TorsoParams::table, last step
        float height = 0.0f;            // her apparent height (player metres), last step
        // Limb pendulums: world direction (unit), angular velocity (rad/s),
        // and their joints' drawn motion (game units; acc in real m/s^2).
        bool limbs_ok = false;
        bool joint_ok[kTorsoLimbs] = {};
        float limb_dir[kTorsoLimbs][3] = {};
        float limb_vel[kTorsoLimbs][3] = {};
        float joint_prev[kTorsoLimbs][3] = {};
        float joint_vel[kTorsoLimbs][3] = {};
        float joint_acc[kTorsoLimbs][3] = {};
        Mat4 limb_rotation[kTorsoLimbs] = { Mat4::identity(), Mat4::identity(), Mat4::identity(),
                                            Mat4::identity(), Mat4::identity() };
        Mat4 knee_rotation[2] = { Mat4::identity(), Mat4::identity() };  // legs 2/3
    };

    // Clears everything (leaving god mode): nothing held or flying, fists forgotten.
    void torso_reset(TorsoState& s);

    // The grab zone, for drawing and aiming: the capsule a..b (her pelvis to
    // chest) with this radius (game units). False without her body.
    bool torso_zone(const TorsoParams& p, const TorsoInput& in, float a[3], float b[3],
                    float* radius);

    // Stateless: hand h is placed to grab her (zone, palm facing, size, not
    // blocked by part_held / other_grab); the fist itself is not looked at.
    bool torso_aim(const TorsoParams& p, const TorsoInput& in, int h);

    // One frame. `events` may be null.
    void torso_step(TorsoState& s, const TorsoParams& p, const TorsoInput& in,
                    TorsoEvents* events);

    // She is drawn away from where she really is: held or flying.
    bool torso_drawn(const TorsoState& s);

    // inverse(in.body) * drawn: her real pose -> where she is drawn (apply to
    // every bone after the limb and knee rotations). Identity when not drawn.
    Mat4 torso_body_matrix(const TorsoState& s, const TorsoInput& in);

    // Limb l's turn (pure rotation, her real-pose frame) and the same
    // turned about `joint` (its real position): translate(-joint) * R *
    // translate(joint). Identity for a bad index or when not drawn.
    Mat4 torso_limb_rotation(const TorsoState& s, int limb);
    Mat4 torso_limb_turn(const TorsoState& s, int limb, const float joint[3]);

    // A free hand touching a drawn (held or flying) limb: `point` (game
    // space) moving at `velocity` (game units / s) knocks any limb whose drawn
    // segment (its joint along limb_dir, the length it has in `in`) passes
    // within `radius` (game units): the limb's swing gains dir x v / L, v the
    // velocity in real m/s (capped), eased in over ~1/8 s (so a hand at rest
    // pushes nothing) and times `strength`. True when a limb was touched.
    bool torso_nudge(TorsoState& s, const TorsoParams& p, const TorsoInput& in,
                     const float point[3], const float velocity[3], float radius,
                     float strength, float dt);

    // Ends a flight (the caller has put her down where she landed).
    void torso_land(TorsoState& s);

    // A flight hit something that is not a floor: her centre is moved to
    // `centre` (the caller's last free point) and she bounces off the
    // surface of unit normal `normal` (throw_body_bounce). Her spin turns
    // back at half rate; the limbs fly on.
    void torso_bounce(TorsoState& s, const float centre[3], const float normal[3],
                      float restitution, float friction);
    // Her flying centre (and the drawn pose) moved to `centre`.
    void torso_fly_move(TorsoState& s, const float centre[3]);
}
