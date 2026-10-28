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
        // OpenXR: a runtime manifest to use instead of the registered
        // 32-bit one (XR_RUNTIME_JSON), and whether each frame's copy
        // into the runtime's swapchain is waited for before it is handed
        // over.
        // VR backend (1.1.0): 0 auto (OpenXR, falling back to OpenVR when
        // openxr_loader.dll is missing or no OpenXR runtime starts),
        // 1 OpenXR only, 2 OpenVR / SteamVR only.
        int vr_runtime = 0;
        wchar_t xr_runtime_json[MAX_PATH] = L"";
        bool xr_copy_wait = true;
        // OpenXR: convert the controllers' grip pose to the OpenVR grip pose
        // the hand tuning was made against (known for Reverb G2 and Touch).
        bool xr_grip_match_openvr = true;
        bool stereo = true;
        bool stereo_same_frame = true;
        bool stereo_lock_pose = true;

        bool head_tracking = true;
        bool head_rotation = true;
        bool head_position = true;
        bool hmd_drives_camera = true;
        bool hmd_drives_position = true;
        // Roomscale (2026-10-09): in first person on the ground, walking in
        // the room moves Lara (beyond a lean deadzone, metres), with the
        // game's collision probe stopping her -- and the view -- at walls.
        bool roomscale = true;
        float roomscale_deadzone = 0.15f;
        // Roomscale walk (2026-10-10): a real step of more than 0.4 m is
        // walked by the game itself (walk key + direction keys), so Lara
        // turns and walks with her own animations.
        bool roomscale_walk = true;
        // Wall touch (user 2026-10-10, as in the newer Tomb Raider games):
        // in third person, walking beside a wall with nothing drawn, Lara
        // reaches out and runs her near hand along it. Up to
        // wall_touch_max_speed metres per second (walking, not running).
        bool wall_touch = true;
        // Stereo water reflection (2026-10-11): the game's planar reflection
        // drawn once per eye. Needs Reflections on in the Setup dialog.
        // Experimental and off: no visible difference was confirmed, and it
        // draws the reflection twice.
        bool water_reflection = false;
        // Water Effects (underwater / surface distortion) per eye: the
        // screen copy it samples is split by eye. Needs Water Effects on in
        // the Setup dialog. Experimental and off: user test 2026-10-11 still
        // showed it in one eye, following the head (the full-screen
        // distortion layer, FXDRAW_WaterFX 0x00446D90, is not understood
        // well enough yet).
        bool water_effects = false;
        float wall_touch_max_speed = 2.6f;
        // Hardcore first person (2026-10-10, a user's request): the view
        // turns with Lara's body through flips, rolls and spins instead of
        // staying level.
        bool first_person_hardcore = false;
        // Tilts under this many degrees stay level (walking); it eases in
        // to a full follow at three times it.
        float first_person_hardcore_threshold = 20.0f;
        // First person: the hands (and what they hold) stop at walls and
        // the ground instead of passing through (2026-10-09; off by default
        // since 2026-10-10 at the user's request).
        bool hand_collision = false;
        // First person: the hands cannot go into Lara's own torso.
        bool hand_body_collision = true;
        // Torso spring (2026-10-10, first-person full body): her chest
        // sways on a damped spring about the neck -- rotation only, at most
        // body_spring_angle degrees -- from turning, the walk's bob and the
        // player's hands pressing on it.
        bool body_spring = true;
        float body_spring_angle = 4.0f;
        // Soft parts (2026-10-10): the chest's own front children (her
        // breast bones in the skeleton dump) bounce on their own springs,
        // mostly up and down, and give under the hands; at most
        // body_soft_angle degrees.
        bool body_soft = true;
        float body_soft_angle = 14.0f;
        // Undocumented: size of the soft parts (1 = as modelled).
        float body_soft_scale = 1.0f;
        // F1..F9 developer keys (0 frees them for other mods' menus).
        bool function_keys = true;
        // Other mods' Dear ImGui menus shown as a VR panel.
        bool imgui_panel = true;
        // Third person (2026-10-14): how loose the soft parts are sideways
        // -- their side-to-side frequency in Hz (lower swings wider and
        // slower; up and down stays at 2.2) -- and how quickly a swing
        // dies away (damping ratio; lower rings longer).
        float body_soft_sway_hz = 3.0f;
        float body_soft_damping = 0.53f;
        // Soft parts' shape (2026-10-14): squash and stretch along the bone
        // (0 none .. 1 strong, volume kept) and how much of a lighter second
        // mass's overrun -- the ripple on top of the swing -- is shown.
        float body_soft_stretch = 0.35f;
        float body_soft_ripple = 0.3f;
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
        bool first_person_tracked_hands = true;
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
        // One-hand catches secured automatically in first person:
        // 0 off, 1 swing bars only, 2 ledges and bars.
        int auto_secure_catch = 0;
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
        // Fullscreen Effects (the game's glow/bloom pass, which reads the
        // two-eye target as one image): same choices, off by default.
        int fullscreen_effects = 0;
        // The game's FSAA (anti-aliasing): on by default; its strength is
        // capped by msaa.
        int fsaa = 1;
        // Comfort vignette: 0 off, 1 while jumping/rolling, 2 on any
        // movement (walk, run, jump, roll, shimmy, climb, smooth turn).
        int comfort_vignette = 0;
        // Third-person camera preset: 0 classic (the game's camera,
        // smoothed), 1 shoulder (close behind Lara at 1:1, auto-centre),
        // 2 board (Lara a few inches tall on a tabletop world).
        int third_person_mode = 0;
        // third_person_mode = all: start in classic, and the view switch
        // cycles classic -> shoulder -> board -> first person (2026-10-06).
        bool view_cycle_all = true;
        float third_person_smoothing = 0.12f;   // seconds
        // Seconds a view switch takes to glide from one view to the next
        // (position and world scale); 0 cuts.
        float view_transition_seconds = 0.3f;
        // Picture (2026-10-14): brightness (-0.5 .. 0.5, added) and contrast
        // (0.5 .. 2, about mid grey) on the frame sent to the headset, and
        // whether the game's own brightness / contrast sliders (its gamma
        // ramp, ignored by Windows in a window) are applied too.
        float picture_brightness = 0.0f;
        float picture_contrast = 1.0f;
        bool picture_follow_game = true;
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
        // God-hand pick-up (board mode with board_god_hand only). Zones are
        // metres at her size: head grip zone, claim zone (a hand this close
        // keeps its grip/trigger from the game) and arm grip zone (0 = no
        // arm grab). god_arm_lead: a held arm leads until pulled past her
        // reach. god_hang_arm: Legend's old dangle by the wrist. god_throw:
        // letting go mid-swing throws her (speed x strength, own gravity);
        // god_land_anim plays that animation on landing (-1 none);
        // god_drop_height caps the fall when put down over a floor.
        float god_head_zone = 0.3f;
        float god_claim_zone = 0.6f;
        float god_arm_zone = 0.25f;
        bool god_arm_lead = true;
        bool god_hang_arm = false;
        bool god_arms_limp = false;
        bool god_throw = true;
        float god_throw_strength = 1.0f;
        float god_throw_gravity = 9.81f;
        int god_land_anim = -1;
        float god_drop_height = 1.0f;
        // god_grab_pose: grabs come from the hand's pose -- a pinch (trigger
        // + thumb down) or a fist (trigger + grip); grip alone only points
        // and presses. 0 = the old grip-button grabs.
        bool god_grab_pose = true;
        // Experimental: grab her hand or arm and drag her up like a
        // ragdoll (arm zones, procedural IK hang). Off: the head grab with
        // Legend's own dangle animation.
        bool god_drag_hand = false;
        // Whole-body push (god mode): a hand leaning on her tilts head,
        // chest, waist, arms, hips and legs by up to these degrees on a
        // spring (hz, damping), scaled by body_push_strength.
        bool body_push = true;
        float body_push_strength = 1.0f;
        float body_push_hz = 2.5f;
        float body_push_damping = 0.9f;
        float body_push_head_degrees = 6.0f;
        float body_push_chest_degrees = 4.0f;
        float body_push_waist_degrees = 3.0f;
        float body_push_arm_degrees = 8.0f;
        float body_push_hips_degrees = 3.0f;
        float body_push_leg_degrees = 2.0f;
        // A hand on a soft part (chest or rear) pushes her whole body only
        // this share as hard as one on the rest of her (0 = not at all): the
        // soft part gives, the body barely does.
        float body_push_soft = 0.0f;
        // Rear soft body: the buttocks as masses on springs (hz, damping,
        // gain, max_cm of lag), the share of motion kept per axis, the
        // region's size/shape/placement, the dent a fingertip makes, and two
        // tuning aids (debug lines, test push).
        bool body_rear = true;
        float body_rear_hz = 4.0f;
        float body_rear_damping = 0.47f;
        float body_rear_gain = 0.55f;
        float body_rear_max_cm = 3.7f;
        float body_rear_side = 0.6f;
        float body_rear_back = 1.0f;
        float body_rear_up = 1.0f;
        // Baseline fullness (cm at her size, negative = slimmer): how far
        // the cheeks stand out at rest, along the same falloff. The size,
        // width and height only change the region that moves.
        float body_rear_fullness_cm = 1.25f;
        // The absolute most any point of the cheeks moves from rest, cm at
        // her size, whatever the gain or a hand's pull (limit x tanh(move /
        // limit): eased toward it all the way, never stopping dead at it).
        float body_rear_limit_cm = 1.5f;
        float body_rear_size = 0.8f;
        float body_rear_width = 1.21f;
        float body_rear_height = 0.8f;
        float body_rear_spread_cm = 4.6f;
        float body_rear_core = 0.36f;
        float body_rear_lift_cm = -0.2f;
        float body_rear_inward = 0.5f;
        float body_rear_touch_cm = 0.0f;
        float body_rear_cleft_cm = 0.0f;
        float body_rear_cleft_width_cm = 2.0f;
        float body_rear_seam_cm = 1.0f;
        bool body_rear_debug = false;
        float body_rear_test_cm = 0.0f;
        float body_rear_dent = 0.5f;
        // Third-person camera collision: the board seat and the head-moved
        // eye stop short of geometry (0 = allow clipping into it).
        bool camera_collision = true;
        // First person: bullets go exactly where the gun points (0 = keep
        // the game's random spread).
        bool first_person_no_spread = true;
        // Visual recoil kick on each of Lara's shots (0 = off, 1 = default
        // strength, up to 2) and a controller vibration per shot.
        float weapon_recoil = 1.0f;
        bool weapon_haptics = true;
        // The game's own gamepad rumble (screen shakes, explosions) played
        // on both VR controllers (2026-10-08).
        bool shake_rumble = true;
        // Cutscenes (2026-10-09): false = as before (the head turns the
        // cutscene camera), true = theatre -- the cutscene is seen through
        // a window fixed in the room, in a black space, still 3D (leaning
        // looks round into the scene). [vr] cutscene_mode = normal/theatre.
        bool cutscene_theatre = false;
        float cutscene_screen_width = 3.0f;      // metres
        // metres; 0 = 16:9 from the width. The corner handle sets both.
        float cutscene_screen_height = 0.0f;
        float cutscene_screen_distance = 2.5f;   // metres ahead
        // Scene scale in the theatre: 0 = auto (each shot scaled so Lara
        // sits at the screen), else a fixed factor (1 = life size).
        float cutscene_scale = 0.0f;
        // Gameplay theatre (2026-10-10): third-person play (not board mode)
        // through the same room-fixed window. Off by default.
        bool gameplay_theatre = false;
        // The room round the window, 0xRRGGBB: black, or a chroma-key
        // colour for headsets/streamers that swap it for passthrough.
        unsigned theatre_backdrop = 0x000000;
        // Board-mode diorama (2026-10-10): the tabletop world seen through
        // a box with open front and top; outside it the room is dark
        // (theatre_backdrop). Metres; the box is centred on Lara's spot and
        // reaches board_diorama_below under her floor.
        bool board_diorama = false;
        float board_diorama_width = 0.6f;
        float board_diorama_depth = 0.6f;
        float board_diorama_height = 0.45f;
        float board_diorama_below = 0.15f;
        // Extra height from Lara's HeadSegment joint to her eye position,
        // measured in game units. The joint is at neck/collar level.
        float first_person_eye_height = 70.0f;
        // Legacy INI setting retained for old installations. First person
        // now fixes Lara's eye offset in root space to remove idle sway.
        float first_person_smoothing = 0.08f;

        bool ui_passthrough = true;
        bool menu_world_locked = true;
        // The in-game pause menu: true pins it
        // level in the room at the heading the head had when it opened.
        bool pause_world_locked = true;
        // Hide the gameplay HUD (health, ammo, prompts) outright instead of
        // letting it fade. World markers and the binocular view stay.
        bool hide_gameplay_hud = false;
        float ui_scale = 0.6f;
        // 1 follows the headset exactly; lower values leave part of the
        // current head angle in the gameplay HUD so edge elements move toward
        // the centre of view. 0 is fully world-relative. Menus have their own
        // policies.
        float hud_follow = 0.85f;

        // The settings overlay in the headset: a square panel placed in the
        // room in front of the head when it opens (and at a recentre or a
        // change of these), distance metres away, width degrees wide, turned
        // yaw degrees to the left and pitch degrees up. Holding the menu
        // button open_hold seconds opens it (0 = off: the button always goes
        // to the game).
        float overlay_vr_distance = 1.0f;
        float overlay_vr_width_degrees = 38.0f;
        float overlay_vr_yaw_degrees = 15.0f;
        float overlay_vr_pitch_degrees = -10.0f;
        float overlay_vr_open_hold = 0.6f;

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
        bool immersive_controls = true;
        // Gear (medipack, binoculars, weapon switch, light) in immersive
        // third person: false = the 3D gear cross, true = the radial menu
        // (left stick long press, then a stick direction, with the game's
        // own flat cross shown). Immersive off always uses the radial menu.
        // [controls] gear_menu = cross / radial (2026-10-09).
        bool gear_menu_radial = true;
        // Analog walk (2026-10-10): the game only gets keys, so the stick
        // was run-or-nothing. A light push also holds the game's Walk key
        // (input slot 20 Left Shift or 21 Left Ctrl, whichever is Walk).
        bool stick_walk = true;
        // Finger tracking (2026-10-10): Lara's fingers follow SteamVR's
        // skeletal input (controller finger sensors, or Quest / Index hand
        // tracking), from her model's straight rest pose instead of her
        // animation. finger_rest_curl is how curled that pose already is.
        bool finger_tracking = false;   // off by default (user 2026-10-10)
        float finger_rest_curl = 0.0f;
        float stick_walk_threshold = 0.7f;   // stick below this walks
        unsigned short walk_vk = 0xA0;       // VK_LSHIFT
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
        // Smooth turning speed at full stick, degrees per second.
        float smooth_turn_speed = 120.0f;
        // Motorbike handlebars (first person, immersive): 0 off, 1 grab
        // for show (the stick rides), 2 the hands steer (stick throttle),
        // 3 the hands steer and the right grip twists the throttle.
        int bike_handlebars = 1;
        // Hand travel (metres) for full steering lock, and twist (degrees)
        // for full throttle.
        float bike_steer_range = 0.10f;
        float bike_twist_range = 30.0f;
        // Draw the handlebar grab points (rings).
        bool bike_handlebar_markers = false;
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
    // For the in-game settings overlay: the live values, and one key
    // written to trlvr.ini. Everything else reads config().
    Config& config_edit();
    void config_save_setting(const wchar_t* section, const wchar_t* key,
                             const wchar_t* value);
    // Hardcore first person from the pause menu: set and saved.
    void config_set_first_person_hardcore(bool on);
    // The other VR Settings toggles (pause menu, 2026-10-10): each sets the
    // live value and saves it to trlvr.ini.
    void config_set_left_handed(bool on);
    // Theatre window / board diorama size from their corner handles
    // (metres); `save` writes trlvr.ini (at the end of a drag).
    void config_set_theatre_size(float width, float height, bool save);
    void config_set_diorama_size(float width, float height, float below,
                                 bool save);
    // 0 off, 1 cutscenes, 2 cutscenes and gameplay.
    void config_set_theatre(int mode);
    // 0 classic, 1 shoulder, 2 board, 3 all (the view switch cycles them).
    void config_set_third_person_mode(int mode);
    void config_set_full_body(bool on);
    // 0 both, 1 stick, 2 sliders.
    // Hands against the world and Lara's body.
    void config_set_hand_collision(bool world, bool body);
    void load_config();
    unsigned config_defaults_added();
}
