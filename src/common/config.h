#pragma once
// User-facing settings read from trlvr.ini beside the game executable.

#include <windows.h>

namespace trlvr
{
    struct Config
    {
        wchar_t backend[MAX_PATH] = L"auto";
        bool logging = true;
        bool force_windowed = true;
        int window_width = 0;
        int window_height = 0;
        bool window_borderless = false;
        bool release_cursor_unfocused = true;

        bool vr_enabled = true;
        bool vr_submit = true;
        bool stereo = true;
        bool stereo_same_frame = true;
        bool stereo_lock_pose = true;

        bool head_tracking = true;
        bool head_rotation = true;
        bool head_position = true;
        bool hmd_drives_camera = true;
        bool hmd_drives_position = true;
        bool hmd_aim = true;
        float world_scale = 291.0f;
        float head_rotation_scale = 1.0f;
        float head_position_scale = 1.0f;
        bool level_horizon = true;
        bool camera_auto_center = false;
        bool camera_shake = false;

        // Experimental and deliberately opt-in. The ordinary third-person
        // path remains untouched unless this is enabled.
        bool first_person = false;
        // Experimental source-mesh hand-only rendering.
        bool first_person_tracked_hands = false;
        // First-person body: false = hands only, true = Lara's full body
        // (head removed) with her arms bent to reach the tracked hands.
        bool first_person_full_body = false;
        // 2: hands hang off SteamVR's grip pose (palm pivot, controller-local
        // tuning, left = mirror of right). 1: the original raw-pose model.
        int hand_model = 2;
        // Hold the eye at standing height over the ledge top during the
        // retail mantle's kneel (1), or follow the raw animation (0).
        bool first_person_mantle_smoothing = true;
        // Crouching the real body far enough holds the game's crouch button.
        bool physical_crouch = true;
        // One-hand catches (the game's quick "grab" prompt) secure
        // themselves in first person: no grab or button needed.
        bool auto_secure_catch = false;
        // MSAA cap in samples (0 off, 2, 4, 8) when the game's FSAA is on.
        int msaa = 4;
        // First-person world objects: true = drawn whenever their room is
        // visible (no object culling), false = culled against the wide
        // headset frustum (much cheaper on the CPU; the default since the
        // user's A/B test, 2026-10-05). F2 toggles it in game.
        bool first_person_object_culling_open = false;
        // Terrain strips: true = every strip of a visible room is drawn
        // (fixes small strips vanishing in VR), false = culled against the
        // wide headset frustum (cheaper, may bring that back). Shift+F2.
        bool terrain_culling_open = false; // frustum by default (user test 2026-10-05)
        // Depth of field: -1 = the game's own setting (launcher), 0 = off,
        // 1 = on. Applied when the game loads its graphics settings.
        int depth_of_field = 0;
        // Comfort vignette: 0 off, 1 while jumping/rolling, 2 on any
        // movement (walk, run, jump, roll, shimmy, climb, smooth turn).
        int comfort_vignette = 0;
        // Third-person camera preset: 0 classic (the game's camera,
        // smoothed), 1 shoulder (close behind Lara at 1:1, auto-centre),
        // 2 board (Lara a few inches tall on a tabletop world).
        int third_person_mode = 0;
        float third_person_smoothing = 0.12f;   // seconds
        float shoulder_distance = 1.3f;         // metres behind the focus
        float shoulder_height = 0.0f;           // metres above it
        float shoulder_side = 0.35f;            // metres to the right
        float board_lara_height_cm = 15.0f;     // Lara's apparent height
        float board_distance = 0.5f;            // table metres back
        float board_height = 0.35f;             // table metres up
        float board_hand_back = 0.08f;          // metres, fingertips->palm
        // Board-mode god-hand cheats (pinch pick-up, palm platform, finger
        // flick). Only with third_person_mode = board, immersive_controls
        // = 1 and first_person = 0 as well.
        bool board_god_hand = true;
        // Third-person camera collision: the board seat and the head-moved
        // eye stop short of geometry (0 = allow clipping into it).
        bool camera_collision = true;
        // First person: bullets go exactly where the gun points (0 = keep
        // the game's random spread).
        bool first_person_no_spread = true;
        // Mission Prep backdrop: size relative to the view (1 = exactly
        // fills it) and how slowly it turns after the head (seconds).
        float mission_prep_backdrop_scale = 1.15f;
        float mission_prep_backdrop_lag = 0.4f;
        // Visual recoil kick on each of Lara's shots (0 = off, 1 = default
        // strength, up to 2) and a controller vibration per shot.
        float weapon_recoil = 1.0f;
        bool weapon_haptics = true;
        // Extra height from Lara's HeadSegment joint to her eye position,
        // measured in game units. The joint is at neck/collar level.
        float first_person_eye_height = 70.0f;
        // Legacy INI setting retained for old installations. First person
        // now fixes Lara's eye offset in root space to remove idle sway.
        float first_person_smoothing = 0.08f;

        bool ui_passthrough = true;
        bool menu_world_locked = true;
        float ui_scale = 0.6f;
        // 1 follows the headset exactly; lower values leave part of the
        // current head angle in the gameplay HUD so edge elements move toward
        // the centre of view. 0 is fully world-relative. Menus have their own
        // policies.
        float hud_follow = 0.85f;

        // The safe production policy keeps portal/room culling bypassed and
        // widens only the engine's much cheaper object frustum.
        float cull_fov_degrees = 120.0f;

        // Remove the flat-screen aspect prescale (screenX/YRatio) that the
        // engine bakes into wcTransformf. Left in, it sits before the head
        // rotation and bends every view ray away from the true one as the
        // head pitches or turns.
        bool view_prescale_fix = true;

        bool controllers = true;
        // Opt-in body-relative gestures layered over the normal SteamVR
        // bindings. The standard bindings remain the default and fallback.
        bool immersive_controls = false;
        // Left-handed: long guns and the aim ray use the left controller and
        // the triggers swap (left fires; right does Action / precision aim).
        bool left_handed = false;
        // Gripping near a long gun's fore-end with the free hand steadies it:
        // the gun then points from the trigger hand toward that hand.
        bool two_handed_guns = true;
        // Immersive pistols: false = akimbo (either hip draws both), true =
        // single (each hip draws its own pistol; one pistol fires at half
        // the rate, both together are akimbo).
        bool single_pistols = false;
        // Controller aim ray: 0 = main hand; 1 = both hands averaged when
        // two pistols are drawn; 2 = those hand(s) averaged with the HMD.
        int aim_mode = 0;
        // First-person turning on the right stick: smooth (false) or snap
        // turns of snap_turn_degrees.
        bool snap_turn = false;
        float snap_turn_degrees = 30.0f;
        // 0 auto (proximity engages and releases, no button), 1 toggle
        // (free-hand grip press at the foregrip takes hold, press again to
        // let go), 2 hold (grip must stay pressed).
        int two_handed_mode = 0;
        // Foregrip distance ahead of the trigger palm along the barrel, and
        // the engage / release radii around it (metres). Release is wider so
        // a hand hovering at the edge does not flicker the hold.
        float two_handed_grip_distance = 0.30f;
        float two_handed_radius = 0.15f;
        float two_handed_release_radius = 0.22f;
        // First-person weapons aim along the right controller instead of
        // the headset centre ray.
        bool controller_aim = true;
        // Replace the game's flat combat reticles with a 3D crosshair at the
        // point the shot will actually hit (first person only).
        bool vr_crosshair = true;
        float look_speed = 900.0f;
        bool look_vertical = true;
    };

    const Config& config();
    void load_config();
    unsigned config_defaults_added();
}
