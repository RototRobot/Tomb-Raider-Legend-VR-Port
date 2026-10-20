// Tests for src/vr/finger_input (included by mathtest.cpp).
#include "../vr/finger_input.h"

static void test_finger_input()
{
    using namespace trlvr;
    const float kOneMinusInvE = 1.0f - std::exp(-1.0f);

    // Open hand: no sensors, nothing touched or pressed.
    {
        FingerInput open;
        float c[5], s[4];
        finger_curls_wanted(open, 0.0f, c, s);
        CHECK(near(c[0], 0.05f));
        CHECK(near(c[1], 0.0f));
        CHECK(near(c[2], 0.0f));
        CHECK(near(c[3], 0.0f));
        CHECK(near(c[4], 0.0f));
        CHECK(near(s[0], 0.55f));
        CHECK(near(s[1], 0.55f));
        CHECK(near(s[2], 0.55f));
        CHECK(near(s[3], 0.55f));
    }

    // Full grip with sensors and the index finger pulled fully on the trigger.
    {
        FingerInput in;
        in.trigger = 1.0f;
        in.grip = 1.0f;
        in.has_sensors = true;
        in.touch_trigger = true;
        in.touch_thumb = true;
        float c[5], s[4];
        finger_curls_wanted(in, 0.0f, c, s);
        CHECK(near(c[0], 0.65f));
        CHECK(near(c[1], 0.9f));
        CHECK(near(c[2], 0.9f));
        CHECK(near(c[3], 0.9f));
        CHECK(near(c[4], 0.9f));
        CHECK(near(s[0], 0.4f));
        CHECK(near(s[3], 0.4f));
    }

    // Sensors: a light reading without the touch flag is no finger on the
    // trigger; a real pull is (the sensor can drop out at full press).
    {
        FingerInput in;
        in.trigger = 0.15f;
        in.has_sensors = true;
        float c[5], s[4];
        finger_curls_wanted(in, 0.0f, c, s);
        CHECK(near(c[1], 0.0f));
        CHECK(near(c[0], 0.05f));
        in.trigger = 1.0f;
        finger_curls_wanted(in, 0.0f, c, s);
        CHECK(near(c[1], 0.9f));
    }

    // A pull of 0.9 or more is a full one (worn triggers stop short of 1).
    {
        FingerInput in;
        in.has_sensors = true;
        in.touch_trigger = true;
        in.trigger = 0.92f;
        in.grip = 0.9f;
        float c[5], s[4];
        finger_curls_wanted(in, 0.0f, c, s);
        CHECK(near(c[1], 0.9f));
        CHECK(near(c[2], 0.9f));
    }

    // No-sensor path: a pull above 0.05 counts as on the trigger, a face
    // button as the thumb.
    {
        FingerInput in;
        in.trigger = 0.5f;
        in.thumb_button = true;
        float c[5], s[4];
        finger_curls_wanted(in, 0.0f, c, s);
        CHECK(near(c[1], 0.35f + 0.55f * 0.5f / 0.9f));
        CHECK(near(c[0], 0.55f));

        in.trigger = 0.04f;
        finger_curls_wanted(in, 0.0f, c, s);
        CHECK(near(c[1], 0.0f));
    }

    // Without sensors the touch flags are ignored.
    {
        FingerInput in;
        in.touch_trigger = true;
        in.touch_thumb = true;
        float c[5], s[4];
        finger_curls_wanted(in, 0.0f, c, s);
        CHECK(near(c[1], 0.0f));
        CHECK(near(c[0], 0.05f));
    }

    // The first step snaps to the wanted values and marks the state valid.
    {
        FingerCurls state;
        FingerInput in;
        in.grip = 1.0f;
        in.has_sensors = true;
        in.touch_thumb = true;
        finger_curls_step(state, in, 0.0f, 0.016f);
        CHECK(state.valid);
        CHECK(near(state.curl[0], 0.65f));
        CHECK(near(state.curl[2], 0.9f));
        CHECK(near(state.spread[0], 0.4f));
    }

    // Easing: after one 50 ms time constant the index covers 1 - 1/e of the gap.
    {
        FingerCurls state;
        FingerInput open;
        finger_curls_step(state, open, 0.0f, 0.016f);
        FingerInput pull;
        pull.trigger = 1.0f;
        pull.has_sensors = true;
        pull.touch_trigger = true;
        finger_curls_step(state, pull, 0.0f, 0.05f);
        CHECK(near(state.curl[1], 0.9f * kOneMinusInvE));
    }

    // A large dt (> 0.25 s) and a zero dt both snap.
    {
        FingerCurls state;
        FingerInput open;
        finger_curls_step(state, open, 0.0f, 0.016f);
        FingerInput pull;
        pull.trigger = 1.0f;
        pull.has_sensors = true;
        pull.touch_trigger = true;
        finger_curls_step(state, pull, 0.0f, 0.3f);
        CHECK(near(state.curl[1], 0.9f));

        FingerCurls zero;
        finger_curls_step(zero, open, 0.0f, 0.016f);
        // dt 0 (a coarse clock that has not ticked) holds the pose: a
        // sensor flicker on such a frame must not snap a finger.
        finger_curls_step(zero, pull, 0.0f, 0.0f);
        CHECK(near(zero.curl[1], 0.0f));
    }

    // Reset clears the state, so the next step snaps again.
    {
        FingerCurls state;
        FingerInput pull;
        pull.trigger = 1.0f;
        pull.has_sensors = true;
        pull.touch_trigger = true;
        finger_curls_step(state, pull, 0.0f, 0.016f);
        finger_curls_reset(state);
        CHECK(!state.valid);
        CHECK(near(state.curl[1], 0.0f));
        finger_curls_step(state, FingerInput(), 0.0f, 0.016f);
        CHECK(state.valid);
        CHECK(near(state.curl[1], 0.0f));
    }

    // rest_curl floors index, middle, ring and pinky, not the thumb.
    {
        FingerInput open;
        float c[5], s[4];
        finger_curls_wanted(open, 0.3f, c, s);
        CHECK(near(c[0], 0.05f));
        CHECK(near(c[1], 0.3f));
        CHECK(near(c[2], 0.3f));
        CHECK(near(c[3], 0.3f));
        CHECK(near(c[4], 0.3f));

        // Above the floor, the rest value changes nothing.
        FingerInput grip;
        grip.grip = 1.0f;
        grip.trigger = 1.0f;
        grip.has_sensors = true;
        grip.touch_trigger = true;
        finger_curls_wanted(grip, 0.3f, c, s);
        CHECK(near(c[1], 0.9f));
        CHECK(near(c[2], 0.9f));

        // rest_curl is clamped to 0.6.
        finger_curls_wanted(open, 0.9f, c, s);
        CHECK(near(c[1], 0.6f));
    }
    // Gestures from what the controller does (finger_curls_wanted): grip
    // alone points (presses only), trigger + thumb pinches, trigger + grip is
    // a fist; both of those grab. A finger resting on the trigger is not a grab.
    {
        auto gesture = [](float trigger, float grip, bool on_trigger, bool thumb)
        {
            FingerInput in;
            in.has_sensors = true;
            in.trigger = trigger;
            in.grip = grip;
            in.touch_trigger = on_trigger;
            in.touch_thumb = thumb;
            float c[5], s[4];
            finger_curls_wanted(in, 0.0f, c, s);
            return finger_gesture(c);
        };
        CHECK(gesture(0.0f, 1.0f, false, true) == HandGesture::Point);
        CHECK(gesture(0.0f, 1.0f, false, false) == HandGesture::Point);
        CHECK(gesture(0.0f, 1.0f, true, true) != HandGesture::Fist);       // resting, not pulling
        CHECK(!finger_gesture_grabs(gesture(0.0f, 1.0f, true, true)));
        CHECK(gesture(1.0f, 0.0f, true, true) == HandGesture::Pinch);
        CHECK(gesture(1.0f, 1.0f, true, true) == HandGesture::Fist);
        CHECK(gesture(1.0f, 1.0f, true, false) == HandGesture::Fist);
        CHECK(gesture(0.0f, 0.0f, false, false) == HandGesture::Open);
        CHECK(gesture(1.0f, 0.0f, true, false) == HandGesture::Open);      // trigger alone, thumb up
        CHECK(finger_gesture_grabs(HandGesture::Pinch) && finger_gesture_grabs(HandGesture::Fist));
        CHECK(!finger_gesture_grabs(HandGesture::Point) && !finger_gesture_grabs(HandGesture::Open));
    }

    // Grabbing with hysteresis: a fist starts it; a dip of the index to 0.5
    // (a flickering trigger) keeps it; an open index lets go; it does not
    // start again at 0.5.
    {
        const float fist[5] = { 0.6f, 0.9f, 0.9f, 0.9f, 0.9f };
        const float dip[5] = { 0.6f, 0.5f, 0.9f, 0.9f, 0.9f };
        const float open_index[5] = { 0.6f, 0.3f, 0.9f, 0.9f, 0.9f };
        const float pinch_dip[5] = { 0.35f, 0.5f, 0.0f, 0.0f, 0.0f };
        CHECK(finger_grabbing(fist, false));
        CHECK(finger_grabbing(dip, true));
        CHECK(!finger_grabbing(dip, false));
        CHECK(!finger_grabbing(open_index, true));
        CHECK(finger_grabbing(pinch_dip, true));
        CHECK(!finger_grabbing(pinch_dip, false));
    }
}
