#pragma once
// Marks the game's half-resolution water-composite pass so the stereo layer
// can divide that off-screen target into a left and a right half as well.

namespace trlvr
{
    void water_stereo_init();
    bool water_stereo_active();

}
