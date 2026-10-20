#pragma once
// A point mass on a damped spring towards a moving target: the 3D jiggle of
// the buttocks (ported from the Underworld mod's skin_deform; the shader
// patcher is not part of it, Legend skins on the CPU). Pure maths -- no
// config(), no game memory, no Windows calls -- so mathtest links it.

namespace trlvr
{
    // A point mass on a damped spring towards a moving target.
    struct JiggleParams
    {
        float hz = 3.0f;                // natural frequency
        float damping = 0.3f;           // damping ratio (1 = critical)
        float max_offset = 1.0f;        // the shown offset eases toward this (units)
        float touch_offset = 0.0f;      // ... plus this while a hand is on it (eased)
        float reset_distance = 100.0f;  // a target jump this large in one step resets (units)
    };

    struct JiggleState
    {
        bool valid = false;
        float pos[3] = {};
        float vel[3] = {};
        float target[3] = {};           // last step's target
        float boost = 0.0f;             // 0..1: a hand on it (eased in 0.08 s, out 0.5 s)
    };

    // A hand on the mass this step (god mode; rear::soft_contact in
    // soft_body.h gathers the press and kick).
    struct JiggleContact
    {
        bool touched = false;           // a hand is on it: the limit loosens
        bool gripped = false;           // a closed grip holds it: stiff, near-critical
        float shift[3] = {};            // where the hand has taken it: added to the target (units)
        float kick[3] = {};             // velocity added once, this step (units/s)
    };

    // Advances by dt seconds (fixed sub-steps, so the result does not depend
    // on the frame rate) and returns the mass's offset from the target
    // (world units), eased toward the limit (limit x tanh(lag / limit),
    // limit = max_offset + boost x touch_offset); past the limit the spring
    // stiffens (and damps outward motion) so a hard swing rounds off, and the
    // mass is held within three times it. A first step, a dt outside (0, 0.25] or a target
    // jump beyond reset_distance puts the mass on the target at rest.
    // contact may be null (no hand).
    void jiggle_step(JiggleState& s, const float target[3], float dt, const JiggleParams& p,
                     float offset[3], const JiggleContact* contact = nullptr);
}
