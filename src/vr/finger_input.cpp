#include "finger_input.h"

#include <cmath>

namespace trlvr
{
    namespace
    {
        const float kEaseSeconds = 0.05f;
        const float kMaxDt = 0.25f;
        const float kTriggerDeadzone = 0.05f;
        const float kMaxRestCurl = 0.6f;
        // A pull this far is a full one (worn or faulty controllers stop
        // short of 1); a pull past kPulledTouch means the finger is on it.
        const float kFullPress = 0.9f;
        const float kPulledTouch = 0.2f;

        float full_press(float v)
        {
            v /= kFullPress;
            return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
        }

        float clamp_rest(float rest_curl)
        {
            if (rest_curl < 0.0f) return 0.0f;
            if (rest_curl > kMaxRestCurl) return kMaxRestCurl;
            return rest_curl;
        }
    }

    void finger_curls_wanted(const FingerInput& input, float rest_curl,
                             float curl_out[5], float spread_out[4])
    {
        const bool thumb_down = input.has_sensors ? input.touch_thumb : input.thumb_button;
        // A trigger pulled in counts as touched even if the sensor says
        // not: a sensor dropping out at full press would straighten the
        // finger and let go of her.
        const bool on_trigger = input.has_sensors
            ? input.touch_trigger || input.trigger > kPulledTouch
            : input.trigger > kTriggerDeadzone;

        const float rest = clamp_rest(rest_curl);
        const float grip = full_press(input.grip);

        float thumb = thumb_down ? 0.55f + 0.1f * grip : 0.05f;
        float index = on_trigger ? 0.35f + 0.55f * full_press(input.trigger) : 0.0f;
        float middle = 0.9f * grip;
        float ring = middle;
        float pinky = middle;

        // The rest floor applies to the four fingers, not the thumb.
        if (index < rest) index = rest;
        if (middle < rest) middle = rest;
        if (ring < rest) ring = rest;
        if (pinky < rest) pinky = rest;

        curl_out[0] = thumb;
        curl_out[1] = index;
        curl_out[2] = middle;
        curl_out[3] = ring;
        curl_out[4] = pinky;

        const float spread = 0.4f + 0.15f * (1.0f - grip);
        spread_out[0] = spread;
        spread_out[1] = spread;
        spread_out[2] = spread;
        spread_out[3] = spread;
    }

    void finger_curls_step(FingerCurls& state, const FingerInput& input,
                           float rest_curl, float dt)
    {
        float wanted_curl[5];
        float wanted_spread[4];
        finger_curls_wanted(input, rest_curl, wanted_curl, wanted_spread);

        // dt 0 (a coarse clock that has not ticked): hold, do not snap.
        if (state.valid && dt == 0.0f)
            return;
        if (!state.valid || dt < 0.0f || dt > kMaxDt)
        {
            for (int i = 0; i < 5; ++i) state.curl[i] = wanted_curl[i];
            for (int i = 0; i < 4; ++i) state.spread[i] = wanted_spread[i];
            state.valid = true;
            return;
        }

        const float follow = 1.0f - std::exp(-dt / kEaseSeconds);
        for (int i = 0; i < 5; ++i)
            state.curl[i] += (wanted_curl[i] - state.curl[i]) * follow;
        for (int i = 0; i < 4; ++i)
            state.spread[i] += (wanted_spread[i] - state.spread[i]) * follow;
    }

    void finger_curls_reset(FingerCurls& state)
    {
        state.valid = false;
        for (int i = 0; i < 5; ++i) state.curl[i] = 0.0f;
        for (int i = 0; i < 4; ++i) state.spread[i] = 0.0f;
    }

    HandGesture finger_gesture(const float curl[5])
    {
        const float thumb = curl[0], index = curl[1];
        const float others = (curl[2] + curl[3] + curl[4]) / 3.0f;
        const bool closed = others > 0.5f;
        if (index > 0.6f && closed)
            return HandGesture::Fist;
        if (index > 0.6f && thumb > 0.4f)
            return HandGesture::Pinch;
        if (index < 0.4f && closed)
            return HandGesture::Point;
        return HandGesture::Open;
    }

    bool finger_grabbing(const float curl[5], bool was)
    {
        // Starting: as finger_gesture's pinch or fist. Holding on: a clearly
        // opened hand to let go, so a flickering trigger does not drop her.
        const float index_min = was ? 0.45f : 0.6f;
        const float others_min = was ? 0.35f : 0.5f;
        const float thumb_min = was ? 0.3f : 0.4f;
        const float others = (curl[2] + curl[3] + curl[4]) / 3.0f;
        return curl[1] > index_min && (others > others_min || curl[0] > thumb_min);
    }
}
