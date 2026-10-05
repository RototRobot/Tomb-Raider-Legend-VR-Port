#pragma once
namespace trlvr
{
    void cull_init();
    // F2: first-person object culling open / headset frustum.
    void cull_toggle_object_culling();
    // Shift+F2: terrain strip culling open / headset frustum.
    void cull_toggle_terrain_culling();
    // Rebuild object visibility planes after the final HMD camera transform.
    void cull_rebuild_headset_volume(void* camera, bool first_person);
}
