#pragma once
// Widening the engine's own field of view so its culling matches the headset.
// See camera_fov.cpp for why this is done at the camera rather than at the
// culling frustum.

namespace trlvr
{
    void camera_fov_init();
}
