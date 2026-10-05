#pragma once
// MSAA cap (2026-10-05). With FSAA on, the game asks for the highest
// D3DMULTISAMPLE_NONMASKABLE quality it is offered; DXVK maps quality q to
// 1 << q samples, so it chose quality 3 = 8x MSAA on the 4888x2392
// side-by-side target -- the GPU scene took 12-15 ms (30 ms at the water)
// against the 11.1 ms a 90 Hz frame allows. [vr] msaa caps the sample
// count: the qualities offered to the game, the device's presentation
// parameters and every multisampled surface it creates are clamped alike,
// so they always match each other.

#include "../common/config.h"

#include <d3d9.h>

namespace trlvr
{
    // Highest allowed NONMASKABLE quality: 2x -> 1, 4x -> 2, 8x -> 3;
    // -1 when multisampling is off.
    inline int msaa_max_quality()
    {
        const int samples = config().msaa;
        if (samples >= 16) return 4;
        if (samples >= 8) return 3;
        if (samples >= 4) return 2;
        if (samples >= 2) return 1;
        return -1;
    }

    // Clamps one multisample request in place. Returns true if it changed.
    inline bool msaa_clamp(D3DMULTISAMPLE_TYPE* type, DWORD* quality)
    {
        if (!type || !quality || *type == D3DMULTISAMPLE_NONE)
            return false;
        const int max_quality = msaa_max_quality();
        if (max_quality < 0)
        {
            *type = D3DMULTISAMPLE_NONE;
            *quality = 0;
            return true;
        }
        if (*type == D3DMULTISAMPLE_NONMASKABLE)
        {
            if (*quality > DWORD(max_quality))
            {
                *quality = DWORD(max_quality);
                return true;
            }
            return false;
        }
        const D3DMULTISAMPLE_TYPE most =
            static_cast<D3DMULTISAMPLE_TYPE>(1 << max_quality);
        if (*type > most)
        {
            *type = most;
            *quality = 0;
            return true;
        }
        return false;
    }
}
