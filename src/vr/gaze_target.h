#pragma once
namespace trlvr
{
    // [vr] gaze_target: third-person lock-on picks the candidate nearest the
    // headset's centre ray (within gaze_target_degrees), else the game's own.
    void gaze_target_init();
    // The headset's centre ray in game world space, once per camera build.
    void gaze_target_set_ray(const float eye[3], const float direction[3]);
}
