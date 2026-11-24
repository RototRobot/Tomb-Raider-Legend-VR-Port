#pragma once
// Surface-locked water waves (2026-11-12).
//
// Water Effects draws a 52x45 grid over the screen (FXDRAW_WaterFX
// 0x00446D90) whose texture coordinates are the screen position plus a
// wobble. The wobble is worked out per grid cell from the cell's row and
// column, a fixed random sequence and a time phase (0x00446FC0..0x004470F1),
// so it is fixed to the screen even in the flat game. In VR that put it on
// the player's face: it turned with the head and was the same in both eyes
// (user 2026-11-12). Here each eye gets its own grid: where that eye's ray
// through each grid point meets the water plane decides the wobble, so the
// waves stay on the surface. Distant water, and rays that never reach the
// plane (under water), are locked to the direction instead. The game's own
// grid is read where WaterFX submits it (the call at 0x004471F6), for its
// wave strength and colour.
//
// The water's height is the one Lara's swimming uses (PlayerData::work
// +0x808, also WaterFX's under-water test), kept after she leaves the water
// while the camera stays nearby. Reading it from the reflection pass was
// tried and dropped (2026-11-12): that pass draws its own low-detail
// terrain, mirrored and flipped, so the plane could not be told apart from
// each piece's placement.

#include <d3d9.h>

namespace trlvr
{
    // One captured grid: its colour and how strong its wobble is.
    struct WaterLayer
    {
        bool valid = false;
        DWORD colour = 0;
        // Base coordinates as the game lays them out: u = u0 + u1 * x and
        // v = v0 + v1 * y for grid x, y in -1..1.
        float u0 = 0.5f, u1 = 0.5f, v0 = 0.5f, v1 = 0.5f;
        // Root mean square of the game's wobble, in texture units.
        float rms_u = 0.0f, rms_v = 0.0f;
    };

    // Hooks the grid's submit call (when [vr] water_effects and
    // water_waves are on).
    void water_waves_init();

    // A Water Effects screen layer is about to be drawn per eye (kind 0 over
    // the screen copy, 1 over the reflection): the grid the game submitted
    // for it. False: none captured, draw the game's own.
    bool water_waves_begin_layer(int kind, WaterLayer* out);

    // Draws this eye's grid in place of the game's, with the game's shader
    // and states and its bare projection (game_c0) in c0. False: nothing
    // drawn, issue the game's draw.
    bool water_waves_draw(IDirect3DDevice9* device, const WaterLayer& layer,
                          int eye, const float* game_c0);
}
