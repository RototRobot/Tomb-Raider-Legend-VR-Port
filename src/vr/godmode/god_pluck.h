#pragma once
// Picking Lara up with a pinch and holding her by the feet (ported back from
// the Underworld mod, which took it from Legend's board pick-up). Pure maths
// -- no config(), no game memory, no Windows calls, no clocks -- so mathtest
// links it. The caller feeds one PluckInput per frame and reads the state
// back.
//
// Game space is z-up. A hand grips inside a zone around her feet to pick her
// up; she then hangs from the hand's pinch point as a pendulum, her limbs
// lagging behind on slower pendulums with a little idle sway. The drawn body
// is  translate(-grab) * body_rotation * translate(hang)  (row vectors), the
// arms (limb 0/1, at the shoulders) and legs (2/3, at the hips) turned about
// their joints by limb_rotation[l] first.
//
// Left out on purpose (Legend has it, docs/GOD_HANDS_PLAN.md phase 5): the
// palm platform and hand-to-hand handover. The zone is computed by one
// function (pluck_zone) and the held/not-held split is in one place, so a
// `palm` field can be added later without reshaping the API.

#include "../vr_math.h"

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
        // Arm zones (user 2026-10-19): a capsule along each arm, shoulder to
        // fingertips (PluckInput::arm_*), this radius at her scale plus 1 cm of
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
        // Pendulum, real metres: body tilt angles about world x/y, and the
        // lagging limb tilts (0/1 arms, 2/3 legs).
        float last_hand[3] = {};
        float hand_vel[3] = {};
        float hand_acc[2] = {};
        float tilt[2] = {};
        float tilt_vel[2] = {};
        float limb_tilt[4][2] = {};
        float phase = 0.0f;
        bool wrist_pivot = false;       // hung by a wrist this frame
        float pivot[3] = {};
        int part = kPluckNone;          // what the holding grip holds (kept in flight)
        bool lifted = true;             // false: an arm lead, her body not moved yet
        float arm_offset[3] = {};       // arm grab: her wrist - pinch point at the grab
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
        // Thrown: the hang point flies (game units/s), her feet stay
        // fly_offset from it; fly_prev = her feet last step (the caller
        // probes fly_prev -> target for what she hits).
        bool flying = false;
        float fly_vel[3] = {};
        float fly_offset[3] = {};
        float fly_prev[3] = {};
        float fly_time = 0.0f;
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

    // Ends a flight (the caller has put her down where she landed).
    void pluck_land(PluckState& s);

    // A flight hit something that is not a floor: her feet are moved to
    // `feet` (the caller's last free point) and she bounces off, keeping
    // `keep` of her speed: back horizontally off a wall, or (ceiling) her
    // rise turned into a fall.
    void pluck_bounce(PluckState& s, const float feet[3], float keep, bool ceiling);

    // translate(-grab) * body_rotation * translate(hang): maps the grab point
    // (her root - grab_offset, or the wrist pivot: in.wrist_pivot for a head
    // grab, in.arm_pivot for an arm grab) onto the hang point. Uses the root
    // and pivots of `in` (the current frame).
    Mat4 pluck_body_matrix(const PluckState& s, const PluckInput& in);
}
