#include "board_leash.h"

namespace trlvr
{
    void board_leash(float anchor[3], const float lara[3], float radius_units,
                     float height_units)
    {
        const float dx = lara[0] - anchor[0], dy = lara[1] - anchor[1];
        const float dist = sqrtf(dx * dx + dy * dy);
        if (radius_units <= 0.0f)
        {
            anchor[0] = lara[0];
            anchor[1] = lara[1];
        }
        else if (dist > radius_units)
        {
            const float f = (dist - radius_units) / dist;
            anchor[0] += dx * f;
            anchor[1] += dy * f;
        }
        const float dz = lara[2] - anchor[2];
        if (height_units <= 0.0f)
            anchor[2] = lara[2];
        else if (dz > height_units)
            anchor[2] = lara[2] - height_units;
        else if (dz < -height_units)
            anchor[2] = lara[2] + height_units;
    }

    float board_stick_axis(float v, float dead)
    {
        if (!(v == v))
            return 0.0f;
        if (dead < 0.0f)
            dead = 0.0f;
        if (dead >= 1.0f || fabsf(v) < dead)
            return 0.0f;
        const float t = (v - (v > 0.0f ? dead : -dead)) / (1.0f - dead);
        return t > 1.0f ? 1.0f : t < -1.0f ? -1.0f : t;
    }

    bool board_stick_step(float* zoom, float* scale, float zoom_in,
                          float scale_in, float zoom_speed, float scale_speed,
                          float dt, float zoom_lo, float zoom_hi,
                          float scale_lo, float scale_hi)
    {
        if (!zoom || !scale || !(dt > 0.0f) || !(dt < 0.5f))
            return false;
        bool changed = false;
        if (zoom_in != 0.0f && zoom_in == zoom_in && zoom_speed > 0.0f)
        {
            float z = *zoom * expf(-zoom_in * zoom_speed * dt);
            z = z < zoom_lo ? zoom_lo : z > zoom_hi ? zoom_hi : z;
            if (z == z && z != *zoom)
            {
                *zoom = z;
                changed = true;
            }
        }
        if (scale_in != 0.0f && scale_in == scale_in && scale_speed > 0.0f)
        {
            float s = *scale * expf(-scale_in * scale_speed * dt);
            s = s < scale_lo ? scale_lo : s > scale_hi ? scale_hi : s;
            if (s == s && s != *scale)
            {
                *scale = s;
                changed = true;
            }
        }
        return changed;
    }
}
