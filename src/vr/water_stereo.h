#pragma once
// Marks the game's half-resolution water-composite pass so the stereo layer
// can divide that off-screen target into a left and a right half as well.

namespace trlvr
{
    void water_stereo_init();
    bool water_stereo_active();

    // The Water Effects layer switched live from the settings menu.
    // water_stereo_effects_live: false when the code is not the expected
    // retail or patched bytes; *on is whether the layer is drawn.
    bool water_stereo_effects_live(bool* on);
    void water_stereo_set_effects_live(bool on);
}
