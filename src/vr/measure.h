#pragma once
// Measures the game's world against a known real-world size, once.

namespace trlvr
{
    void measure_init();

    // Called with the Camera the auto-centre hook already receives. The camera
    // knows which instance it is following, which is a far better way to find
    // Lara than matching her name.
    void measure_note_camera(void* camera);
}
