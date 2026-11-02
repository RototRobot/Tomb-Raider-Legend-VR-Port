#pragma once
// Picking Lara up with a pinch and holding her by the feet (ported back from the
// Underworld mod, which took it from Legend's board pick-up). Pure
// maths -- no config(), no game memory, no Windows calls, no clocks -- so
// mathtest links it. The caller feeds one PluckInput per frame and reads the
// state back.
//
// Game space is z-up. A hand grips inside a zone around her feet to pick her
// up; she then hangs from the hand's pinch point as a pendulum, each limb on
// its own damped pendulum about its joint (ragdoll, 2026-10-20: gravity and
// the joint's acceleration while held, the throw's kick and her spin in
// flight) with a little idle sway, the knees a little bent. The drawn body
// is  translate(-grab) * body_rotation * translate(hang)  (row vectors), the
// arms (limb 0/1, at the shoulders) and legs (2/3, at the hips) turned about
// their joints by limb_rotation[l] first.
//
// Left out on purpose: the palm platform and hand-to-hand handover. Legend's
// own palm logic lives in camera_head.cpp; the zone is computed by one function
// (pluck_zone) and the held/not-held split is in one place, so a `palm` field
// can be added later without reshaping the API.

#include "../vr_math.h"
#include "god_throw.h"

namespace trlvr
{
    struct PluckParams
    {
        float metre = 1.0f;     // game units per real metre
        float table = 1.0f;     // game units per table metre (metre * board factor)
        bool enabled = true;    // false: behaves like pluck_reset
        // Underworld (user 2026-10-16): she is picked up only by her head
        // (or near it): the zone is a ball round PluckInput::head instead of
        // Legend's column round her whole body, so a grip on her chest holds
        // the chest, and grips elsewhere stay the game's.
        bool head_only = false;
        float head_radius = 0.10f;      // metres at her scale ([camera] god_head_zone)
        // Arm zones (user 2026-10-19; her hand only since 2026-10-20): a capsule
        // over each hand, wrist to fingertips (PluckInput::arm_*; the arm
        // grab eases her hand into the pinch), this radius at her scale plus 1 cm of
        // the player's hand; 0 = no arm grabs ([camera] god_arm_zone). The
        // zone the grip is deepest in (distance / radius) wins.
        float arm_radius = 0.0f;
        // An arm grab first moves only her arm (the caller bends it toward
        // PluckState::lead_target) and lifts her once the hand pulls past the
        // arm's reach; off, she is lifted at once ([camera] god_arm_lead).
        bool arm_lead = true;
        // Throwing ([camera] god_throw*): let go while the hand moves faster
        // than throw_min_speed (real m/s, the peak of the last 0.15 s) and she
        // flies from the hand at that velocity times throw_scale, under
        // throw_gravity (real m/s^2: she is a doll on the player's table).
        // After a grab, the pinch point still moves for a moment (the fingers
        // finish closing; the held hand takes the pinch pose): for this long
        // the hold re-anchors to it, so she does not follow that motion.
        float settle_seconds = 0.0f;
        bool throw_enabled = false;
        float throw_min_speed = 0.6f;
        float throw_scale = 1.0f;
        float throw_gravity = 9.81f;
        // A throw is no faster than this (real m/s, before throw_scale);
        // hand samples over 4x it are tracking jumps and ignored.
        float throw_max_speed = 6.0f;
        float throw_max_time = 3.0f;    // seconds; then events.flight_over
        // Release velocity ([camera] god_throw_velocity): 1 = the fastest
        // 5-frame average of the hand's last 0.15 s, one outlier dropped
        // (god_throw's ThrowTracker); 0 = the old peak of the smoothed
        // velocity. throw_radius: her centre's ball (kThrowRadius at her
        // size) times this ([camera] god_throw_radius).
        int throw_velocity = 1;
        float throw_radius = 1.0f;
        // Ragdoll limbs (user 2026-10-20): each limb is its own damped
        // pendulum about its shoulder / hip, driven by gravity and the
        // acceleration of its joint (the hand's plus the body's swing), with
        // a little muscle tone toward her pose. `ragdoll` scales how far
        // they swing from the body (0 = they turn with the body, 1 = the
        // default, up to 2; [camera] god_ragdoll). In a throw gravity does
        // not swing them (free fall): the throw's kick and the body's spin
        // do. knee_bend: radians of relaxed knee bend while drawn, more as a
        // thigh swings forward (the shin hangs toward gravity; [camera]
        // god_knee_bend in degrees). leg_spread: radians each leg hangs out
        // to her side. throw_spin: the tumble a throw starts, rad/s per
        // real m/s of the throw across ([camera] god_throw_spin; 0 = she
        // only keeps the swing she had).
        float ragdoll = 1.0f;
        float knee_bend = 0.21f;
        float leg_spread = 0.06f;
        float throw_spin = 1.0f;
        // Two hands (user 2026-10-20, [camera] god_two_hands): while she
        // hangs by one arm, the other hand may take her other arm (its zone
        // is that arm as drawn, PluckInput::drawn_*). She then hangs below
        // both hands: her shoulder midpoint under the point between the two
        // wrists, as high as the higher hand's arm needs (an arm reaches no
        // further than PluckInput::arm_reach), her body yawed so her
        // shoulder line follows the hand-to-hand direction. The change to
        // and from that hang is blended over two_blend_seconds; letting go
        // of one hand leaves her on the other, letting go of both throws
        // with the faster hand. Off: as before (one hand).
        bool two_hands = false;
        float two_blend_seconds = 0.25f;
    };

    // Which part of her a grip holds (PluckState::part).
    enum : int { kPluckNone = -1, kPluckHead = 0, kPluckArm0 = 1, kPluckArm1 = 2 };
    constexpr float kPluckGraceSeconds = 0.25f;

    struct PluckInput
    {
        float dt = 0.0f;                // seconds; outside (0, 0.1] no physics this frame
        bool grip[2] = {};              // hand grip pressed (0 left, 1 right)
        bool point_valid[2] = {};
        float point[2][3] = {};         // each hand's pinch point (or index tip), game space
        float root[3] = {};             // Lara's real feet
        bool head_valid = false;        // head_only: her head (between head base and top)
        float head[3] = {};
        bool part_held[2] = {};         // that hand holds one of her soft parts: no pick-up
        bool wrist_pivot_valid = false; // a dangle animation hangs her by a wrist
        float wrist_pivot[3] = {};      // that wrist's position
        bool floor_valid = false;       // probe straight down under the held target
        float floor[3] = {};
        // Her arms (0/1, the caller's arm chains): the grab capsule from the
        // shoulder to the fingertips, the real wrist, the arm's reach
        // (shoulder to wrist, stretched), and the wrist of that arm raised
        // straight up (the hang pivot of an arm grab).
        bool arm_valid[2] = {};
        float arm_shoulder[2][3] = {};
        float arm_end[2][3] = {};
        float arm_wrist[2][3] = {};
        float arm_reach[2] = {};
        float arm_pivot[2][3] = {};
        // Her facing and her right, horizontal unit vectors (x, y) in game
        // space: the knees bend backward, the legs spread to her sides
        // (limbs 0/2 are her left, 1/3 her right). Zero: no bend, no spread.
        float facing[2] = {};
        float right[2] = {};
        // Two hands: her arms (chains 0/1) as they were drawn last frame,
        // game space (the shoulder, the fingertips, the wrist); not valid
        // while she is not drawn. arm_limb: the limb (0 her left arm, 1 her
        // right) of each chain, so a held arm's own pendulum can rest.
        bool drawn_arm_valid[2] = {};
        float drawn_shoulder[2][3] = {};
        float drawn_end[2][3] = {};
        float drawn_wrist[2][3] = {};
        int arm_limb[2] = { 0, 1 };
    };

    struct PluckEvents
    {
        bool picked_up = false;
        bool released = false;
        int hand = -1;                  // the hand of either event
        bool dropped_to_floor = false;  // released with a floor below
        float target[3] = {};           // where her feet were held (release)
        bool hover_enter[2] = {};       // a free hand just entered the zone
        int part = kPluckNone;          // picked_up: what the grip holds
        bool lifted = false;            // an arm lead just lifted her off her feet
        bool moved = true;              // released: false = she was never lifted (arm lead)
        bool thrown = false;            // released into a flight (PluckState::flying)
        float throw_speed = 0.0f;       // thrown: the hand's speed, real m/s
        bool flight_over = false;       // flying for throw_max_time: put her down
        // Two hands: second_grab = `hand` took her other arm (`part`) while
        // the first held her; second_release = `hand` let go of one of the
        // two, she hangs on by the other.
        bool second_grab = false;
        bool second_release = false;
    };

    struct PluckState
    {
        int hand = -1;                  // hand holding Lara, -1 none
        bool was[2] = {};               // grip last frame
        bool inside[2] = {};            // hand in the zone last frame
        // Seconds since each hand was last in a zone, and which: a grip
        // closing pulls the pinch point (thumb and index tips) back into the
        // palm, out of a small zone, before it reads as a grab, so a press
        // within kPluckGraceSeconds still takes that zone.
        float inside_age[2] = { 1e9f, 1e9f };
        int last_zone[2] = { kPluckNone, kPluckNone };
        bool hover = false;             // a free hand is in the zone
        float root[3] = {};             // her real feet, last step
        float grab_offset[3] = {};      // root - pinch point at the grab
        float carry[3] = {};            // drawn offset while held (target - root)
        float target[3] = {};           // where her feet are held
        float hang[3] = {};             // pinch point of the holding hand
        bool floor_valid = false;
        float floor[3] = {};
        // Pendulum, real metres: body tilt angles (radians toward world +x /
        // +y that her feet swing; a tilt vector (tx, ty) is a turn of
        // |tilt| about the horizontal axis (ty, -tx)), held within +-1.2,
        // free while flying (she may tumble). limb_swing / limb_swing_vel:
        // each limb's own pendulum, its swing from the body in the same
        // tilt terms (0/1 arms, 2/3 legs); limb_tilt = where each limb is
        // drawn, tilt plus its swing (world, while held).
        float last_hand[3] = {};
        float hand_vel[3] = {};
        float hand_acc[2] = {};
        float tilt[2] = {};
        float tilt_vel[2] = {};
        float limb_tilt[4][2] = {};
        float limb_swing[4][2] = {};
        float limb_swing_vel[4][2] = {};
        float phase = 0.0f;
        // Thrown: her centre (half way from the grab to her feet) is drawn
        // spin_offset from the hang point, and she turns about it.
        float spin_offset[3] = {};
        float table = 0.0f;             // PluckParams::table, last step (pluck_bounce)
        Mat4 knee_rotation[2] = { Mat4::identity(), Mat4::identity() };  // legs 2/3: shin about the knee
        bool wrist_pivot = false;       // hung by a wrist this frame
        float pivot[3] = {};
        int part = kPluckNone;          // what the holding grip holds (kept in flight)
        bool lifted = true;             // false: an arm lead, her body not moved yet
        float arm_offset[3] = {};       // arm grab: her wrist - pinch point at the grab
        // Arm grab, once settled: arm_offset eases to this (her wrist - the
        // middle of her hand), so her hand sits in the pinch instead of
        // floating where it was gripped (user 2026-10-20). 2: the second hand.
        float arm_snap[3] = {};
        float arm_snap2[3] = {};
        float lead_target[3] = {};      // arm lead: where her wrist is drawn
        float lead_lift = 0.0f;         // arm lead: shoulder-to-wrist distance that lifts her
        float settle = 0.0f;            // seconds of re-anchoring left (PluckParams::settle_seconds)
        float anchor[3] = {};           // what the hold keeps still while settling: her
                                        // root (head grab) or wrist (arm grab) at the grab
        // The hand's velocity (real m/s): smoothed, and its peak over the
        // last 0.15 s (what a throw takes).
        float vel_smooth[3] = {};
        float peak_vel[3] = {};
        float peak_age = 0.0f;
        ThrowTracker track;             // per-frame hand velocities (throw_velocity 1)
        ThrowTracker track2;            // ... of the second hand
        // Thrown (2026-10-22): her drawn centre (hang + spin_offset) is a
        // projectile, `fly` (god_throw); the caller sweeps fly.prev -> fly.pos
        // with fly.radius. hang follows it; target = her feet as drawn
        // (turned with the tumble), fly_prev = target last step.
        bool flying = false;
        ThrowBody fly;
        float fly_prev[3] = {};
        float fly_time = 0.0f;
        // Two hands (PluckParams::two_hands): the second hand and the arm it
        // holds (part2 is kept in a flight, as part is), its wrist - pinch
        // offset, settle and anchor as for the first, and its velocity
        // (real m/s) for a throw. The first hand is always `hand`: when it
        // lets go first the second takes its place.
        int hand2 = -1;
        int part2 = kPluckNone;
        float arm_offset2[3] = {};
        float anchor2[3] = {};
        float settle2 = 0.0f;
        float lead_lift2 = 0.0f;        // arm lead (taken while she stands): lead_lift of the second arm
        float last_hand2[3] = {};
        float hand_vel2[3] = {};
        float vel_smooth2[3] = {};
        float peak_vel2[3] = {};
        float peak_age2 = 0.0f;
        // 0 one hand .. 1 two hands, moving linearly over two_blend_seconds
        // (the hang uses it eased). two_rel: the two-hand hang point (her
        // shoulder midpoint) less the first hand's wrist target, frozen
        // while the blend runs back after a release.
        float two_t = 0.0f;
        float two_rel[3] = {};
        // Her body's turn about the vertical (radians, eased toward the
        // hand-to-hand direction while two hands hold her, then kept);
        // body_rotation = yaw then tilt.
        float yaw = 0.0f;
        // Where each held arm's wrist is drawn (game space, by arm chain),
        // and the chain just let go of while the blend runs back (-1 none).
        float wrist_target[2][3] = {};
        int release_chain = -1;
        Mat4 body_rotation = Mat4::identity();
        Mat4 limb_rotation[4] = { Mat4::identity(), Mat4::identity(),
                                  Mat4::identity(), Mat4::identity() };
    };

    // Drops a held Lara where she was (leaving board gameplay) and clears
    // everything, the grips seen so far included.
    void pluck_reset(PluckState& s);

    // Pinch zone around her feet: horizontal radius, and the z range.
    // (head_only: the head ball's radius is pluck_head_radius.)
    void pluck_zone(const PluckParams& p, const float root[3],
                    float* radius, float* bottom, float* top);

    // Head ball: head_radius at her scale plus 1 cm of the player's own
    // hand. (A grip on a soft part still wins: PluckInput::part_held.)
    float pluck_head_radius(const PluckParams& p);

    // Near her (the god hands' input claim, user 2026-10-16: a larger zone
    // than touching): within `reach` metres at her scale, plus 3 cm of the
    // player's hand, of the segment from her feet to the top of her head.
    bool pluck_near(const PluckParams& p, const float feet[3], const float head_top[3],
                    const float point[3], float reach);

    // Distance from p to the segment a-b.
    float pluck_segment_distance(const float a[3], const float b[3], const float p[3]);

    // One frame. `events` may be null.
    void pluck_step(PluckState& s, const PluckParams& p, const PluckInput& in,
                    PluckEvents* events);

    // She is drawn away from where she really is: held and lifted, or flying.
    bool pluck_drawn(const PluckState& s);

    // A free hand pushing her while she is drawn (held or flying): the hand's
    // velocity (game units / s) swings the held body a little further along
    // it (a fraction of the pendulum's kick) and kicks the limbs' swing, eased
    // in over ~1/8 s so a hand at rest pushes nothing; times `strength`.
    // False when nothing is drawn or the velocity is negligible.
    bool pluck_nudge(PluckState& s, const float velocity[3], float strength, float dt);

    // Ends a flight (the caller has put her down where she landed).
    void pluck_land(PluckState& s);

    // A flight hit something that is not a floor: her centre is moved to
    // `centre` (the caller's last free point) and she bounces off the
    // surface of unit normal `normal` (throw_body_bounce: restitution of
    // the speed into it, the speed along it times 1 - friction).
    void pluck_bounce(PluckState& s, const float centre[3], const float normal[3],
                      float restitution, float friction);
    // Her flying centre moved to `centre` (a release from behind a wall:
    // the caller's last free point); the path is unchanged otherwise.
    void pluck_fly_move(PluckState& s, const float centre[3]);

    // translate(-grab) * body_rotation * translate(hang): maps the grab point
    // (her root - grab_offset, or the wrist pivot: in.wrist_pivot for a head
    // grab, in.arm_pivot for an arm grab) onto the hang point. Uses the root
    // and pivots of `in` (the current frame). Flying: she turns about her
    // centre instead, translate(-centre) * body_rotation *
    // translate(hang + spin_offset) (the same matrix on the frame of the throw).
    //
    // The drawn pose, in her own (unrotated) frame before this matrix: each
    // shin turned by knee_rotation about its knee, then each limb by
    // limb_rotation about its shoulder / hip.
    Mat4 pluck_body_matrix(const PluckState& s, const PluckInput& in);

    // Her drawn centre (half way from the grab point to her feet, through
    // pluck_body_matrix; flying: fly.pos, the same point).
    void pluck_centre(const PluckState& s, const PluckInput& in, float out[3]);

    // Two hands: the blend toward the two-hand hang (0..1, eased), and
    // whether her arm chain c is drawn reaching to wrist_target[c] (held by
    // either hand of a two-hand hold, or the first hand's arm while the
    // blend runs). The hang's pivot (the matrix's grab point) blends from
    // the raised wrist to her shoulder midpoint by the weight.
    float pluck_two_weight(const PluckState& s);
    bool pluck_arm_reaches(const PluckState& s, int chain);
}
