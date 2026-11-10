#pragma once
// The left stick as the D-pad (the gear items). Pure state
// machine: no config(), no game memory, no Windows calls (mathtest links it).
//
// A click of the modifier (X in the Underworld control scheme), tapped or held,
// arms it. While armed the stick does not move Lara; the next slide out of the
// centre presses the one D-pad direction it points at (the stronger axis, no
// diagonals) for as long as it stays out. A stick already pushed when the click
// went down must come back to the centre first. Back at the centre: held, the
// modifier stays armed for another slide; let go, it disarms. A tap with no
// slide disarms after kDpadArmSeconds. After disarming, movement stays blocked
// until the stick is back in the centre, so a slide never turns into a step.

namespace trlvr
{
    // XINPUT_GAMEPAD_DPAD_* bits.
    enum : unsigned short
    {
        kDpadUp = 0x0001, kDpadDown = 0x0002, kDpadLeft = 0x0004, kDpadRight = 0x0008,
    };

    const float kDpadPress = 0.5f;      // stick out past this presses
    const float kDpadCentre = 0.3f;     // back inside this is the centre
    const float kDpadArmSeconds = 1.5f; // a tap waits this long for a slide

    struct DpadGesture
    {
        bool armed = false;
        bool need_centre = false;   // the stick has to come back first
        bool block_move = false;    // disarmed, but the stick is still out
        bool prev_modifier = false;
        unsigned short pressed = 0; // the D-pad bit being held
        float idle = 0.0f;          // seconds armed with the modifier up
    };

    struct DpadOut
    {
        unsigned short buttons = 0; // D-pad bits for the game
        bool stick_to_game = true;  // false: send the game a centred left stick
        bool modifier = false;      // armed: the modifier's other uses apply
    };

    // One poll. `cancel` (a recentre: both stick clicks) disarms without a
    // press. dt in seconds.
    DpadOut dpad_gesture_update(DpadGesture& g, bool modifier_down, bool cancel,
                                float lx, float ly, float dt);
}
