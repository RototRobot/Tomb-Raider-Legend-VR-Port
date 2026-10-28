#pragma once
// Hand contact on a soft part of Lara's body, ported from the Underworld mod
// for the rear touch: which way a hand has pressed a part since it took
// hold, whether it grips, and the kick of an arriving hand. Pure maths -- no
// config(), no game memory, no Windows calls -- so mathtest links it.
//
// Legend already has its own SoftSpring inside camera_head.cpp, so these
// types live in trlvr::rear and the contact record is called RearContact.
// The spring itself is not here: rear_jiggle.h's jiggle_step takes what
// soft_contact gathers (press -> JiggleContact::shift, kick -> ::kick).

#include "../vr_math.h"

namespace trlvr
{
namespace rear
{
    // What soft_contact fills in for one soft part; the caller reads it each
    // frame. Times are the caller's clock in seconds; 0 means never.
    struct RearContact
    {
        float press[2][3] = {};         // per hand: how far its point has moved
                                        // since it took hold, root-relative
        double press_time[2] = { 0.0, 0.0 };
        double grab_time[2] = { 0.0, 0.0 };     // last frame a grip held it
        float kick[3] = {};             // velocity impulse waiting to be taken
        bool kick_pending = false;      // the caller clears it once it has used kick
    };

    // What soft_contact keeps per hand AND per soft part (one of these for
    // each pairing; a hand holds only one part at a time).
    struct SoftContactState
    {
        bool holding = false;           // this hand has taken hold of the part
        bool grabbing = false;          // ... and its grip is closed on it
        bool touching_last = false;     // inside the body last frame
        bool have_last = false;
        float entry[3] = {};            // hand point at first contact, root-relative
        float last_point[3] = {};
        double last_time = 0.0;
    };

    // One hand, one frame. point: the finger/palm point doing the pressing
    // (the one nearest the part); root: Lara's root; hand_velocity: that
    // point's velocity relative to her, units/s; normal: the surface normal
    // at the contact, outward (from BodyHit). The hand that is carrying her
    // is not fed.
    struct SoftContactInput
    {
        bool touching = false;          // point is inside her body this frame
        bool point_valid = true;        // point is fresh (seen < 250 ms ago)
        bool velocity_valid = true;     // hand_velocity is real
        int hand = 0;                   // 0 or 1
        float point[3] = {};
        float root[3] = {};
        float hand_velocity[3] = {};
        float normal[3] = {};
        bool gripping = false;
        float metre = 291.0f;
        double now_seconds = 0.0;
    };

    // Presses, kicks and grips on one soft part. Call once per frame per
    // hand per part: `touching` for the part the hand is in (or the nearest
    // within 8 cm), touching = false for the others so their state
    // releases. A held grip keeps hold even with the hand outside her body,
    // until the grip opens. While a hand holds one part (state.holding) do
    // not pick another for it. Returns true on the frame the hand first
    // touches (for a haptic pulse). An arriving hand's velocity into the
    // surface (over 0.2 m/s, last sample under 100 ms old) becomes a kick:
    // 0.6 x its velocity, at most 2.5 m/s worth.
    bool soft_contact(RearContact& contact, SoftContactState& hand_state,
                      const SoftContactInput& in);
}
}
