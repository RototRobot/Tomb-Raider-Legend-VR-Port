#pragma once
// The leash on a table-top board's anchor (god-mode seat): the anchor follows
// Lara only once she is further than the radius away. Pure maths.

#include <cmath>

namespace trlvr
{
    // Pulls anchor toward lara (units, z up) so it stays within radius_units
    // of her in the horizontal plane and height_units above or below her.
    // A radius of 0 or less puts the anchor on her horizontally; a height of
    // 0 or less (the default) likewise vertically. Past the radius the
    // anchor moves by (lara - anchor) * (d - radius) / d in x and y, so it
    // ends exactly on the radius with its direction kept.
    void board_leash(float anchor[3], const float lara[3], float radius_units,
                     float height_units = 0.0f);

    // A stick axis with a dead zone (as Underworld's board stick): 0 inside
    // |v| < dead, then rescaled so the dead zone's edge is 0 and full tilt
    // is 1; clamped to -1..1. NaN gives 0.
    float board_stick_axis(float v, float dead = 0.2f);

    // One frame of the board view's right stick (Underworld's camera_rig
    // zoom and scale). zoom: the view's distance multiplier (smaller =
    // closer); scale: the world's shrink factor (Lara 175/scale cm tall).
    // Inputs -1..1, stick up positive: up zooms in and makes Lara bigger:
    //   zoom  *= exp(-zoom_in  * zoom_speed  * dt), clamped to zoom_lo..hi
    //   scale *= exp(-scale_in * scale_speed * dt), clamped to scale_lo..hi
    // An axis with no input (or speed <= 0) is left untouched, unclamped.
    // dt outside (0, 0.5) -- a hitch -- changes nothing. Returns whether
    // either value changed.
    bool board_stick_step(float* zoom, float* scale, float zoom_in,
                          float scale_in, float zoom_speed, float scale_speed,
                          float dt, float zoom_lo, float zoom_hi,
                          float scale_lo, float scale_hi);
}
