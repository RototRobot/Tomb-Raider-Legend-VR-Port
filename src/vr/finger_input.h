#pragma once
// Finger curls for the VR hands (ported from the Underworld mod's
// finger_input). Pure maths -- no config(), no game memory, no Windows calls
// -- so mathtest links it.
//
// Per hand, per frame, the controller's readings become a wanted pose:
//   thumb_down  = touch_thumb (sensors) or thumb_button (no sensors)
//   on_trigger  = touch_trigger || trigger > 0.2 (sensors; a real pull
//                 counts even if the sensor drops out at full press)
//                 or trigger > 0.05 (no sensors)
//   thumb  = thumb_down ? 0.55 + 0.1*grip : 0.05
//   index  = on_trigger ? 0.35 + 0.55*trigger : 0
//   middle = ring = pinky = 0.9*grip
//   spread (index, middle, ring, pinky) = 0.4 + 0.15*(1 - grip)
// trigger and grip are read as full at 0.9 (worn controllers stop short of
// 1). Spread is a 0..1 amount of finger splay, not an angle; the hand model
// maps it to an angle itself. rest_curl (0..0.6) is a floor: index, middle,
// ring and pinky are raised to it, so a relaxed hand never looks fully flat.
// The thumb is not floored.
//
// The displayed curls ease toward the wanted ones with a 50 ms time constant
// (follow = 1 - exp(-dt / 0.05)). The first frame, dt <= 0 and dt > 0.25 snap
// straight to the wanted values. Inputs are expected in 0..1.

namespace trlvr
{
    struct FingerInput
    {
        float trigger = 0.0f;       // index trigger pull, 0 .. 1
        float grip = 0.0f;          // grip / squeeze, 0 .. 1
        bool has_sensors = false;   // controller has capacitive touch sensors
        bool touch_trigger = false; // index finger resting on the trigger
        bool touch_thumb = false;   // thumb on stick, face button or thumb rest
        bool thumb_button = false;  // no sensors: any face button or stick click
    };

    struct FingerCurls
    {
        bool valid = false;
        float curl[5] = {};     // thumb, index, middle, ring, pinky; 0 = open
        float spread[4] = {};   // index, middle, ring, pinky; 0..1 splay amount
    };

    // Wanted curls and spreads for one frame, before easing. curl_out gets 5
    // values (thumb first), spread_out 4. rest_curl is clamped to 0 .. 0.6.
    void finger_curls_wanted(const FingerInput& input, float rest_curl,
                             float curl_out[5], float spread_out[4]);

    // Advances state one frame toward the wanted pose. dt in seconds. The
    // first call after a reset (or with dt <= 0 or dt > 0.25) snaps to the
    // wanted values and marks the state valid.
    void finger_curls_step(FingerCurls& state, const FingerInput& input,
                           float rest_curl, float dt);

    // Clears the state so the next step snaps.
    void finger_curls_reset(FingerCurls& state);

    // The hand's gesture from its curls (thumb, index, middle, ring, pinky).
    // Point: index straight (< 0.4, i.e. not pulling the trigger) with the
    // other three curled (> 0.5, the grip). Fist: index pulled in (> 0.6)
    // and the other three curled. Pinch: index pulled in with the thumb down
    // (> 0.4) and the others open. Open: anything else. Grabbing (god mode)
    // = pinch or fist; open and point only press.
    enum class HandGesture : int { Open = 0, Point = 1, Pinch = 2, Fist = 3 };
    HandGesture finger_gesture(const float curl[5]);
    inline bool finger_gesture_grabs(HandGesture g)
    {
        return g == HandGesture::Pinch || g == HandGesture::Fist;
    }

    // Grabbing with hysteresis: `was` = grabbing last frame. Starts where
    // finger_gesture_grabs does; once grabbing, lets go only when the index
    // opens below 0.45 or both the other fingers (0.35) and thumb (0.3) open.
    bool finger_grabbing(const float curl[5], bool was);
}
