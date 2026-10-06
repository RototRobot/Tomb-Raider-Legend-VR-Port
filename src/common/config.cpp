#include "config.h"
#include "log.h"
#include <cmath>

#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <vector>

namespace trlvr
{
    namespace
    {
        Config g_config;
        unsigned g_defaults_added = 0;

        struct IniDefault
        {
            const wchar_t* section;
            const wchar_t* key;
            const wchar_t* value;
        };

        // Keep this list in step with kTemplate and the settings read below.
        // Existing values are never rewritten; this only migrates older INIs
        // forward when a release introduces a new setting.
        const IniDefault kDefaults[] = {
            { L"general", L"backend", L"auto" },
            { L"general", L"logging", L"1" },
            { L"general", L"force_windowed", L"1" },
            { L"general", L"window_width", L"0" },
            { L"general", L"window_height", L"0" },
            { L"general", L"window_borderless", L"0" },
            { L"general", L"release_cursor_unfocused", L"1" },

            { L"vr", L"enabled", L"1" },
            { L"vr", L"submit", L"1" },
            { L"vr", L"stereo", L"1" },
            { L"vr", L"stereo_same_frame", L"1" },
            { L"vr", L"stereo_lock_pose", L"1" },
            { L"vr", L"head_tracking", L"1" },
            { L"vr", L"head_rotation", L"1" },
            { L"vr", L"head_position", L"1" },
            { L"vr", L"hmd_drives_camera", L"1" },
            { L"vr", L"hmd_drives_position", L"1" },
            { L"vr", L"hmd_aim", L"1" },
            { L"vr", L"world_scale", L"291" },
            { L"vr", L"head_rotation_scale", L"1.0" },
            { L"vr", L"head_position_scale", L"1.0" },
            { L"vr", L"level_horizon", L"1" },
            { L"vr", L"camera_auto_center", L"0" },
            { L"vr", L"camera_shake", L"0" },
            { L"vr", L"first_person", L"0" },
            { L"vr", L"first_person_tracked_hands", L"0" },
            { L"vr", L"first_person_body", L"hands" },
            { L"vr", L"hand_model", L"2" },
            { L"vr", L"first_person_mantle_smoothing", L"1" },
            { L"vr", L"physical_crouch", L"1" },
            { L"vr", L"auto_secure_catch", L"off" },
            { L"vr", L"msaa", L"4" },
            { L"vr", L"first_person_object_culling", L"frustum" },
            { L"vr", L"terrain_culling", L"frustum" },
            { L"vr", L"depth_of_field", L"off" },
            { L"vr", L"comfort_vignette", L"off" },
            { L"vr", L"third_person_mode", L"classic" },
            { L"vr", L"third_person_smoothing", L"0.12" },
            { L"vr", L"shoulder_distance", L"1.3" },
            { L"vr", L"shoulder_height", L"0.0" },
            { L"vr", L"shoulder_side", L"0.35" },
            { L"vr", L"board_lara_height_cm", L"15" },
            { L"vr", L"board_distance", L"0.5" },
            { L"vr", L"board_height", L"0.35" },
            { L"vr", L"board_hand_back", L"0.08" },
            { L"vr", L"board_god_hand", L"1" },
            { L"vr", L"camera_collision", L"1" },
            { L"vr", L"first_person_no_spread", L"1" },
            { L"vr", L"mission_prep_backdrop_scale", L"1.15" },
            { L"vr", L"mission_prep_backdrop_lag", L"0.4" },
            { L"vr", L"weapon_recoil", L"1.0" },
            { L"vr", L"weapon_haptics", L"1" },
            { L"vr", L"first_person_eye_height", L"70" },
            { L"vr", L"first_person_smoothing", L"0.08" },
            { L"vr", L"ui_passthrough", L"1" },
            { L"vr", L"menu_world_locked", L"1" },
            { L"vr", L"ui_scale", L"0.6" },
            { L"vr", L"hud_follow", L"0.85" },
            { L"vr", L"cull_fov_degrees", L"120" },

            { L"controls", L"controllers", L"1" },
            { L"controls", L"immersive_controls", L"0" },
            { L"controls", L"left_handed", L"0" },
            { L"controls", L"two_handed_guns", L"1" },
            { L"controls", L"pistol_mode", L"akimbo" },
            { L"controls", L"aim_mode", L"hand" },
            { L"controls", L"turn_mode", L"smooth" },
            { L"controls", L"snap_turn_degrees", L"30" },
            { L"controls", L"two_handed_mode", L"auto" },
            { L"controls", L"two_handed_grip_distance", L"0.30" },
            { L"controls", L"two_handed_radius", L"0.15" },
            { L"controls", L"two_handed_release_radius", L"0.22" },
            { L"controls", L"controller_aim", L"1" },
            { L"controls", L"vr_crosshair", L"1" },
            { L"vr", L"view_prescale_fix", L"1" },
            { L"controls", L"look_speed", L"900" },
            { L"controls", L"look_vertical", L"1" },

            { L"developer", L"hand_left_x", L"0" },
            { L"developer", L"hand_left_y", L"0" },
            { L"developer", L"hand_left_z", L"0" },
            { L"developer", L"hand_right_x", L"0" },
            { L"developer", L"hand_right_y", L"0" },
            { L"developer", L"hand_right_z", L"0" },
            { L"developer", L"hand_left_rot_x", L"0" },
            { L"developer", L"hand_left_rot_y", L"0" },
            { L"developer", L"hand_left_rot_z", L"0" },
            { L"developer", L"hand_right_rot_x", L"0" },
            { L"developer", L"hand_right_rot_y", L"0" },
            { L"developer", L"hand_right_rot_z", L"0" },
            { L"developer", L"hand_tuning_debug", L"0" },
            { L"developer", L"first_person_belt_height", L"12" },
            { L"developer", L"ledge_grab_height", L"0.12" },
            { L"developer", L"ledge_grab_debug_draw", L"0" },
            { L"developer", L"holster_left_x", L"-0.24" },
            { L"developer", L"holster_right_x", L"0.24" },
            { L"developer", L"holster_y", L"-0.63" },
            { L"developer", L"holster_z", L"-0.07" },
            { L"developer", L"holster_radius_x", L"0.13" },
            { L"developer", L"holster_radius_y", L"0.15" },
            { L"developer", L"holster_radius_z", L"0.18" },
            { L"developer", L"holster_debug_draw", L"0" },
            { L"developer", L"gear_tuning", L"0" },
            { L"developer", L"aim_tuning_debug", L"0" },
            { L"developer", L"grip_prompt_scale", L"0.25" },
        };

        const wchar_t* kTemplate =
            L"; Tomb Raider: Legend VR settings\r\n"
            L"; Delete this file to recreate the current defaults.\r\n"
            L"; Rendering resolution is controlled by SteamVR's per-application setting.\r\n"
            L"; Restart the game after changing it.\r\n"
            L"\r\n"
            L"[general]\r\n"
            L"; auto uses d3d9_dxvk.dll when installed; dxvk requires it; system is flat test mode.\r\n"
            L"backend = auto\r\n"
            L"logging = 1\r\n"
            L"force_windowed = 1\r\n"
            L"; 0 fits the mirror window to the monitor. Set both for an exact client size.\r\n"
            L"window_width = 0\r\n"
            L"window_height = 0\r\n"
            L"window_borderless = 0\r\n"
            L"release_cursor_unfocused = 1\r\n"
            L"\r\n"
            L"[vr]\r\n"
            L"; 0 is the untouched flat-game baseline.\r\n"
            L"enabled = 1\r\n"
            L"submit = 1\r\n"
            L"stereo = 1\r\n"
            L"; Both eyes in one game frame. Keep Depth of Field and Fullscreen Effects off.\r\n"
            L"stereo_same_frame = 1\r\n"
            L"stereo_lock_pose = 1\r\n"
            L"\r\n"
            L"head_tracking = 1\r\n"
            L"head_rotation = 1\r\n"
            L"head_position = 1\r\n"
            L"hmd_drives_camera = 1\r\n"
            L"hmd_drives_position = 1\r\n"
            L"hmd_aim = 1\r\n"
            L"; Lara measures about 510 game units / 1.75 metres.\r\n"
            L"world_scale = 291\r\n"
            L"head_rotation_scale = 1.0\r\n"
            L"head_position_scale = 1.0\r\n"
            L"level_horizon = 1\r\n"
            L"camera_auto_center = 0\r\n"
            L"camera_shake = 0\r\n"
            L"; EXPERIMENTAL: move the view to Lara's head and hide her head render group.\r\n"
            L"; 0 preserves the normal third-person game exactly.\r\n"
            L"first_person = 0\r\n"
            L"; Experimental hands cut from Lara's source mesh at controller poses.\r\n"
            L"first_person_tracked_hands = 0\r\n"
            L"; hands = only Lara's hands are drawn in first person; full = her\r\n"
            L"; whole body (head removed), arms bent to reach the controllers.\r\n"
            L"first_person_body = hands\r\n"
            L"; 2 = hands pivot in the palm on SteamVR's grip pose; the left hand\r\n"
            L"; mirrors the tuned right hand. 1 = the original hand placement.\r\n"
            L"hand_model = 2\r\n"
            L"; 1 keeps the view at standing height while Lara kneels at the top\r\n"
            L"; of a ledge pull-up; 0 follows the game's raw mantle animation.\r\n"
            L"first_person_mantle_smoothing = 1\r\n"
            L"; 1 = crouching for real (about 35 cm down) makes Lara crouch.\r\n"
            L"physical_crouch = 1\r\n"
            L"; One-hand catches (the game's quick grab prompt) in first person:\r\n"
            L"; off = grab the hold to secure it, bars = swing bars secure\r\n"
            L"; themselves, all = ledges and bars secure themselves.\r\n"
            L"auto_secure_catch = off\r\n"
            L"; Most anti-aliasing samples the game may use when its FSAA option\r\n"
            L"; is on: 0 (off), 2, 4 or 8. The game picks the most offered; 8 on\r\n"
            L"; a VR-sized image costs the GPU heavily. Takes effect at start-up.\r\n"
            L"msaa = 4\r\n"
            L"; First-person object culling: frustum culls objects against the wide\r\n"
            L"; headset view (much faster); open draws every object in a visible\r\n"
            L"; room, also behind you. F2 switches it in game.\r\n"
            L"first_person_object_culling = frustum\r\n"
            L"; Terrain culling: frustum culls terrain strips against the wide\r\n"
            L"; headset view (much faster); open draws every strip of a visible\r\n"
            L"; room (try it if small terrain pieces ever vanish). Shift+F2.\r\n"
            L"terrain_culling = frustum\r\n"
            L"; Depth of field: off (recommended in VR: it is a flat-screen blur\r\n"
            L"; pass), on, or game (keep the launcher's setting). Read at start.\r\n"
            L"depth_of_field = off\r\n"
            L"; Darkens the edge of view: off, jumps (jumping and rolling) or\r\n"
            L"; full (any movement, shimmying, climbing and smooth turning).\r\n"
            L"comfort_vignette = off\r\n"
            L"; Third-person camera: classic (the game's camera, smoothed),\r\n"
            L"; shoulder (close behind Lara, life size) or board (Lara a few\r\n"
            L"; inches tall on a tabletop world); all = start in classic and let\r\n"
            L"; the view switch cycle classic, shoulder, board, first person.\r\n"
            L"third_person_mode = classic\r\n"
            L"; Seconds the third-person camera position takes to catch up.\r\n"
            L"third_person_smoothing = 0.12\r\n"
            L"; Shoulder camera, metres: behind, above and to the right.\r\n"
            L"shoulder_distance = 1.3\r\n"
            L"shoulder_height = 0.0\r\n"
            L"shoulder_side = 0.35\r\n"
            L"; Board camera: Lara's apparent height (cm), and where you sit\r\n"
            L"; from her in table metres (back, up).\r\n"
            L"board_lara_height_cm = 15\r\n"
            L"board_distance = 0.5\r\n"
            L"board_height = 0.35\r\n"
            L"; Board mode: metres the hands sit back along the palm.\r\n"
            L"board_hand_back = 0.08\r\n"
            L"; Board mode cheats: pinch Lara up, stand her on a palm, flick\r\n"
            L"; her over (hold Y). Needs board mode, immersive controls and\r\n"
            L"; third person too.\r\n"
            L"board_god_hand = 1\r\n"
            L"; Third-person camera collision: the view stops short of rock\r\n"
            L"; and walls instead of passing into them (0 = allow clipping).\r\n"
            L"camera_collision = 1\r\n"
            L"; First person: no random bullet spread -- shots go exactly where\r\n"
            L"; the gun points (0 = keep the game's spread).\r\n"
            L"first_person_no_spread = 1\r\n"
            L"; Backdrop behind the Mission Prep menu: size relative to the view\r\n"
            L"; (1 = exactly fills it) and how slowly it follows the head (s).\r\n"
            L"mission_prep_backdrop_scale = 1.15\r\n"
            L"mission_prep_backdrop_lag = 0.4\r\n"
            L"; Visual gun kick per shot: 0 = off, 1 = default, up to 2.\r\n"
            L"weapon_recoil = 1.0\r\n"
            L"; 1 = the firing controller vibrates on each shot.\r\n"
            L"weapon_haptics = 1\r\n"
            L"; Eye height above Lara's HeadSegment joint, in game units.\r\n"
            L"first_person_eye_height = 70\r\n"
            L"; Legacy setting: the eye anchor now stays fixed relative to Lara's root.\r\n"
            L"first_person_smoothing = 0.08\r\n"
            L"\r\n"
            L"ui_passthrough = 1\r\n"
            L"; Menus stay where they opened; the gameplay HUD follows the headset.\r\n"
            L"menu_world_locked = 1\r\n"
            L"ui_scale = 0.6\r\n"
            L"; 1.0 is rigidly head-locked; 0.85 gives the gameplay HUD gentle drag.\r\n"
            L"hud_follow = 0.85\r\n"
            L"\r\n"
            L"; Object visibility only. Room culling is conservatively bypassed for VR.\r\n"
            L"cull_fov_degrees = 120\r\n"
            L"; Draw the world at true proportions (removes the game's flat-screen\r\n"
            L"; aspect prescale). F7 toggles it live for comparison.\r\n"
            L"view_prescale_fix = 1\r\n"
            L"\r\n"
            L"[controls]\r\n"
            L"controllers = 1\r\n"
            L"; 1 enables body-relative gestures. Chest + left grip toggles the personal light.\r\n"
            L"immersive_controls = 0\r\n"
            L"; 1 = left-handed: long guns and aiming follow the left controller,\r\n"
            L"; the left trigger fires and the right trigger does Action/aim.\r\n"
            L"left_handed = 0\r\n"
            L"; 1 = grip with the free hand near a long gun's front to hold it\r\n"
            L"; two-handed; the gun and aim then pivot toward that hand.\r\n"
            L"two_handed_guns = 1\r\n"
            L"; akimbo = either hip draws both pistols; single = each hip draws\r\n"
            L"; its own pistol, fired by that hand's trigger (one pistol fires at\r\n"
            L"; half the rate; draw both for akimbo).\r\n"
            L"pistol_mode = akimbo\r\n"
            L"; hand = aim with the main hand; hands = with two pistols drawn, aim\r\n"
            L"; between both hands; hands_head = the hand(s) averaged with where\r\n"
            L"; the headset looks (one gun: half hand, half headset).\r\n"
            L"aim_mode = hand\r\n"
            L"; smooth = the right stick turns continuously; snap = it turns in\r\n"
            L"; steps of snap_turn_degrees (first person).\r\n"
            L"turn_mode = smooth\r\n"
            L"snap_turn_degrees = 30\r\n"
            L"; auto = bring the free hand to the foregrip to hold, move it away\r\n"
            L"; to let go; toggle = press its grip there to hold, press again to\r\n"
            L"; let go; hold = keep its grip pressed.\r\n"
            L"two_handed_mode = auto\r\n"
            L"; Foregrip distance ahead of the trigger hand, and the engage /\r\n"
            L"; release radius around it, in metres.\r\n"
            L"two_handed_grip_distance = 0.30\r\n"
            L"two_handed_radius = 0.15\r\n"
            L"two_handed_release_radius = 0.22\r\n"
            L"; First person: aim weapons with the right controller, not the headset.\r\n"
            L"controller_aim = 1\r\n"
            L"; First person: 3D crosshair at the impact point replaces the game's reticles.\r\n"
            L"vr_crosshair = 1\r\n"
            L"look_speed = 900\r\n"
            L"look_vertical = 1\r\n"
            L"\r\n"
            L"[developer]\r\n"
            L"; Headset-local hand position in game units; controller-relative rotation in degrees.\r\n"
            L"; Rotation order is X, Y, Z. Press F5 to reload.\r\n"
            L"hand_left_x = 0\r\n"
            L"hand_left_y = 0\r\n"
            L"hand_left_z = 0\r\n"
            L"hand_right_x = 0\r\n"
            L"hand_right_y = 0\r\n"
            L"hand_right_z = 0\r\n"
            L"hand_left_rot_x = 0\r\n"
            L"hand_left_rot_y = 0\r\n"
            L"hand_left_rot_z = 0\r\n"
            L"hand_right_rot_x = 0\r\n"
            L"hand_right_rot_y = 0\r\n"
            L"hand_right_rot_z = 0\r\n"
            L"; Controller crosshairs and numpad live alignment controls.\r\n"
            L"hand_tuning_debug = 0\r\n"
            L"; Visual height of player-facing holstered gear in game units. F5 reloads.\r\n"
            L"; Positive moves it up; this does not move the grip zones.\r\n"
            L"first_person_belt_height = 12\r\n"
            L"; Height above Lara's animated wrist used for ledge grip, in metres. F5 reloads.\r\n"
            L"ledge_grab_height = 0.12\r\n"
            L"; Show the shared ledge/vine grip box while climbing. F5 reloads.\r\n"
            L"ledge_grab_debug_draw = 0\r\n"
            L"; Holster grab zones in metres relative to headset yaw. F5 reloads.\r\n"
            L"; Cyan: ready; yellow: hand inside; magenta: guns drawn.\r\n"
            L"; Click either controller grip in a zone to draw or holster both pistols.\r\n"
            L"holster_left_x = -0.24\r\n"
            L"holster_right_x = 0.24\r\n"
            L"holster_y = -0.63\r\n"
            L"holster_z = -0.07\r\n"
            L"holster_radius_x = 0.13\r\n"
            L"holster_radius_y = 0.15\r\n"
            L"holster_radius_z = 0.18\r\n"
            L"holster_debug_draw = 0\r\n"
            L"; 1 enables gear placement: double-click the right stick (or F6) in\r\n"
            L"; first person, then place pistols, binoculars/grapple and the light\r\n"
            L"; with the triggers. The positions save here automatically.\r\n"
            L"gear_tuning = 0\r\n"
            L"; 1 shows the controller aim laser in first person. Numpad 8/2 pitch,\r\n"
            L"; 4/6 yaw, 5 reset, 7 saves aim_pitch/aim_yaw. F5 reloads.\r\n"
            L"aim_tuning_debug = 0\r\n"
            L"; Size of the secure-grip prompt shown on the left hand, as a\r\n"
            L"; fraction of its retail size. F5 reloads.\r\n"
            L"grip_prompt_scale = 0.25\r\n";

        bool read_bool(const wchar_t* section, const wchar_t* key,
                       bool fallback, const wchar_t* path)
        {
            return GetPrivateProfileIntW(section, key, fallback ? 1 : 0, path) != 0;
        }

        float read_float(const wchar_t* section, const wchar_t* key,
                         const wchar_t* fallback, const wchar_t* path)
        {
            wchar_t text[64]{};
            GetPrivateProfileStringW(section, key, fallback, text, 64, path);
            return (float)_wtof(text);
        }

        void add_missing_defaults(const wchar_t* path)
        {
            static const wchar_t* missing = L"{TRLVR-SETTING-NOT-PRESENT}";
            wchar_t current[128]{};

            g_defaults_added = 0;
            for (const IniDefault& setting : kDefaults)
            {
                GetPrivateProfileStringW(setting.section, setting.key,
                    missing, current, _countof(current), path);
                if (wcscmp(current, missing) == 0 &&
                    WritePrivateProfileStringW(setting.section, setting.key,
                                               setting.value, path))
                {
                    ++g_defaults_added;
                }
            }
        }
    }

    const Config& config()
    {
        return g_config;
    }

    void load_config()
    {
        wchar_t path[MAX_PATH]{};
        swprintf_s(path, L"%strlvr.ini", exe_dir());

        if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
        {
            FILE* f = nullptr;
            _wfopen_s(&f, path, L"wb");
            if (f)
            {
                const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
                fwrite(bom, 1, sizeof(bom), f);
                const int bytes = WideCharToMultiByte(CP_UTF8, 0, kTemplate,
                    -1, nullptr, 0, nullptr, nullptr);
                if (bytes > 1)
                {
                    std::vector<char> utf8((size_t)bytes);
                    WideCharToMultiByte(CP_UTF8, 0, kTemplate, -1,
                        utf8.data(), bytes, nullptr, nullptr);
                    fwrite(utf8.data(), 1, (size_t)bytes - 1, f);
                }
                fclose(f);
            }
        }

        add_missing_defaults(path);

        GetPrivateProfileStringW(L"general", L"backend", L"auto",
                                 g_config.backend, MAX_PATH, path);
        g_config.logging = read_bool(L"general", L"logging", true, path);
        g_config.force_windowed = read_bool(L"general", L"force_windowed", true, path);
        g_config.window_width = GetPrivateProfileIntW(L"general", L"window_width", 0, path);
        g_config.window_height = GetPrivateProfileIntW(L"general", L"window_height", 0, path);
        g_config.window_borderless = read_bool(L"general", L"window_borderless", false, path);
        g_config.release_cursor_unfocused =
            read_bool(L"general", L"release_cursor_unfocused", true, path);

        g_config.vr_enabled = read_bool(L"vr", L"enabled", true, path);
        g_config.vr_submit = read_bool(L"vr", L"submit", true, path);
        g_config.stereo = read_bool(L"vr", L"stereo", true, path);
        g_config.stereo_same_frame = read_bool(L"vr", L"stereo_same_frame", true, path);
        g_config.stereo_lock_pose = read_bool(L"vr", L"stereo_lock_pose", true, path);
        g_config.head_tracking = read_bool(L"vr", L"head_tracking", true, path);
        g_config.head_rotation = read_bool(L"vr", L"head_rotation", true, path);
        g_config.head_position = read_bool(L"vr", L"head_position", true, path);
        g_config.hmd_drives_camera = read_bool(L"vr", L"hmd_drives_camera", true, path);
        g_config.hmd_drives_position = read_bool(L"vr", L"hmd_drives_position", true, path);
        g_config.hmd_aim = read_bool(L"vr", L"hmd_aim", true, path);

        float value = read_float(L"vr", L"world_scale", L"291", path);
        g_config.world_scale = value > 0.0f ? value : 291.0f;
        value = read_float(L"vr", L"head_rotation_scale", L"1.0", path);
        g_config.head_rotation_scale =
            value >= 0.1f && value <= 5.0f ? value : 1.0f;
        value = read_float(L"vr", L"head_position_scale", L"1.0", path);
        g_config.head_position_scale = value >= 0.0f ? value : 1.0f;

        g_config.level_horizon = read_bool(L"vr", L"level_horizon", true, path);
        g_config.camera_auto_center = read_bool(L"vr", L"camera_auto_center", false, path);
        g_config.camera_shake = read_bool(L"vr", L"camera_shake", false, path);
        g_config.first_person = read_bool(L"vr", L"first_person", false, path);
        g_config.first_person_tracked_hands = read_bool(
            L"vr", L"first_person_tracked_hands", false, path);
        {
            wchar_t body[32] = {};
            GetPrivateProfileStringW(L"vr", L"first_person_body", L"hands",
                                     body, 32, path);
            g_config.first_person_full_body = _wcsnicmp(body, L"full", 4) == 0;
        }
        g_config.hand_model = GetPrivateProfileIntW(L"vr", L"hand_model", 2,
                                                    path) == 1 ? 1 : 2;
        g_config.first_person_mantle_smoothing = read_bool(
            L"vr", L"first_person_mantle_smoothing", true, path);
        g_config.physical_crouch = read_bool(
            L"vr", L"physical_crouch", true, path);
        {
            // off / bars / all; 0 and 1 as before (1 = everything, as
            // Preview 1 documented it), 2 also reads as all.
            wchar_t secure[16]{};
            GetPrivateProfileStringW(L"vr", L"auto_secure_catch", L"off",
                                     secure, 16, path);
            g_config.auto_secure_catch =
                _wcsnicmp(secure, L"bar", 3) == 0 ? 1 :
                (_wcsicmp(secure, L"all") == 0 || _wcsicmp(secure, L"1") == 0 ||
                 _wcsicmp(secure, L"2") == 0 || _wcsicmp(secure, L"on") == 0)
                    ? 2 : 0;
        }
        g_config.msaa = GetPrivateProfileIntW(L"vr", L"msaa", 4, path);
        {
            wchar_t culling[32]{};
            GetPrivateProfileStringW(L"vr", L"first_person_object_culling",
                                     L"frustum", culling, 32, path);
            g_config.first_person_object_culling_open =
                _wcsnicmp(culling, L"open", 4) == 0;
            GetPrivateProfileStringW(L"vr", L"terrain_culling", L"frustum",
                                     culling, 32, path);
            g_config.terrain_culling_open =
                _wcsnicmp(culling, L"open", 4) == 0;
            GetPrivateProfileStringW(L"vr", L"depth_of_field", L"off",
                                     culling, 32, path);
            g_config.depth_of_field =
                _wcsnicmp(culling, L"on", 2) == 0 ? 1
                : _wcsnicmp(culling, L"game", 4) == 0 ? -1 : 0;
        }
        {
            const float backdrop = read_float(
                L"vr", L"mission_prep_backdrop_scale", L"1.15", path);
            g_config.mission_prep_backdrop_scale =
                std::isfinite(backdrop) && backdrop >= 0.3f &&
                backdrop <= 6.0f ? backdrop : 1.15f;
            const float lag = read_float(
                L"vr", L"mission_prep_backdrop_lag", L"0.4", path);
            g_config.mission_prep_backdrop_lag =
                std::isfinite(lag) && lag >= 0.0f && lag <= 3.0f ? lag : 0.4f;
        }
        {
            wchar_t vignette[32] = {};
            GetPrivateProfileStringW(L"vr", L"comfort_vignette", L"off",
                                     vignette, 32, path);
            g_config.comfort_vignette =
                _wcsicmp(vignette, L"full") == 0 ? 2
                : _wcsicmp(vignette, L"jumps") == 0 ? 1 : 0;
        }
        {
            wchar_t mode[32] = {};
            GetPrivateProfileStringW(L"vr", L"third_person_mode", L"classic",
                                     mode, 32, path);
            g_config.third_person_mode =
                _wcsicmp(mode, L"shoulder") == 0 ? 1
                : _wcsicmp(mode, L"board") == 0 ? 2 : 0;
            g_config.view_cycle_all = _wcsicmp(mode, L"all") == 0;
            auto clamp_read = [&](const wchar_t* key, const wchar_t* def,
                                  float lo, float hi, float fallback) {
                const float v = read_float(L"vr", key, def, path);
                return std::isfinite(v) && v >= lo && v <= hi ? v : fallback;
            };
            g_config.third_person_smoothing = clamp_read(
                L"third_person_smoothing", L"0.12", 0.0f, 2.0f, 0.12f);
            g_config.shoulder_distance = clamp_read(
                L"shoulder_distance", L"1.3", 0.3f, 6.0f, 1.3f);
            g_config.shoulder_height = clamp_read(
                L"shoulder_height", L"0.0", -1.0f, 2.0f, 0.0f);
            g_config.shoulder_side = clamp_read(
                L"shoulder_side", L"0.35", -1.5f, 1.5f, 0.35f);
            g_config.board_lara_height_cm = clamp_read(
                L"board_lara_height_cm", L"15", 3.0f, 100.0f, 15.0f);
            g_config.board_distance = clamp_read(
                L"board_distance", L"0.5", 0.0f, 3.0f, 0.5f);
            g_config.board_height = clamp_read(
                L"board_height", L"0.35", 0.0f, 3.0f, 0.35f);
            g_config.board_hand_back = clamp_read(
                L"board_hand_back", L"0.08", -0.3f, 0.3f, 0.08f);
            g_config.board_god_hand = read_bool(
                L"vr", L"board_god_hand", true, path);
            g_config.camera_collision = read_bool(
                L"vr", L"camera_collision", true, path);
            g_config.first_person_no_spread = read_bool(
                L"vr", L"first_person_no_spread", true, path);
        }
        value = read_float(L"vr", L"weapon_recoil", L"1.0", path);
        g_config.weapon_recoil = std::isfinite(value)
            ? (value < 0.0f ? 0.0f : value > 2.0f ? 2.0f : value) : 1.0f;
        g_config.weapon_haptics = read_bool(
            L"vr", L"weapon_haptics", true, path);
        value = read_float(L"vr", L"first_person_eye_height", L"70", path);
        g_config.first_person_eye_height =
            value >= 0.0f && value <= 180.0f ? value : 70.0f;
        value = read_float(L"vr", L"first_person_smoothing", L"0.08", path);
        g_config.first_person_smoothing =
            value >= 0.0f && value <= 1.0f ? value : 0.08f;
        g_config.ui_passthrough = read_bool(L"vr", L"ui_passthrough", true, path);
        g_config.menu_world_locked = read_bool(L"vr", L"menu_world_locked", true, path);
        value = read_float(L"vr", L"ui_scale", L"0.6", path);
        g_config.ui_scale = value >= 0.1f && value <= 2.0f ? value : 0.6f;
        value = read_float(L"vr", L"hud_follow", L"0.85", path);
        g_config.hud_follow = value >= 0.0f && value <= 1.0f ? value : 0.85f;
        value = read_float(L"vr", L"cull_fov_degrees", L"120", path);
        g_config.cull_fov_degrees =
            value >= 100.0f && value <= 150.0f ? value : 120.0f;
        g_config.view_prescale_fix =
            read_bool(L"vr", L"view_prescale_fix", true, path);

        g_config.controllers = read_bool(L"controls", L"controllers", true, path);
        g_config.immersive_controls =
            read_bool(L"controls", L"immersive_controls", false, path);
        g_config.left_handed =
            read_bool(L"controls", L"left_handed", false, path);
        g_config.two_handed_guns =
            read_bool(L"controls", L"two_handed_guns", true, path);
        {
            wchar_t mode[32] = {};
            GetPrivateProfileStringW(L"controls", L"pistol_mode", L"akimbo",
                                     mode, 32, path);
            g_config.single_pistols = _wcsnicmp(mode, L"single", 6) == 0;
            wchar_t aim[32] = {};
            GetPrivateProfileStringW(L"controls", L"aim_mode", L"hand", aim,
                                     32, path);
            g_config.aim_mode = _wcsicmp(aim, L"hands_head") == 0 ? 2
                              : _wcsicmp(aim, L"hands") == 0 ? 1 : 0;
            wchar_t turn[32] = {};
            GetPrivateProfileStringW(L"controls", L"turn_mode", L"smooth",
                                     turn, 32, path);
            g_config.snap_turn = _wcsicmp(turn, L"snap") == 0;
            const float degrees = read_float(L"controls",
                                             L"snap_turn_degrees", L"30",
                                             path);
            g_config.snap_turn_degrees =
                std::isfinite(degrees) && degrees >= 5.0f && degrees <= 180.0f
                    ? degrees : 30.0f;
        }
        {
            wchar_t mode[32] = {};
            GetPrivateProfileStringW(L"controls", L"two_handed_mode", L"auto",
                                     mode, 32, path);
            g_config.two_handed_mode =
                _wcsnicmp(mode, L"toggle", 6) == 0 ? 1
                : _wcsnicmp(mode, L"hold", 4) == 0 ? 2 : 0;
            float v = read_float(L"controls", L"two_handed_grip_distance",
                                 L"0.30", path);
            g_config.two_handed_grip_distance =
                std::isfinite(v) && v >= 0.10f && v <= 0.80f ? v : 0.30f;
            v = read_float(L"controls", L"two_handed_radius", L"0.15", path);
            g_config.two_handed_radius =
                std::isfinite(v) && v >= 0.03f && v <= 0.40f ? v : 0.15f;
            v = read_float(L"controls", L"two_handed_release_radius", L"0.22",
                           path);
            g_config.two_handed_release_radius =
                std::isfinite(v) && v <= 0.60f ? v : 0.22f;
            if (g_config.two_handed_release_radius <
                g_config.two_handed_radius + 0.03f)
                g_config.two_handed_release_radius =
                    g_config.two_handed_radius + 0.03f;
        }
        g_config.look_vertical = read_bool(L"controls", L"look_vertical", true, path);
        g_config.controller_aim =
            read_bool(L"controls", L"controller_aim", true, path);
        g_config.vr_crosshair =
            read_bool(L"controls", L"vr_crosshair", true, path);
        value = read_float(L"controls", L"look_speed", L"900", path);
        g_config.look_speed = value >= 50.0f && value <= 20000.0f ? value : 900.0f;
    }

    unsigned config_defaults_added()
    {
        return g_defaults_added;
    }
}
