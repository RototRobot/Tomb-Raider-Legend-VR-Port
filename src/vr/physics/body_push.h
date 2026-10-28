#pragma once
// General push response of Lara's body to the god hands: not a
// wobble, just the give a body has when a hand leans on it. Three segments --
// head, chest, waist -- each tilt a few degrees AWAY from a push, on a damped
// spring near critical damping (no visible oscillation), and come back when
// the hand leaves. A hand arriving fast adds a short kick on top of the
// sustained press. Pure maths -- no config(), no game memory, no Windows
// calls -- so mathtest links it.
//
// The tilt is two numbers per segment: how far its top leans along each of
// two horizontal axes (e1, e2, built from `up`; e1 x e2 = up). A lean vector
// (a, b) tips the segment's top toward a*e1 + b*e2 by sqrt(a^2 + b^2)
// radians, i.e. rotates it about up x (a*e1 + b*e2). The caller turns that
// into a rotation of the segment's bones about the segment's pivot bone
// (body_push_rotation) -- head, chest and waist about their pivot bones (the
// bone numbers in the comments below are Underworld's; the caller owns the
// choice, Legend's would be neck 4, spine 2, waist 1) -- applied waist first, then chest, then head, each about
// its pivot's position after the lower segments moved it.

#include "../vr_math.h"

namespace trlvr
{
    enum BodyPushSegment : int
    {
        kPushNone = -1,
        kPushHead = 0,      // pivot bone 17; moves bones 17..51 and 113..152
        kPushChest = 1,     // pivot bone 15; moves 15..105 and 113..152
        kPushWaist = 2,     // pivot bone 14; moves 14..105 and 113..152
        // AUDIT 22.6: the rest of the body. Limb segments are per CHAIN (A / B
        // as in the volume names; which is her left is decided by the caller),
        // and they hang: a push on the limb moves its END with the push, so
        // their lean is reversed against the torso's (lever < 0 below pivot).
        kPushHips = 3,      // pivot bone 1; moves 1..3, 14..112 and 113..152 (not the legs)
        kPushArmA = 4,      // pivot 53; moves 52..76 (clavicle a fraction, caller's choice)
        kPushArmB = 5,      // pivot 78; moves 77..101
        kPushLegA = 6,      // pivot 4; moves 4..8
        kPushLegB = 7,      // pivot 9; moves 9..13
        kPushSegments = 8
    };

    struct BodyPushParams
    {
        float strength = 1.0f;          // x every push, 0 .. 3
        float hz = 2.5f;                // spring frequency
        float damping = 0.9f;           // damping ratio; ~1 = no overshoot
        // head, chest, waist, hips, arm A, arm B, leg A, leg B
        float max_degrees[kPushSegments] = { 6.0f, 4.0f, 3.0f, 3.0f, 8.0f, 8.0f, 2.0f, 2.0f };
        // A push on a segment also moves the one below it: the head's drive
        // x chain[0] goes to the chest, the chest's total x chain[1] to the
        // waist.
        float chain[2] = { 0.3f, 0.4f };
        float arm_chain = 0.2f;         // each arm's drive x this goes to the chest
        float hips_chain = 0.3f;        // the waist's total x this goes to the hips
        float press_metres = 0.04f;     // depth that gives the full sustained tilt
        float lever_metres = 0.08f;     // height above the pivot for the full lever
        // Limbs: the lever is the distance of the contact from the limb's
        // pivot over this (arm: shoulder, a hand ~0.6 m out gives the full
        // lever, an upper arm ~0.15 m about a quarter); its sign is that of
        // the contact's height relative to the pivot (below = limb end follows
        // the push), blended over +-5 cm so a level limb does not flip.
        float arm_lever_metres = 0.5f;
        float leg_lever_metres = 0.8f;
        // The feet must not visibly slide: a leg's maximum lean is cut to
        // foot_limit_metres / its reach (hip to toe, set by the caller).
        float foot_limit_metres = 0.02f;
        float leg_reach_metres[2] = { 0.9f, 0.9f };     // leg A, leg B
    };

    struct BodyPushState
    {
        bool valid = false;             // up / e1 / e2 are set
        float up[3] = { 0.0f, 0.0f, 1.0f };
        float e1[3] = { 1.0f, 0.0f, 0.0f };
        float e2[3] = { 0.0f, 1.0f, 0.0f };
        float angle[kPushSegments][2] = {};     // the springs' lean, radians
        float vel[kPushSegments][2] = {};       // radians per second
        float shown[kPushSegments][2] = {};     // the lean after the soft limit
    };

    // One hand's contact with her body this frame.
    struct BodyPushContact
    {
        int segment = kPushNone;        // which segment it pushes (or kPushNone)
        float point[3] = {};            // the contact point, world
        float normal[3] = {};           // outward surface normal (BodyHit::normal)
        float depth = 0.0f;             // how far inside (BodyHit::depth), units
        float velocity[3] = {};         // the hand's velocity relative to her, units/s
        bool arriving = false;          // the first frame of this contact: add the kick
        bool velocity_valid = true;
        float weight = 1.0f;            // scales both terms (soft parts push lightly)
    };

    // One frame. contacts / n: this frame's contacts (any number; each adds).
    // pivot: the head, chest and waist pivot bones' world positions. up: her
    // vertical (any length; non-finite or zero resets the state). metre: game
    // units per metre. dt in seconds; a dt of 0 only updates the output, a
    // large one is stepped in pieces up to 0.1 s. Non-finite state or input
    // is dropped, never propagated.
    void body_push_step(BodyPushState& state, const BodyPushParams& params,
                        const BodyPushContact* contacts, int n,
                        const float pivot[kPushSegments][3], const float up[3],
                        float metre, float dt);

    // The rotation (rotation only, no translation: turn bones about the
    // pivot with rotation_about_point) for a segment; identity at rest.
    // `fraction` scales the angle (the arm's clavicle takes a share).
    Mat4 body_push_rotation(const BodyPushState& state, int segment, float fraction = 1.0f);

    // Any lean or speed above a tiny threshold. The step snaps what is below
    // it to exactly zero, so false means the bones need not be touched.
    bool body_push_active(const BodyPushState& state);

    // Forget everything (she was picked up, god mode ended).
    void body_push_reset(BodyPushState& state);

    // Which segment a body volume's hit pushes (volume names from
    // body_volumes.cpp): head, neck -> head; chest, upper chest, clavicles
    // and the chest soft boxes -> chest; abdomen -> waist; pelvis -> hips;
    // "upper arm / forearm / hand A|B" -> arm A|B; "thigh / shin A|B" -> leg
    // A|B (the chain is the volume name's last letter); feet and the buttock
    // soft box (soft index `rear_soft`) -> none. `soft` is BodyHit::soft
    // (-1 firm).
    int body_push_segment_for_volume(const char* name, int soft, int rear_soft);
}
