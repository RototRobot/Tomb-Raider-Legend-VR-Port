#pragma once
// The settings overlay in the headset (ported from the Underworld mod):
// where its panel goes in the room, and the menu button's hold-to-open.
// Pure logic, tested in mathtest; vr_submit.cpp draws the panel and
// vr_input.cpp feeds the button.

namespace trlvr
{
    // A square panel in OpenVR tracking space (metres; x right, y up, z
    // back, so straight ahead is -z): its centre and the unit axes along its
    // width (level) and height. It faces the head it was placed from.
    struct MenuPanelPose
    {
        float centre[3];
        float right[3];
        float up[3];
    };

    // Placed from the head at head[3], whose level forward is (forward_x,
    // forward_z) (need not be normalised; false if it has no length): the
    // level direction straight ahead, turned yaw_left_degrees to the left
    // and pitch_up_degrees up, distance metres out.
    bool menu_panel_place(const float head[3], float forward_x, float forward_z,
                          float distance, float yaw_left_degrees,
                          float pitch_up_degrees, MenuPanelPose* out);

    // Its corners at half_size metres from the centre each way, in triangle
    // strip order: top left, top right, bottom left, bottom right (texture
    // u, v = 0,0 / 1,0 / 0,1 / 1,1).
    void menu_panel_corners(const MenuPanelPose& pose, float half_size,
                            float out[4][3]);

    // Half its width for a panel width_degrees wide at distance metres.
    float menu_panel_half_size(float distance, float width_degrees);

    // The menu (pause) button with hold-to-open. With the overlay closed
    // and hold_seconds > 0 the button does not reach the game while it is
    // down: let go before hold_seconds it is a tap (the caller pulses
    // pause), held to hold_seconds it opens the overlay and that press never
    // reaches the game. While the overlay is open, or its input is held
    // back after closing (`overlay_busy`), the button is the overlay's.
    enum MenuHoldEvent
    {
        MenuHoldNone,
        MenuHoldTap,        // pulse the game's pause
        MenuHoldOpen,       // open the overlay
    };

    struct MenuHold
    {
        int state = 0;                  // idle, pending, consumed
        unsigned long long down_ms = 0;
    };

    // Once a frame. *to_game: the button as the game should see it held
    // this frame (only with hold-to-open off).
    MenuHoldEvent menu_hold_update(MenuHold& hold, bool down,
                                   unsigned long long now_ms,
                                   float hold_seconds, bool overlay_busy,
                                   bool* to_game);
}
