#pragma once
namespace trlvr
{
    void ui_space_init();
    bool ui_drawing_interface();
    // A HUD sprite the game places over a world point (lock-on reticle,
    // hazard caution icon, grab-prompt hand): texture pages identified by
    // the F9 page capture of 2026-10-02.
    bool ui_drawing_world_marker();
    bool ui_interface_world_locked();
    // True during the retail fullscreen screen-wipe draw path.
    bool ui_drawing_screen_wipe();
    bool ui_menu_active();
    bool ui_pause_menu_active();
    // Root screen in the 2000 range: the level loading screen (2001 seen
    // immediately before every level start). Its picture is a flat quad on
    // the scene path, like a movie.
    bool ui_loading_screen_active();
    // Mission Prep (root screen 3001): its staged 3D scene is drawn without
    // eye separation, like a loading screen.
    bool ui_mission_prep_active();
    short ui_root_screen_id();
}
